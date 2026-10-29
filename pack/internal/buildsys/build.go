package buildsys

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strings"
	"time"

	"capabilityos/pack/internal/config"
	"capabilityos/pack/internal/progress"
)

type UserlandOptions struct {
	AppID    string
	AppIDs   []string
	Force    bool
	Progress progress.Reporter
}

type KernelOptions struct {
	StepOverride string
	Progress     progress.Reporter
}

type UserlandResult struct {
	RebuiltSources            int
	CopiedArtifacts           int
	ReusedArtifacts           int
	DirectoryArtifactsChanged int
	SkippedApps               int
	ChangedArtifacts          []string
	RebuiltApps               []string
}

type KernelResult struct {
	Step    string
	Output  string
	Changed bool
}

type fileAppResult struct {
	Rebuilt         bool
	ArtifactChanged bool
	Directory       bool
	DirectoryDigest []byte
}

type fileAppPlan struct {
	App        config.App
	Artifact   string
	Source     config.FileSource
	SourcePath string
	RecipeID   string
}

type recipeRun struct {
	Before  map[string]recipeOutputState
	Changed bool
}

func BuildKernel(workspace *config.Workspace, opts KernelOptions) (KernelResult, error) {
	hasher := hasherFor(workspace)
	defer flushHasher(hasher)
	span := progress.Use(opts.Progress).Start("kernel", 3)
	defer span.Close()
	step := workspace.Kernel.Step
	if opts.StepOverride != "" {
		step = opts.StepOverride
	}
	if step == "" {
		step = "limine"
	}
	span.Set(1, "hashing kernel output")
	kernelDir := workspace.Path(workspace.Kernel.Dir)
	output := KernelOutputPath(workspace, step)
	before, err := hashIfExists(hasher, output)
	if err != nil {
		span.Fail("kernel output check failed")
		return KernelResult{}, err
	}
	span.Set(2, "preparing zig cache")
	localCache, globalCache := kernelCacheDirs(workspace)
	if err := os.MkdirAll(localCache, 0o755); err != nil {
		span.Fail("kernel cache failed")
		return KernelResult{}, err
	}
	if err := os.MkdirAll(globalCache, 0o755); err != nil {
		span.Fail("kernel cache failed")
		return KernelResult{}, err
	}
	span.Set(3, "zig build "+step)
	cmd := exec.Command("zig", "build", "--cache-dir", localCache, "--global-cache-dir", globalCache, step)
	cmd.Dir = kernelDir
	if err := run(cmd); err != nil {
		span.Fail("kernel build failed")
		return KernelResult{}, err
	}
	hasher.Invalidate(output)
	after, err := hasher.Hash(output)
	if err != nil {
		span.Fail("kernel output hash failed")
		return KernelResult{}, err
	}
	if err := writeKernelInputRecord(workspace, step, output, hasher); err != nil {
		span.Fail("kernel input record failed")
		return KernelResult{}, err
	}
	changed := !bytes.Equal(before, after)
	if changed {
		span.Done("kernel rebuilt")
	} else {
		span.Done("kernel unchanged")
	}
	return KernelResult{Step: step, Output: output, Changed: changed}, nil
}

func KernelOutputPath(workspace *config.Workspace, step string) string {
	if step == "" {
		step = workspace.Kernel.Step
	}
	if step == "" {
		step = "limine"
	}
	kernelDir := workspace.Path(workspace.Kernel.Dir)
	if step == "limine" {
		return filepath.Join(kernelDir, "zig-out", "bin", "limine", "pacha-kernel.elf")
	}
	if step == "boot-diag" {
		return filepath.Join(kernelDir, "zig-out", "bin", "limine", "DIAGBOOT.ELF")
	}
	return filepath.Join(kernelDir, "zig-out", "bin", step)
}

func BuildUserland(workspace *config.Workspace, opts UserlandOptions) (UserlandResult, error) {
	var result UserlandResult
	if err := CleanupLegacyState(workspace); err != nil {
		return result, err
	}
	hasher := hasherFor(workspace)
	defer flushHasher(hasher)
	apps, err := selectApps(workspace, opts.AppID, opts.AppIDs)
	if err != nil {
		return result, err
	}
	span := progress.Use(opts.Progress).Start("userland apps", int64(len(apps)))
	defer span.Close()
	var plans []fileAppPlan
	recipePlans := make(map[string][]int)
	var recipeRoots []string
	for _, app := range apps {
		if workspace.Skipped(app) {
			result.SkippedApps++
			span.Add(1, app.ID+" skipped")
			continue
		}
		if app.Kind() != "file" {
			span.Fail(app.ID + " unsupported")
			return result, fmt.Errorf("app %s has unsupported kind %s", app.ID, app.Kind())
		}
		source, err := app.FileSource()
		if err != nil {
			span.Fail(app.ID + " failed")
			return result, err
		}
		plan := fileAppPlan{
			App:        app,
			Artifact:   workspace.ArtifactPath(app),
			Source:     source,
			SourcePath: workspace.Path(source.Path),
			RecipeID:   source.Recipe,
		}
		plans = append(plans, plan)
		if source.Recipe != "" {
			if _, exists := recipePlans[source.Recipe]; !exists {
				recipeRoots = append(recipeRoots, source.Recipe)
			}
			recipePlans[source.Recipe] = append(recipePlans[source.Recipe], len(plans)-1)
		}
	}

	runs, err := scheduleRecipes(workspace, recipeRoots, opts.Force, hasher, span)
	if err != nil {
		span.Fail("recipe build failed")
		return result, err
	}

	for index := range plans {
		plan := &plans[index]
		span.Message(plan.App.ID + " file")
		if plan.Source.Recipe == "" && !pathExists(plan.SourcePath) {
			span.Fail(plan.App.ID + " source missing")
			return result, fmt.Errorf("missing source for app %s: %s", plan.App.ID, plan.Source.Path)
		}
		appResult, err := publishFileApp(workspace, *plan, runs, hasher)
		if err != nil {
			span.Fail(plan.App.ID + " failed")
			return result, err
		}
		if appResult.Rebuilt {
			result.RebuiltSources++
			result.RebuiltApps = append(result.RebuiltApps, plan.App.ID)
		}
		if appResult.ArtifactChanged {
			result.CopiedArtifacts++
			result.ChangedArtifacts = append(result.ChangedArtifacts, plan.Artifact)
			if appResult.Directory {
				result.DirectoryArtifactsChanged++
			}
		} else {
			result.ReusedArtifacts++
		}
		if len(appResult.DirectoryDigest) != 0 {
			if err := writeDirectoryArtifactDigest(workspace, plan.App.ID, appResult.DirectoryDigest); err != nil {
				span.Fail(plan.App.ID + " state failed")
				return result, err
			}
		}
		span.Add(1, plan.App.ID)
	}
	span.Done("userland ready")
	return result, nil
}

func CleanupLegacyState(workspace *config.Workspace) error {
	directory := workspace.Path(workspace.State, "build")
	entries, err := os.ReadDir(directory)
	if errors.Is(err, os.ErrNotExist) {
		return nil
	}
	if err != nil {
		return err
	}
	for _, entry := range entries {
		if entry.IsDir() || !strings.HasSuffix(entry.Name(), ".sha256") || strings.HasSuffix(entry.Name(), ".tree.sha256") {
			continue
		}
		if err := os.Remove(filepath.Join(directory, entry.Name())); err != nil && !errors.Is(err, os.ErrNotExist) {
			return err
		}
	}
	return nil
}

func selectApps(workspace *config.Workspace, appID string, appIDs []string) ([]config.App, error) {
	if len(appIDs) != 0 {
		if appID != "" {
			return nil, fmt.Errorf("select either AppID or AppIDs")
		}
		apps := make([]config.App, 0, len(appIDs))
		seen := make(map[string]bool, len(appIDs))
		for _, id := range appIDs {
			if seen[id] {
				return nil, fmt.Errorf("duplicate app: %s", id)
			}
			app, ok := workspace.App(id)
			if !ok {
				return nil, fmt.Errorf("app not found: %s", id)
			}
			seen[id] = true
			apps = append(apps, app)
		}
		return apps, nil
	}
	if appID != "" {
		app, ok := workspace.App(appID)
		if !ok {
			return nil, fmt.Errorf("app not found: %s", appID)
		}
		return []config.App{app}, nil
	}
	return workspace.Apps(), nil
}

func publishFileApp(workspace *config.Workspace, plan fileAppPlan, runs map[string]recipeRun, hasher *Hasher) (fileAppResult, error) {
	sourceInfo, err := os.Stat(plan.SourcePath)
	if err != nil {
		return fileAppResult{}, err
	}
	recipe, recipeRan := runs[plan.RecipeID]
	if sourceInfo.IsDir() {
		digest, err := treeDigest(plan.SourcePath)
		if err != nil {
			return fileAppResult{}, err
		}
		previous, exists, err := readDirectoryArtifactDigest(workspace, plan.App.ID)
		if err != nil {
			return fileAppResult{}, err
		}
		linked, err := linkDirArtifact(workspace, plan.SourcePath, plan.Artifact)
		if err != nil {
			return fileAppResult{}, err
		}
		changed := linked || !exists || !bytes.Equal(previous, digest)
		current := recipeOutputState{Path: plan.Source.Path, Kind: "dir", Digest: hex.EncodeToString(digest)}
		before, existed := recipe.Before[plan.SourcePath]
		return fileAppResult{
			Rebuilt:         recipeRan && (!existed || !sameOutputDigest(before, current)),
			ArtifactChanged: changed,
			Directory:       true,
			DirectoryDigest: digest,
		}, nil
	}
	copied, err := copyFileIfChanged(plan.SourcePath, plan.Artifact, hasher)
	if err != nil {
		return fileAppResult{}, err
	}
	rebuilt := false
	if recipeRan {
		digest, err := hasher.Hash(plan.SourcePath)
		if err != nil {
			return fileAppResult{}, err
		}
		current := recipeOutputState{Path: plan.Source.Path, Kind: "file", Digest: hex.EncodeToString(digest)}
		before, existed := recipe.Before[plan.SourcePath]
		rebuilt = !existed || !sameOutputDigest(before, current)
	}
	return fileAppResult{Rebuilt: rebuilt, ArtifactChanged: copied}, nil
}

func copyFileIfChanged(source string, dest string, hasher *Hasher) (bool, error) {
	same, err := sameFileContents(source, dest, hasher)
	if err != nil {
		return false, err
	}
	if same {
		return false, nil
	}
	if err := os.MkdirAll(filepath.Dir(dest), 0o755); err != nil {
		return false, err
	}
	src, err := os.Open(source)
	if err != nil {
		return false, err
	}
	defer src.Close()
	tmp := dest + ".tmp"
	dst, err := os.OpenFile(tmp, os.O_CREATE|os.O_TRUNC|os.O_WRONLY, 0o755)
	if err != nil {
		return false, err
	}
	if _, err := io.Copy(dst, src); err != nil {
		_ = dst.Close()
		_ = os.Remove(tmp)
		return false, err
	}
	if err := dst.Close(); err != nil {
		_ = os.Remove(tmp)
		return false, err
	}
	if err := os.Rename(tmp, dest); err != nil {
		_ = os.Remove(tmp)
		return false, err
	}
	hasher.Invalidate(dest)
	if info, err := os.Stat(source); err == nil {
		_ = os.Chtimes(dest, info.ModTime(), info.ModTime())
	}
	return true, nil
}

func linkDirArtifact(workspace *config.Workspace, source string, dest string) (bool, error) {
	sourceAbs, err := filepath.Abs(source)
	if err != nil {
		return false, err
	}
	destAbs, err := filepath.Abs(dest)
	if err != nil {
		return false, err
	}
	if link, err := os.Readlink(destAbs); err == nil {
		linkAbs := link
		if !filepath.IsAbs(linkAbs) {
			linkAbs = filepath.Join(filepath.Dir(destAbs), linkAbs)
		}
		linkAbs, _ = filepath.Abs(linkAbs)
		if linkAbs == sourceAbs {
			return false, nil
		}
	}
	if err := ensureArtifactTarget(workspace, destAbs); err != nil {
		return false, err
	}
	if err := os.RemoveAll(destAbs); err != nil {
		return false, err
	}
	if err := os.MkdirAll(filepath.Dir(destAbs), 0o755); err != nil {
		return false, err
	}
	if err := os.Symlink(sourceAbs, destAbs); err != nil {
		return false, err
	}
	return true, nil
}

func ensureArtifactTarget(workspace *config.Workspace, path string) error {
	root, err := filepath.Abs(workspace.Path(workspace.Artifacts, "userland"))
	if err != nil {
		return err
	}
	rel, err := filepath.Rel(root, path)
	if err != nil {
		return err
	}
	if rel == "." || strings.HasPrefix(rel, ".."+string(filepath.Separator)) || rel == ".." || filepath.IsAbs(rel) {
		return fmt.Errorf("refusing to replace path outside userland artifacts: %s", path)
	}
	return nil
}

func directoryArtifactDigestPath(workspace *config.Workspace, appID string) string {
	return filepath.Join(workspace.Path(workspace.State), "build", sanitizeFilename(appID)+".tree.sha256")
}

func readDirectoryArtifactDigest(workspace *config.Workspace, appID string) ([]byte, bool, error) {
	data, err := os.ReadFile(directoryArtifactDigestPath(workspace, appID))
	if errors.Is(err, os.ErrNotExist) {
		return nil, false, nil
	}
	if err != nil {
		return nil, false, err
	}
	digest, err := hex.DecodeString(string(bytes.TrimSpace(data)))
	if err != nil {
		return nil, false, fmt.Errorf("read directory artifact digest for %s: %w", appID, err)
	}
	if len(digest) != sha256.Size {
		return nil, false, fmt.Errorf("read directory artifact digest for %s: got %d bytes, want %d", appID, len(digest), sha256.Size)
	}
	return digest, true, nil
}

func writeDirectoryArtifactDigest(workspace *config.Workspace, appID string, digest []byte) error {
	path := directoryArtifactDigestPath(workspace, appID)
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return err
	}
	return os.WriteFile(path, []byte(fmt.Sprintf("%x\n", digest)), 0o644)
}

type treeDigestEntry struct {
	path       string
	typeBits   os.FileMode
	permission os.FileMode
	size       int64
	mtimeNS    int64
	linkTarget string
	content    string
}

func treeDigest(path string) ([]byte, error) {
	return treeDigestFor(path, true, nil)
}

func treeOutputDigest(path string, hasher *Hasher) ([]byte, error) {
	return treeDigestFor(path, false, hasher)
}

func treeDigestFor(path string, includeMtime bool, hasher *Hasher) ([]byte, error) {
	root, err := filepath.EvalSymlinks(path)
	if err != nil {
		return nil, err
	}
	var entries []treeDigestEntry
	err = filepath.WalkDir(root, func(item string, entry os.DirEntry, err error) error {
		if err != nil {
			return err
		}
		info, err := os.Lstat(item)
		if err != nil {
			return err
		}
		rel, err := filepath.Rel(root, item)
		if err != nil {
			return err
		}
		itemDigest := treeDigestEntry{
			path:       filepath.ToSlash(rel),
			typeBits:   info.Mode().Type(),
			permission: info.Mode().Perm(),
			size:       info.Size(),
			mtimeNS:    info.ModTime().UnixNano(),
		}
		if info.Mode()&os.ModeSymlink != 0 {
			itemDigest.linkTarget, err = os.Readlink(item)
			if err != nil {
				return err
			}
		} else if info.Mode().IsRegular() {
			var content []byte
			var hashErr error
			if hasher == nil {
				content, hashErr = hashFileUncached(item)
			} else {
				content, hashErr = hasher.Hash(item)
			}
			if hashErr != nil {
				return hashErr
			}
			itemDigest.content = hex.EncodeToString(content)
		}
		entries = append(entries, itemDigest)
		return nil
	})
	if err != nil {
		return nil, err
	}
	sort.Slice(entries, func(i, j int) bool { return entries[i].path < entries[j].path })
	hash := sha256.New()
	writeHashString(hash, "pacgo-directory-artifact\n")
	for _, entry := range entries {
		writeHashField(hash, entry.path)
		writeHashString(hash, fmt.Sprintf("%d\x00%d\x00%d\x00", entry.typeBits, entry.permission, entry.size))
		if includeMtime {
			writeHashString(hash, fmt.Sprintf("%d\x00", entry.mtimeNS))
		}
		writeHashField(hash, entry.linkTarget)
		writeHashField(hash, entry.content)
	}
	return hash.Sum(nil), nil
}

func writeHashField(hash io.Writer, value string) {
	writeHashString(hash, fmt.Sprintf("%d:", len(value)))
	writeHashString(hash, value)
	writeHashString(hash, "\x00")
}

func sanitizeFilename(value string) string {
	var out strings.Builder
	for _, ch := range value {
		if ch >= 'a' && ch <= 'z' || ch >= 'A' && ch <= 'Z' || ch >= '0' && ch <= '9' || ch == '_' || ch == '-' || ch == '.' {
			out.WriteRune(ch)
		} else {
			out.WriteByte('_')
		}
	}
	if out.Len() == 0 {
		return "app"
	}
	return out.String()
}

func writeHashString(hash io.Writer, value string) {
	_, _ = io.WriteString(hash, value)
}

func sameFileContents(lhs string, rhs string, hasher *Hasher) (bool, error) {
	lhsInfo, err := os.Stat(lhs)
	if err != nil {
		return false, err
	}
	rhsInfo, err := os.Stat(rhs)
	if errors.Is(err, os.ErrNotExist) {
		return false, nil
	}
	if err != nil {
		return false, err
	}
	if lhsInfo.IsDir() || rhsInfo.IsDir() {
		return false, nil
	}
	if lhsInfo.Size() != rhsInfo.Size() {
		return false, nil
	}
	if lhsInfo.Size() == 0 {
		return true, nil
	}
	lhsHash, err := hasher.Hash(lhs)
	if err != nil {
		return false, err
	}
	rhsHash, err := hasher.Hash(rhs)
	if err != nil {
		return false, err
	}
	return bytes.Equal(lhsHash, rhsHash), nil
}

func run(cmd *exec.Cmd) error {
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	cmd.Stdin = os.Stdin
	started := time.Now()
	err := cmd.Run()
	if err != nil {
		return fmt.Errorf("%s failed after %s: %w", strings.Join(cmd.Args, " "), time.Since(started).Round(time.Millisecond), err)
	}
	return nil
}

func pathExists(path string) bool {
	_, err := os.Stat(path)
	return err == nil
}
