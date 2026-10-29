// SPDX-License-Identifier: MIT
package buildsys

import (
	"os"
	"path/filepath"
	"reflect"
	"slices"
	"strings"
	"testing"

	"capabilityos/pack/internal/config"
)

func TestNormalNativeProducerClosure(t *testing.T) {
	root, err := config.FindRoot(".")
	if err != nil {
		t.Fatal(err)
	}
	workspace, err := config.Load(root)
	if err != nil {
		t.Fatal(err)
	}
	assertInputs := func(t *testing.T, appID string, required []string) {
		t.Helper()
		app, ok := workspace.App(appID)
		if !ok {
			t.Fatalf("missing producer %s", appID)
		}
		source, err := app.FileSource()
		if err != nil {
			t.Fatal(err)
		}
		recipe, ok := workspace.Recipe(source.Recipe)
		if !ok {
			t.Fatalf("%s has no recipe", appID)
		}
		seen := map[string]bool{}
		for _, input := range recipe.Inputs {
			if seen[input.Path] {
				t.Errorf("%s duplicates input %s", appID, input.Path)
			}
			seen[input.Path] = true
		}
		deps := recipeInputs(workspace, recipe)
		for _, required := range required {
			want := workspace.Path(required)
			covered := false
			for _, dep := range deps {
				if dep == want {
					covered = true
					break
				}
				info, err := os.Stat(dep)
				if err == nil && info.IsDir() && strings.HasPrefix(want, dep+string(filepath.Separator)) {
					covered = true
					break
				}
			}
			if !covered {
				t.Errorf("%s can reuse stale output after %s changes", appID, required)
			}
		}
	}

	// Native CMake dependencies used to be guessed by parsing one project
	// helper. Every former consumer now delegates the complete source/header
	// closure to Ninja and keeps the sourced jobserver helper explicit.
	for _, appID := range strings.Fields("pachaos_capsule unixd netd logd termd gpud usbd inputd storage_boot seed0boot seed0root filed lpr_supervisor installerd linux_lpr_runtime") {
		t.Run(appID, func(t *testing.T) {
			app, ok := workspace.App(appID)
			if !ok {
				t.Fatalf("missing app %s", appID)
			}
			source, err := app.FileSource()
			if err != nil {
				t.Fatal(err)
			}
			recipe, ok := workspace.Recipe(source.Recipe)
			if !ok {
				t.Fatalf("missing recipe for %s", appID)
			}
			if len(recipe.Discover) != 1 || recipe.Discover[0].Kind != "ninja" {
				t.Fatalf("%s discovery = %#v", appID, recipe.Discover)
			}
			if !containsString(recipe.Tools, "ninja") || !containsString(recipe.Tools, "clang") {
				t.Fatalf("%s tools = %#v", appID, recipe.Tools)
			}
			foundJobs := false
			for _, input := range recipe.Inputs {
				foundJobs = foundJobs || input.Path == "pack/scripts/lib/jobs.sh"
			}
			if !foundJobs {
				t.Fatalf("%s does not declare pack/scripts/lib/jobs.sh", appID)
			}
		})
	}
	for _, appID := range []string{"pachaos_musl_libc", "pachaos_musl_ldso"} {
		assertInputs(t, appID, []string{
			"musl/pachaos/build/runtime.mk", "musl/pachaos/syscall/native.c",
			"kobox2/protocol/generated/include/kobox2/filesystem_layout.h",
			"userland/filed/src/exec_payload.c", "userland/filed/include/filed/ipc_protocol.h",
			"userland/libipc/include/pacha/service_abi.h",
		})
	}

	// The LPR runtime's headers come from Ninja discovery (asserted above); its
	// project compiles these shared sources.
	lprProject, err := os.ReadFile(workspace.Path("userland", "personality", "linux", "CMakeLists.txt"))
	if err != nil {
		t.Fatal(err)
	}
	for _, source := range []string{
		"userland/kobox2_owner/fs_channel.c",
		"kobox2/protocol/src/filesystem.c", "kobox2/protocol/src/protocol.c",
		"kobox2/protocol/src/virtqueue.c", "kobox2/protocol/src/virtqueue_memory.c",
		"kobox2/protocol/arch/x86_64/virtqueue_atomic.c",
		"userland/libipc/src/status.c", "userland/personality/src/lpr_manifest.c",
	} {
		if !strings.Contains(string(lprProject), "${REPO_ROOT}/"+source+"\"") {
			t.Errorf("linux_lpr_runtime does not compile %s", source)
		}
	}
	// The committed protocol layouts are compiled directly; the schema they are
	// generated from is held to them by one check that every consumer needs.
	assertNeeds := func(t *testing.T, appID string, need string) {
		t.Helper()
		app, ok := workspace.App(appID)
		if !ok {
			t.Fatalf("missing producer %s", appID)
		}
		source, err := app.FileSource()
		if err != nil {
			t.Fatal(err)
		}
		recipe, ok := workspace.Recipe(source.Recipe)
		if !ok {
			t.Fatalf("%s has no recipe", appID)
		}
		if !containsString(recipe.Needs, need) {
			t.Errorf("%s does not need %s", appID, need)
		}
	}
	assertNeeds(t, "linux_lpr_runtime", "kobox2_protocol_check")
	assertNeeds(t, "linux_lpr_runtime", "musl_libc")
	assertNeeds(t, "linux_lpr_runtime", "zydis_source")
	// build-sandbox.sh writes a depfile naming every source and header of
	// the ELF, so the sandbox recipes track the gpud, libpacha, protocol and
	// owner sources each profile selects without declaring them.
	for appID, elf := range map[string]string{
		"amdgpu_probe_policy":        ".artifacts/kobox2-amdgpu-package/sandbox.elf",
		"amdgpu_driver_archive":      ".artifacts/kobox2-amdgpu-package/sandbox.elf",
		"kobox2_storage_package":     ".artifacts/kobox2-storage-production/sandbox.elf",
		"kobox2_usb_hid_sandbox":     ".artifacts/kobox2-usb-hid-production/sandbox.elf",
		"kobox2_usb_storage_sandbox": ".artifacts/kobox2-usb-storage-xhci-production/sandbox.elf",
		"kobox2_nvme_sandbox":        ".artifacts/kobox2-nvme-production/sandbox.elf",
		"kobox2_network_sandbox":     ".artifacts/kobox2-network-production/sandbox.elf",
	} {
		app, ok := workspace.App(appID)
		if !ok {
			t.Fatalf("missing producer %s", appID)
		}
		source, err := app.FileSource()
		if err != nil {
			t.Fatal(err)
		}
		recipe, _ := workspace.Recipe(source.Recipe)
		discovered := false
		for _, discovery := range recipe.Discover {
			discovered = discovered || discovery.Kind == "depfile" && discovery.Glob == elf+".d"
		}
		if !discovered || !slices.Contains(recipe.Outputs, elf) {
			t.Errorf("%s: recipe %s does not build %s with its depfile", appID, recipe.ID, elf)
		}
		assertInputs(t, appID, []string{"userland/kobox2_adapter/build-sandbox.sh", "userland/kobox2_adapter/sandbox_main.c"})
	}
	// These ELFs compile the owner boot code; Ninja discovery (asserted above)
	// tracks it and its headers once the project lists it as a source.
	for _, appID := range []string{"netd", "gpud", "usbd", "filed"} {
		project, err := os.ReadFile(workspace.Path("userland", appID, "CMakeLists.txt"))
		if err != nil {
			t.Fatal(err)
		}
		if !strings.Contains(string(project), "/userland/kobox2_owner/boot.c") {
			t.Errorf("%s does not compile userland/kobox2_owner/boot.c", appID)
		}
	}
}

func TestProductionKbuildRecipesUseDiscoveredSourceClosure(t *testing.T) {
	root, err := config.FindRoot(".")
	if err != nil {
		t.Fatal(err)
	}
	workspace, err := config.Load(root)
	if err != nil {
		t.Fatal(err)
	}
	expected := map[string]map[string]int{
		"gpud_package":    {"kbuild": 1, "kbuild-dir-file": 1, "depfile": 2},
		"usb_hid_package": {"kbuild": 1, "kbuild-dir-file": 1, "depfile": 1},
		"storage_package": {"kbuild": 1, "kbuild-dir-file": 1, "depfile": 3},
		"block_packages":  {"kbuild": 1, "kbuild-dir-file": 2, "depfile": 6},
		"network_package": {"kbuild": 1, "kbuild-dir-file": 1, "depfile": 3},
		"amdgpu_package":  {"kbuild": 1, "kbuild-dir-file": 1, "depfile": 4},
	}
	for _, id := range strings.Fields("gpud_package usb_hid_package storage_package block_packages network_package amdgpu_package") {
		recipe, ok := workspace.Recipe(id)
		if !ok {
			t.Fatalf("missing recipe %s", id)
		}
		kinds := map[string]int{}
		for _, discovery := range recipe.Discover {
			kinds[discovery.Kind]++
		}
		if !reflect.DeepEqual(kinds, expected[id]) {
			t.Errorf("%s does not cover canonical Kbuild, selected provider Kbuild, and direct compiler depfiles: %#v", id, recipe.Discover)
		}
		found := false
		for _, input := range recipe.Inputs {
			if input.Path == "kobox2/linux-sandbox/kobox" {
				found = reflect.DeepEqual(input.Exclude, []string{"*.c", "*.h", "*.S"})
			}
		}
		if !found {
			t.Errorf("%s does not safely narrow the kobox kbuild source", id)
		}
		// The sandbox recipes own every userland source. A userland input or
		// a depfile glob reaching sandbox.elf.d would rerun the whole kbuild
		// runtime for a libpacha edit. gpud's package driver script is the one
		// userland file a kbuild recipe runs.
		for _, input := range recipe.Inputs {
			if strings.HasPrefix(input.Path, "userland/") && input.Path != "userland/gpud/build-package.sh" {
				t.Errorf("%s declares userland input %s", id, input.Path)
			}
		}
		for _, discovery := range recipe.Discover {
			if discovery.Kind == "depfile" && !strings.Contains(discovery.Glob, "/runtime/") &&
				!strings.Contains(discovery.Glob, "/.deps/") && !strings.Contains(discovery.Glob, "/provider/") &&
				!strings.Contains(discovery.Glob, "/manifest-build/") && !strings.HasSuffix(discovery.Glob, "-manifest-generator.d") &&
				!strings.HasPrefix(discovery.Glob, ".artifacts/kobox2-amdgpu-hosted/") &&
				!strings.HasPrefix(discovery.Glob, ".artifacts/kobox2-amdgpu-mapping/") &&
				!strings.HasPrefix(discovery.Glob, ".artifacts/gpud-production") {
				t.Errorf("%s depfile glob %s may match a sandbox depfile", id, discovery.Glob)
			}
		}
	}
}
