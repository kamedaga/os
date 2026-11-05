cmake_minimum_required(VERSION 3.16)
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/pachaos-native.toolchain.cmake")
endif()
project(klogtail C ASM)
set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
include("${REPO_ROOT}/userland/cmake/pachaos_native.cmake")
pachaos_native_executable(klogtail
  SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/main.c"
  INCLUDE_DIRECTORIES
    "${REPO_ROOT}/userland/libpacha/include"
    "${REPO_ROOT}/musl/pachaos/include")
