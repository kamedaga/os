package buildsys

import (
	"crypto/sha256"
	"encoding/gob"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"syscall"
	"time"

	"capabilityos/pack/internal/config"
)

type fileIdentity struct {
	Dev     uint64
	Ino     uint64
	Size    int64
	MtimeNS int64
	CtimeNS int64
	Mode    uint32
}

type hashCacheEntry struct {
	Identity fileIdentity
	SHA256   []byte
}

type hashCacheFile struct {
	WriteTimeNS int64
	Entries     map[string]hashCacheEntry
}

type Hasher struct {
	mu             sync.Mutex
	path           string
	loadedWriteNS  int64
	persistent     map[string]hashCacheEntry
	memo           map[string][]byte
	dirty          bool
	fileHashReader func(string) ([]byte, error)
}

var workspaceHashers sync.Map

func hashCachePath(workspace *config.Workspace) string {
	path := filepath.Join(workspace.Path(workspace.State), "hashcache")
	abs, err := filepath.Abs(path)
	if err != nil {
		return filepath.Clean(path)
	}
	return filepath.Clean(abs)
}

// BeginRun gives all build operations in one pacgo process a shared memoizing
// hasher. Library callers that do not opt in receive a fresh hasher per API call.
func BeginRun(workspace *config.Workspace) {
	path := hashCachePath(workspace)
	workspaceHashers.LoadOrStore(path, newHasher(path))
}

func hasherFor(workspace *config.Workspace) *Hasher {
	path := hashCachePath(workspace)
	if cached, ok := workspaceHashers.Load(path); ok {
		return cached.(*Hasher)
	}
	return newHasher(path)
}

func flushHasher(hasher *Hasher) {
	if err := hasher.Flush(); err != nil {
		_, _ = fmt.Fprintf(os.Stderr, "warning: could not flush hash cache %s: %v\n", hasher.path, err)
	}
}

func newHasher(path string) *Hasher {
	hasher := &Hasher{
		path:       path,
		persistent: make(map[string]hashCacheEntry),
		memo:       make(map[string][]byte),
	}
	hasher.fileHashReader = hashFileUncached
	file, err := os.Open(path)
	if err != nil {
		return hasher
	}
	defer file.Close()
	var cache hashCacheFile
	if err := gob.NewDecoder(file).Decode(&cache); err != nil || cache.Entries == nil {
		return hasher
	}
	hasher.loadedWriteNS = cache.WriteTimeNS
	hasher.persistent = cache.Entries
	return hasher
}

func (h *Hasher) Hash(path string) ([]byte, error) {
	return h.hash(path, true)
}

// HashCurrent ignores the per-run memo and revalidates the file's stat
// identity, for files that the current run itself may have rewritten.
func (h *Hasher) HashCurrent(path string) ([]byte, error) {
	return h.hash(path, false)
}

func (h *Hasher) hash(path string, useMemo bool) ([]byte, error) {
	abs, err := filepath.Abs(path)
	if err != nil {
		return nil, err
	}
	abs = filepath.Clean(abs)
	h.mu.Lock()
	defer h.mu.Unlock()
	if digest, ok := h.memo[abs]; ok && useMemo {
		return append([]byte(nil), digest...), nil
	}
	info, err := os.Stat(abs)
	if err != nil {
		return nil, err
	}
	identity := statIdentity(info)
	if entry, ok := h.persistent[abs]; ok && entry.Identity == identity && h.trustLoaded(entry) {
		h.memo[abs] = append([]byte(nil), entry.SHA256...)
		return append([]byte(nil), entry.SHA256...), nil
	}
	digest, err := h.fileHashReader(abs)
	if err != nil {
		return nil, err
	}
	h.memo[abs] = append([]byte(nil), digest...)
	h.persistent[abs] = hashCacheEntry{Identity: identity, SHA256: append([]byte(nil), digest...)}
	h.dirty = true
	return digest, nil
}

func (h *Hasher) trustLoaded(entry hashCacheEntry) bool {
	if h.loadedWriteNS == 0 {
		return false
	}
	cutoff := h.loadedWriteNS - int64(2*time.Second)
	return entry.Identity.MtimeNS < cutoff && entry.Identity.CtimeNS < cutoff
}

func (h *Hasher) Invalidate(paths ...string) {
	h.mu.Lock()
	defer h.mu.Unlock()
	for _, path := range paths {
		abs, err := filepath.Abs(path)
		if err != nil {
			continue
		}
		abs = filepath.Clean(abs)
		prefix := abs + string(filepath.Separator)
		for cached := range h.memo {
			if cached == abs || strings.HasPrefix(cached, prefix) {
				delete(h.memo, cached)
			}
		}
		for cached := range h.persistent {
			if cached == abs || strings.HasPrefix(cached, prefix) {
				delete(h.persistent, cached)
				h.dirty = true
			}
		}
	}
}

func (h *Hasher) Flush() error {
	h.mu.Lock()
	defer h.mu.Unlock()
	if !h.dirty {
		return nil
	}
	if err := os.MkdirAll(filepath.Dir(h.path), 0o755); err != nil {
		return err
	}
	tmp, err := os.CreateTemp(filepath.Dir(h.path), ".hashcache-*")
	if err != nil {
		return err
	}
	tmpPath := tmp.Name()
	ok := false
	defer func() {
		_ = tmp.Close()
		if !ok {
			_ = os.Remove(tmpPath)
		}
	}()
	writeTime := time.Now().UnixNano()
	if err := gob.NewEncoder(tmp).Encode(hashCacheFile{WriteTimeNS: writeTime, Entries: h.persistent}); err != nil {
		return err
	}
	if err := tmp.Sync(); err != nil {
		return err
	}
	if err := tmp.Close(); err != nil {
		return err
	}
	if err := os.Rename(tmpPath, h.path); err != nil {
		return err
	}
	ok = true
	h.loadedWriteNS = writeTime
	h.dirty = false
	return nil
}

func statIdentity(info os.FileInfo) fileIdentity {
	identity := fileIdentity{Size: info.Size(), MtimeNS: info.ModTime().UnixNano(), Mode: uint32(info.Mode())}
	if stat, ok := info.Sys().(*syscall.Stat_t); ok {
		identity.Dev = uint64(stat.Dev)
		identity.Ino = stat.Ino
		identity.CtimeNS = stat.Ctim.Sec*int64(time.Second) + stat.Ctim.Nsec
	}
	return identity
}

func hashFileUncached(path string) ([]byte, error) {
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

func hashIfExists(hasher *Hasher, path string) ([]byte, error) {
	digest, err := hasher.Hash(path)
	if errors.Is(err, os.ErrNotExist) {
		return nil, nil
	}
	return digest, err
}

// FileDigest returns the sha256 of path through the run's stat-keyed hash
// cache, so packaging stages can compare large images without rereading them.
func FileDigest(workspace *config.Workspace, path string) ([]byte, error) {
	hasher := hasherFor(workspace)
	defer flushHasher(hasher)
	return hasher.HashCurrent(path)
}
