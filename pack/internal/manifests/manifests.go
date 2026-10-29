package manifests

import (
	"bytes"
	"crypto/sha256"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"sync"

	"capabilityos/pack/internal/config"
	"capabilityos/pack/internal/progress"
)

type Outputs struct {
	Bootfs  string
	Rootfs  string
	Startup string
}

type Result struct {
	Outputs Outputs
	Bootfs  int
	Rootfs  int
	Startup int
	Cached  bool
}

type Options struct {
	Progress progress.Reporter
}

type entry struct {
	ImagePath  string
	SourcePath string
	Dir        bool
}

type directoryManifestEntry struct {
	Relative string
	Type     os.FileMode
}

type directoryManifestSnapshot struct {
	SourceRoot string
	Entries    []directoryManifestEntry
}

var (
	runningExecutableIdentityOnce   sync.Once
	runningExecutableIdentityDigest []byte
	runningExecutableIdentityErr    error
	manifestExecutableIdentity      = runningExecutableIdentity
)

type startupNode struct {
	AppID    string
	Action   string
	Name     string
	Path     string
	Label    string
	Load     string
	After    []string
	Requires []string
	Provides []string
	Ensure   []string
	Block    []string
	Device   []string
	Input    []string
}

func Generate(workspace *config.Workspace) (Result, error) {
	return GenerateWithOptions(workspace, Options{})
}

func GenerateWithOptions(workspace *config.Workspace, opts Options) (Result, error) {
	return generate(workspace, opts.Progress)
}

func ExistingIfFresh(workspace *config.Workspace) (Result, bool, error) {
	return ExistingIfFreshWithOptions(workspace, Options{})
}

func ExistingIfFreshWithOptions(workspace *config.Workspace, opts Options) (Result, bool, error) {
	span := progress.Use(opts.Progress).Start("manifests cache", 3)
	defer span.Close()
	outputs := manifestOutputs(workspace)
	span.Set(1, "checking manifest content key")
	fresh, err := outputsFresh(workspace, outputs)
	if err != nil || !fresh {
		if err != nil {
			span.Fail("manifest cache check failed")
		} else {
			span.Done("manifests need regeneration")
		}
		return Result{}, false, err
	}
	span.Set(2, "counting bootfs entries")
	bootfsCount, err := countEntries(outputs.Bootfs)
	if err != nil {
		span.Fail("bootfs count failed")
		return Result{}, false, err
	}
	span.Message("counting rootfs entries")
	rootfsCount, err := countEntries(outputs.Rootfs)
	if err != nil {
		span.Fail("rootfs count failed")
		return Result{}, false, err
	}
	span.Set(3, "counting startup entries")
	startupCount, err := countEntries(outputs.Startup)
	if err != nil {
		span.Fail("startup count failed")
		return Result{}, false, err
	}
	span.Done("manifests up-to-date")
	return Result{
		Outputs: outputs,
		Bootfs:  bootfsCount,
		Rootfs:  rootfsCount,
		Startup: startupCount,
		Cached:  true,
	}, true, nil
}

func generate(workspace *config.Workspace, reporter progress.Reporter) (Result, error) {
	span := progress.Use(reporter).Start("manifests", 4)
	defer span.Close()
	apps := manifestApps(workspace)
	outputs := manifestOutputs(workspace)
	snapshots, key, err := manifestInputSnapshot(workspace, apps)
	if err != nil {
		span.Fail("manifest inputs failed")
		return Result{}, err
	}
	if err := removeManifestKey(workspace); err != nil {
		span.Fail("manifest key invalidation failed")
		return Result{}, err
	}
	span.Set(1, "rendering startup manifest")
	startupContents, startupCount, err := renderStartup(workspace, apps)
	if err != nil {
		span.Fail("startup manifest failed")
		return Result{}, err
	}
	if err := writeIfChanged(outputs.Startup, startupContents); err != nil {
		span.Fail("startup manifest write failed")
		return Result{}, err
	}
	span.Set(2, "rendering bootfs manifest")
	bootfsContents, bootfsCount, err := renderFS(workspace, "bootfs", apps, snapshots, outputs.Bootfs, outputs.Startup, span)
	if err != nil {
		span.Fail("bootfs manifest failed")
		return Result{}, err
	}
	if err := writeIfChanged(outputs.Bootfs, bootfsContents); err != nil {
		span.Fail("bootfs manifest write failed")
		return Result{}, err
	}
	span.Set(3, "rendering rootfs manifest")
	rootfsContents, rootfsCount, err := renderFS(workspace, "rootfs", apps, snapshots, outputs.Rootfs, outputs.Startup, span)
	if err != nil {
		span.Fail("rootfs manifest failed")
		return Result{}, err
	}
	if err := writeIfChanged(outputs.Rootfs, rootfsContents); err != nil {
		span.Fail("rootfs manifest write failed")
		return Result{}, err
	}
	if err := writeManifestKey(workspace, key); err != nil {
		span.Fail("manifest key write failed")
		return Result{}, err
	}
	span.Set(4, "manifest files written")
	span.Done("manifests generated")
	return Result{
		Outputs: outputs,
		Bootfs:  bootfsCount,
		Rootfs:  rootfsCount,
		Startup: startupCount,
		Cached:  false,
	}, nil
}

func manifestOutputs(workspace *config.Workspace) Outputs {
	return Outputs{
		Bootfs:  workspace.Path(workspace.Manifests.Bootfs),
		Rootfs:  workspace.Path(workspace.Manifests.Rootfs),
		Startup: workspace.Path(workspace.Manifests.Startup),
	}
}

func manifestApps(workspace *config.Workspace) []config.App {
	var apps []config.App
	for _, app := range workspace.Apps() {
		if workspace.Skipped(app) && !workspace.Skip.IncludeSkippedArtifacts {
			continue
		}
		apps = append(apps, app)
	}
	return apps
}

func renderFS(workspace *config.Workspace, fsName string, apps []config.App, snapshots map[string]directoryManifestSnapshot, manifestPath string, startupPath string, span progress.Span) (string, int, error) {
	var entries []entry
	if fsName == "rootfs" {
		for _, dir := range workspace.RootfsDirs {
			entries = append(entries, entry{ImagePath: cleanImagePath(dir), Dir: true})
		}
	}
	for _, app := range apps {
		artifact := workspace.ArtifactPath(app)
		for _, publish := range app.Publishes(fsName) {
			if snapshot, ok := snapshots[artifact]; ok {
				count := 0
				if span != nil {
					span.Message(fmt.Sprintf("%s %s: scanning %s", fsName, app.ID, publish.Path))
				}
				if err := appendDirectoryEntries(&entries, cleanImagePath(publish.Path), snapshot, manifestPath, func(path string) {
					count++
					if span != nil && count%64 == 0 {
						span.Message(fmt.Sprintf("%s %s: %d files", fsName, app.ID, count))
					}
				}); err != nil {
					return "", 0, err
				}
			} else {
				sourcePath, err := sourcePathForManifest(manifestPath, artifact)
				if err != nil {
					return "", 0, err
				}
				entries = append(entries, entry{
					ImagePath:  cleanImagePath(publish.Path),
					SourcePath: sourcePath,
				})
			}
		}
	}
	if includes(workspace.StartupManifest.Include, fsName) {
		sourcePath, err := sourcePathForManifest(manifestPath, startupPath)
		if err != nil {
			return "", 0, err
		}
		entries = append(entries, entry{
			ImagePath:  cleanImagePath(workspace.StartupManifest.Path),
			SourcePath: sourcePath,
		})
	}
	sortEntries(entries)
	if err := ensureNoDuplicates(fsName, entries); err != nil {
		return "", 0, err
	}
	var out strings.Builder
	out.WriteString("# Generated by pacgo. DO NOT EDIT.\n")
	for _, item := range entries {
		out.WriteString(item.ImagePath)
		out.WriteByte('=')
		if item.Dir {
			out.WriteString("@dir")
		} else {
			out.WriteString(filepath.ToSlash(item.SourcePath))
		}
		out.WriteByte('\n')
	}
	return out.String(), len(entries), nil
}

func appendDirectoryEntries(entries *[]entry, imageRoot string, snapshot directoryManifestSnapshot, manifestPath string, onFile func(path string)) error {
	for _, item := range snapshot.Entries {
		if item.Relative == "." || item.Type != 0 {
			continue
		}
		path := filepath.Join(snapshot.SourceRoot, filepath.FromSlash(item.Relative))
		sourcePath, err := sourcePathForManifest(manifestPath, path)
		if err != nil {
			return err
		}
		*entries = append(*entries, entry{
			ImagePath:  joinImagePath(imageRoot, item.Relative),
			SourcePath: sourcePath,
		})
		if onFile != nil {
			onFile(path)
		}
	}
	return nil
}

func renderStartup(workspace *config.Workspace, apps []config.App) (string, int, error) {
	nodes, err := startupNodes(apps)
	if err != nil {
		return "", 0, err
	}
	var out strings.Builder
	out.WriteString("# Generated by pacgo. DO NOT EDIT.\n")
	for _, node := range nodes {
		out.WriteString("action=")
		out.WriteString(node.Action)
		out.WriteString(" name=")
		out.WriteString(node.Name)
		out.WriteString(" path=")
		out.WriteString(node.Path)
		out.WriteString(" label=")
		out.WriteString(node.Label)
		out.WriteString(" load=")
		out.WriteString(node.Load)
		appendSingle(&out, "block", node.Block)
		appendSingle(&out, "input", node.Input)
		appendSingle(&out, "device", node.Device)
		appendList(&out, "after", node.After)
		appendList(&out, "requires", node.Requires)
		appendList(&out, "ensure", node.Ensure)
		appendList(&out, "provides", node.Provides)
		out.WriteByte('\n')
	}
	return out.String(), len(nodes), nil
}

func startupNodes(apps []config.App) ([]startupNode, error) {
	var nodes []startupNode
	names := map[string]bool{}
	for _, app := range apps {
		startup, ok := app.StartupConfig()
		if !ok {
			continue
		}
		publish, ok := findPublish(app, startup.Publish)
		if !ok {
			return nil, fmt.Errorf("startup.publish %q missing for app %s", startup.Publish, app.ID)
		}
		node := startupNode{
			AppID:    app.ID,
			Action:   startup.Action,
			Name:     startup.Name,
			Path:     publish.Path,
			Label:    startup.Label,
			Load:     startup.Load,
			After:    startup.After,
			Requires: startup.Requires,
			Provides: startup.Provides,
			Ensure:   startup.Ensure,
			Block:    startup.Block,
			Device:   startup.Device,
			Input:    startup.Input,
		}
		if node.Action == "" || node.Name == "" || node.Path == "" || node.Load == "" {
			return nil, fmt.Errorf("startup fields missing for app %s", app.ID)
		}
		if names[node.Name] {
			return nil, fmt.Errorf("duplicate startup name: %s", node.Name)
		}
		names[node.Name] = true
		nodes = append(nodes, node)
	}
	for _, node := range nodes {
		for _, dep := range node.After {
			if !names[dep] {
				return nil, fmt.Errorf("startup.after for app %s references unknown startup name %q", node.AppID, dep)
			}
		}
	}
	sort.Slice(nodes, func(i, j int) bool { return nodes[i].Name < nodes[j].Name })
	return orderStartup(nodes)
}

func orderStartup(nodes []startupNode) ([]startupNode, error) {
	var ordered []startupNode
	emitted := map[string]bool{}
	for len(nodes) > 0 {
		index := -1
		for i, node := range nodes {
			if allIn(node.After, emitted) {
				index = i
				break
			}
		}
		if index < 0 {
			var names []string
			for _, node := range nodes {
				names = append(names, node.Name)
			}
			return nil, fmt.Errorf("startup dependency cycle detected: %s", strings.Join(names, ", "))
		}
		node := nodes[index]
		nodes = append(nodes[:index], nodes[index+1:]...)
		emitted[node.Name] = true
		ordered = append(ordered, node)
	}
	return ordered, nil
}

func findPublish(app config.App, id string) (config.Publish, bool) {
	for _, publish := range app.Publishes("bootfs") {
		if publish.ID == id {
			return publish, true
		}
	}
	for _, publish := range app.Publishes("rootfs") {
		if publish.ID == id {
			return publish, true
		}
	}
	return config.Publish{}, false
}

func appendSingle(out *strings.Builder, key string, values []string) {
	if len(values) == 0 {
		return
	}
	if len(values) != 1 {
		return
	}
	out.WriteByte(' ')
	out.WriteString(key)
	out.WriteByte('=')
	out.WriteString(values[0])
}

func appendList(out *strings.Builder, key string, values []string) {
	if len(values) == 0 {
		return
	}
	out.WriteByte(' ')
	out.WriteString(key)
	out.WriteByte('=')
	out.WriteString(strings.Join(values, ","))
}

func sortEntries(entries []entry) {
	sort.SliceStable(entries, func(i, j int) bool {
		if entries[i].Dir != entries[j].Dir {
			return entries[i].Dir
		}
		return entries[i].ImagePath < entries[j].ImagePath
	})
}

func ensureNoDuplicates(fsName string, entries []entry) error {
	seen := map[string]bool{}
	for _, item := range entries {
		if seen[item.ImagePath] {
			return fmt.Errorf("duplicate %s publish path: %s", fsName, item.ImagePath)
		}
		seen[item.ImagePath] = true
	}
	return nil
}

func writeIfChanged(path string, contents string) error {
	if existing, err := os.ReadFile(path); err == nil && string(existing) == contents {
		return nil
	}
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return err
	}
	return os.WriteFile(path, []byte(contents), 0o644)
}

func outputsFresh(workspace *config.Workspace, outputs Outputs) (bool, error) {
	for _, path := range []string{outputs.Bootfs, outputs.Rootfs, outputs.Startup} {
		_, err := os.Stat(path)
		if os.IsNotExist(err) {
			return false, nil
		}
		if err != nil {
			return false, err
		}
	}
	stored, err := os.ReadFile(manifestKeyPath(workspace))
	if os.IsNotExist(err) {
		return false, nil
	}
	if err != nil {
		return false, err
	}
	_, current, err := manifestInputSnapshot(workspace, manifestApps(workspace))
	if err != nil {
		return false, err
	}
	return bytes.Equal(bytes.TrimSpace(stored), []byte(fmt.Sprintf("%x", current))), nil
}

func manifestInputSnapshot(workspace *config.Workspace, apps []config.App) (map[string]directoryManifestSnapshot, []byte, error) {
	configBytes, err := os.ReadFile(workspace.ConfigPath)
	if err != nil {
		return nil, nil, err
	}
	executableIdentity, err := manifestExecutableIdentity()
	if err != nil {
		return nil, nil, err
	}
	hash := sha256.New()
	writeHashString(hash, "pacgo-manifests\n")
	writeHashBytes(hash, executableIdentity)
	writeHashBytes(hash, configBytes)
	snapshots := make(map[string]directoryManifestSnapshot)
	for _, app := range apps {
		if len(app.Publishes("bootfs")) == 0 && len(app.Publishes("rootfs")) == 0 {
			continue
		}
		artifact := workspace.ArtifactPath(app)
		writeHashString(hash, "artifact\x00")
		writeHashField(hash, app.ID)
		writeHashField(hash, filepath.ToSlash(artifact))
		info, err := os.Stat(artifact)
		if os.IsNotExist(err) {
			writeHashField(hash, "missing")
			continue
		}
		if err != nil {
			return nil, nil, err
		}
		if !info.IsDir() {
			writeHashField(hash, "regular-file")
			continue
		}
		writeHashField(hash, "directory")
		snapshot, err := snapshotDirectory(artifact)
		if err != nil {
			return nil, nil, err
		}
		snapshots[artifact] = snapshot
		writeHashField(hash, filepath.ToSlash(snapshot.SourceRoot))
		for _, entry := range snapshot.Entries {
			writeHashField(hash, entry.Relative)
			writeHashString(hash, fmt.Sprintf("%d\x00", entry.Type))
		}
	}
	return snapshots, hash.Sum(nil), nil
}

func runningExecutableIdentity() ([]byte, error) {
	runningExecutableIdentityOnce.Do(func() {
		path, err := os.Executable()
		if err != nil {
			runningExecutableIdentityErr = fmt.Errorf("locate pacgo executable: %w", err)
			return
		}
		path, err = filepath.EvalSymlinks(path)
		if err != nil {
			runningExecutableIdentityErr = fmt.Errorf("resolve pacgo executable: %w", err)
			return
		}
		file, err := os.Open(path)
		if err != nil {
			runningExecutableIdentityErr = fmt.Errorf("open pacgo executable: %w", err)
			return
		}
		defer file.Close()
		hash := sha256.New()
		if _, err := io.Copy(hash, file); err != nil {
			runningExecutableIdentityErr = fmt.Errorf("hash pacgo executable: %w", err)
			return
		}
		runningExecutableIdentityDigest = hash.Sum(nil)
	})
	return runningExecutableIdentityDigest, runningExecutableIdentityErr
}

func snapshotDirectory(path string) (directoryManifestSnapshot, error) {
	root, err := filepath.EvalSymlinks(path)
	if err != nil {
		return directoryManifestSnapshot{}, err
	}
	root, err = filepath.Abs(root)
	if err != nil {
		return directoryManifestSnapshot{}, err
	}
	snapshot := directoryManifestSnapshot{SourceRoot: root}
	err = filepath.WalkDir(root, func(item string, dirEntry fs.DirEntry, err error) error {
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
		snapshot.Entries = append(snapshot.Entries, directoryManifestEntry{
			Relative: filepath.ToSlash(rel),
			Type:     info.Mode().Type(),
		})
		return nil
	})
	if err != nil {
		return directoryManifestSnapshot{}, err
	}
	sort.Slice(snapshot.Entries, func(i, j int) bool {
		return snapshot.Entries[i].Relative < snapshot.Entries[j].Relative
	})
	return snapshot, nil
}

func manifestKeyPath(workspace *config.Workspace) string {
	return workspace.Path(workspace.State, "manifests.key")
}

func writeManifestKey(workspace *config.Workspace, key []byte) error {
	path := manifestKeyPath(workspace)
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return err
	}
	return os.WriteFile(path, []byte(fmt.Sprintf("%x\n", key)), 0o644)
}

func removeManifestKey(workspace *config.Workspace) error {
	err := os.Remove(manifestKeyPath(workspace))
	if os.IsNotExist(err) {
		return nil
	}
	return err
}

func writeHashString(hash io.Writer, value string) {
	_, _ = io.WriteString(hash, value)
}

func writeHashBytes(hash io.Writer, value []byte) {
	writeHashString(hash, fmt.Sprintf("%d:", len(value)))
	_, _ = hash.Write(value)
	writeHashString(hash, "\x00")
}

func writeHashField(hash io.Writer, value string) {
	writeHashBytes(hash, []byte(value))
}

func countEntries(path string) (int, error) {
	bytes, err := os.ReadFile(path)
	if err != nil {
		return 0, err
	}
	count := 0
	for _, line := range strings.Split(string(bytes), "\n") {
		line = strings.TrimSpace(line)
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		count++
	}
	return count, nil
}

func sourcePathForManifest(manifestPath string, sourcePath string) (string, error) {
	rel, err := filepath.Rel(filepath.Dir(manifestPath), sourcePath)
	if err != nil {
		return "", err
	}
	return filepath.ToSlash(rel), nil
}

func cleanImagePath(path string) string {
	if path == "" || path == "/" {
		return "/"
	}
	return "/" + strings.Trim(path, "/")
}

func joinImagePath(root string, relative string) string {
	root = strings.TrimRight(root, "/")
	relative = strings.TrimLeft(relative, "/")
	if root == "" {
		return "/" + relative
	}
	if relative == "" {
		return root
	}
	return root + "/" + relative
}

func includes(values []string, needle string) bool {
	for _, value := range values {
		if value == needle {
			return true
		}
	}
	return false
}

func allIn(values []string, set map[string]bool) bool {
	for _, value := range values {
		if !set[value] {
			return false
		}
	}
	return true
}
