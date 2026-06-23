cmake_minimum_required(VERSION 3.16)

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

project(schedulerd C ASM)

set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(SCHEDULERD_ELF "${CMAKE_BINARY_DIR}/schedulerd.elf")

set(SCHEDULERD_RUNTIME_SOURCES
  ${REPO_ROOT}/verified/scheduling/src/pacha_eevdf.c
  ${REPO_ROOT}/verified/scheduling/src/pacha_sched.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/sched_loop.c
)

string(REPLACE ";" " " SCHEDULERD_RUNTIME_SOURCES_ENV "${SCHEDULERD_RUNTIME_SOURCES}")

add_custom_command(
  OUTPUT "${SCHEDULERD_ELF}"
  COMMAND
    ${CMAKE_COMMAND} -E env
      "PACHAOS_MUSL_APP_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/src/main.c"
      "PACHAOS_MUSL_EXTRA_SOURCES=${SCHEDULERD_RUNTIME_SOURCES_ENV}"
      "PACHAOS_MUSL_EXTRA_INCLUDE_DIRS=${CMAKE_CURRENT_SOURCE_DIR}/src ${REPO_ROOT}/verified/scheduling/include ${REPO_ROOT}/userland/libpacha/include"
      bash "${REPO_ROOT}/musl/pachaos/build/build-smokes.sh" "${SCHEDULERD_ELF}"
  DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/main.c"
    "${REPO_ROOT}/verified/scheduling/src/pacha_eevdf.c"
    "${REPO_ROOT}/verified/scheduling/src/pacha_sched.c"
    "${REPO_ROOT}/verified/scheduling/include/pacha_eevdf.h"
    "${REPO_ROOT}/verified/scheduling/include/pacha_sched.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/sched_loop.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/sched_loop.h"
    "${REPO_ROOT}/musl/pachaos/build/build-smokes.sh"
  WORKING_DIRECTORY "${REPO_ROOT}"
  VERBATIM
)

add_custom_target(schedulerd ALL DEPENDS "${SCHEDULERD_ELF}")
