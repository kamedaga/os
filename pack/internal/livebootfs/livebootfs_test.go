package livebootfs

import (
	"bytes"
	"encoding/base64"
	"encoding/binary"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"capabilityos/pack/internal/config"
)

func TestAuthorizedKeyRequiresSingleUnrestrictedEd25519Line(t *testing.T) {
	payload := make([]byte, 51)
	binary.BigEndian.PutUint32(payload[:4], 11)
	copy(payload[4:15], "ssh-ed25519")
	binary.BigEndian.PutUint32(payload[15:19], 32)
	for i := 19; i < len(payload); i++ {
		payload[i] = byte(i)
	}
	key := "ssh-ed25519 " + base64.StdEncoding.EncodeToString(payload) + " test\n"
	if err := validateAuthorizedKey([]byte(key)); err != nil {
		t.Fatal(err)
	}
	for _, invalid := range []string{
		"", "ssh-rsa " + base64.StdEncoding.EncodeToString(payload),
		"command=sh " + key, key + key,
		"ssh-ed25519 invalid!\n",
	} {
		if err := validateAuthorizedKey([]byte(invalid)); err == nil {
			t.Errorf("accepted invalid key %q", invalid)
		}
	}
}

func TestDefaultSpecUsesExistingAppsAndHasShellClosure(t *testing.T) {
	root, err := config.FindRoot(".")
	if err != nil {
		t.Fatal(err)
	}
	workspace, err := config.Load(root)
	if err != nil {
		t.Fatal(err)
	}
	spec, err := LoadSpec(workspace.Path(SpecPath))
	if err != nil {
		t.Fatal(err)
	}
	for _, destination := range []string{
		"/etc/pacha/gpu-probe.conf", "/srv/gpud.elf",
		"/etc/pacha/installer-mode",
	} {
		if _, present := spec.Files[destination]; present {
			t.Errorf("ordinary live bootfs must not arm GPU inspection: %s", destination)
		}
	}
	for destination, appID := range spec.Files {
		if _, ok := workspace.App(appID); !ok {
			t.Errorf("%s refers to missing app %s", destination, appID)
		}
	}
	for _, destination := range []string{
		"/etc/pacha/boot-profile",
		"/bin/ash", "/bin/sh", "/bin/busybox",
		"/usr/lib/pachaos/ld-musl-x86_64.so.1", "/usr/lib/pachaos/libc.so",
		"/lib/ld-musl-x86_64.so.1", "/lib/libc.so",
		"/lib/linux/ld-musl-x86_64.so.1",
		"/lib/libc.musl-x86_64.so.1",
		"/lib/pacha/lpr-linux-x86_64.so",
		"/sbin/filed.elf", "/sbin/lpr_supervisor.elf",
		"/srv/termd.elf", "/srv/unixd.elf",
		"/usr/lib/kobox/linux_tty_core.ko",
		"/usr/lib/kobox/linux_tty_n_null.ko",
	} {
		if spec.Files[destination] == "" {
			t.Errorf("missing console dependency %s", destination)
		}
	}
	for destination, appID := range map[string]string{
		"/usr/lib/pachaos/ld-musl-x86_64.so.1": "pachaos_musl_ldso",
		"/usr/lib/pachaos/libc.so":             "pachaos_musl_libc",
		"/lib/ld-musl-x86_64.so.1":             "lpr_linux_musl_libc",
		"/lib/libc.so":                         "lpr_linux_musl_libc",
	} {
		if spec.Files[destination] != appID {
			t.Errorf("runtime ABI mismatch: %s is %s, want %s", destination, spec.Files[destination], appID)
		}
	}
}

func TestInstallerMarkerIsSeparateFromOrdinaryLiveProfile(t *testing.T) {
	root, err := config.FindRoot(".")
	if err != nil {
		t.Fatal(err)
	}
	workspace, err := config.Load(root)
	if err != nil {
		t.Fatal(err)
	}
	overlay, err := LoadSpec(workspace.Path("pack/installer-bootfs.yaml"))
	if err != nil {
		t.Fatal(err)
	}
	if overlay.Files["/etc/pacha/installer-mode"] != "installer_mode_marker" ||
		overlay.Files["/srv/kobox2/nvme/modules/nvme.ko"] != "kobox2_nvme_module" ||
		overlay.Files["/srv/installerd.elf"] != "installerd" ||
		overlay.Files["/sbin/pacha-install-runner"] != "installer_runner" ||
		overlay.Files["/sbin/pacha-live-import"] != "live_desktop_import" ||
		overlay.Files["/usr/bin/pacha-live-desktop"] != "live_desktop_launcher" ||
		overlay.Files["/sbin/pacha-install-progress"] != "installer_progress" ||
		overlay.Files["/sbin/e2fsck"] != "installer_e2fsck" {
		t.Fatalf("unexpected installer overlay: %+v", overlay.Files)
	}
	marker, err := os.ReadFile(workspace.Path("pack/installer-bootfs/marker"))
	if err != nil || string(marker) != "installer-v1\n" {
		t.Fatalf("invalid installer marker %q: %v", marker, err)
	}
}

func TestAMDGPUProbeOverlayContainsOnlyOptionalPackages(t *testing.T) {
	root, err := config.FindRoot(".")
	if err != nil {
		t.Fatal(err)
	}
	workspace, err := config.Load(root)
	if err != nil {
		t.Fatal(err)
	}
	overlay, err := LoadSpec(workspace.Path("pack/live-amdgpu-probe.yaml"))
	if err != nil {
		t.Fatal(err)
	}
	if len(overlay.Files) != 7 ||
		overlay.Files["/etc/pacha/gpu-probe.conf"] != "amdgpu_probe_policy" ||
		overlay.Files["/srv/gpud.elf"] != "gpud" ||
		overlay.Files["/srv/amdgpu-mesa-runtime.tar.gz"] != "amdgpu_mesa_runtime" ||
		overlay.Files["/srv/amdgpu-driver-package.tar.gz"] != "amdgpu_driver_archive" ||
		overlay.Files["/srv/amdgpu-driver-package.tar.gz.sha256"] != "amdgpu_driver_archive_digest" ||
		overlay.Files["/srv/frame-profile.tar.gz"] != "live_frame_profile" ||
		overlay.Files["/usr/bin/pacha-frame-profile"] != "live_frame_runner" {
		t.Fatalf("unexpected AMDGPU probe overlay: %+v", overlay.Files)
	}
	if overlay.SHA256["amdgpu_mesa_runtime"] == "" {
		t.Fatal("optional Mesa archive must be pinned by digest")
	}
	for destination, appID := range overlay.Files {
		if _, ok := workspace.App(appID); !ok {
			t.Errorf("%s refers to missing app %s", destination, appID)
		}
		if strings.Contains(destination, "firmware") || strings.Contains(destination, "modules") {
			t.Errorf("driver bytes must not enter the bootfs overlay: %s", destination)
		}
	}
}

func TestBootScratchPackagingBudget(t *testing.T) {
	// The kernel arena grows dynamically. Exercise pack's conservative policy
	// at its actual aligned boundaries instead of matching an obsolete constant
	// in the kernel source.
	metadata := int64((maxBootfsBytes+4095)/4096) * 8
	for _, test := range []struct {
		name                string
		init, load, archive int64
		want                bool
	}{
		{"load fits", 1, bootScratchBytes - 8, 0, true},
		{"load alignment overflows", 1, bootScratchBytes - 7, 0, false},
		{"image fits", bootScratchBytes - 8, 1, 0, true},
		{"image alignment overflows", bootScratchBytes - 7, 1, 0, false},
		{"metadata fits", bootScratchBytes - metadata, 1, maxBootfsBytes, true},
		{"metadata alignment overflows", bootScratchBytes - metadata + 1, 1, maxBootfsBytes, false},
	} {
		t.Run(test.name, func(t *testing.T) {
			if got := fitsBootScratch(test.init, test.load, test.archive); got != test.want {
				t.Fatalf("fitsBootScratch(%d, %d, %d) = %v, want %v", test.init, test.load, test.archive, got, test.want)
			}
		})
	}
}

func TestLargeLiveRootStagesDirectlyWithoutProcessSizedExpansion(t *testing.T) {
	root := t.TempDir()
	workspace := &config.Workspace{
		Root: root, Artifacts: ".artifacts",
		AppsMap: map[string]config.App{},
	}
	for _, name := range []string{"profile", "filed", "seed0root", "payload", "seed0boot"} {
		workspace.AppsMap[name] = config.App{ID: name, File: "unused", Out: name + ".bin"}
		artifact := workspace.ArtifactPath(workspace.AppsMap[name])
		if err := os.MkdirAll(filepath.Dir(artifact), 0o755); err != nil {
			t.Fatal(err)
		}
		data := []byte(name)
		if name == "payload" {
			data = bytes.Repeat([]byte("reproducible live RAM payload\n"), 4500000)
		} else if name == "seed0boot" {
			data = testBootInitELF()
		}
		if err := os.WriteFile(artifact, data, 0o644); err != nil {
			t.Fatal(err)
		}
	}
	spec := "schema: 1\nfiles:\n" +
		"  /etc/pacha/boot-profile: profile\n" +
		"  /sbin/filed.elf: filed\n" +
		"  /sbin/seed0root.elf: seed0root\n" +
		"  /srv/payload.bin: payload\n"
	if err := os.MkdirAll(filepath.Dir(workspace.Path(SpecPath)), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(workspace.Path(SpecPath), []byte(spec), 0o644); err != nil {
		t.Fatal(err)
	}
	result, err := Build(workspace, Options{NoBuild: true})
	if err != nil {
		t.Fatal(err)
	}
	inner, err := os.ReadFile(workspace.Path(OutputDir, "ROOTFS.IMG"))
	if err != nil {
		t.Fatal(err)
	}
	if len(inner) <= 128*1024*1024 || len(inner) >= maxBootfsBytes ||
		!fitsBootScratch(int64(len(testBootInitELF())), 4096, result.Bytes) {
		t.Fatalf("unexpected direct root size; root=%d bootfs=%d", len(inner), result.Bytes)
	}
	staged, err := os.ReadFile(result.Image)
	if err != nil || !bytes.Equal(staged, inner) {
		t.Fatalf("large live root was not staged directly: %v", err)
	}
	if result.Manifest != workspace.Path(OutputDir, ManifestName) {
		t.Fatalf("unexpected manifest: %s", result.Manifest)
	}
}

func TestBuildNoBuildLeavesNormalImagesUntouched(t *testing.T) {
	root := t.TempDir()
	if err := os.MkdirAll(filepath.Join(root, "pack"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(root, SpecPath), []byte("schema: 1\nfiles:\n  /bin/ash: ash\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	workspace := &config.Workspace{
		Root: root, Artifacts: ".artifacts",
		AppsMap: map[string]config.App{
			"ash":       {ID: "ash", File: "unused", Out: "ASH.ELF"},
			"seed0boot": {ID: "seed0boot", File: "unused", Out: "SEED0BT.ELF"},
		},
	}
	initArtifact := workspace.ArtifactPath(workspace.AppsMap["seed0boot"])
	if err := os.MkdirAll(filepath.Dir(initArtifact), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(initArtifact, testBootInitELF(), 0o755); err != nil {
		t.Fatal(err)
	}
	artifact := workspace.ArtifactPath(workspace.AppsMap["ash"])
	if err := os.MkdirAll(filepath.Dir(artifact), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(artifact, []byte("test shell image"), 0o755); err != nil {
		t.Fatal(err)
	}
	for _, filename := range []string{"BOOTFS.IMG", "limine-boot.img", "disk.img"} {
		if err := os.WriteFile(workspace.Path(".artifacts", filename), []byte("preserve"), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	result, err := Build(workspace, Options{NoBuild: true})
	if err != nil {
		t.Fatal(err)
	}
	if result.Entries != 1 || result.Bytes == 0 ||
		result.Manifest != workspace.Path(OutputDir, ManifestName) ||
		result.Image != workspace.Path(OutputDir, ImageName) {
		t.Fatalf("unexpected result: %+v", result)
	}
	manifest, err := os.ReadFile(result.Manifest)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(manifest), "/bin/ash=../userland/ash/ASH.ELF\n") {
		t.Fatalf("bad manifest: %s", manifest)
	}
	image, err := os.ReadFile(result.Image)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.HasPrefix(image, []byte{'B', 'T', 'F', 'S'}) {
		t.Fatalf("bad bootfs magic: %x", image[:4])
	}
	for _, filename := range []string{"BOOTFS.IMG", "limine-boot.img", "disk.img"} {
		data, err := os.ReadFile(workspace.Path(".artifacts", filename))
		if err != nil || string(data) != "preserve" {
			t.Fatalf("normal image %s changed: %q, %v", filename, data, err)
		}
	}
	if err := os.WriteFile(workspace.Path("unused"), []byte("updated script"), 0o644); err != nil {
		t.Fatal(err)
	}
	if _, err := Build(workspace, Options{NoBuild: true}); err == nil || !strings.Contains(err.Error(), "stale staged file") {
		t.Fatalf("stale source accepted: %v", err)
	}
	if after, err := os.ReadFile(result.Image); err != nil || !bytes.Equal(after, image) {
		t.Fatalf("stale source failure changed prior image: %v", err)
	}
	if err := os.WriteFile(workspace.Path("unused"), []byte("test shell image"), 0o644); err != nil {
		t.Fatal(err)
	}
	if _, err := Build(workspace, Options{NoBuild: true}); err != nil {
		t.Fatalf("matching copied source rejected: %v", err)
	}
	badLock := "schema: 1\nsha256:\n  ash: " + strings.Repeat("0", 64) + "\nfiles:\n  /bin/ash: ash\n"
	if err := os.WriteFile(workspace.Path(SpecPath), []byte(badLock), 0o644); err != nil {
		t.Fatal(err)
	}
	if _, err := Build(workspace, Options{NoBuild: true}); err == nil {
		t.Fatal("checksum mismatch accepted")
	}
	after, err := os.ReadFile(result.Image)
	if err != nil || !bytes.Equal(after, image) {
		t.Fatalf("checksum failure changed prior image: %v", err)
	}
}

func TestBootScratchIncludesInitImageAndLoadWindow(t *testing.T) {
	if !fitsBootScratch(9, 4096, maxBootfsBytes) {
		t.Fatal("bootfs staging cap should be independent of init scratch")
	}
	if fitsBootScratch(9, 4096, maxBootfsBytes+1) {
		t.Fatal("archive accepted beyond bootfs staging capacity")
	}
	if fitsBootScratch(9, bootScratchBytes-15, 0) {
		t.Fatal("init ELF load window accepted beyond boot scratch capacity")
	}
}

func testBootInitELF() []byte {
	image := make([]byte, 120)
	copy(image, []byte{'\x7f', 'E', 'L', 'F', 2, 1, 1})
	binary.LittleEndian.PutUint16(image[16:], 3)  // ET_DYN
	binary.LittleEndian.PutUint16(image[18:], 62) // x86-64
	binary.LittleEndian.PutUint32(image[20:], 1)
	binary.LittleEndian.PutUint64(image[32:], 64) // program header offset
	binary.LittleEndian.PutUint16(image[52:], 64)
	binary.LittleEndian.PutUint16(image[54:], 56)
	binary.LittleEndian.PutUint16(image[56:], 1)
	binary.LittleEndian.PutUint32(image[64:], 1) // PT_LOAD
	binary.LittleEndian.PutUint32(image[68:], 5)
	binary.LittleEndian.PutUint64(image[96:], uint64(len(image)))
	binary.LittleEndian.PutUint64(image[104:], 4096)
	binary.LittleEndian.PutUint64(image[112:], 4096)
	return image
}

func TestRequireELFDependenciesRejectsMissingRuntime(t *testing.T) {
	available := map[string]bool{"/lib/ld-musl-x86_64.so.1": true}
	if err := requireELFDependencies("/bin/ash", "/lib/ld-musl-x86_64.so.1", nil, available); err != nil {
		t.Fatal(err)
	}
	if err := requireELFDependencies("/bin/ash", "/lib/missing-loader.so", nil, available); err == nil {
		t.Fatal("missing interpreter accepted")
	}
	if err := requireELFDependencies("/bin/ash", "", []string{"libc.musl-x86_64.so.1"}, available); err == nil {
		t.Fatal("missing library accepted")
	}
	available["/lib/libc.musl-x86_64.so.1"] = true
	if err := requireELFDependencies("/bin/ash", "/lib/ld-musl-x86_64.so.1", []string{"libc.musl-x86_64.so.1"}, available); err != nil {
		t.Fatal(err)
	}
}
