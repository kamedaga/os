cmake_minimum_required(VERSION 3.16)

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

project(storage_boot C ASM)

set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
include("${REPO_ROOT}/userland/cmake/pachaos_native_dependencies.cmake")
set(STORAGE_BOOT_ELF "${CMAKE_BINARY_DIR}/storage_boot.elf")

set(STORAGE_BOOT_RUNTIME_SOURCES
  ${REPO_ROOT}/userland/libipc/src/ipc.c
  ${REPO_ROOT}/userland/libpacha/src/syscall.c
  ${REPO_ROOT}/userland/kobox2_owner/bootfs.c
)

string(REPLACE ";" " " STORAGE_BOOT_RUNTIME_SOURCES_ENV "${STORAGE_BOOT_RUNTIME_SOURCES}")

add_custom_command(
  OUTPUT "${STORAGE_BOOT_ELF}"
  COMMAND
    ${CMAKE_COMMAND} -E env
      "PACHAOS_MUSL_APP_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/src/main.c"
      "PACHAOS_MUSL_EXTRA_SOURCES=${STORAGE_BOOT_RUNTIME_SOURCES_ENV}"
      "PACHAOS_MUSL_EXTRA_INCLUDE_DIRS=${CMAKE_CURRENT_SOURCE_DIR}/include ${REPO_ROOT}/userland/storage/include ${REPO_ROOT}/userland/libipc/include ${REPO_ROOT}/userland/libpacha/include"
      "PACHAOS_MUSL_EXTRA_CFLAGS=-D__pachaos__"
      bash "${REPO_ROOT}/musl/pachaos/build/build-smokes.sh" "${STORAGE_BOOT_ELF}"
  DEPENDS ${PACHAOS_NATIVE_DEPENDENCIES}
    "${CMAKE_CURRENT_SOURCE_DIR}/src/main.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/storage_boot/boot_config.h"
    "${REPO_ROOT}/userland/kobox2_owner/bootfs.h"
    "${REPO_ROOT}/userland/storage/include/storage/bootstrap.h"
    "${REPO_ROOT}/userland/libipc/include/pacha/ipc.h"
    "${REPO_ROOT}/userland/libipc/include/pacha/launch.h"
    "${REPO_ROOT}/musl/pachaos/build/build-smokes.sh"
    "${REPO_ROOT}/musl/pachaos/include/pachaos/abi.h"
    "${REPO_ROOT}/userland/libpacha/include/pacha/abi.h"
    ${STORAGE_BOOT_RUNTIME_SOURCES}
  WORKING_DIRECTORY "${REPO_ROOT}"
  VERBATIM
)

add_custom_target(storage_boot ALL DEPENDS "${STORAGE_BOOT_ELF}")
