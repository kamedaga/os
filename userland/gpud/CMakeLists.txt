cmake_minimum_required(VERSION 3.16)

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
project(gpud C ASM)

option(GPUD_DRM_RPC_PROFILE "Measure native DRM request mapping costs" OFF)
set(GPUD_DRM_PROFILE_FLAGS "")
if(GPUD_DRM_RPC_PROFILE)
  set(GPUD_DRM_PROFILE_FLAGS "-DGPUD_DRM_RPC_PROFILE=1")
endif()

set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(GPUD_ELF "${CMAKE_BINARY_DIR}/gpud.elf")
set(SANDBOX_ELF "${CMAKE_BINARY_DIR}/sandbox.elf")
set(PROTOCOL "${REPO_ROOT}/kobox2/protocol")

file(GLOB_RECURSE SANDBOX_INPUTS CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/gpu_sessions.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/gpu_channel.c"
  "${REPO_ROOT}/userland/kobox2_adapter/*.c"
  "${REPO_ROOT}/userland/kobox2_adapter/*.h"
  "${REPO_ROOT}/userland/kobox2_adapter/*.S"
  "${REPO_ROOT}/userland/libpacha/include/*.h"
  "${REPO_ROOT}/userland/libpacha/src/syscall.c"
  "${REPO_ROOT}/kobox2/linux-sandbox/kobox/*.c"
  "${REPO_ROOT}/kobox2/linux-sandbox/kobox/*.h"
  "${PROTOCOL}/*.c" "${PROTOCOL}/*.h")
add_custom_command(
  OUTPUT "${SANDBOX_ELF}"
  COMMAND bash "${REPO_ROOT}/userland/kobox2_adapter/build-sandbox.sh" "${SANDBOX_ELF}"
  DEPENDS ${SANDBOX_INPUTS} "${REPO_ROOT}/userland/kobox2_adapter/build-sandbox.sh"
  WORKING_DIRECTORY "${REPO_ROOT}"
  VERBATIM)
add_custom_target(gpud_sandbox DEPENDS "${SANDBOX_ELF}")

set(GPUD_SOURCES
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
string(REPLACE ";" " " GPUD_SOURCES_ENV "${GPUD_SOURCES}")
file(GLOB_RECURSE GPUD_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.h")

add_custom_command(
  OUTPUT "${GPUD_ELF}"
  COMMAND ${CMAKE_COMMAND} -E env
    "PACHAOS_MUSL_APP_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/main.c"
    "PACHAOS_MUSL_EXTRA_SOURCES=${GPUD_SOURCES_ENV}"
    "PACHAOS_MUSL_EXTRA_INCLUDE_DIRS=${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/include ${REPO_ROOT}/userland/filed/include ${REPO_ROOT}/userland/libfiled_client/include ${REPO_ROOT}/userland/libipc/include ${REPO_ROOT}/userland/libcapsule/include ${REPO_ROOT}/userland/libpacha/include ${REPO_ROOT}/kobox2/include ${PROTOCOL}/include ${PROTOCOL}/generated/include"
    "PACHAOS_MUSL_EXTRA_CFLAGS=-std=c11 -Oz -ffunction-sections -fdata-sections -Wframe-larger-than=65536 -Werror=frame-larger-than ${GPUD_DRM_PROFILE_FLAGS}"
    "PACHAOS_MUSL_EXTRA_LINK_FLAGS=-Wl,--gc-sections"
    bash "${REPO_ROOT}/musl/pachaos/build/build-smokes.sh" "${GPUD_ELF}"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/main.c" ${GPUD_SOURCES} ${GPUD_HEADERS}
    "${REPO_ROOT}/userland/libpacha/include/pacha/abi.h"
    "${REPO_ROOT}/musl/pachaos/include/pachaos/abi.h"
    "${REPO_ROOT}/musl/upstream/arch/pachaos/syscall_arch.h"
    "${REPO_ROOT}/musl/pachaos/build/build-smokes.sh" "${SANDBOX_ELF}"
  WORKING_DIRECTORY "${REPO_ROOT}"
  VERBATIM)
add_custom_target(gpud ALL DEPENDS "${GPUD_ELF}" "${SANDBOX_ELF}")
