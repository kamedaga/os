cmake_minimum_required(VERSION 3.16)
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
project(unixd C)
option(UNIXD_PROFILE "Measure control request stages (test builds only)" OFF)
set(UNIXD_PROFILE_FLAG "")
if(UNIXD_PROFILE)
  set(UNIXD_PROFILE_FLAG "-DUNIXD_PROFILE=1")
endif()

set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(UNIXD_ELF "${CMAKE_BINARY_DIR}/unixd.elf")
set(UNIXD_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/src/service.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/broker.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/rights.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/escrow.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/transport.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/notify.c
  ${REPO_ROOT}/userland/libipc/src/ipc.c
  ${REPO_ROOT}/userland/libipc/src/status.c
  ${REPO_ROOT}/userland/libpacha/src/syscall.c
  ${REPO_ROOT}/userland/libpacha/src/trace.c)
string(REPLACE ";" " " UNIXD_SOURCE_ENV "${UNIXD_SOURCES}")

add_custom_command(
  OUTPUT "${UNIXD_ELF}"
  COMMAND ${CMAKE_COMMAND} -E env
    "PACHAOS_MUSL_APP_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/src/main.c"
    "PACHAOS_MUSL_EXTRA_SOURCES=${UNIXD_SOURCE_ENV}"
    "PACHAOS_MUSL_EXTRA_INCLUDE_DIRS=${CMAKE_CURRENT_SOURCE_DIR}/include ${REPO_ROOT}/userland/filed/include ${REPO_ROOT}/userland/libipc/include ${REPO_ROOT}/userland/libpacha/include"
    "PACHAOS_MUSL_EXTRA_CFLAGS=-D__pachaos__ ${UNIXD_PROFILE_FLAG}"
    bash "${REPO_ROOT}/musl/pachaos/build/build-smokes.sh" "${UNIXD_ELF}"
  DEPENDS ${UNIXD_SOURCES}
    src/main.c src/service.h src/broker.h src/broker_internal.h src/rights.h src/escrow.h
    include/unixd/transfer.h
    ${REPO_ROOT}/userland/filed/include/filed/unix_path.h
    include/unixd/ipc_protocol.h include/unixd/transport.h include/unixd/notify.h
    ${REPO_ROOT}/userland/libipc/include/pacha/ipc.h
    ${REPO_ROOT}/userland/libpacha/include/pacha/abi.h
    ${REPO_ROOT}/musl/pachaos/build/build-smokes.sh
    ${REPO_ROOT}/musl/upstream/arch/pachaos/syscall_arch.h
  WORKING_DIRECTORY "${REPO_ROOT}"
  VERBATIM)
add_custom_target(unixd ALL DEPENDS "${UNIXD_ELF}")
