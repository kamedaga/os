package limine

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"

	"capabilityos/pack/internal/config"
)

func recordTestWorkspace(t *testing.T) (*config.Workspace, string, Inputs) {
	t.Helper()
	for _, tool := range []string{"sgdisk", "mformat", "mcopy", "mmd"} {
		if _, err := exec.LookPath(tool); err != nil {
			t.Skipf("%s unavailable", tool)
		}
	}
	root := t.TempDir()
	dataDir := filepath.Join(root, "limine-data")
	if err := os.MkdirAll(dataDir, 0o755); err != nil {
		t.Fatal(err)
	}
	limine := filepath.Join(root, "limine")
	script := "#!/bin/sh\ncase \"$1\" in\n--print-datadir) echo " + dataDir + " ;;\nbios-install) exit 0 ;;\n*) exit 1 ;;\nesac\n"
	inputs := Inputs{
		KernelELF:   filepath.Join(root, "build", "kernel.elf"),
		InitELF:     filepath.Join(root, "build", "init.elf"),
		BootfsImage: filepath.Join(root, "build", "BOOTFS.IMG"),
	}
	for path, data := range map[string]string{
		limine: script,
		filepath.Join(dataDir, "limine-bios.sys"): "bios stage",
		filepath.Join(dataDir, "BOOTX64.EFI"):     "uefi stage",
		inputs.KernelELF:                          "kernel v1",
		inputs.InitELF:                            "init v1",
		inputs.BootfsImage:                        "bootfs v1",
	} {
		if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(path, []byte(data), 0o755); err != nil {
			t.Fatal(err)
		}
	}
	workspace := &config.Workspace{Root: root, Artifacts: ".artifacts", State: ".artifacts/pack"}
	return workspace, limine, inputs
}

func buildRecordTestImage(t *testing.T, workspace *config.Workspace, limine string, inputs Inputs) Result {
	t.Helper()
	result, err := BuildImageWithOptions(workspace, inputs, Options{Limine: limine})
	if err != nil {
		t.Fatal(err)
	}
	return result
}

func TestOrdinaryImageIsReusedOnlyWhileInputsAndImageMatch(t *testing.T) {
	workspace, limine, inputs := recordTestWorkspace(t)
	if first := buildRecordTestImage(t, workspace, limine, inputs); first.Skipped {
		t.Fatal("first build was skipped")
	}
	second := buildRecordTestImage(t, workspace, limine, inputs)
	if !second.Skipped {
		t.Fatal("unchanged inputs rebuilt the image")
	}

	if err := os.WriteFile(inputs.BootfsImage, []byte("bootfs v2"), 0o644); err != nil {
		t.Fatal(err)
	}
	if rebuilt := buildRecordTestImage(t, workspace, limine, inputs); rebuilt.Skipped {
		t.Fatal("changed bootfs reused the old image")
	}
	staged, err := os.ReadFile(workspace.Path(".artifacts", "BOOTFS.IMG"))
	if err != nil || string(staged) != "bootfs v2" {
		t.Fatalf("loose bootfs snapshot = %q, %v", staged, err)
	}

	file, err := os.OpenFile(second.Image, os.O_WRONLY, 0)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := file.WriteAt([]byte("guest wrote here"), 4<<20); err != nil {
		t.Fatal(err)
	}
	if err := file.Close(); err != nil {
		t.Fatal(err)
	}
	if rebuilt := buildRecordTestImage(t, workspace, limine, inputs); rebuilt.Skipped {
		t.Fatal("modified image was reused")
	}
	if forced, err := BuildImageWithOptions(workspace, inputs, Options{Limine: limine, Force: true}); err != nil || forced.Skipped {
		t.Fatalf("forced build skipped=%v err=%v", forced.Skipped, err)
	}
}

func TestImageFreshnessNamesTheOutdatedBootFile(t *testing.T) {
	workspace, limine, inputs := recordTestWorkspace(t)
	result := buildRecordTestImage(t, workspace, limine, inputs)
	reasons, err := ImageFreshness(workspace, result.Image, inputs)
	if err != nil || len(reasons) != 0 {
		t.Fatalf("fresh image reported %v, %v", reasons, err)
	}
	if err := os.WriteFile(inputs.KernelELF, []byte("kernel v2"), 0o644); err != nil {
		t.Fatal(err)
	}
	reasons, err = ImageFreshness(workspace, result.Image, inputs)
	if err != nil {
		t.Fatal(err)
	}
	if len(reasons) != 1 || !strings.Contains(reasons[0], "KERNEL.ELF") {
		t.Fatalf("reasons = %v", reasons)
	}

	missing, err := ImageFreshness(workspace, filepath.Join(workspace.Root, "absent.img"), inputs)
	if err != nil || len(missing) != 1 || !strings.Contains(missing[0], "missing") {
		t.Fatalf("missing image reasons = %v, %v", missing, err)
	}
}
