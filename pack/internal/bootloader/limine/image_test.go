package limine

import (
	"crypto/sha256"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

func TestInstallerDataPartitionReadback(t *testing.T) {
	for _, tool := range []string{"sgdisk", "mformat", "mcopy"} {
		if _, err := exec.LookPath(tool); err != nil {
			t.Skipf("%s unavailable", tool)
		}
	}
	dir := t.TempDir()
	image := filepath.Join(dir, "installer.img")
	payload := filepath.Join(dir, "rootfs.tar")
	systemBootfs := filepath.Join(dir, "system.bootfs")
	systemConfig := filepath.Join(dir, "limine.conf")
	kernel := filepath.Join(dir, "kernel.elf")
	init := filepath.Join(dir, "init.elf")
	limineEFI := filepath.Join(dir, "limine.efi")
	desktop := filepath.Join(dir, "desktop.tgz")
	if err := os.WriteFile(payload, []byte("explicit installer archive fixture"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(systemBootfs, []byte("ordinary storage boot archive"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(systemConfig, []byte("/PachaOS installed\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(desktop, []byte("optional desktop fixture"), 0o644); err != nil {
		t.Fatal(err)
	}
	for path, data := range map[string]string{
		kernel: "kernel", init: "init", limineEFI: "limine",
	} {
		if err := os.WriteFile(path, []byte(data), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	if err := createEmptyImage(image, 1<<20); err != nil {
		t.Fatal(err)
	}
	bootBytes, err := imageSize(image)
	if err != nil {
		t.Fatal(err)
	}
	dataBytes := roundImageBytes(64 << 20)
	if err := os.Truncate(image, bootBytes+dataBytes); err != nil {
		t.Fatal(err)
	}
	if err := partitionImage(image, bootBytes, dataBytes); err != nil {
		t.Fatal(err)
	}
	if err := writeInstallerPayload(image, bootBytes, dataBytes, payload,
		systemBootfs, systemConfig, kernel, init, limineEFI); err != nil {
		t.Fatal(err)
	}
	if err := writeDesktopPayload(image, bootBytes, desktop); err != nil {
		t.Fatal(err)
	}
	if err := verifyFATFile(image, bootBytes, "DESKTOP.TGZ", desktop); err != nil {
		t.Fatal(err)
	}
	desktopDigest := filepath.Join(dir, "desktop.sha")
	if out, err := exec.Command("mcopy", "-o", "-i",
		image+"@@"+fmt.Sprint(bootBytes), "::/DESKTOP.SHA", desktopDigest).CombinedOutput(); err != nil {
		t.Fatalf("read DESKTOP.SHA: %s: %v", out, err)
	}
	digestBytes, err := os.ReadFile(desktopDigest)
	if err != nil {
		t.Fatal(err)
	}
	wantDesktopDigest := fmt.Sprintf("%x  DESKTOP.TGZ\n", sha256.Sum256([]byte("optional desktop fixture")))
	if string(digestBytes) != wantDesktopDigest {
		t.Fatalf("unexpected desktop digest: %q", digestBytes)
	}
	metadata := filepath.Join(dir, "install.meta")
	if out, err := exec.Command("mcopy", "-o", "-i",
		image+"@@"+fmt.Sprint(bootBytes), "::/INSTALL.META", metadata).CombinedOutput(); err != nil {
		t.Fatalf("read INSTALL.META: %s: %v", out, err)
	}
	metadataBytes, err := os.ReadFile(metadata)
	if err != nil {
		t.Fatal(err)
	}
	wantMetadata := fmt.Sprintf("1 %d %d %d %d %d %d\n",
		len("explicit installer archive fixture"),
		len("ordinary storage boot archive"),
		len("kernel"), len("init"), len("limine"),
		len("/PachaOS installed\n"))
	if string(metadataBytes) != wantMetadata {
		t.Fatalf("INSTALL.META=%q, want %q", metadataBytes, wantMetadata)
	}
	if out, err := exec.Command("sgdisk", "--verify", image).CombinedOutput(); err != nil {
		t.Fatalf("invalid GPT: %s: %v", out, err)
	}
	for number, name := range map[string]string{"2": "pacha-live-esp", "3": "pacha-install-data"} {
		out, err := exec.Command("sgdisk", "--info="+number, image).CombinedOutput()
		if err != nil || !strings.Contains(string(out), name) {
			t.Fatalf("partition %s: %s: %v", number, out, err)
		}
	}
	dataInfo, err := exec.Command("sgdisk", "--info=3", image).CombinedOutput()
	if err != nil || !strings.Contains(string(dataInfo),
		"Partition GUID code: EBD0A0A2-B9E5-4433-87C0-68B6B72699C7") {
		t.Fatalf("installer data must not masquerade as ESP: %s: %v", dataInfo, err)
	}
	// The installer must refuse a corrupted payload before it can write a target.
	if err := os.WriteFile(payload, []byte("tampered"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := verifyFATFile(image, bootBytes, "ROOTFS.TAR", payload); err == nil {
		t.Fatal("accepted corrupted payload")
	}
}

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
