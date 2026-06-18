cmake_minimum_required(VERSION 3.16)

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

project(seed0boot C ASM)

set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(SEED0BOOT_ELF "${CMAKE_BINARY_DIR}/seed0boot.elf")

add_custom_command(
  OUTPUT "${SEED0BOOT_ELF}"
  COMMAND
    ${CMAKE_COMMAND} -E env
      "PACHAOS_MUSL_APP_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/src/main.c"
      "PACHAOS_MUSL_EXTRA_SOURCES=${CMAKE_CURRENT_SOURCE_DIR}/src/bootfs_reader.c ${CMAKE_CURRENT_SOURCE_DIR}/src/next_stage_loader.c ${CMAKE_CURRENT_SOURCE_DIR}/src/pachaos_capsule_launcher.c ${REPO_ROOT}/userland/libipc/src/ipc.c ${REPO_ROOT}/userland/libcapsule/src/capsule.c ${REPO_ROOT}/userland/libpacha/src/syscall.c"
      "PACHAOS_MUSL_EXTRA_INCLUDE_DIRS=${CMAKE_CURRENT_SOURCE_DIR}/src ${REPO_ROOT}/userland/pachaos_capsule/include ${REPO_ROOT}/userland/storage_boot/include ${REPO_ROOT}/userland/libipc/include ${REPO_ROOT}/userland/libcapsule/include ${REPO_ROOT}/userland/libpacha/include ${REPO_ROOT}/_kobox/include ${REPO_ROOT}/_kobox/src"
      bash "${REPO_ROOT}/musl/pachaos/build/build-smokes.sh" "${SEED0BOOT_ELF}"
  DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/main.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/bootstrap_abi.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/bootfs_reader.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/bootfs_reader.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/next_stage_loader.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/next_stage_loader.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/pachaos_capsule_launcher.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/pachaos_capsule_launcher.c"
    "${REPO_ROOT}/userland/pachaos_capsule/include/pachaos_capsule/boot_config.h"
    "${REPO_ROOT}/userland/storage_boot/include/storage_boot/boot_config.h"
    "${REPO_ROOT}/musl/pachaos/build/build-smokes.sh"
    "${REPO_ROOT}/userland/libipc/include/pacha/ipc.h"
    "${REPO_ROOT}/userland/libipc/src/ipc.c"
    "${REPO_ROOT}/userland/libcapsule/include/pacha/capsule.h"
    "${REPO_ROOT}/userland/libcapsule/src/capsule.c"
    "${REPO_ROOT}/userland/libpacha/include/pacha/syscall.h"
    "${REPO_ROOT}/userland/libpacha/src/syscall.c"
  WORKING_DIRECTORY "${REPO_ROOT}"
  VERBATIM
)

add_custom_target(seed0boot ALL DEPENDS "${SEED0BOOT_ELF}")
