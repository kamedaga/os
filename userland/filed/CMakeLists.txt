cmake_minimum_required(VERSION 3.16)
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
project(filed C ASM)
set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
include("${REPO_ROOT}/userland/cmake/pachaos_native_dependencies.cmake")
set(PROTOCOL "${REPO_ROOT}/kobox2/protocol")
set(FILED_ELF "${CMAKE_BINARY_DIR}/filed.elf")
set(FILED_SANDBOX_ELF "${CMAKE_BINARY_DIR}/sandbox.elf")
set(ZYDIS_SOURCE "${REPO_ROOT}/.artifacts/third_party/zydis-v4.1.1/source")
set(FILED_PROFILE_CFLAGS "$ENV{PACHAOS_FILED_EXTRA_CFLAGS}")
set(FILED_EXTRA_SOURCES
  "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/storage.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/storage_owner.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/relay.c"
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
string(REPLACE ";" " " FILED_EXTRA_SOURCES_ENV "${FILED_EXTRA_SOURCES}")
file(GLOB_RECURSE FILED_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/exec/*.h")
add_custom_command(OUTPUT "${FILED_ELF}"
  COMMAND bash "${REPO_ROOT}/pack/scripts/download_zydis.sh"
  COMMAND ${CMAKE_COMMAND} -E env
    "PACHAOS_MUSL_APP_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/src/main.c"
    "PACHAOS_MUSL_EXTRA_SOURCES=${FILED_EXTRA_SOURCES_ENV}"
    "PACHAOS_MUSL_EXTRA_INCLUDE_DIRS=${CMAKE_CURRENT_SOURCE_DIR}/include ${REPO_ROOT}/userland/storage/include ${REPO_ROOT}/userland/libipc/include ${REPO_ROOT}/userland/libcapsule/include ${REPO_ROOT}/userland/libpacha/include ${REPO_ROOT}/userland/personality/include ${REPO_ROOT}/userland/personality/linux/decoder ${REPO_ROOT}/userland/gpud ${REPO_ROOT}/kobox2/include ${PROTOCOL}/include ${PROTOCOL}/generated/include ${ZYDIS_SOURCE}/include ${ZYDIS_SOURCE}/src ${ZYDIS_SOURCE}/dependencies/zycore/include"
    "PACHAOS_MUSL_EXTRA_CFLAGS=-std=c11 -D__pachaos__ -DZYDIS_STATIC_BUILD -DZYDIS_MINIMAL_MODE -DZYDIS_DISABLE_ENCODER -DZYDIS_DISABLE_FORMATTER -DZYDIS_DISABLE_SEGMENT -DZYAN_NO_LIBC -Wno-unused-parameter -Wno-deprecated-non-prototype ${FILED_PROFILE_CFLAGS}"
    bash "${REPO_ROOT}/musl/pachaos/build/build-smokes.sh" "${FILED_ELF}"
  DEPENDS ${PACHAOS_NATIVE_DEPENDENCIES} "${CMAKE_CURRENT_SOURCE_DIR}/src/main.c" ${FILED_EXTRA_SOURCES} ${FILED_HEADERS}
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/internal_microbench.inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/internal_microbench_clock.h"
    "${REPO_ROOT}/musl/pachaos/build/build-smokes.sh"
    "${REPO_ROOT}/pack/scripts/download_zydis.sh"
  WORKING_DIRECTORY "${REPO_ROOT}" VERBATIM)
file(GLOB_RECURSE FILED_SANDBOX_INPUTS CONFIGURE_DEPENDS
  "${REPO_ROOT}/userland/kobox2_adapter/*.c"
  "${REPO_ROOT}/userland/kobox2_adapter/*.h"
  "${REPO_ROOT}/userland/kobox2_owner/*.c"
  "${REPO_ROOT}/userland/kobox2_owner/*.h"
  "${REPO_ROOT}/kobox2/linux-sandbox/kobox/*.c"
  "${REPO_ROOT}/kobox2/linux-sandbox/kobox/*.h"
  "${PROTOCOL}/*.c" "${PROTOCOL}/*.h")
add_custom_command(OUTPUT "${FILED_SANDBOX_ELF}"
  COMMAND bash "${REPO_ROOT}/userland/kobox2_adapter/build-sandbox.sh"
    "${FILED_SANDBOX_ELF}" --storage
  DEPENDS ${FILED_SANDBOX_INPUTS}
    "${REPO_ROOT}/userland/kobox2_adapter/build-sandbox.sh"
  WORKING_DIRECTORY "${REPO_ROOT}" VERBATIM)
add_custom_target(filed ALL DEPENDS "${FILED_ELF}" "${FILED_SANDBOX_ELF}")
