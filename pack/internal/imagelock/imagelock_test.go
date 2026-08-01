package imagelock

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

func TestAcquireContentionReleaseAndStaleSidecar(t *testing.T) {
	image := filepath.Join(t.TempDir(), "disk.img")
	if err := os.WriteFile(image, []byte("image"), 0o644); err != nil {
		t.Fatal(err)
	}
	stale := image + ".pacgo.lock"
	if err := os.WriteFile(stale, []byte("stale metadata is irrelevant"), 0o644); err != nil {
		t.Fatal(err)
	}
	first, err := Acquire(image)
	if err != nil {
		t.Fatal(err)
	}
	second, err := Acquire(image)
	if err == nil {
		_ = second.Close()
		t.Fatal("second lock unexpectedly succeeded")
	}
	if !strings.Contains(err.Error(), "image is busy") {
		t.Fatalf("contention error = %v", err)
	}
	if err := first.Close(); err != nil {
		t.Fatal(err)
	}
	third, err := Acquire(image)
	if err != nil {
		t.Fatal(err)
	}
	if err := third.Close(); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(stale); err != nil {
		t.Fatalf("permanent sidecar disappeared: %v", err)
	}
}

func TestAcquireCanonicalizesSymlinkAlias(t *testing.T) {
	dir := t.TempDir()
	image := filepath.Join(dir, "disk.img")
	if err := os.WriteFile(image, []byte("image"), 0o644); err != nil {
		t.Fatal(err)
	}
	alias := filepath.Join(dir, "disk-alias.img")
	if err := os.Symlink(image, alias); err != nil {
		t.Fatal(err)
	}
	canonicalImage, err := CanonicalPath(image)
	if err != nil {
		t.Fatal(err)
	}
	canonicalAlias, err := CanonicalPath(alias)
	if err != nil {
		t.Fatal(err)
	}
	if canonicalAlias != canonicalImage {
		t.Fatalf("alias canonical path = %q, want %q", canonicalAlias, canonicalImage)
	}
	first, err := Acquire(alias)
	if err != nil {
		t.Fatal(err)
	}
	defer first.Close()
	if _, err := Acquire(image); err == nil {
		t.Fatal("target lock unexpectedly bypassed symlink alias")
	}
}

func TestAcquireCanonicalizesRelativeDanglingSymlinkAlias(t *testing.T) {
	dir := t.TempDir()
	targetDir := filepath.Join(dir, "target")
	if err := os.Mkdir(targetDir, 0o755); err != nil {
		t.Fatal(err)
	}
	target := filepath.Join(targetDir, "missing.img")
	alias := filepath.Join(dir, "alias.img")
	if err := os.Symlink("target/missing.img", alias); err != nil {
		t.Fatal(err)
	}
	assertAliasesContend(t, alias, target)
}

func TestAcquireCanonicalizesDoubleDanglingSymlinkAlias(t *testing.T) {
	dir := t.TempDir()
	linksDir := filepath.Join(dir, "links")
	targetDir := filepath.Join(dir, "target")
	for _, path := range []string{linksDir, targetDir} {
		if err := os.Mkdir(path, 0o755); err != nil {
			t.Fatal(err)
		}
	}
	target := filepath.Join(targetDir, "missing.img")
	intermediate := filepath.Join(linksDir, "intermediate.img")
	if err := os.Symlink("../target/missing.img", intermediate); err != nil {
		t.Fatal(err)
	}
	alias := filepath.Join(dir, "alias.img")
	if err := os.Symlink("links/intermediate.img", alias); err != nil {
		t.Fatal(err)
	}
	assertAliasesContend(t, alias, target)
}

func TestCanonicalPathRejectsSymlinkLoop(t *testing.T) {
	dir := t.TempDir()
	first := filepath.Join(dir, "first.img")
	second := filepath.Join(dir, "second.img")
	if err := os.Symlink("second.img", first); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink("first.img", second); err != nil {
		t.Fatal(err)
	}
	if _, err := CanonicalPath(first); err == nil || !strings.Contains(err.Error(), "too many symbolic links") {
		t.Fatalf("symlink loop error = %v", err)
	}
}

func assertAliasesContend(t *testing.T, alias string, target string) {
	t.Helper()
	canonicalAlias, err := CanonicalPath(alias)
	if err != nil {
		t.Fatal(err)
	}
	canonicalTarget, err := CanonicalPath(target)
	if err != nil {
		t.Fatal(err)
	}
	if canonicalAlias != canonicalTarget {
		t.Fatalf("alias canonical path = %q, want %q", canonicalAlias, canonicalTarget)
	}
	locks, err := Acquire(alias)
	if err != nil {
		t.Fatal(err)
	}
	defer locks.Close()
	if _, err := Acquire(target); err == nil {
		t.Fatal("target lock unexpectedly bypassed dangling symlink alias")
	}
}

func TestExtraFilesInheritedByChild(t *testing.T) {
	image := filepath.Join(t.TempDir(), "disk.img")
	if err := os.WriteFile(image, nil, 0o644); err != nil {
		t.Fatal(err)
	}
	locks, err := Acquire(image)
	if err != nil {
		t.Fatal(err)
	}
	defer locks.Close()
	cmd := exec.Command("/bin/sh", "-c", "test -e /proc/self/fd/3")
	cmd.ExtraFiles = locks.ExtraFiles()
	if output, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("child did not inherit lock descriptor: %v\n%s", err, output)
	}
}
