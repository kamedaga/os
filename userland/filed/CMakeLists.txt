cmake_minimum_required(VERSION 3.16)
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/pachaos-native.toolchain.cmake")
endif()
# Depfile paths are rewritten relative to the build tree for Ninja.
cmake_policy(SET CMP0116 NEW)
project(filed C ASM)
set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
include("${REPO_ROOT}/userland/cmake/pachaos_native.cmake")
set(PROTOCOL "${REPO_ROOT}/kobox2/protocol")
set(FILED_SANDBOX_ELF "${CMAKE_BINARY_DIR}/sandbox.elf")
# Written by pack/scripts/download_zydis.sh (the zydis_source pacgo recipe).
set(ZYDIS_SOURCE "${REPO_ROOT}/.artifacts/third_party/zydis-v4.1.1/tree")
separate_arguments(FILED_PROFILE_CFLAGS UNIX_COMMAND "$ENV{PACHAOS_FILED_EXTRA_CFLAGS}")
set(FILED_EXTRA_SOURCES
  "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/live_root.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/storage.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/storage_owner.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/relay.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/snapshot_cache.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/pager.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/native_thread.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/manifest.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/manifest_storage.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/exec_payload.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/dispatch/identity.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/dispatch/unix_path.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/exec/router.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/exec/linux_lpr/elf.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/exec/linux_lpr/image.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/exec/linux_lpr/map.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/exec/linux_lpr/plan.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/exec/linux_lpr/script.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/exec/linux_lpr/start.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/exec/native/native.c"
  "${REPO_ROOT}/userland/kobox2_owner/boot.c"
  "${REPO_ROOT}/userland/kobox2_owner/bootfs.c"
  "${REPO_ROOT}/userland/kobox2_owner/fs_channel.c"
  "${REPO_ROOT}/userland/gpud/launch.c"
  "${REPO_ROOT}/userland/gpud/launch_native.c"
  "${REPO_ROOT}/userland/gpud/arch/x86_64/launch_image.c"
  "${REPO_ROOT}/userland/gpud/bootstrap.c"
  "${REPO_ROOT}/userland/gpud/process.c"
  "${REPO_ROOT}/userland/gpud/process_native.c"
  "${REPO_ROOT}/userland/gpud/lifecycle.c"
  "${REPO_ROOT}/userland/kobox2_adapter/ipc.c"
  "${REPO_ROOT}/userland/kobox2_adapter/bootstrap.c"
  "${REPO_ROOT}/kobox2/src/controller/controller.c"
  "${REPO_ROOT}/kobox2/src/closure.c"
  "${PROTOCOL}/src/closure_manifest.c"
  "${PROTOCOL}/src/resource_grant.c"
  "${PROTOCOL}/src/sha256.c"
  "${PROTOCOL}/src/filesystem.c"
  "${PROTOCOL}/src/protocol.c"
  "${PROTOCOL}/src/virtqueue.c"
  "${PROTOCOL}/src/virtqueue_memory.c"
  "${PROTOCOL}/arch/x86_64/virtqueue_atomic.c"
  "${REPO_ROOT}/userland/libipc/src/ipc.c"
  "${REPO_ROOT}/userland/libipc/src/status.c"
  "${REPO_ROOT}/userland/libcapsule/src/capsule.c"
  "${REPO_ROOT}/userland/libpacha/src/syscall.c"
  "${REPO_ROOT}/userland/libpacha/src/trace.c"
  "${REPO_ROOT}/userland/personality/linux/decoder/patch_scan.c"
  "${ZYDIS_SOURCE}/src/Decoder.c"
  "${ZYDIS_SOURCE}/src/DecoderData.c"
  "${ZYDIS_SOURCE}/src/SharedData.c")
pachaos_native_executable(filed
  SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/main.c"
    ${FILED_EXTRA_SOURCES}
  INCLUDE_DIRECTORIES
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${REPO_ROOT}/musl/upstream/src/internal"
    "${REPO_ROOT}/musl/upstream/src/include"
    "${REPO_ROOT}/musl/upstream/arch/pachaos"
    "${REPO_ROOT}/userland/storage/include"
    "${REPO_ROOT}/userland/libipc/include"
    "${REPO_ROOT}/userland/libcapsule/include"
    "${REPO_ROOT}/userland/libpacha/include"
    "${REPO_ROOT}/userland/personality/include"
    "${REPO_ROOT}/userland/personality/linux/decoder"
    "${REPO_ROOT}/userland/gpud"
    "${REPO_ROOT}/kobox2/include"
    "${PROTOCOL}/include"
    "${PROTOCOL}/generated/include"
    "${ZYDIS_SOURCE}/include"
    "${ZYDIS_SOURCE}/src"
    "${ZYDIS_SOURCE}/dependencies/zycore/include"
  COMPILE_OPTIONS
    -std=c11
    -D__pachaos__
    -DZYDIS_STATIC_BUILD
    -DZYDIS_MINIMAL_MODE
    -DZYDIS_DISABLE_ENCODER
    -DZYDIS_DISABLE_FORMATTER
    -DZYDIS_DISABLE_SEGMENT
    -DZYAN_NO_LIBC
    -Wno-unused-parameter
    -Wno-deprecated-non-prototype
    ${FILED_PROFILE_CFLAGS})
add_custom_command(OUTPUT "${FILED_SANDBOX_ELF}"
  DEPFILE "${FILED_SANDBOX_ELF}.d"
  COMMAND bash "${REPO_ROOT}/userland/kobox2_adapter/build-sandbox.sh"
    "${FILED_SANDBOX_ELF}" --storage
  DEPENDS "${REPO_ROOT}/userland/kobox2_adapter/build-sandbox.sh"
  WORKING_DIRECTORY "${REPO_ROOT}" VERBATIM)
add_custom_target(filed_sandbox ALL DEPENDS "${FILED_SANDBOX_ELF}")
# Building the filed target delivers both ELFs.
add_dependencies(filed filed_sandbox)
