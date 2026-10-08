cmake_minimum_required(VERSION 3.16)
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
project(logd C ASM)

set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
include("${REPO_ROOT}/userland/cmake/pachaos_native_dependencies.cmake")
set(LOGD_ELF "${CMAKE_BINARY_DIR}/logd.elf")
add_custom_command(
  OUTPUT "${LOGD_ELF}"
  COMMAND
    ${CMAKE_COMMAND} -E env
      "PACHAOS_MUSL_APP_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/src/main.c"
      "PACHAOS_MUSL_EXTRA_SOURCES=${REPO_ROOT}/userland/libipc/src/ipc.c ${REPO_ROOT}/userland/libipc/src/status.c ${REPO_ROOT}/userland/libpacha/src/syscall.c ${REPO_ROOT}/userland/libfiled_client/src/client.c ${REPO_ROOT}/kobox2/protocol/src/filesystem.c"
      "PACHAOS_MUSL_EXTRA_INCLUDE_DIRS=${CMAKE_CURRENT_SOURCE_DIR}/include ${REPO_ROOT}/userland/filed/include ${REPO_ROOT}/userland/libipc/include ${REPO_ROOT}/userland/libpacha/include ${REPO_ROOT}/userland/libfiled_client/include ${REPO_ROOT}/kobox2/protocol/include ${REPO_ROOT}/kobox2/protocol/generated/include"
      bash "${REPO_ROOT}/musl/pachaos/build/build-smokes.sh" "${LOGD_ELF}"
  DEPENDS ${PACHAOS_NATIVE_DEPENDENCIES}
    "${REPO_ROOT}/userland/libfiled_client/src/client.c"
    "${REPO_ROOT}/userland/libfiled_client/include/filed_client/client.h"
    "${REPO_ROOT}/kobox2/protocol/src/filesystem.c"
    "${REPO_ROOT}/kobox2/protocol/include/kobox2/filesystem.h"
    "${REPO_ROOT}/kobox2/protocol/generated/include/kobox2/filesystem_layout.h"
    "${REPO_ROOT}/userland/libpacha/include/pacha/abi.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/main.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/logd/boot_config.h"
    "${REPO_ROOT}/userland/libpacha/include/pacha/log.h"
    "${REPO_ROOT}/userland/filed/include/filed/payload.h"
    "${REPO_ROOT}/userland/libipc/src/ipc.c"
    "${REPO_ROOT}/userland/libipc/src/status.c"
    "${REPO_ROOT}/userland/libpacha/src/syscall.c"
  WORKING_DIRECTORY "${REPO_ROOT}"
  VERBATIM
)
add_custom_target(logd ALL DEPENDS "${LOGD_ELF}")
