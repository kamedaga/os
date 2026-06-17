package buildsys

import (
	"bytes"
	"crypto/sha256"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strings"
	"time"

	"capabilityos/pack/internal/config"
	"capabilityos/pack/internal/progress"
)

type UserlandOptions struct {
	AppID    string
	Force    bool
	Progress progress.Reporter
}

type KernelOptions struct {
	Force    bool
	Progress progress.Reporter
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
	Skipped bool
}

type sourceState struct {
	Exists  bool
	IsDir   bool
	Size    int64
	ModTime int64
}

type rebuildCheck struct {
	Needed      bool
	Fingerprint []byte
}

func BuildKernel(workspace *config.Workspace, opts KernelOptions) (KernelResult, error) {
	span := progress.Use(opts.Progress).Start("kernel", 3)
	defer span.Close()
	step := workspace.Kernel.Step
	if step == "" {
		step = "efi"
	}
	span.Set(1, "checking kernel inputs")
	kernelDir := workspace.Path(workspace.Kernel.Dir)
	output := filepath.Join(kernelDir, "zig-out", "bin", "EFI", "BOOT", "BOOTX64.EFI")
	if !opts.Force {
		upToDate, err := outputUpToDate(output, []string{
			filepath.Join(kernelDir, "build.zig"),
			filepath.Join(kernelDir, "src"),
			filepath.Join(kernelDir, "abi"),
		})
		if err != nil {
			span.Fail("kernel check failed")
			return KernelResult{}, err
		}
		if upToDate {
			span.Done("kernel up-to-date")
			return KernelResult{Step: step, Output: output, Skipped: true}, nil
		}
	}
	span.Set(2, "preparing zig cache")
	localCache := zigCachePath(workspace, "kernel-local")
	globalCache := zigCachePath(workspace, "kernel-global")
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
	span.Done("kernel built")
	return KernelResult{Step: step, Output: output}, nil
}

func BuildUserland(workspace *config.Workspace, opts UserlandOptions) (UserlandResult, error) {
	var result UserlandResult
	rebuilds := map[string]bool{}
	apps, err := selectApps(workspace, opts.AppID)
	if err != nil {
		return result, err
	}
	span := progress.Use(opts.Progress).Start("userland apps", int64(len(apps)))
	defer span.Close()
	for _, app := range apps {
		span.Message(app.ID)
		if workspace.Skipped(app) {
			result.SkippedApps++
			span.Add(1, app.ID+" skipped")
			continue
		}
		artifact := workspace.ArtifactPath(app)
		switch app.Kind() {
		case "file":
			span.Message(app.ID + " file")
			rebuilt, copied, directoryArtifact, err := buildFileApp(workspace, app, artifact, opts.Force, rebuilds)
			if err != nil {
				span.Fail(app.ID + " failed")
				return result, err
			}
			if rebuilt {
				result.RebuiltSources++
				result.RebuiltApps = append(result.RebuiltApps, app.ID)
			}
			if copied {
				result.CopiedArtifacts++
				result.ChangedArtifacts = append(result.ChangedArtifacts, artifact)
				if directoryArtifact {
					result.DirectoryArtifactsChanged++
				}
			} else {
				result.ReusedArtifacts++
			}
		default:
			span.Fail(app.ID + " unsupported")
			return result, fmt.Errorf("app %s has unsupported kind %s", app.ID, app.Kind())
		}
		span.Add(1, app.ID)
	}
	span.Done("userland ready")
	return result, nil
}

func selectApps(workspace *config.Workspace, appID string) ([]config.App, error) {
	if appID != "" {
		app, ok := workspace.App(appID)
		if !ok {
			return nil, fmt.Errorf("app not found: %s", appID)
		}
		return []config.App{app}, nil
	}
	return workspace.Apps(), nil
}

func buildFileApp(workspace *config.Workspace, app config.App, artifact string, force bool, rebuilds map[string]bool) (bool, bool, bool, error) {
	source, err := app.FileSource()
	if err != nil {
		return false, false, false, err
	}
	sourcePath := workspace.Path(source.Path)
	rebuilt := false
	check, err := fileSourceNeedsRebuild(workspace, app, source, sourcePath, force)
	if err != nil {
		return false, false, false, err
	}
	if check.Needed {
		if len(source.Rebuild) == 0 {
			return false, false, false, fmt.Errorf("missing source for app %s: %s", app.ID, source.Path)
		}
		key := strings.Join(source.Rebuild, "\x00")
		if !rebuilds[key] {
			before := statSource(sourcePath)
			cmd := exec.Command(source.Rebuild[0], source.Rebuild[1:]...)
			cmd.Dir = workspace.Root
			if err := run(cmd); err != nil {
				return false, false, false, err
			}
			rebuilds[key] = true
			rebuilt = !sameSourceState(before, statSource(sourcePath))
		}
		if !pathExists(sourcePath) {
			return rebuilt, false, false, fmt.Errorf("rebuild for app %s did not create %s", app.ID, source.Path)
		}
	}
	if len(check.Fingerprint) > 0 {
		if err := writeFileSourceFingerprint(workspace, app.ID, check.Fingerprint); err != nil {
			return rebuilt, false, false, err
		}
	}
	sourceInfo, err := os.Stat(sourcePath)
	if err != nil {
		return rebuilt, false, false, err
	}
	if sourceInfo.IsDir() {
		copied, err := linkDirArtifact(workspace, sourcePath, artifact)
		return rebuilt, copied || rebuilt, true, err
	}
	copied, err := copyFileIfChanged(sourcePath, artifact)
	return rebuilt, copied, false, err
}

func statSource(path string) sourceState {
	info, err := os.Stat(path)
	if err != nil {
		return sourceState{}
	}
	return sourceState{
		Exists:  true,
		IsDir:   info.IsDir(),
		Size:    info.Size(),
		ModTime: info.ModTime().UnixNano(),
	}
}

func sameSourceState(lhs sourceState, rhs sourceState) bool {
	return lhs.Exists == rhs.Exists &&
		lhs.IsDir == rhs.IsDir &&
		lhs.Size == rhs.Size &&
		lhs.ModTime == rhs.ModTime
}

func copyFileIfChanged(source string, dest string) (bool, error) {
	same, err := sameFileContents(source, dest)
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

func replaceIfChanged(source string, dest string) (bool, error) {
	same, err := sameFileContents(source, dest)
	if err != nil {
		return false, err
	}
	if same {
		if info, err := os.Stat(source); err == nil {
			_ = os.Chtimes(dest, info.ModTime(), info.ModTime())
		}
		return false, nil
	}
	if err := os.MkdirAll(filepath.Dir(dest), 0o755); err != nil {
		return false, err
	}
	return true, os.Rename(source, dest)
}

func fileSourceNeedsRebuild(workspace *config.Workspace, app config.App, source config.FileSource, sourcePath string, force bool) (rebuildCheck, error) {
	if len(source.Rebuild) == 0 {
		return rebuildCheck{Needed: force || !pathExists(sourcePath)}, nil
	}
	fingerprint, err := fileSourceFingerprint(workspace, source)
	if err != nil {
		return rebuildCheck{}, err
	}
	if force || !pathExists(sourcePath) {
		return rebuildCheck{Needed: true, Fingerprint: fingerprint}, nil
	}
	existing, err := os.ReadFile(fileSourceFingerprintPath(workspace, app.ID))
	if err != nil {
		return rebuildCheck{Needed: true, Fingerprint: fingerprint}, nil
	}
	if !bytes.Equal(bytes.TrimSpace(existing), []byte(fmt.Sprintf("%x", fingerprint))) {
		return rebuildCheck{Needed: true, Fingerprint: fingerprint}, nil
	}
	return rebuildCheck{Needed: false, Fingerprint: fingerprint}, nil
}

func fileSourceDependencies(workspace *config.Workspace, source config.FileSource) []string {
	seen := map[string]bool{}
	var deps []string
	add := func(path string) {
		if path == "" || strings.HasPrefix(path, "-") {
			return
		}
		if !filepath.IsAbs(path) {
			path = workspace.Path(path)
		}
		path = filepath.Clean(path)
		if seen[path] {
			return
		}
		seen[path] = true
		deps = append(deps, path)
	}
	for _, arg := range source.Rebuild {
		add(arg)
		if strings.HasSuffix(arg, ".sh") {
			for _, dep := range inferScriptDependencies(workspace, arg) {
				add(dep)
			}
		}
	}
	return deps
}

func fileSourceFingerprint(workspace *config.Workspace, source config.FileSource) ([]byte, error) {
	hash := sha256.New()
	writeHashString(hash, "pacgo-file-source-v1\n")
	for _, arg := range source.Rebuild {
		writeHashString(hash, "arg\x00"+arg+"\n")
	}
	for _, dep := range fileSourceDependencies(workspace, source) {
		if err := hashPath(hash, dep); err != nil {
			if os.IsNotExist(err) {
				continue
			}
			return nil, err
		}
	}
	return hash.Sum(nil), nil
}

func hashPath(hash io.Writer, path string) error {
	info, err := os.Stat(path)
	if err != nil {
		return err
	}
	path = filepath.Clean(path)
	if !info.IsDir() {
		return hashFileWithName(hash, path, path, info)
	}
	return filepath.WalkDir(path, func(item string, entry os.DirEntry, err error) error {
		if err != nil {
			return err
		}
		name := entry.Name()
		if entry.IsDir() {
			if name == ".git" || name == ".zig-cache" || name == "zig-out" || name == "CMakeFiles" {
				return filepath.SkipDir
			}
			return nil
		}
		if entry.Type()&os.ModeType != 0 {
			return nil
		}
		info, err := entry.Info()
		if err != nil {
			return err
		}
		return hashFileWithName(hash, path, item, info)
	})
}

func hashFileWithName(hash io.Writer, root string, path string, info os.FileInfo) error {
	rel, err := filepath.Rel(root, path)
	if err != nil {
		return err
	}
	writeHashString(hash, "file\x00"+filepath.ToSlash(path)+"\x00"+filepath.ToSlash(rel)+"\x00")
	writeHashString(hash, fmt.Sprintf("%d\x00", info.Size()))
	fileHash, err := fileHash(path)
	if err != nil {
		return err
	}
	if _, err := hash.Write(fileHash); err != nil {
		return err
	}
	writeHashString(hash, "\n")
	return nil
}

func fileSourceFingerprintPath(workspace *config.Workspace, appID string) string {
	return filepath.Join(workspace.Path(workspace.State), "build", sanitizeFilename(appID)+".sha256")
}

func writeFileSourceFingerprint(workspace *config.Workspace, appID string, fingerprint []byte) error {
	path := fileSourceFingerprintPath(workspace, appID)
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return err
	}
	return os.WriteFile(path, []byte(fmt.Sprintf("%x\n", fingerprint)), 0o644)
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

func inferScriptDependencies(workspace *config.Workspace, script string) []string {
	path := script
	if !filepath.IsAbs(path) {
		path = workspace.Path(path)
	}
	bytes, err := os.ReadFile(path)
	if err != nil {
		return nil
	}
	text := string(bytes)
	sourceFlag := regexp.MustCompile(`(?m)-S[ \t]+([^ \t\\]+)`)
	var deps []string
	for _, match := range sourceFlag.FindAllStringSubmatch(text, -1) {
		deps = append(deps, match[1])
	}
	freestandingHelper := regexp.MustCompile(`(?m)configure_freestanding_cmake[ \t]+(?:"[^"]+"|'[^']+'|[^ \t]+)[ \t]+(?:"([^"]+)"|'([^']+)'|([^ \t\n]+))`)
	for _, match := range freestandingHelper.FindAllStringSubmatch(text, -1) {
		for _, group := range match[1:] {
			if group != "" {
				deps = append(deps, group)
				break
			}
		}
	}
	return deps
}

func outputNeedsBuild(output string, inputs []string) (bool, error) {
	outputInfo, err := os.Stat(output)
	if errors.Is(err, os.ErrNotExist) {
		return true, nil
	}
	if err != nil {
		return false, err
	}
	for _, input := range inputs {
		info, err := os.Stat(input)
		if err != nil {
			return false, err
		}
		if info.ModTime().After(outputInfo.ModTime()) {
			return true, nil
		}
	}
	return false, nil
}

func outputUpToDate(output string, inputs []string) (bool, error) {
	outputInfo, err := os.Stat(output)
	if errors.Is(err, os.ErrNotExist) {
		return false, nil
	}
	if err != nil {
		return false, err
	}
	for _, input := range inputs {
		newer, err := pathNewerThan(input, outputInfo.ModTime())
		if err != nil {
			return false, err
		}
		if newer {
			return false, nil
		}
	}
	return true, nil
}

func pathNewerThan(path string, cutoff time.Time) (bool, error) {
	info, err := os.Stat(path)
	if err != nil {
		return false, err
	}
	if !info.IsDir() {
		return info.ModTime().After(cutoff), nil
	}
	newer := false
	err = filepath.WalkDir(path, func(item string, entry os.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if entry.IsDir() {
			name := entry.Name()
			if name == ".zig-cache" || name == "zig-out" {
				return filepath.SkipDir
			}
			return nil
		}
		info, err := entry.Info()
		if err != nil {
			return err
		}
		if info.ModTime().After(cutoff) {
			newer = true
			return filepath.SkipAll
		}
		return nil
	})
	return newer, err
}

func sameFileContents(lhs string, rhs string) (bool, error) {
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
	lhsHash, err := fileHash(lhs)
	if err != nil {
		return false, err
	}
	rhsHash, err := fileHash(rhs)
	if err != nil {
		return false, err
	}
	return bytes.Equal(lhsHash, rhsHash), nil
}

func fileHash(path string) ([]byte, error) {
	file, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer file.Close()
	hash := sha256.New()
	if _, err := io.Copy(hash, file); err != nil {
		return nil, err
	}
	return hash.Sum(nil), nil
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

func zigCachePath(workspace *config.Workspace, name string) string {
	return filepath.Join(os.TempDir(), "capos-zig-cache", filepath.Base(workspace.Root), name)
}
