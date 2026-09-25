package livebootfs

import (
	"bytes"
	"compress/zlib"
	"encoding/base64"
	"encoding/binary"
	"io"
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
	for destination, appID := range spec.Files {
		if _, ok := workspace.App(appID); !ok {
			t.Errorf("%s refers to missing app %s", destination, appID)
		}
	}
	for _, destination := range []string{
		"/etc/pacha/boot-profile",
		"/bin/ash", "/bin/sh", "/bin/busybox",
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
}

func TestBootScratchLimitMatchesKernel(t *testing.T) {
	if bootScratchBytes != 32*1024*1024 {
		t.Fatal("pack boot scratch limit changed without updating kernel agreement test")
	}
	root, err := config.FindRoot(".")
	if err != nil {
		t.Fatal(err)
	}
	source, err := os.ReadFile(filepath.Join(root, "kernel/src/boot/boot_scratch.zig"))
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Contains(source, []byte("pub const default_bytes: usize = 32 * 1024 * 1024;")) {
		t.Fatal("pack boot scratch limit no longer matches the kernel")
	}
}

func TestLargeLiveRootIsPackedBelowBootScratchLimit(t *testing.T) {
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
			data = bytes.Repeat([]byte("reproducible live RAM payload\n"), 1200000)
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
	if len(inner) < bootScratchBytes || !fitsBootScratch(int64(len(testBootInitELF())), 4096, result.Bytes) {
		t.Fatalf("expected packed root; inner=%d outer=%d", len(inner), result.Bytes)
	}
	packed, err := os.ReadFile(workspace.Path(OutputDir, "ROOTFS.Z"))
	if err != nil {
		t.Fatal(err)
	}
	if string(packed[:8]) != packedRootMagic {
		t.Fatalf("packed magic = %q, want %q", packed[:8], packedRootMagic)
	}
	if got := binary.LittleEndian.Uint64(packed[8:16]); got != uint64(len(inner)) {
		t.Fatalf("packed length = %d, want %d", got, len(inner))
	}
	reader, err := zlib.NewReader(bytes.NewReader(packed[16:]))
	if err != nil {
		t.Fatal(err)
	}
	expanded, err := io.ReadAll(reader)
	if err != nil {
		t.Fatal(err)
	}
	if err := reader.Close(); err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(expanded, inner) {
		t.Fatal("compressed live root does not round-trip")
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
	if !fitsBootScratch(9, 4096, bootScratchBytes-16) {
		t.Fatal("aligned init plus archive should fit exactly")
	}
	if fitsBootScratch(9, 4096, bootScratchBytes-15) {
		t.Fatal("archive accepted beyond aligned boot scratch capacity")
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
