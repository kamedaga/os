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
  # Native FD carriers and exec records must rebuild their embedded consumers.
  "${_pachaos_native_root}/userland/libpacha/include/pacha/log.h"
  "${_pachaos_native_root}/userland/libpacha/include/pacha/bootstrap.h"
  "${_pachaos_native_root}/userland/libpacha/src/log.c"
  "${_pachaos_native_root}/userland/libipc/include/pacha/ipc.h"
  "${_pachaos_native_root}/userland/libipc/src/ipc.c"
  "${_pachaos_native_root}/userland/libcapsule/include/pacha/capsule.h"
  "${_pachaos_native_root}/userland/libcapsule/src/capsule.c"
  "${_pachaos_native_root}/userland/personality/include/personality/lpr_manifest.h"
  "${_pachaos_native_root}/userland/personality/src/lpr_manifest.c"
  "${_pachaos_native_root}/kobox2/protocol/schema/filesystem.json"
  "${_pachaos_native_root}/kobox2/protocol/include/kobox2/filesystem.h"
  "${_pachaos_native_root}/kobox2/protocol/generated/include/kobox2/filesystem_layout.h"
  "${_pachaos_native_root}/kobox2/protocol/src/filesystem.c"
  "${_pachaos_native_root}/userland/filed/src/exec_payload.c"
  "${_pachaos_native_root}/userland/filed/include/filed/payload.h"
  "${_pachaos_native_root}/userland/filed/include/filed/flags.h"
  "${_pachaos_native_root}/userland/filed/include/filed/ipc_protocol.h"
  "${_pachaos_native_root}/userland/libipc/include/pacha/service_abi.h")
# build-app-dynamic.sh links against the runtime that build-runtime.sh (the
# build_runtime pacgo recipe) installed; it never rebuilds it. Relink when any
# of those installed files changes.
if(DEFINED ENV{PACHAOS_MUSL_RUNTIME_DIR})
  set(_pachaos_runtime_lib "$ENV{PACHAOS_MUSL_RUNTIME_DIR}/install/usr/lib")
else()
  set(_pachaos_runtime_lib "${_pachaos_native_root}/.artifacts/musl-pachaos-runtime/install/usr/lib")
endif()
foreach(_pachaos_runtime_file IN ITEMS crt1.o crti.o crtn.o Scrt1.o libc.so)
  list(APPEND PACHAOS_NATIVE_DEPENDENCIES "${_pachaos_runtime_lib}/${_pachaos_runtime_file}")
endforeach()
unset(_pachaos_runtime_file)
unset(_pachaos_runtime_lib)
unset(_pachaos_native_root)
