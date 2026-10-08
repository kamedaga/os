# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.16)
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
project(native_fs_smoke C)
set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
include("${REPO_ROOT}/userland/cmake/pachaos_native_dependencies.cmake")
set(NATIVE_FS_SMOKE_ELF "${CMAKE_BINARY_DIR}/native_fs_smoke.elf")
add_custom_command(
  OUTPUT "${NATIVE_FS_SMOKE_ELF}"
  COMMAND ${CMAKE_COMMAND} -E env
    "PACHAOS_MUSL_APP_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/src/main.c"
    "PACHAOS_MUSL_EXTRA_CFLAGS=-D__pachaos__"
    bash "${REPO_ROOT}/musl/pachaos/build/build-app-dynamic.sh" "${NATIVE_FS_SMOKE_ELF}"
  DEPENDS ${PACHAOS_NATIVE_DEPENDENCIES} "${CMAKE_CURRENT_SOURCE_DIR}/src/main.c"
  WORKING_DIRECTORY "${REPO_ROOT}" VERBATIM)
add_custom_target(native_fs_smoke ALL DEPENDS "${NATIVE_FS_SMOKE_ELF}")
