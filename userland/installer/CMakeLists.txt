cmake_minimum_required(VERSION 3.16)
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/pachaos-native.toolchain.cmake")
endif()
project(installerd C ASM)

set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
include("${REPO_ROOT}/userland/cmake/pachaos_native.cmake")

pachaos_native_executable(installerd
  SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/main.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/block_transport.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/catalog.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/disk_port.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/execute.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/ext4_probe.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/fat32_probe.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/gpt.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/gpt_write.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/menu.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/plan.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/tty.c"
    "${REPO_ROOT}/userland/libipc/src/ipc.c"
    "${REPO_ROOT}/userland/libipc/src/status.c"
    "${REPO_ROOT}/userland/libpacha/src/syscall.c"
  INCLUDE_DIRECTORIES
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${REPO_ROOT}/kobox2/protocol/include"
    "${REPO_ROOT}/kobox2/protocol/generated/include"
    "${REPO_ROOT}/userland/libipc/include"
    "${REPO_ROOT}/userland/libpacha/include"
    "${REPO_ROOT}/userland/termd/include"
    "${REPO_ROOT}/userland/kobox2_adapter")
