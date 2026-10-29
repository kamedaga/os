cmake_minimum_required(VERSION 3.16)
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/pachaos-native.toolchain.cmake")
endif()
# Depfile paths are rewritten relative to the build tree for Ninja.
cmake_policy(SET CMP0116 NEW)
project(gpud C ASM)

option(GPUD_DRM_RPC_PROFILE "Measure native DRM request mapping costs" OFF)
set(GPUD_DRM_PROFILE_FLAGS "")
if(GPUD_DRM_RPC_PROFILE)
  set(GPUD_DRM_PROFILE_FLAGS "-DGPUD_DRM_RPC_PROFILE=1")
endif()

set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
include("${REPO_ROOT}/userland/cmake/pachaos_native.cmake")
set(SANDBOX_ELF "${CMAKE_BINARY_DIR}/sandbox.elf")
set(PROTOCOL "${REPO_ROOT}/kobox2/protocol")

add_custom_command(
  OUTPUT "${SANDBOX_ELF}"
  DEPFILE "${SANDBOX_ELF}.d"
  COMMAND bash "${REPO_ROOT}/userland/kobox2_adapter/build-sandbox.sh" "${SANDBOX_ELF}"
  DEPENDS "${REPO_ROOT}/userland/kobox2_adapter/build-sandbox.sh"
  WORKING_DIRECTORY "${REPO_ROOT}"
  VERBATIM)
add_custom_target(gpud_sandbox ALL DEPENDS "${SANDBOX_ELF}")

set(GPUD_SOURCES
  "${REPO_ROOT}/userland/kobox2_owner/boot.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/discovery.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/discovery_filed.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/inspect_policy.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/package.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/launch.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/launch_native.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/arch/x86_64/launch_image.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/bootstrap.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/process.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/process_native.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/lifecycle.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/gpu_channel.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/gpu_rpc.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/drm_files.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/drm_control.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/drm_translate.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/drm_reply.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/drm_service.c"
  "${REPO_ROOT}/userland/kobox2_adapter/ipc.c"
  "${REPO_ROOT}/userland/kobox2_adapter/bootstrap.c"
  "${REPO_ROOT}/userland/libfiled_client/src/module_image.c"
  "${REPO_ROOT}/userland/libipc/src/ipc.c"
  "${REPO_ROOT}/userland/libipc/src/status.c"
  "${REPO_ROOT}/userland/libcapsule/src/capsule.c"
  "${REPO_ROOT}/userland/libpacha/src/syscall.c"
  "${REPO_ROOT}/userland/libpacha/src/log.c"
  "${REPO_ROOT}/kobox2/src/controller/controller.c"
  "${REPO_ROOT}/kobox2/src/closure.c"
  "${PROTOCOL}/src/closure_manifest.c"
  "${PROTOCOL}/src/resource_grant.c"
  "${PROTOCOL}/src/sha256.c"
  "${PROTOCOL}/src/gpu_session.c"
  "${PROTOCOL}/src/gpu.c"
  "${PROTOCOL}/src/gpu_completion.c"
  "${PROTOCOL}/src/protocol.c"
  "${PROTOCOL}/src/virtqueue.c"
  "${PROTOCOL}/src/virtqueue_memory.c"
  "${PROTOCOL}/arch/x86_64/virtqueue_atomic.c")
pachaos_native_executable(gpud
  SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/main.c"
    ${GPUD_SOURCES}
    "${REPO_ROOT}/userland/libfiled_client/src/client.c"
    "${PROTOCOL}/src/filesystem.c"
  INCLUDE_DIRECTORIES
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${REPO_ROOT}/userland/filed/include"
    "${REPO_ROOT}/userland/libfiled_client/include"
    "${REPO_ROOT}/userland/libipc/include"
    "${REPO_ROOT}/userland/libcapsule/include"
    "${REPO_ROOT}/userland/libpacha/include"
    "${REPO_ROOT}/kobox2/include"
    "${PROTOCOL}/include"
    "${PROTOCOL}/generated/include"
  COMPILE_OPTIONS
    -std=c11
    -Oz
    -ffunction-sections
    -fdata-sections
    -Wframe-larger-than=65536
    -Werror=frame-larger-than
    ${GPUD_DRM_PROFILE_FLAGS}
  LINK_OPTIONS
    -Wl,--gc-sections)
# Building the gpud target delivers both ELFs.
add_dependencies(gpud gpud_sandbox)
