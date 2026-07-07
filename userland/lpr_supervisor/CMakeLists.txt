cmake_minimum_required(VERSION 3.16)

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

project(lpr_supervisor C ASM)

set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(LPR_SUPERVISOR_ELF "${CMAKE_BINARY_DIR}/lpr_supervisor.elf")

add_custom_command(
  OUTPUT "${LPR_SUPERVISOR_ELF}"
  COMMAND
    ${CMAKE_COMMAND} -E env
      "PACHAOS_MUSL_APP_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/src/main.c"
      "PACHAOS_MUSL_EXTRA_SOURCES=${REPO_ROOT}/userland/libipc/src/ipc.c ${REPO_ROOT}/userland/libipc/src/error_conveyor.c ${REPO_ROOT}/userland/libpacha/src/syscall.c"
      "PACHAOS_MUSL_EXTRA_INCLUDE_DIRS=${CMAKE_CURRENT_SOURCE_DIR}/include ${REPO_ROOT}/userland/libipc/include ${REPO_ROOT}/userland/libpacha/include"
      "PACHAOS_MUSL_EXTRA_CFLAGS=-D__pachaos__"
      bash "${REPO_ROOT}/musl/pachaos/build/build-smokes.sh" "${LPR_SUPERVISOR_ELF}"
  DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/main.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/lpr_supervisor/boot_config.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/lpr_supervisor/ipc_protocol.h"
    "${REPO_ROOT}/userland/libipc/src/ipc.c"
    "${REPO_ROOT}/userland/libipc/src/error_conveyor.c"
    "${REPO_ROOT}/userland/libipc/include/pacha/error_conveyor.h"
    "${REPO_ROOT}/userland/libpacha/src/syscall.c"
    "${REPO_ROOT}/musl/pachaos/build/build-smokes.sh"
  WORKING_DIRECTORY "${REPO_ROOT}"
  VERBATIM
)

add_custom_target(lpr_supervisor ALL DEPENDS "${LPR_SUPERVISOR_ELF}")
