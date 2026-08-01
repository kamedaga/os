// Package imagelock provides advisory process-wide exclusion for disk images.
package imagelock

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"syscall"
)

// Set holds exclusive locks for one or more image paths. The sidecar files are
// deliberately permanent: flock state is tied to the open descriptor and is
// therefore released automatically on process exit, including a crash.
type Set struct {
	files []*os.File
}

// Acquire obtains non-blocking exclusive locks for paths. Paths are resolved
// to absolute, symlink-free paths before their sidecar names are chosen, so an
// image and any symlink alias contend on the same lock.
func Acquire(paths ...string) (*Set, error) {
	unique := make(map[string]struct{}, len(paths))
	for _, path := range paths {
		if path == "" {
			continue
		}
		canonical, err := canonicalPath(path)
		if err != nil {
			return nil, err
		}
		unique[canonical] = struct{}{}
	}
	canonicalPaths := make([]string, 0, len(unique))
	for path := range unique {
		canonicalPaths = append(canonicalPaths, path)
	}
	sort.Strings(canonicalPaths)

	set := &Set{}
	for _, path := range canonicalPaths {
		lockPath := path + ".pacgo.lock"
		if err := os.MkdirAll(filepath.Dir(lockPath), 0o755); err != nil {
			set.Close()
			return nil, fmt.Errorf("create image lock directory for %s: %w", path, err)
		}
		file, err := os.OpenFile(lockPath, os.O_CREATE|os.O_RDWR, 0o644)
		if err != nil {
			set.Close()
			return nil, fmt.Errorf("open image lock %s: %w", lockPath, err)
		}
		if err := syscall.Flock(int(file.Fd()), syscall.LOCK_EX|syscall.LOCK_NB); err != nil {
			_ = file.Close()
			set.Close()
			if errors.Is(err, syscall.EWOULDBLOCK) || errors.Is(err, syscall.EAGAIN) {
				return nil, fmt.Errorf("image is busy: %s (held by another pacgo image operation)", path)
			}
			return nil, fmt.Errorf("lock image %s: %w", path, err)
		}
		set.files = append(set.files, file)
	}
	return set, nil
}

// ExtraFiles returns the lock descriptors for exec.Cmd.ExtraFiles. Passing
// them to a QEMU child keeps the advisory locks held if the pacgo parent dies
// while QEMU remains alive.
func (set *Set) ExtraFiles() []*os.File {
	if set == nil {
		return nil
	}
	return append([]*os.File(nil), set.files...)
}

// Close releases all locks. It intentionally never removes sidecar files.
func (set *Set) Close() error {
	if set == nil {
		return nil
	}
	var first error
	for index := len(set.files) - 1; index >= 0; index-- {
		file := set.files[index]
		if err := file.Close(); err != nil && first == nil {
			first = err
		}
	}
	set.files = nil
	return first
}

func canonicalPath(path string) (string, error) {
	abs, err := filepath.Abs(path)
	if err != nil {
		return "", fmt.Errorf("resolve image path %s: %w", path, err)
	}
	return resolveSymlinkComponents(filepath.Clean(abs))
}

func resolveSymlinkComponents(abs string) (string, error) {
	volume := filepath.VolumeName(abs)
	root := volume + string(filepath.Separator)
	if !filepath.IsAbs(abs) {
		return "", fmt.Errorf("image path is not absolute: %s", abs)
	}
	resolved := root
	pending := pathComponents(strings.TrimPrefix(abs, root))
	const maxSymlinks = 40
	symlinks := 0
	for len(pending) != 0 {
		component := pending[0]
		pending = pending[1:]
		switch component {
		case "", ".":
			continue
		case "..":
			resolved = filepath.Dir(resolved)
			continue
		}
		candidate := filepath.Join(resolved, component)
		info, err := os.Lstat(candidate)
		if err != nil {
			if errors.Is(err, os.ErrNotExist) {
				resolved = candidate
				continue
			}
			return "", fmt.Errorf("resolve image path %s: %w", abs, err)
		}
		if info.Mode()&os.ModeSymlink == 0 {
			resolved = candidate
			continue
		}
		symlinks++
		if symlinks > maxSymlinks {
			return "", fmt.Errorf("resolve image path %s: too many symbolic links", abs)
		}
		target, err := os.Readlink(candidate)
		if err != nil {
			return "", fmt.Errorf("read image symlink %s: %w", candidate, err)
		}
		if filepath.IsAbs(target) {
			target = filepath.Clean(target)
			targetVolume := filepath.VolumeName(target)
			resolved = targetVolume + string(filepath.Separator)
			pending = append(pathComponents(strings.TrimPrefix(target, resolved)), pending...)
			continue
		}
		pending = append(pathComponents(target), pending...)
	}
	return filepath.Clean(resolved), nil
}

func pathComponents(path string) []string {
	if path == "" {
		return nil
	}
	return strings.Split(path, string(filepath.Separator))
}

// CanonicalPath exposes the normalized target for diagnostics and focused
// tests without exposing lock implementation details.
func CanonicalPath(path string) (string, error) {
	return canonicalPath(path)
}
