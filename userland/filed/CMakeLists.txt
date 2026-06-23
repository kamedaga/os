cmake_minimum_required(VERSION 3.16)

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

project(filed C ASM)

set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(FILED_ELF "${CMAKE_BINARY_DIR}/filed.elf")
set(FILED_EXTRA_SOURCES
  "${CMAKE_CURRENT_SOURCE_DIR}/src/vfs.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/dispatch.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/exec.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/fd_ipc.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/kobox_backend.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime.c"
  "${REPO_ROOT}/userland/libipc/src/ipc.c"
  "${REPO_ROOT}/userland/libpacha/src/syscall.c"
)
string(REPLACE ";" " " FILED_EXTRA_SOURCES_ENV "${FILED_EXTRA_SOURCES}")

add_custom_command(
  OUTPUT "${FILED_ELF}"
  COMMAND
    ${CMAKE_COMMAND} -E env
      "PACHAOS_MUSL_APP_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/src/main.c"
      "PACHAOS_MUSL_EXTRA_SOURCES=${FILED_EXTRA_SOURCES_ENV}"
      "PACHAOS_MUSL_EXTRA_INCLUDE_DIRS=${CMAKE_CURRENT_SOURCE_DIR}/include ${REPO_ROOT}/userland/koboxd/include ${REPO_ROOT}/userland/libipc/include ${REPO_ROOT}/userland/libpacha/include"
      bash "${REPO_ROOT}/musl/pachaos/build/build-smokes.sh" "${FILED_ELF}"
  DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/filed/bootstrap.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/filed/dispatch.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/filed/exec.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/filed/fd_ipc.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/filed/ipc_protocol.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/filed/kobox_backend.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/filed/runtime.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/filed/vfs.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/main.c"
    ${FILED_EXTRA_SOURCES}
    "${REPO_ROOT}/userland/koboxd/include/koboxd/ipc_protocol.h"
    "${REPO_ROOT}/musl/pachaos/build/build-smokes.sh"
  WORKING_DIRECTORY "${REPO_ROOT}"
  VERBATIM
)

add_custom_target(filed ALL DEPENDS "${FILED_ELF}")
