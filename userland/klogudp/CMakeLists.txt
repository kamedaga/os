cmake_minimum_required(VERSION 3.16)
set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/pachaos-native.toolchain.cmake")
project(klogudp C ASM)
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
include("${REPO_ROOT}/userland/cmake/pachaos_native.cmake")
pachaos_native_executable(klogudp SOURCES src/main.c
  "${REPO_ROOT}/userland/libipc/src/ipc.c"
  "${REPO_ROOT}/userland/libpacha/src/syscall.c"
  INCLUDE_DIRECTORIES "${REPO_ROOT}/userland/libpacha/include"
    "${REPO_ROOT}/userland/libipc/include" "${REPO_ROOT}/userland/netd/include")
