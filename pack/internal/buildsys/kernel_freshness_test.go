package buildsys

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"capabilityos/pack/internal/config"
)

func kernelFreshnessWorkspace(t *testing.T) (*config.Workspace, map[string]string) {
	t.Helper()
	root := t.TempDir()
	bin := filepath.Join(root, "bin")
	workspace := &config.Workspace{Root: root, Artifacts: ".artifacts", State: ".artifacts/pack", Kernel: config.Kernel{Dir: "kernel", Step: "limine"}}
	localCache, globalCache := kernelCacheDirs(workspace)
	files := map[string]string{
		filepath.Join(bin, "zig"):                                  "zig 0.16",
		workspace.Path("kernel", "src", "kernel.zig"):              "kernel source",
		workspace.Path("verified", "scheduling", "src", "eevdf.c"): "scheduler",
		filepath.Join(localCache, "c", "options.zig"):              "generated",
		filepath.Join(globalCache, "o", "lib.zig"):                 "cached",
		KernelOutputPath(workspace, "limine"):                      "elf",
	}
	for path, data := range files {
		if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(path, []byte(data), 0o755); err != nil {
			t.Fatal(err)
		}
	}
	// Prefix numbers mean different roots in different manifests; relative
	// entries count only when they exist below the kernel directory.
	manifest := strings.Join([]string{
		"0",
		"13 1 1 aa 0 src/kernel.zig",
		"9 2 2 bb 3 " + workspace.Path("verified", "scheduling", "src", "eevdf.c"),
		"9 3 3 cc 1 compiler_rt.zig",
		"9 4 4 dd 2 c/options.zig",
		"9 5 5 ee 3 " + filepath.Join(globalCache, "o", "lib.zig"),
		"9 6 6 ff 3 " + workspace.Path("removed.zig"),
		"",
	}, "\n")
	if err := os.MkdirAll(filepath.Join(localCache, "h"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(localCache, "h", "step.txt"), []byte(manifest), 0o644); err != nil {
		t.Fatal(err)
	}
	t.Setenv("PATH", bin)
	return workspace, files
}

func TestZigManifestInputsKeepOnlyProjectFiles(t *testing.T) {
	workspace, _ := kernelFreshnessWorkspace(t)
	inputs, err := zigManifestInputs(workspace)
	if err != nil {
		t.Fatal(err)
	}
	want := []string{
		workspace.Path("kernel", "src", "kernel.zig"),
		workspace.Path("removed.zig"),
		workspace.Path("verified", "scheduling", "src", "eevdf.c"),
	}
	if strings.Join(inputs, "\n") != strings.Join(want, "\n") {
		t.Fatalf("inputs = %v, want %v", inputs, want)
	}
}

func TestKernelFreshnessTracksRecordedInputs(t *testing.T) {
	workspace, _ := kernelFreshnessWorkspace(t)
	if reason, err := KernelFreshness(workspace, "limine"); err != nil || !strings.Contains(reason, "no record") {
		t.Fatalf("before record: %q, %v", reason, err)
	}
	record := func() {
		t.Helper()
		if err := writeKernelInputRecord(workspace, "limine", KernelOutputPath(workspace, "limine"), newHasher(hashCachePath(workspace))); err != nil {
			t.Fatal(err)
		}
	}
	expect := func(fragment string) {
		t.Helper()
		reason, err := KernelFreshness(workspace, "limine")
		if err != nil {
			t.Fatal(err)
		}
		if fragment == "" && reason != "" || fragment != "" && !strings.Contains(reason, fragment) {
			t.Fatalf("reason = %q, want %q", reason, fragment)
		}
	}
	write := func(path, data string) {
		t.Helper()
		if err := os.WriteFile(path, []byte(data), 0o755); err != nil {
			t.Fatal(err)
		}
	}
	record()
	expect("")

	scheduler := workspace.Path("verified", "scheduling", "src", "eevdf.c")
	write(scheduler, "scheduler edited")
	expect("kernel input changed: verified/scheduling/src/eevdf.c")
	record()

	write(workspace.Path("removed.zig"), "now present")
	expect("kernel input appeared: removed.zig")
	if err := os.Remove(workspace.Path("removed.zig")); err != nil {
		t.Fatal(err)
	}
	expect("")

	write(filepath.Join(workspace.Root, "bin", "zig"), "zig 0.17")
	expect("zig toolchain changed")
	record()

	write(KernelOutputPath(workspace, "limine"), "elf replaced")
	expect("changed outside pacgo")
}
