cmake_minimum_required(VERSION 3.16)

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
project(usbd C ASM)

set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(GPUD "${REPO_ROOT}/userland/gpud")
set(PROTOCOL "${REPO_ROOT}/kobox2/protocol")
set(USBD_ELF "${CMAKE_BINARY_DIR}/usbd.elf")

# The controller/process/lifecycle helpers have no GPU dependency; the USB
# binary deliberately omits gpud's DRM, queue, and GPU-session sources.
set(USBD_SOURCES
  "${CMAKE_CURRENT_SOURCE_DIR}/package.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/input_bridge.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/source_client.c"
  "${GPUD}/launch.c"
  "${GPUD}/launch_native.c"
  "${GPUD}/arch/x86_64/launch_image.c"
  "${GPUD}/bootstrap.c"
  "${GPUD}/process.c"
  "${GPUD}/process_native.c"
  "${GPUD}/lifecycle.c"
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
  "${PROTOCOL}/src/sha256.c")
string(REPLACE ";" " " USBD_SOURCES_ENV "${USBD_SOURCES}")
file(GLOB_RECURSE USBD_HEADERS CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/*.h" "${GPUD}/*.h"
  "${REPO_ROOT}/userland/kobox2_adapter/*.h")

add_custom_command(
  OUTPUT "${USBD_ELF}"
  COMMAND ${CMAKE_COMMAND} -E env
    "PACHAOS_MUSL_APP_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/main.c"
    "PACHAOS_MUSL_EXTRA_SOURCES=${USBD_SOURCES_ENV}"
    "PACHAOS_MUSL_EXTRA_INCLUDE_DIRS=${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/include ${REPO_ROOT}/userland/inputd/include ${GPUD} ${REPO_ROOT}/userland/filed/include ${REPO_ROOT}/userland/libfiled_client/include ${REPO_ROOT}/userland/libipc/include ${REPO_ROOT}/userland/libcapsule/include ${REPO_ROOT}/userland/libpacha/include ${REPO_ROOT}/kobox2/include ${REPO_ROOT}/kobox2/linux-sandbox/kobox ${PROTOCOL}/include ${PROTOCOL}/generated/include"
    "PACHAOS_MUSL_EXTRA_CFLAGS=-std=c11 -Oz -ffunction-sections -fdata-sections -Wframe-larger-than=65536 -Werror=frame-larger-than"
    "PACHAOS_MUSL_EXTRA_LINK_FLAGS=-Wl,--gc-sections"
    bash "${REPO_ROOT}/musl/pachaos/build/build-smokes.sh" "${USBD_ELF}"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/main.c" ${USBD_SOURCES} ${USBD_HEADERS}
    "${REPO_ROOT}/userland/libpacha/include/pacha/abi.h"
    "${REPO_ROOT}/musl/pachaos/build/build-smokes.sh"
    "${REPO_ROOT}/musl/upstream/arch/pachaos/syscall_arch.h"
  WORKING_DIRECTORY "${REPO_ROOT}"
  VERBATIM)
add_custom_target(usbd ALL DEPENDS "${USBD_ELF}")
