// SPDX-License-Identifier: MIT
package buildsys

import (
	"os"
	"path/filepath"
	"regexp"
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
		seen := map[string]bool{}
		for _, input := range source.Input {
			if seen[input] {
				t.Errorf("%s duplicates input %s", appID, input)
			}
			seen[input] = true
		}
		deps := fileSourceDependencies(workspace, source)
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

	// CMake's inner dependency list is not traversed by the outer pack cache.
	// Audit its real inputs so adding a codec or port helper cannot silently
	// leave normal boot/installer producers with a successful stale build.
	common := "userland/cmake/pachaos_native_dependencies.cmake"
	data, err := os.ReadFile(workspace.Path(common))
	if err != nil {
		t.Fatal(err)
	}
	native := []string{common}
	for _, match := range regexp.MustCompile(`"\$\{_pachaos_native_root\}/([^"\n]+)"`).FindAllStringSubmatch(string(data), -1) {
		native = append(native, match[1])
	}
	if len(native) < 20 {
		t.Fatal("native dependency list was not parsed")
	}
	for _, appID := range strings.Fields("pachaos_capsule unixd netd logd termd gpud usbd inputd storage_boot seed0boot seed0root filed lpr_supervisor installerd") {
		t.Run(appID, func(t *testing.T) { assertInputs(t, appID, native) })
	}
	for _, appID := range []string{"pachaos_musl_libc", "pachaos_musl_ldso"} {
		assertInputs(t, appID, []string{
			"musl/pachaos/build/runtime.mk", "musl/pachaos/syscall/native.c",
			"kobox2/protocol/generated/include/kobox2/filesystem_layout.h",
			"userland/filed/src/exec_payload.c", "userland/filed/include/filed/ipc_protocol.h",
			"userland/libipc/include/pacha/service_abi.h",
		})
	}

	lpr := []string{
		"userland/kobox2_owner/fs_channel.c", "userland/kobox2_owner/fs_channel.h",
		"kobox2/protocol/schema/filesystem.json", "kobox2/protocol/generated/include/kobox2/filesystem_layout.h",
		"kobox2/protocol/src/filesystem.c", "kobox2/protocol/src/protocol.c",
		"kobox2/protocol/src/virtqueue.c", "kobox2/protocol/src/virtqueue_memory.c",
		"kobox2/protocol/arch/x86_64/virtqueue_atomic.c",
		"userland/libipc/src/status.c", "userland/libipc/include/pacha/service_abi.h",
		"userland/filed/include/filed/ipc_protocol.h", "userland/filed/include/filed/payload.h",
		"userland/personality/src/lpr_manifest.c",
		"userland/netd/include/netd/ipc_protocol.h", "userland/termd/include/termd/ipc_protocol.h",
		"userland/inputd/include/inputd/ipc_protocol.h",
		"userland/installer/disk_port.h", "userland/installer/block_transport.h",
		"userland/installer/block_protocol.h", "userland/installer/progress_protocol.h",
	}
	assertInputs(t, "linux_lpr_runtime", lpr)
	for _, appID := range []string{"amdgpu_probe_policy", "amdgpu_driver_archive"} {
		assertInputs(t, appID, []string{
			"userland/kobox2_adapter/sandbox_config.h", "userland/kobox2_adapter/sandbox_main.c",
			"kobox2/protocol/src/gpu.c", "kobox2/protocol/generated/include/kobox2/protocol_layout.h",
			"userland/gpud/gpu_channel.c", "userland/gpud/gpu_channel.h",
			"userland/gpud/gpu_sessions.c", "userland/gpud/gpu_sessions.h",
			"userland/libpacha/src/log.c", "userland/libpacha/src/syscall.c",
			"userland/libipc/include/pacha/service_abi.h",
		})
	}
	assertInputs(t, "kobox2_storage_package", []string{
		"userland/kobox2_adapter/fs_queue.c", "userland/kobox2_adapter/fs_queue.h",
		"userland/kobox2_owner/fs_channel.c", "userland/kobox2_owner/fs_channel.h",
		"kobox2/protocol/src/filesystem.c", "kobox2/protocol/generated/include/kobox2/filesystem_layout.h",
	})
	for _, appID := range []string{"netd", "gpud", "usbd", "filed"} {
		assertInputs(t, appID, []string{"userland/kobox2_owner/boot.c", "userland/kobox2_owner/boot.h"})
	}
}
