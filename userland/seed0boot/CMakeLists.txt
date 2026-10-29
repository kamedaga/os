cmake_minimum_required(VERSION 3.16)
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/pachaos-native.toolchain.cmake")
endif()

project(seed0boot C ASM)

set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
include("${REPO_ROOT}/userland/cmake/pachaos_native.cmake")

pachaos_native_executable(seed0boot
  SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/main.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/bootfs_reader.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/next_stage_loader.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/pachaos_capsule_launcher.c"
    "${REPO_ROOT}/userland/libipc/src/ipc.c"
    "${REPO_ROOT}/userland/libcapsule/src/capsule.c"
    "${REPO_ROOT}/userland/libpacha/src/syscall.c"
  INCLUDE_DIRECTORIES
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${REPO_ROOT}/userland/storage_boot/include"
    "${REPO_ROOT}/userland/filed/include"
    "${REPO_ROOT}/userland/libipc/include"
    "${REPO_ROOT}/userland/libcapsule/include"
    "${REPO_ROOT}/userland/libpacha/include")
