cmake_minimum_required(VERSION 3.16)
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/pachaos-native.toolchain.cmake")
endif()
project(logd C ASM)

set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
include("${REPO_ROOT}/userland/cmake/pachaos_native.cmake")

pachaos_native_executable(logd
  SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/main.c"
    "${REPO_ROOT}/userland/libipc/src/ipc.c"
    "${REPO_ROOT}/userland/libipc/src/status.c"
    "${REPO_ROOT}/userland/libpacha/src/syscall.c"
    "${REPO_ROOT}/userland/libfiled_client/src/client.c"
    "${REPO_ROOT}/kobox2/protocol/src/filesystem.c"
  INCLUDE_DIRECTORIES
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${REPO_ROOT}/userland/filed/include"
    "${REPO_ROOT}/userland/libipc/include"
    "${REPO_ROOT}/userland/libpacha/include"
    "${REPO_ROOT}/userland/libfiled_client/include"
    "${REPO_ROOT}/kobox2/protocol/include"
    "${REPO_ROOT}/kobox2/protocol/generated/include")
