cmake_minimum_required(VERSION 3.16)

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

project(seed0_next C ASM)

set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(SEED0_NEXT_ELF "${CMAKE_BINARY_DIR}/seed0_next.elf")

add_custom_command(
  OUTPUT "${SEED0_NEXT_ELF}"
  COMMAND
    ${CMAKE_COMMAND} -E env
      "PACHAOS_MUSL_APP_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/src/main.c"
      bash "${REPO_ROOT}/musl/pachaos/build/build-smokes.sh" "${SEED0_NEXT_ELF}"
  DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/main.c"
    "${REPO_ROOT}/musl/pachaos/build/build-smokes.sh"
  WORKING_DIRECTORY "${REPO_ROOT}"
  VERBATIM
)

add_custom_target(seed0_next ALL DEPENDS "${SEED0_NEXT_ELF}")
