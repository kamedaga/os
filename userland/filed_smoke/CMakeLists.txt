cmake_minimum_required(VERSION 3.16)

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

project(filed_smoke C ASM)

set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(FILED_SMOKE_ELF "${CMAKE_BINARY_DIR}/filed_smoke.elf")
set(FILED_SMOKE_EXTRA_SOURCES
  "${REPO_ROOT}/userland/libipc/src/ipc.c"
  "${REPO_ROOT}/userland/libpacha/src/syscall.c"
)
string(REPLACE ";" " " FILED_SMOKE_EXTRA_SOURCES_ENV "${FILED_SMOKE_EXTRA_SOURCES}")

add_custom_command(
  OUTPUT "${FILED_SMOKE_ELF}"
  COMMAND
    ${CMAKE_COMMAND} -E env
      "PACHAOS_MUSL_APP_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/src/main.c"
      "PACHAOS_MUSL_EXTRA_SOURCES=${FILED_SMOKE_EXTRA_SOURCES_ENV}"
      "PACHAOS_MUSL_EXTRA_INCLUDE_DIRS=${CMAKE_CURRENT_SOURCE_DIR}/include ${REPO_ROOT}/userland/filed/include ${REPO_ROOT}/userland/libipc/include ${REPO_ROOT}/userland/libpacha/include"
      bash "${REPO_ROOT}/musl/pachaos/build/build-smokes.sh" "${FILED_SMOKE_ELF}"
  DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/filed_smoke/bootstrap.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/main.c"
    "${REPO_ROOT}/userland/filed/include/filed/ipc_protocol.h"
    "${REPO_ROOT}/userland/libipc/src/ipc.c"
    "${REPO_ROOT}/userland/libpacha/src/syscall.c"
    "${REPO_ROOT}/musl/pachaos/build/build-smokes.sh"
  WORKING_DIRECTORY "${REPO_ROOT}"
  VERBATIM
)

add_custom_target(filed_smoke ALL DEPENDS "${FILED_SMOKE_ELF}")
