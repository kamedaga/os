package cli

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"capabilityos/pack/internal/config"
	"capabilityos/pack/internal/imagelock"
)

func TestCreateSeed0rootProfileScratchUsesUniqueLockedSnapshots(t *testing.T) {
	root := t.TempDir()
	artifacts := filepath.Join(root, ".artifacts")
	if err := os.MkdirAll(artifacts, 0o755); err != nil {
		t.Fatal(err)
	}
	bootSource := filepath.Join(artifacts, "limine-boot.img")
	rootSource := filepath.Join(artifacts, "disk.img")
	if err := os.WriteFile(bootSource, []byte("boot source"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(rootSource, []byte("root source"), 0o644); err != nil {
		t.Fatal(err)
	}
	ctx := &context{workspace: &config.Workspace{
		Root:      root,
		Artifacts: ".artifacts",
		Disk:      config.Disk{Image: ".artifacts/disk.img"},
	}}

	firstDir, firstBoot, firstRoot, err := createSeed0rootProfileScratch(ctx, bootSource)
	if err != nil {
		t.Fatal(err)
	}
	defer os.RemoveAll(firstDir)
	secondDir, secondBoot, secondRoot, err := createSeed0rootProfileScratch(ctx, bootSource)
	if err != nil {
		t.Fatal(err)
	}
	defer os.RemoveAll(secondDir)
	if firstDir == secondDir || firstBoot == secondBoot || firstRoot == secondRoot {
		t.Fatalf("scratch paths collided: first=%q/%q/%q second=%q/%q/%q", firstDir, firstBoot, firstRoot, secondDir, secondBoot, secondRoot)
	}
	for _, snapshot := range []struct {
		path string
		want string
	}{
		{firstBoot, "boot source"}, {firstRoot, "root source"},
		{secondBoot, "boot source"}, {secondRoot, "root source"},
	} {
		data, err := os.ReadFile(snapshot.path)
		if err != nil {
			t.Fatal(err)
		}
		if got := string(data); got != snapshot.want {
			t.Fatalf("snapshot %s = %q, want %q", snapshot.path, got, snapshot.want)
		}
		if !strings.HasPrefix(snapshot.path, filepath.Join(artifacts, "seed0root-profile-")) {
			t.Fatalf("snapshot path is not run-specific: %s", snapshot.path)
		}
	}
}

func TestCreateSeed0rootProfileScratchFailsBeforeWritingWhenSourceBusy(t *testing.T) {
	root := t.TempDir()
	artifacts := filepath.Join(root, ".artifacts")
	if err := os.MkdirAll(artifacts, 0o755); err != nil {
		t.Fatal(err)
	}
	bootSource := filepath.Join(artifacts, "limine-boot.img")
	rootSource := filepath.Join(artifacts, "disk.img")
	for _, path := range []string{bootSource, rootSource} {
		if err := os.WriteFile(path, []byte("source"), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	locks, err := imagelock.Acquire(bootSource, rootSource)
	if err != nil {
		t.Fatal(err)
	}
	defer locks.Close()
	ctx := &context{workspace: &config.Workspace{
		Root:      root,
		Artifacts: ".artifacts",
		Disk:      config.Disk{Image: ".artifacts/disk.img"},
	}}
	if _, _, _, err := createSeed0rootProfileScratch(ctx, bootSource); err == nil {
		t.Fatal("scratch snapshot unexpectedly ignored source image lock")
	}
	entries, err := os.ReadDir(artifacts)
	if err != nil {
		t.Fatal(err)
	}
	for _, entry := range entries {
		if strings.HasPrefix(entry.Name(), "seed0root-profile-") {
			t.Fatalf("busy snapshot left scratch directory %s", entry.Name())
		}
	}
}
