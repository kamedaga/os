# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/../../cmake/pachaos-native.toolchain.cmake")
endif()

project(pachaos_lpr_runtime C ASM)

# The LPR runtime is loaded into Linux processes, so it compiles against the
# musl headers of the default libc build (musl_libc recipe) and links nothing:
# no crt, no libc, every symbol hidden except the namespace map's.
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/pachaos_native.cmake")
# The Generic platform of the native toolchain has no shared libraries; this
# one is an ELF shared object linked by the flags below alone.
set_property(GLOBAL PROPERTY TARGET_SUPPORTS_SHARED_LIBS TRUE)
set(CMAKE_SHARED_LIBRARY_PREFIX "")
set(CMAKE_SHARED_LIBRARY_SUFFIX ".so")
set(CMAKE_SHARED_LIBRARY_CREATE_C_FLAGS "-shared")
set(CMAKE_SHARED_LIBRARY_C_FLAGS "")

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(LPR_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(LPR_RUNTIME "${LPR_DIR}/runtime")
set(LPR_LIBC_DIR "${PACHAOS_MUSL_LIBC_ROOT}/default")
if(NOT EXISTS "${LPR_LIBC_DIR}/config")
  message(FATAL_ERROR "${LPR_LIBC_DIR} is not a built libc; build the pacgo recipe musl_libc first")
endif()
set(ZYDIS_SOURCE "${REPO_ROOT}/.artifacts/third_party/zydis-v4.1.1/tree")
if(NOT EXISTS "${ZYDIS_SOURCE}/.revision")
  message(FATAL_ERROR "${ZYDIS_SOURCE} is missing; build the pacgo recipe zydis_source first")
endif()
separate_arguments(LPR_EXTRA_CFLAGS UNIX_COMMAND "$ENV{PACHAOS_LPR_EXTRA_CFLAGS}")

set(LPR_COMMON_OPTIONS
  "--sysroot=${LPR_LIBC_DIR}/sysroot"
  -std=c11
  -O2
  -ffreestanding
  -fPIC
  -fvisibility=hidden
  -fno-stack-protector
  -fno-builtin)
set(LPR_FRAME_OPTIONS -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer)
set(LPR_WARNING_OPTIONS -Wall -Wextra -Werror)
set(ZYDIS_DEFINITIONS
  ZYDIS_STATIC_BUILD
  ZYDIS_MINIMAL_MODE
  ZYDIS_DISABLE_ENCODER
  ZYDIS_DISABLE_FORMATTER
  ZYDIS_DISABLE_SEGMENT
  ZYAN_NO_LIBC)
set(ZYDIS_INCLUDE_DIRECTORIES
  "${ZYDIS_SOURCE}/include"
  "${ZYDIS_SOURCE}/src"
  "${ZYDIS_SOURCE}/dependencies/zycore/include")

# lpr_zydis_options(<target> <option>...): the flags every LPR object shares.
function(lpr_zydis_options target)
  target_compile_options(${target} PRIVATE ${LPR_COMMON_OPTIONS} ${ARGN})
  target_compile_definitions(${target} PRIVATE ${ZYDIS_DEFINITIONS})
  target_include_directories(${target} PRIVATE ${ZYDIS_INCLUDE_DIRECTORIES})
endfunction()

# The decoder-only Zydis subset, unmodified upstream sources.
add_library(lpr_zydis OBJECT
  "${ZYDIS_SOURCE}/src/Decoder.c"
  "${ZYDIS_SOURCE}/src/DecoderData.c"
  "${ZYDIS_SOURCE}/src/SharedData.c")
lpr_zydis_options(lpr_zydis)

add_library(lpr_patch_scan OBJECT "${LPR_DIR}/decoder/patch_scan.c")
lpr_zydis_options(lpr_patch_scan ${LPR_WARNING_OPTIONS})
target_include_directories(lpr_patch_scan PRIVATE
  "${REPO_ROOT}/userland/personality/include"
  "${LPR_DIR}/decoder"
  "${REPO_ROOT}/userland/libipc/include")

add_library(lpr_scanner OBJECT "${LPR_RUNTIME}/lpr_runtime.c")
lpr_zydis_options(lpr_scanner ${LPR_WARNING_OPTIONS} ${LPR_FRAME_OPTIONS})
target_include_directories(lpr_scanner PRIVATE
  "${REPO_ROOT}/userland/personality/include"
  "${REPO_ROOT}/userland/libipc/include")

add_library(lpr_runtime SHARED
  "${LPR_RUNTIME}/lpr_signal.c"
  "${LPR_RUNTIME}/lpr_vsyscall.c"
  "${LPR_RUNTIME}/lpr_zpoline.c"
  "${LPR_RUNTIME}/lpr_syscall_catalog.c"
  "${LPR_RUNTIME}/lpr_memory.c"
  "${LPR_RUNTIME}/lpr_mlock.c"
  "${REPO_ROOT}/userland/personality/src/lpr_manifest.c"
  "${LPR_RUNTIME}/lpr_vfs_local.c"
  "${LPR_RUNTIME}/lpr_fd/table.c"
  "${LPR_RUNTIME}/lpr_error.c"
  "${LPR_RUNTIME}/lpr_process/client.c"
  "${LPR_RUNTIME}/lpr_unix/client.c"
  "${LPR_RUNTIME}/lpr_unix/cache.c"
  "${LPR_RUNTIME}/lpr_unix/profile.c"
  "${LPR_RUNTIME}/lpr_unix/mapping.c"
  "${LPR_RUNTIME}/lpr_unix/notify.c"
  "${LPR_RUNTIME}/lpr_unix/wait.c"
  "${LPR_RUNTIME}/lpr_unix/context.c"
  "${LPR_RUNTIME}/lpr_unix/socket.c"
  "${LPR_RUNTIME}/lpr_unix/address.c"
  "${LPR_RUNTIME}/lpr_unix/poll.c"
  "${LPR_RUNTIME}/lpr_unix/message.c"
  "${LPR_RUNTIME}/lpr_unix/datagram.c"
  "${LPR_RUNTIME}/lpr_unix/rights.c"
  "${LPR_RUNTIME}/lpr_unix/handoff.c"
  "${LPR_RUNTIME}/lpr_unix/options.c"
  "${REPO_ROOT}/userland/unixd/src/client_wire.c"
  "${REPO_ROOT}/userland/unixd/src/transport.c"
  "${REPO_ROOT}/userland/unixd/src/notify.c"
  "${REPO_ROOT}/userland/unixd/src/notify_client.c"
  "${LPR_RUNTIME}/lpr_filed.c"
  "${LPR_RUNTIME}/lpr_common/runtime_support.c"
  "${LPR_RUNTIME}/lpr_tty/client.c"
  "${LPR_RUNTIME}/lpr_drm/client.c"
  "${LPR_RUNTIME}/lpr_block/client.c"
  "${LPR_RUNTIME}/lpr_input/client.c"
  "${LPR_RUNTIME}/lpr_audio/client.c"
  "${LPR_RUNTIME}/lpr_process/bootstrap_state.c"
  "${LPR_RUNTIME}/lpr_process/capability.c"
  "${LPR_RUNTIME}/lpr_process/credentials.c"
  "${LPR_RUNTIME}/lpr_process/compat.c"
  "${LPR_RUNTIME}/lpr_tty/runtime.c"
  "${LPR_RUNTIME}/lpr_fd/control.c"
  "${LPR_RUNTIME}/lpr_fd/device.c"
  "${LPR_RUNTIME}/lpr_fd/event.c"
  "${LPR_RUNTIME}/lpr_fd/memfd.c"
  "${LPR_RUNTIME}/lpr_fd/ops.c"
  "${LPR_RUNTIME}/lpr_fd/dup_pipe.c"
  "${LPR_RUNTIME}/lpr_pipe/io.c"
  "${LPR_RUNTIME}/lpr_timerfd.c"
  "${LPR_RUNTIME}/lpr_inotify.c"
  "${LPR_RUNTIME}/lpr_signalfd.c"
  "${LPR_RUNTIME}/lpr_sync_file.c"
  "${LPR_RUNTIME}/lpr_wait.c"
  "${LPR_RUNTIME}/lpr_vfs/path.c"
  "${LPR_RUNTIME}/lpr_vfs/rpc.c"
  "${LPR_RUNTIME}/lpr_vfs/control_cache.c"
  "${LPR_RUNTIME}/lpr_vfs/session.c"
  "${REPO_ROOT}/userland/kobox2_owner/fs_channel.c"
  "${REPO_ROOT}/kobox2/protocol/src/filesystem.c"
  "${REPO_ROOT}/kobox2/protocol/src/protocol.c"
  "${REPO_ROOT}/kobox2/protocol/src/virtqueue.c"
  "${REPO_ROOT}/kobox2/protocol/src/virtqueue_memory.c"
  "${REPO_ROOT}/kobox2/protocol/arch/x86_64/virtqueue_atomic.c"
  "${LPR_RUNTIME}/lpr_vfs/io.c"
  "${LPR_RUNTIME}/lpr_vfs/proc.c"
  "${LPR_RUNTIME}/lpr_vfs/ops.c"
  "${LPR_RUNTIME}/lpr_process/exec.c"
  "${LPR_RUNTIME}/lpr_process/syscalls.c"
  "${LPR_RUNTIME}/lpr_entry_stack.c"
  "${LPR_RUNTIME}/lpr_fd/metadata.c"
  "${LPR_RUNTIME}/lpr_epoll.c"
  "${LPR_RUNTIME}/lpr_socket.c"
  "${LPR_RUNTIME}/lpr_dispatch.c"
  "${REPO_ROOT}/userland/libipc/src/status.c"
  "${REPO_ROOT}/userland/libpacha/src/trace.c"
  "${LPR_RUNTIME}/support/arena.c"
  "${LPR_RUNTIME}/support/elf.c"
  "${LPR_RUNTIME}/support/string.c"
  "${LPR_RUNTIME}/support/syscall.c"
  "${LPR_RUNTIME}/lpr_entry.S"
  $<TARGET_OBJECTS:lpr_scanner>
  $<TARGET_OBJECTS:lpr_patch_scan>
  $<TARGET_OBJECTS:lpr_zydis>)
lpr_zydis_options(lpr_runtime ${LPR_WARNING_OPTIONS} ${LPR_FRAME_OPTIONS} ${LPR_EXTRA_CFLAGS})
target_include_directories(lpr_runtime PRIVATE
  "${REPO_ROOT}/userland/personality/include"
  "${REPO_ROOT}/musl/pachaos/include"
  "${REPO_ROOT}/userland/libpacha/include"
  "${REPO_ROOT}/userland/libipc/include"
  "${REPO_ROOT}/userland/filed/include"
  "${REPO_ROOT}/userland/lpr_supervisor/include"
  "${REPO_ROOT}/userland/netd/include"
  "${REPO_ROOT}/userland/unixd/include"
  "${REPO_ROOT}/userland/termd/include"
  "${REPO_ROOT}/userland/gpud/include"
  "${REPO_ROOT}/userland/inputd/include"
  "${REPO_ROOT}/userland"
  "${REPO_ROOT}/kobox2/protocol/include"
  "${REPO_ROOT}/kobox2/protocol/generated/include")
set(LPR_NAMESPACE_MAP "${LPR_RUNTIME}/lpr_namespace.map")
set_target_properties(lpr_runtime PROPERTIES
  OUTPUT_NAME "lpr-linux-x86_64"
  DEFINE_SYMBOL ""
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
  NO_SONAME ON
  LINK_DEPENDS "${LPR_NAMESPACE_MAP}")
target_link_options(lpr_runtime PRIVATE
  ${LPR_COMMON_OPTIONS}
  ${LPR_FRAME_OPTIONS}
  ${LPR_EXTRA_CFLAGS}
  -nostdlib
  -fuse-ld=lld
  -Wl,--no-undefined
  -Wl,-Bsymbolic
  -Wl,-Bsymbolic-functions
  "-Wl,--version-script=${LPR_NAMESPACE_MAP}"
  -Wl,-soname,lpr-linux-x86_64.so)
# A runtime that leaks internals into the guest Linux namespace is not built.
add_custom_command(TARGET lpr_runtime POST_BUILD
  COMMAND bash "${LPR_DIR}/check-lpr-namespace.sh" "$<TARGET_FILE:lpr_runtime>"
  VERBATIM)
