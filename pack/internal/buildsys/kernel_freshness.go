package buildsys

import (
	"bufio"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strconv"
	"strings"

	"capabilityos/pack/internal/config"
)

// kernelInputRecord describes what the last successful pacgo kernel build
// consumed. Zig keeps the exact input closure in its cache manifests; pacgo
// owns the cache directory, so every project file named by any manifest in it
// is recorded. Older manifests only widen the set, which can over-report a
// change but never hide one.
type kernelInputRecord struct {
	Zig    recordedFile   `json:"zig"`
	Output string         `json:"output"`
	Files  []recordedFile `json:"files"`
}

type recordedFile struct {
	Path   string `json:"path"`
	SHA256 string `json:"sha256"`
}

func kernelCacheDirs(workspace *config.Workspace) (string, string) {
	return workspace.Path(workspace.Artifacts, "zig-cache", "kernel-local"),
		workspace.Path(workspace.Artifacts, "zig-cache", "kernel-global")
}

func kernelInputRecordPath(workspace *config.Workspace, step string) string {
	return filepath.Join(workspace.Path(workspace.State), "build", "kernel-"+sanitizeFilename(step)+".json")
}

func writeKernelInputRecord(workspace *config.Workspace, step string, output string, hasher *Hasher) error {
	record, err := snapshotKernelInputs(workspace, output, hasher)
	if err != nil {
		return err
	}
	data, err := json.MarshalIndent(record, "", "  ")
	if err != nil {
		return err
	}
	path := kernelInputRecordPath(workspace, step)
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return err
	}
	tmp := path + ".tmp"
	if err := os.WriteFile(tmp, append(data, '\n'), 0o644); err != nil {
		return err
	}
	return os.Rename(tmp, path)
}

func snapshotKernelInputs(workspace *config.Workspace, output string, hasher *Hasher) (kernelInputRecord, error) {
	zig, err := snapshotZig(hasher)
	if err != nil {
		return kernelInputRecord{}, err
	}
	outputDigest, err := hasher.Hash(output)
	if err != nil {
		return kernelInputRecord{}, err
	}
	paths, err := zigManifestInputs(workspace)
	if err != nil {
		return kernelInputRecord{}, err
	}
	files := make([]recordedFile, 0, len(paths))
	for _, path := range paths {
		digest, err := hashIfExists(hasher, path)
		if err != nil {
			return kernelInputRecord{}, err
		}
		// An empty digest records an input that must stay absent.
		files = append(files, recordedFile{Path: path, SHA256: hex.EncodeToString(digest)})
	}
	return kernelInputRecord{Zig: zig, Output: hex.EncodeToString(outputDigest), Files: files}, nil
}

func snapshotZig(hasher *Hasher) (recordedFile, error) {
	resolved, err := exec.LookPath("zig")
	if err != nil {
		return recordedFile{}, err
	}
	resolved, err = filepath.EvalSymlinks(resolved)
	if err != nil {
		return recordedFile{}, err
	}
	digest, err := hasher.Hash(resolved)
	if err != nil {
		return recordedFile{}, err
	}
	return recordedFile{Path: resolved, SHA256: hex.EncodeToString(digest)}, nil
}

// zigManifestInputs returns the existing project files named by the kernel's
// zig cache manifests. Manifest prefixes differ per manifest (build root, zig
// lib, cache), so a relative entry counts only when it exists below the kernel
// directory. Zig library and cache files are represented by the zig binary.
func zigManifestInputs(workspace *config.Workspace) ([]string, error) {
	localCache, globalCache := kernelCacheDirs(workspace)
	kernelDir := workspace.Path(workspace.Kernel.Dir)
	manifests, err := filepath.Glob(filepath.Join(localCache, "h", "*.txt"))
	if err != nil {
		return nil, err
	}
	seen := make(map[string]bool)
	for _, manifest := range manifests {
		entries, err := readZigManifestPaths(manifest)
		if err != nil {
			return nil, err
		}
		for _, entry := range entries {
			path := entry
			if !filepath.IsAbs(path) {
				path = filepath.Join(kernelDir, path)
			}
			path = filepath.Clean(path)
			if seen[path] || pathWithinAbs(path, localCache) || pathWithinAbs(path, globalCache) {
				continue
			}
			info, err := os.Stat(path)
			if err != nil || info.IsDir() {
				if filepath.IsAbs(entry) && errors.Is(err, os.ErrNotExist) {
					// An absolute input that vanished is still a change.
					seen[path] = true
				}
				continue
			}
			seen[path] = true
		}
	}
	paths := make([]string, 0, len(seen))
	for path := range seen {
		paths = append(paths, path)
	}
	sort.Strings(paths)
	return paths, nil
}

// readZigManifestPaths parses "size inode mtime hash prefix path" lines.
func readZigManifestPaths(path string) ([]string, error) {
	file, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer file.Close()
	var paths []string
	scanner := bufio.NewScanner(file)
	scanner.Buffer(make([]byte, 64*1024), 1024*1024)
	for scanner.Scan() {
		fields := strings.SplitN(scanner.Text(), " ", 6)
		if len(fields) != 6 || fields[5] == "" {
			continue
		}
		numeric := true
		for _, field := range []string{fields[0], fields[1], fields[2], fields[4]} {
			if _, err := strconv.ParseUint(field, 10, 64); err != nil {
				numeric = false
				break
			}
		}
		if numeric {
			paths = append(paths, fields[5])
		}
	}
	return paths, scanner.Err()
}

// KernelFreshness reports why the kernel output of step may not match its
// sources, without building. An empty reason means the recorded inputs are
// unchanged.
func KernelFreshness(workspace *config.Workspace, step string) (string, error) {
	if step == "" {
		step = workspace.Kernel.Step
	}
	if step == "" {
		step = "limine"
	}
	data, err := os.ReadFile(kernelInputRecordPath(workspace, step))
	if errors.Is(err, os.ErrNotExist) {
		return "no record of a pacgo kernel build", nil
	}
	if err != nil {
		return "", err
	}
	var record kernelInputRecord
	if err := json.Unmarshal(data, &record); err != nil {
		return "kernel build record unreadable", nil
	}
	hasher := hasherFor(workspace)
	defer flushHasher(hasher)
	zig, err := snapshotZig(hasher)
	if err != nil {
		return "", err
	}
	if zig != record.Zig {
		return "zig toolchain changed: " + zig.Path, nil
	}
	for _, file := range record.Files {
		digest, err := hashIfExists(hasher, file.Path)
		if err != nil {
			return "", err
		}
		current := hex.EncodeToString(digest)
		switch {
		case current == file.SHA256:
		case current == "":
			return "kernel input removed: " + workspace.Rel(file.Path), nil
		case file.SHA256 == "":
			return "kernel input appeared: " + workspace.Rel(file.Path), nil
		default:
			return "kernel input changed: " + workspace.Rel(file.Path), nil
		}
	}
	output := KernelOutputPath(workspace, step)
	digest, err := hasher.Hash(output)
	if errors.Is(err, os.ErrNotExist) {
		return "kernel output missing: " + workspace.Rel(output), nil
	}
	if err != nil {
		return "", err
	}
	if hex.EncodeToString(digest) != record.Output {
		return fmt.Sprintf("kernel output %s changed outside pacgo", workspace.Rel(output)), nil
	}
	return "", nil
}
