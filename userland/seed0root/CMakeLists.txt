cmake_minimum_required(VERSION 3.16)

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

project(seed0root C ASM)

set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(SEED0ROOT_ELF "${CMAKE_BINARY_DIR}/seed0root.elf")

add_custom_command(
  OUTPUT "${SEED0ROOT_ELF}"
  COMMAND
    ${CMAKE_COMMAND} -E env
      "PACHAOS_MUSL_APP_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/src/main.c"
      "PACHAOS_MUSL_EXTRA_SOURCES=${REPO_ROOT}/userland/libipc/src/ipc.c ${REPO_ROOT}/userland/libpacha/src/syscall.c"
      "PACHAOS_MUSL_EXTRA_INCLUDE_DIRS=${REPO_ROOT}/userland/filed/include ${REPO_ROOT}/userland/libipc/include ${REPO_ROOT}/userland/libpacha/include"
      bash "${REPO_ROOT}/musl/pachaos/build/build-smokes.sh" "${SEED0ROOT_ELF}"
  DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/main.c"
    "${REPO_ROOT}/userland/filed/include/filed/bootstrap.h"
    "${REPO_ROOT}/userland/filed/include/filed/ipc_protocol.h"
    "${REPO_ROOT}/userland/libipc/src/ipc.c"
    "${REPO_ROOT}/userland/libpacha/src/syscall.c"
    "${REPO_ROOT}/musl/pachaos/build/build-smokes.sh"
  WORKING_DIRECTORY "${REPO_ROOT}"
  VERBATIM
)

add_custom_target(seed0root ALL DEPENDS "${SEED0ROOT_ELF}")
