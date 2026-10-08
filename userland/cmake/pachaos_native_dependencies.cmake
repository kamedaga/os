# SPDX-License-Identifier: MIT

# Every native ELF embeds this translation and these codecs. Depending only
# on an obsolete upstream arch header leaves successful incremental builds
# running stale filesystem opcodes and credential/exec layouts.
get_filename_component(_pachaos_native_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(PACHAOS_NATIVE_DEPENDENCIES
  "${CMAKE_CURRENT_LIST_FILE}"
  "${_pachaos_native_root}/musl/pachaos/build/build-smokes.sh"
  "${_pachaos_native_root}/musl/pachaos/build/build-app-dynamic.sh"
  "${_pachaos_native_root}/musl/pachaos/build/build-runtime.sh"
  "${_pachaos_native_root}/musl/pachaos/build/runtime.mk"
  "${_pachaos_native_root}/musl/pachaos/syscall/native.c"
  "${_pachaos_native_root}/musl/pachaos/syscall/native_raw.h"
  "${_pachaos_native_root}/musl/pachaos/syscall/syscall_arch.h"
  "${_pachaos_native_root}/musl/pachaos/syscall/thread_area.c"
  "${_pachaos_native_root}/musl/pachaos/cli/pachaos_cli_shim.c"
  "${_pachaos_native_root}/musl/pachaos/include/pachaos/abi.h"
  "${_pachaos_native_root}/userland/libpacha/include/pacha/abi.h"
  "${_pachaos_native_root}/userland/libpacha/include/pacha/syscall.h"
  "${_pachaos_native_root}/userland/libpacha/src/syscall.c"
  "${_pachaos_native_root}/kobox2/protocol/schema/filesystem.json"
  "${_pachaos_native_root}/kobox2/protocol/include/kobox2/filesystem.h"
  "${_pachaos_native_root}/kobox2/protocol/generated/include/kobox2/filesystem_layout.h"
  "${_pachaos_native_root}/kobox2/protocol/src/filesystem.c"
  "${_pachaos_native_root}/userland/filed/src/exec_payload.c"
  "${_pachaos_native_root}/userland/filed/include/filed/payload.h"
  "${_pachaos_native_root}/userland/filed/include/filed/flags.h"
  "${_pachaos_native_root}/userland/filed/include/filed/ipc_protocol.h"
  "${_pachaos_native_root}/userland/libipc/include/pacha/service_abi.h")
unset(_pachaos_native_root)
