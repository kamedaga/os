cmake_minimum_required(VERSION 3.16)
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
project(installerd C ASM)

set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
include("${REPO_ROOT}/userland/cmake/pachaos_native_dependencies.cmake")
set(INSTALLER_ELF "${CMAKE_BINARY_DIR}/installerd.elf")

add_custom_command(
  OUTPUT "${INSTALLER_ELF}"
  COMMAND ${CMAKE_COMMAND} -E env
    "PACHAOS_MUSL_APP_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/main.c"
    "PACHAOS_MUSL_EXTRA_SOURCES=${CMAKE_CURRENT_SOURCE_DIR}/block_transport.c ${CMAKE_CURRENT_SOURCE_DIR}/catalog.c ${CMAKE_CURRENT_SOURCE_DIR}/disk_port.c ${CMAKE_CURRENT_SOURCE_DIR}/execute.c ${CMAKE_CURRENT_SOURCE_DIR}/ext4_probe.c ${CMAKE_CURRENT_SOURCE_DIR}/fat32_probe.c ${CMAKE_CURRENT_SOURCE_DIR}/gpt.c ${CMAKE_CURRENT_SOURCE_DIR}/gpt_write.c ${CMAKE_CURRENT_SOURCE_DIR}/menu.c ${CMAKE_CURRENT_SOURCE_DIR}/plan.c ${CMAKE_CURRENT_SOURCE_DIR}/tty.c ${REPO_ROOT}/userland/libipc/src/ipc.c ${REPO_ROOT}/userland/libipc/src/status.c ${REPO_ROOT}/userland/libpacha/src/syscall.c"
    "PACHAOS_MUSL_EXTRA_INCLUDE_DIRS=${CMAKE_CURRENT_SOURCE_DIR} ${REPO_ROOT}/kobox2/protocol/include ${REPO_ROOT}/kobox2/protocol/generated/include ${REPO_ROOT}/userland/libipc/include ${REPO_ROOT}/userland/libpacha/include ${REPO_ROOT}/userland/termd/include ${REPO_ROOT}/userland/kobox2_adapter"
    bash "${REPO_ROOT}/musl/pachaos/build/build-smokes.sh" "${INSTALLER_ELF}"
  DEPENDS ${PACHAOS_NATIVE_DEPENDENCIES} main.c bootstrap.h block_transport.c block_transport.h catalog.c
    catalog.h disk_port.c disk_port.h execute.c execute.h ext4_probe.c
    ext4_probe.h fat32_probe.c fat32_probe.h gpt.c gpt.h gpt_write.c
    gpt_write.h menu.c menu.h plan.c plan.h tty.c tty.h
  WORKING_DIRECTORY "${REPO_ROOT}"
  VERBATIM)

add_custom_target(installerd ALL DEPENDS "${INSTALLER_ELF}")
