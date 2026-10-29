cmake_minimum_required(VERSION 3.16)
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/pachaos-native.toolchain.cmake")
endif()
project(unixd C)
option(UNIXD_PROFILE "Measure control request stages (test builds only)" OFF)
set(UNIXD_PROFILE_FLAG "")
if(UNIXD_PROFILE)
  set(UNIXD_PROFILE_FLAG "-DUNIXD_PROFILE=1")
endif()

set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
include("${REPO_ROOT}/userland/cmake/pachaos_native.cmake")

pachaos_native_executable(unixd
  SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/service.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/broker.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rights.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/escrow.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/transport.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/notify.c
    ${REPO_ROOT}/userland/libipc/src/ipc.c
    ${REPO_ROOT}/userland/libipc/src/status.c
    ${REPO_ROOT}/userland/libpacha/src/syscall.c
    ${REPO_ROOT}/userland/libpacha/src/trace.c
  INCLUDE_DIRECTORIES
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${REPO_ROOT}/userland/filed/include
    ${REPO_ROOT}/userland/libipc/include
    ${REPO_ROOT}/userland/libpacha/include
  COMPILE_OPTIONS
    -D__pachaos__
    ${UNIXD_PROFILE_FLAG})
