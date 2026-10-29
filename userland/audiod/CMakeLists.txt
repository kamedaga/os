cmake_minimum_required(VERSION 3.16)
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/pachaos-native.toolchain.cmake")
endif()
project(audiod C ASM)

set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
include("${REPO_ROOT}/userland/cmake/pachaos_native.cmake")
set(GPUD "${REPO_ROOT}/userland/gpud")
set(PROTOCOL "${REPO_ROOT}/kobox2/protocol")

# Reuse the device-neutral controller/process/lifecycle and PCI package
# helpers. Audio links no USB, network or GPU service implementation.
set(AUDIOD_SOURCES
  "${REPO_ROOT}/userland/kobox2_owner/boot.c"
  "${REPO_ROOT}/userland/usbd/package.c"
  "${GPUD}/launch.c"
  "${GPUD}/launch_native.c"
  "${GPUD}/arch/x86_64/launch_image.c"
  "${GPUD}/bootstrap.c"
  "${GPUD}/process.c"
  "${GPUD}/process_native.c"
  "${GPUD}/lifecycle.c"
  "${REPO_ROOT}/userland/kobox2_adapter/ipc.c"
  "${REPO_ROOT}/userland/kobox2_adapter/block_client.c"
  "${REPO_ROOT}/userland/kobox2_adapter/bootstrap.c"
  "${REPO_ROOT}/userland/libfiled_client/src/module_image.c"
  "${REPO_ROOT}/userland/libipc/src/ipc.c"
  "${REPO_ROOT}/userland/libipc/src/status.c"
  "${REPO_ROOT}/userland/libcapsule/src/capsule.c"
  "${REPO_ROOT}/userland/libpacha/src/syscall.c"
  "${REPO_ROOT}/kobox2/src/controller/controller.c"
  "${REPO_ROOT}/kobox2/src/closure.c"
  "${PROTOCOL}/src/closure_manifest.c"
  "${PROTOCOL}/src/block_device.c"
  "${PROTOCOL}/src/resource_grant.c"
  "${PROTOCOL}/src/sha256.c")
pachaos_native_executable(audiod
  SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/main.c"
    ${AUDIOD_SOURCES}
    "${REPO_ROOT}/userland/libfiled_client/src/client.c"
    "${REPO_ROOT}/kobox2/protocol/src/filesystem.c"
  INCLUDE_DIRECTORIES
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${REPO_ROOT}/userland/usbd/include"
    "${GPUD}"
    "${REPO_ROOT}/userland/filed/include"
    "${REPO_ROOT}/userland/libfiled_client/include"
    "${REPO_ROOT}/userland/libipc/include"
    "${REPO_ROOT}/userland/libcapsule/include"
    "${REPO_ROOT}/userland/libpacha/include"
    "${REPO_ROOT}/kobox2/include"
    "${REPO_ROOT}/kobox2/linux-sandbox/kobox"
    "${PROTOCOL}/include"
    "${PROTOCOL}/generated/include"
  COMPILE_OPTIONS
    -std=c11
    -Oz
    -ffunction-sections
    -fdata-sections
    -Wframe-larger-than=65536
    -Werror=frame-larger-than
  LINK_OPTIONS
    -Wl,--gc-sections)
