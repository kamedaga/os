package buildsys

import (
	"bytes"
	"encoding/gob"
	"os"
	"path/filepath"
	"sync"
	"testing"
	"time"
)

func TestHasherMemoizesUntilInvalidated(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "input")
	if err := os.WriteFile(path, []byte("content"), 0o644); err != nil {
		t.Fatal(err)
	}
	hasher := newHasher(filepath.Join(dir, "hashcache"))
	reads := 0
	hasher.fileHashReader = func(path string) ([]byte, error) {
		reads++
		return hashFileUncached(path)
	}
	if _, err := hasher.Hash(path); err != nil {
		t.Fatal(err)
	}
	if _, err := hasher.Hash(path); err != nil {
		t.Fatal(err)
	}
	if reads != 1 {
		t.Fatalf("file reads = %d, want 1", reads)
	}
	hasher.Invalidate(dir)
	if _, err := hasher.Hash(path); err != nil {
		t.Fatal(err)
	}
	if reads != 2 {
		t.Fatalf("file reads after invalidation = %d, want 2", reads)
	}
}

func TestHasherConcurrentHashInvalidateFlush(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "input")
	if err := os.WriteFile(path, []byte("content"), 0o644); err != nil {
		t.Fatal(err)
	}
	hasher := newHasher(filepath.Join(dir, "hashcache"))
	var workers sync.WaitGroup
	for worker := 0; worker < 8; worker++ {
		workers.Add(1)
		go func() {
			defer workers.Done()
			for iteration := 0; iteration < 30; iteration++ {
				if _, err := hasher.Hash(path); err != nil {
					t.Errorf("Hash: %v", err)
					return
				}
				hasher.Invalidate(path)
				if err := hasher.Flush(); err != nil {
					t.Errorf("Flush: %v", err)
					return
				}
			}
		}()
	}
	workers.Wait()
}

func TestHashCacheReusesMatchingStat(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "input")
	cachePath := filepath.Join(dir, "hashcache")
	if err := os.WriteFile(path, []byte("content"), 0o644); err != nil {
		t.Fatal(err)
	}
	first := newHasher(cachePath)
	digest, err := first.Hash(path)
	if err != nil {
		t.Fatal(err)
	}
	file, err := os.Create(cachePath)
	if err != nil {
		t.Fatal(err)
	}
	if err := gob.NewEncoder(file).Encode(hashCacheFile{WriteTimeNS: time.Now().Add(3 * time.Second).UnixNano(), Entries: first.persistent}); err != nil {
		t.Fatal(err)
	}
	_ = file.Close()
	second := newHasher(cachePath)
	second.fileHashReader = func(string) ([]byte, error) {
		t.Fatal("matching persistent stat was rehashed")
		return nil, nil
	}
	got, err := second.Hash(path)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(got, digest) {
		t.Fatalf("digest = %x, want %x", got, digest)
	}
}

func TestHashCacheRecentEntryRehashesSameSizeAndMtime(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "input")
	cachePath := filepath.Join(dir, "hashcache")
	if err := os.WriteFile(path, []byte("old"), 0o644); err != nil {
		t.Fatal(err)
	}
	info, _ := os.Stat(path)
	oldDigest, _ := hashFileUncached(path)
	if err := os.WriteFile(path, []byte("new"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.Chtimes(path, info.ModTime(), info.ModTime()); err != nil {
		t.Fatal(err)
	}
	currentInfo, _ := os.Stat(path)
	file, err := os.Create(cachePath)
	if err != nil {
		t.Fatal(err)
	}
	entry := hashCacheEntry{Identity: statIdentity(currentInfo), SHA256: oldDigest}
	if err := gob.NewEncoder(file).Encode(hashCacheFile{WriteTimeNS: time.Now().UnixNano(), Entries: map[string]hashCacheEntry{path: entry}}); err != nil {
		t.Fatal(err)
	}
	_ = file.Close()
	hasher := newHasher(cachePath)
	got, err := hasher.Hash(path)
	if err != nil {
		t.Fatal(err)
	}
	want, _ := hashFileUncached(path)
	if !bytes.Equal(got, want) || bytes.Equal(got, oldDigest) {
		t.Fatalf("recent cache returned %x, want %x", got, want)
	}
}

func TestHashCacheIgnoresCorruptFile(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "input")
	cachePath := filepath.Join(dir, "hashcache")
	if err := os.WriteFile(path, []byte("content"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(cachePath, []byte("not gob"), 0o644); err != nil {
		t.Fatal(err)
	}
	hasher := newHasher(cachePath)
	if _, err := hasher.Hash(path); err != nil {
		t.Fatal(err)
	}
	if err := hasher.Flush(); err != nil {
		t.Fatal(err)
	}
	if len(newHasher(cachePath).persistent) != 1 {
		t.Fatal("corrupt cache was not rebuilt")
	}
}
