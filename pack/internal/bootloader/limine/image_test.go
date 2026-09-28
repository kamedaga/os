package limine

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestLiveConfigHasOnlyNormalAndManualDiagnosticEntries(t *testing.T) {
	path := filepath.Join(t.TempDir(), "limine.conf")
	if err := writeConfig(path, true); err != nil {
		t.Fatal(err)
	}
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	text := string(data)
	for _, want := range []string{
		"default_entry: 1", "path: boot():/KERNEL.ELF",
		"path: boot():/DIAGBOOT.ELF",
	} {
		if !strings.Contains(text, want) {
			t.Fatalf("live config missing %q", want)
		}
	}
	if strings.Count(text, "    protocol: limine") != 2 {
		t.Fatalf("expected exactly two boot entries:\n%s", text)
	}
}

func TestFATPartitionExcludesBackupGPT(t *testing.T) {
	for _, imageBytes := range []int64{64 << 20, 192 << 20, 512 << 20} {
		sectors, err := fatPartitionSectors(imageBytes)
		if err != nil || fatPartitionByte+sectors*512 != imageBytes-33*512 {
			t.Fatalf("image=%d sectors=%d err=%v", imageBytes, sectors, err)
		}
	}
	for _, invalid := range []int64{0, fatPartitionByte, (64 << 20) + 1} {
		if _, err := fatPartitionSectors(invalid); err == nil {
			t.Fatalf("accepted invalid extent %d", invalid)
		}
	}
}

func TestOrdinaryConfigHasNoDiagnosticEntry(t *testing.T) {
	path := filepath.Join(t.TempDir(), "limine.conf")
	if err := writeConfig(path, false); err != nil {
		t.Fatal(err)
	}
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if strings.Contains(string(data), "DIAGBOOT.ELF") {
		t.Fatalf("ordinary config unexpectedly includes diagnostic entry:\n%s", data)
	}
}

func TestImageCapacityTracksOptionalBootfsWithoutGrowingOrdinaryImage(t *testing.T) {
	path := filepath.Join(t.TempDir(), "boot.img")
	for _, item := range []struct{ files, want int64 }{
		{22 * 1024 * 1024, 64 * 1024 * 1024},
		{107 * 1024 * 1024, 192 * 1024 * 1024},
	} {
		if err := createEmptyImage(path, item.files); err != nil {
			t.Fatal(err)
		}
		info, err := os.Stat(path)
		if err != nil {
			t.Fatal(err)
		}
		if info.Size() != item.want {
			t.Fatalf("files=%d image=%d want=%d", item.files, info.Size(), item.want)
		}
	}
}

func TestBootInputStagingReplacesStaleLooseKernelAndKeepsBootfs(t *testing.T) {
	dir := t.TempDir()
	source := filepath.Join(dir, "built-kernel.elf")
	kernel := filepath.Join(dir, "KERNEL.ELF")
	init := filepath.Join(dir, "built-init.elf")
	bootfs := filepath.Join(dir, "BOOTFS.IMG")
	diagnostic := filepath.Join(dir, "built-diag.elf")
	for path, data := range map[string]string{
		source: "new kernel", kernel: "stale kernel", init: "init", bootfs: "bootfs", diagnostic: "diagnostic",
	} {
		if err := os.WriteFile(path, []byte(data), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	staged, err := stageBootInputs(dir, Inputs{KernelELF: source, InitELF: init, BootfsImage: bootfs, DiagnosticELF: diagnostic})
	if err != nil {
		t.Fatal(err)
	}
	for path, want := range map[string]string{
		staged.KernelELF: "new kernel", staged.InitELF: "init", staged.BootfsImage: "bootfs", staged.DiagnosticELF: "diagnostic", source: "new kernel",
	} {
		data, err := os.ReadFile(path)
		if err != nil || string(data) != want {
			t.Fatalf("%s: got %q error=%v want=%q", path, data, err, want)
		}
	}
	if staged.KernelELF != kernel {
		t.Fatalf("image input is not staged kernel: %s", staged.KernelELF)
	}
}
