package buildsys

import (
	"os"
	"os/exec"
	"path/filepath"
	"reflect"
	"strings"
	"testing"

	"capabilityos/pack/internal/config"
)

func TestParseDepfileEscapesAndContinuations(t *testing.T) {
	data := []byte("out\\ file.o: ../src/main.c ../include/a\\ b.h \\\n ../include/back\\\\slash.h\nother: generated.h\n")
	want := []string{"../src/main.c", "../include/a b.h", "../include/back\\slash.h", "generated.h"}
	if got := parseDepfile(data); !reflect.DeepEqual(got, want) {
		t.Fatalf("parseDepfile() = %#v, want %#v", got, want)
	}
}

func TestKbuildDiscoveryResolvesOutputThenSourceAndRejectsMissingDeps(t *testing.T) {
	root := t.TempDir()
	source := filepath.Join(root, "linux")
	build := filepath.Join(root, ".artifacts", "build")
	mustWriteDiscoveryFile(t, filepath.Join(source, "Makefile"), "all:\n")
	mustWriteDiscoveryFile(t, filepath.Join(source, "drivers", "demo.c"), "source\n")
	mustWriteDiscoveryFile(t, filepath.Join(source, "include", "demo.h"), "header\n")
	mustWriteDiscoveryFile(t, filepath.Join(source, "include", "prefer.h"), "source header\n")
	mustWriteDiscoveryFile(t, filepath.Join(source, "Kconfig"), "config DEMO\n")
	mustWriteDiscoveryFile(t, filepath.Join(build, "Makefile"), "include "+filepath.Join(source, "Makefile")+"\n")
	missing := filepath.Join(source, "include", "optional.h")
	command := "source_drivers/demo.o := drivers/demo.c\n\n" +
		"deps_drivers/demo.o := \\\n  include/demo.h \\\n  include/prefer.h \\\n  $(wildcard " + missing + ") \\\n  include/generated/autoconf.h\n"
	mustWriteDiscoveryFile(t, filepath.Join(build, "drivers", ".demo.o.cmd"), command)
	mustWriteDiscoveryFile(t, filepath.Join(build, "include", "generated", "autoconf.h"), "generated\n")
	mustWriteDiscoveryFile(t, filepath.Join(build, "include", "prefer.h"), "generated header\n")
	mustWriteDiscoveryFile(t, filepath.Join(build, "include", "config", "auto.conf.cmd"), "deps_config := Kconfig\n")

	got, err := discoverKbuild(build)
	if err != nil {
		t.Fatal(err)
	}
	for _, wanted := range []string{
		filepath.Join(source, "drivers", "demo.c"), filepath.Join(source, "include", "demo.h"),
		filepath.Join(source, "Kconfig"), filepath.Join(build, "include", "generated", "autoconf.h"),
		filepath.Join(build, "include", "prefer.h"),
	} {
		if !containsString(got.Files, wanted) {
			t.Errorf("missing discovered path %s in %#v", wanted, got.Files)
		}
	}
	if !reflect.DeepEqual(got.Negative, []string{missing}) {
		t.Fatalf("negative = %#v, want %s", got.Negative, missing)
	}
	if containsString(got.Files, filepath.Join(source, "include", "prefer.h")) {
		t.Fatal("srctree path won over the O-relative generated header")
	}

	mustWriteDiscoveryFile(t, filepath.Join(build, "drivers", ".demo.o.cmd"),
		"source_drivers/demo.o := drivers/demo.c\n"+
			"deps_drivers/demo.o := include/does-not-exist.h\n")
	_, err = discoverKbuild(build)
	if err == nil || !strings.Contains(err.Error(), "include/does-not-exist.h") {
		t.Fatalf("missing non-wildcard dependency error = %v", err)
	}
}

func TestKbuildDiscoveryUsesRecordedProviderDirectory(t *testing.T) {
	root := t.TempDir()
	workspace := &config.Workspace{Root: root, Artifacts: ".artifacts"}
	source := filepath.Join(root, "linux")
	build := filepath.Join(root, ".artifacts", "providers", "selected")
	mustWriteDiscoveryFile(t, filepath.Join(source, "Makefile"), "all:\n")
	mustWriteDiscoveryFile(t, filepath.Join(source, "kobox", "provider.c"), "int provider;\n")
	mustWriteDiscoveryFile(t, filepath.Join(build, "Makefile"), "include "+filepath.Join(source, "Makefile")+"\n")
	mustWriteDiscoveryFile(t, filepath.Join(build, ".provider.o.cmd"), "source_provider.o := kobox/provider.c\n")
	mustWriteDiscoveryFile(t, filepath.Join(root, ".artifacts", "output", "provider-kbuild-dir"), ".artifacts/providers/selected\n")

	got, err := discoverKbuildDirFile(workspace, ".artifacts/output/provider-kbuild-dir")
	if err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(got.Files, []string{filepath.Join(source, "kobox", "provider.c")}) {
		t.Fatalf("files = %#v", got.Files)
	}
}

func TestKbuildDiscoveryResolvesSubmakeVpath(t *testing.T) {
	root := t.TempDir()
	source := filepath.Join(root, "linux")
	build := filepath.Join(root, ".artifacts", "build")
	mustWriteDiscoveryFile(t, filepath.Join(source, "Makefile"), "all:\n")
	wanted := filepath.Join(source, "tools", "lib", "subcmd", "exec-cmd.c")
	mustWriteDiscoveryFile(t, wanted, "int main(void) { return 0; }\n")
	mustWriteDiscoveryFile(t, filepath.Join(build, "Makefile"), "include "+filepath.Join(source, "Makefile")+"\n")
	mustWriteDiscoveryFile(t, filepath.Join(build, "tools", "objtool", "libsubcmd", ".exec-cmd.o.cmd"),
		"source_"+filepath.Join(build, "tools", "objtool", "libsubcmd", "exec-cmd.o")+" := exec-cmd.c\n")

	got, err := discoverKbuild(build)
	if err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(got.Files, []string{wanted}) {
		t.Fatalf("files = %#v, want %s", got.Files, wanted)
	}
}

func TestDiscoveredCrossRecipeOutputRequiresNeeds(t *testing.T) {
	root := t.TempDir()
	producer := filepath.Join(root, ".artifacts", "producer.out")
	mustWriteDiscoveryFile(t, producer, "generated")
	mustWriteDiscoveryFile(t, filepath.Join(root, ".artifacts", "deps", "app.d"), "app: "+producer+"\n")
	workspace := &config.Workspace{Root: root, Artifacts: ".artifacts", RecipesMap: map[string]config.Recipe{
		"producer": {ID: "producer", Outputs: []string{".artifacts/producer.out"}},
		"consumer": {ID: "consumer", Discover: []config.Discovery{{Kind: "depfile", Glob: ".artifacts/deps/*.d"}}},
	}}
	_, err := discoverRecipeInputs(workspace, workspace.RecipesMap["consumer"], environmentMap(os.Environ()))
	if err == nil || !strings.Contains(err.Error(), "add producer to consumer.needs") {
		t.Fatalf("error = %v", err)
	}
	consumer := workspace.RecipesMap["consumer"]
	consumer.Needs = []string{"producer"}
	workspace.RecipesMap["consumer"] = consumer
	got, err := discoverRecipeInputs(workspace, consumer, environmentMap(os.Environ()))
	if err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(got.Files, []string{producer}) {
		t.Fatalf("files = %#v, want %s", got.Files, producer)
	}
}

func TestNinjaDiscoveryDrivesStatusAndExcludedFilesDoNot(t *testing.T) {
	for _, tool := range []string{"cmake", "ninja", "cc"} {
		if _, err := exec.LookPath(tool); err != nil {
			t.Skipf("%s is unavailable", tool)
		}
	}
	root := t.TempDir()
	source := filepath.Join(root, "src")
	build := filepath.Join(root, ".artifacts", "cmake", "app")
	mustWriteDiscoveryFile(t, filepath.Join(source, "CMakeLists.txt"), "cmake_minimum_required(VERSION 3.16)\nproject(app C)\nadd_executable(app main.c)\n")
	mustWriteDiscoveryFile(t, filepath.Join(source, "main.c"), "#include \"used.h\"\nint main(void) { return VALUE; }\n")
	mustWriteDiscoveryFile(t, filepath.Join(source, "used.h"), "#define VALUE 0\n")
	mustWriteDiscoveryFile(t, filepath.Join(source, "unrelated.c"), "int unrelated;\n")
	runTinyCommand(t, root, "cmake", "-S", source, "-B", build, "-G", "Ninja")
	runTinyCommand(t, root, "cmake", "--build", build)
	discovered, err := discoverNinja(build, environmentMap(os.Environ()))
	if err != nil {
		t.Fatal(err)
	}
	for _, wanted := range []string{filepath.Join(source, "CMakeLists.txt"), filepath.Join(source, "main.c"), filepath.Join(source, "used.h")} {
		if !containsString(discovered.Files, wanted) {
			t.Errorf("Ninja did not report %s", wanted)
		}
	}
	workspace := &config.Workspace{
		Root: root, Artifacts: ".artifacts", State: ".artifacts/pack",
		RecipesMap: map[string]config.Recipe{
			"app": {
				ID: "app", Run: []string{"cmake", "--build", build},
				Inputs:   []config.RecipeInput{{Path: "src", Exclude: []string{"*.c", "*.h", "*.S"}}},
				Discover: []config.Discovery{{Kind: "ninja", Dir: ".artifacts/cmake/app"}},
				Outputs:  []string{".artifacts/cmake/app/app"},
			},
		},
	}
	if _, err := AdoptRecipes(workspace, []string{"app"}); err != nil {
		t.Fatal(err)
	}
	mustWriteDiscoveryFile(t, filepath.Join(source, "unrelated.c"), "int unrelated_changed;\n")
	statuses, err := RecipeStatuses(workspace)
	if err != nil {
		t.Fatal(err)
	}
	if !statuses[0].UpToDate {
		t.Fatalf("unrelated excluded source made recipe stale: %+v", statuses[0])
	}
	mustWriteDiscoveryFile(t, filepath.Join(source, "used.h"), "#define VALUE 12345\n")
	statuses, err = RecipeStatuses(workspace)
	if err != nil {
		t.Fatal(err)
	}
	if statuses[0].UpToDate || !strings.Contains(statuses[0].Reason, "discovered input changed") {
		t.Fatalf("included header status = %+v", statuses[0])
	}
}

func TestNegativeDependencyAppearingMakesStatusStale(t *testing.T) {
	root := t.TempDir()
	source := filepath.Join(root, "linux")
	build := filepath.Join(root, ".artifacts", "kbuild")
	missing := filepath.Join(source, "include", "shadow.h")
	mustWriteDiscoveryFile(t, filepath.Join(source, "Makefile"), "all:\n")
	mustWriteDiscoveryFile(t, filepath.Join(source, "main.c"), "int main;\n")
	mustWriteDiscoveryFile(t, filepath.Join(build, "Makefile"), "include "+filepath.Join(source, "Makefile")+"\n")
	mustWriteDiscoveryFile(t, filepath.Join(build, ".main.o.cmd"), "source_main.o := main.c\ndeps_main.o := $(wildcard "+missing+")\n")
	mustWriteDiscoveryFile(t, filepath.Join(root, "result"), "ok\n")
	workspace := &config.Workspace{Root: root, Artifacts: ".artifacts", State: ".artifacts/pack", RecipesMap: map[string]config.Recipe{
		"app": {
			ID: "app", Run: []string{"sh", "-c", "true"},
			Inputs:   []config.RecipeInput{{Path: "linux", Exclude: []string{"*.c", "*.h", "*.S"}}},
			Discover: []config.Discovery{{Kind: "kbuild", Dir: ".artifacts/kbuild"}}, Outputs: []string{"result"},
		},
	}}
	if _, err := AdoptRecipes(workspace, []string{"app"}); err != nil {
		t.Fatal(err)
	}
	mustWriteDiscoveryFile(t, missing, "shadow\n")
	statuses, err := RecipeStatuses(workspace)
	if err != nil {
		t.Fatal(err)
	}
	if statuses[0].UpToDate || !strings.Contains(statuses[0].Reason, "negative dep appeared") {
		t.Fatalf("status = %+v", statuses[0])
	}
}

func runTinyCommand(t *testing.T, directory, name string, args ...string) {
	t.Helper()
	cmd := exec.Command(name, args...)
	cmd.Dir = directory
	if output, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("%s failed: %v\n%s", name, err, output)
	}
}

func mustWriteDiscoveryFile(t *testing.T, path, contents string) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte(contents), 0o644); err != nil {
		t.Fatal(err)
	}
}

func TestNinjaQueryInputsSkipsOrderOnlyAndOutputs(t *testing.T) {
	output := []byte(`build.ninja:
  input: RERUN_CMAKE
    | /src/CMakeLists.txt
    | /src/cmake/deps.cmake
    explicit.in
    || order-only.stamp
    |@ validation.stamp
  outputs:
    other.ninja
`)
	got := ninjaQueryInputs(output)
	want := []string{"/src/CMakeLists.txt", "/src/cmake/deps.cmake", "explicit.in"}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("inputs = %#v, want %#v", got, want)
	}
}
