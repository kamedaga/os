cmake_minimum_required(VERSION 3.16)
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/pachaos-native.toolchain.cmake")
endif()
project(usbd C ASM)

set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
include("${REPO_ROOT}/userland/cmake/pachaos_native.cmake")
set(GPUD "${REPO_ROOT}/userland/gpud")
set(PROTOCOL "${REPO_ROOT}/kobox2/protocol")

# The controller/process/lifecycle helpers have no GPU dependency; the USB
# binary deliberately omits gpud's DRM, queue, and GPU-session sources.
set(USBD_SOURCES
  "${REPO_ROOT}/userland/kobox2_owner/boot.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/package.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/input_bridge.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/source_client.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/media_probe.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/install_port.c"
  "${REPO_ROOT}/userland/installer/gpt.c"
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
pachaos_native_executable(usbd
  SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/main.c"
    ${USBD_SOURCES}
    "${REPO_ROOT}/userland/libfiled_client/src/client.c"
    "${REPO_ROOT}/kobox2/protocol/src/filesystem.c"
  INCLUDE_DIRECTORIES
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${REPO_ROOT}/userland/inputd/include"
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
