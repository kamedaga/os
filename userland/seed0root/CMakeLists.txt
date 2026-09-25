cmake_minimum_required(VERSION 3.16)

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

project(seed0root C ASM)

set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(SEED0ROOT_ELF "${CMAKE_BINARY_DIR}/seed0root.elf")
set(LIVE_VTERM_BUILD "${REPO_ROOT}/.artifacts/third_party/libvterm-0.3.3/build")
set(LIVE_VTERM_SOURCES "")
foreach(VTERM_UNIT encoding keyboard mouse parser pen screen state unicode vterm)
  string(APPEND LIVE_VTERM_SOURCES " ${LIVE_VTERM_BUILD}/src/${VTERM_UNIT}.c")
endforeach()
option(SEED0ROOT_UNIXD_CONTRACT_TEST "Run native/UNIX probes with netd stopped (test image only)" OFF)
option(SEED0ROOT_LAUNCH_GRANT_TEST "Run native process-create grant transaction probe (test image only)" OFF)
option(SEED0ROOT_NATIVE_PRIMITIVES_TEST "Run native execution and clock primitives (test image only)" OFF)
option(SEED0ROOT_CREDENTIAL_TEST "Run Linux credential fork/exec probe (test image only)" OFF)
option(SEED0ROOT_ACCOUNT_LAUNCH_TEST "Direct service account and capability regression launches" OFF)
option(SEED0ROOT_REAL_SERVICE_TEST "Isolated D-Bus service-account acceptance launches" OFF)
option(SEED0ROOT_GPUD_RESTART_TEST "Force and verify one production gpud sandbox restart" OFF)
option(SEED0ROOT_LPR_THREAD_SIGNAL_TEST "Run LPR thread signals before GPU startup (test image only)" OFF)
option(SEED0ROOT_USB_HID_INPUT_TEST "Read QEMU USB HID events through inputd IPC (test image only)" OFF)
set(SEED0ROOT_TEST_ENV)
set(SEED0ROOT_TEST_CFLAGS "$ENV{PACHAOS_MUSL_EXTRA_CFLAGS}")
if(SEED0ROOT_LPR_THREAD_SIGNAL_TEST)
  string(APPEND SEED0ROOT_TEST_CFLAGS " -DSEED0ROOT_LPR_THREAD_SIGNAL_TEST=1")
endif()
if(SEED0ROOT_USB_HID_INPUT_TEST)
  string(APPEND SEED0ROOT_TEST_CFLAGS " -DSEED0ROOT_USB_HID_INPUT_TEST=1")
  set(SEED0ROOT_USB_HID_TEST_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/usb_input_smoke.c")
endif()
if(SEED0ROOT_REAL_SERVICE_TEST)
  string(APPEND SEED0ROOT_TEST_CFLAGS " -DSEED0ROOT_REAL_SERVICE_TEST=1")
endif()
if(SEED0ROOT_GPUD_RESTART_TEST)
  string(APPEND SEED0ROOT_TEST_CFLAGS " -DSEED0ROOT_GPUD_RESTART_TEST=1")
endif()
if(SEED0ROOT_ACCOUNT_LAUNCH_TEST)
  string(APPEND SEED0ROOT_TEST_CFLAGS " -DSEED0ROOT_ACCOUNT_LAUNCH_TEST=1")
endif()
if(SEED0ROOT_UNIXD_CONTRACT_TEST)
  string(APPEND SEED0ROOT_TEST_CFLAGS " -DSEED0ROOT_UNIXD_CONTRACT_TEST=1")
endif()
if(SEED0ROOT_LAUNCH_GRANT_TEST)
  string(APPEND SEED0ROOT_TEST_CFLAGS " -DSEED0ROOT_LAUNCH_GRANT_TEST=1")
endif()
if(SEED0ROOT_NATIVE_PRIMITIVES_TEST)
  string(APPEND SEED0ROOT_TEST_CFLAGS " -DSEED0ROOT_NATIVE_PRIMITIVES_TEST=1")
endif()
if(SEED0ROOT_CREDENTIAL_TEST)
  string(APPEND SEED0ROOT_TEST_CFLAGS " -DSEED0ROOT_CREDENTIAL_TEST=1")
endif()
list(APPEND SEED0ROOT_TEST_ENV "PACHAOS_MUSL_EXTRA_CFLAGS=${SEED0ROOT_TEST_CFLAGS}")

add_custom_command(
  OUTPUT "${SEED0ROOT_ELF}"
  COMMAND
    bash "${REPO_ROOT}/pack/scripts/prepare_live_console_assets.sh"
  COMMAND
    ${CMAKE_COMMAND} -E env
      ${SEED0ROOT_TEST_ENV}
      "PACHAOS_MUSL_APP_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/src/main.c"
      "PACHAOS_MUSL_EXTRA_SOURCES=${REPO_ROOT}/userland/seed0root/src/live_console.c ${REPO_ROOT}/userland/libipc/src/ipc.c ${REPO_ROOT}/userland/libipc/src/status.c ${REPO_ROOT}/userland/libpacha/src/syscall.c ${REPO_ROOT}/userland/libcapsule/src/capsule.c ${REPO_ROOT}/userland/personality/src/lpr_manifest.c ${SEED0ROOT_USB_HID_TEST_SOURCE}${LIVE_VTERM_SOURCES}"
      "PACHAOS_MUSL_EXTRA_INCLUDE_DIRS=${REPO_ROOT}/userland/seed0root/src ${LIVE_VTERM_BUILD} ${LIVE_VTERM_BUILD}/include ${REPO_ROOT}/userland/filed/include ${REPO_ROOT}/userland/storage/include ${REPO_ROOT}/userland/lpr_supervisor/include ${REPO_ROOT}/userland/personality/include ${REPO_ROOT}/userland/libipc/include ${REPO_ROOT}/userland/libpacha/include ${REPO_ROOT}/userland/libcapsule/include ${REPO_ROOT}/userland/netd/include ${REPO_ROOT}/userland/termd/include ${REPO_ROOT}/userland/gpud/include ${REPO_ROOT}/userland/usbd/include ${REPO_ROOT}/userland/inputd/include ${REPO_ROOT}/userland/unixd/include"
      bash "${REPO_ROOT}/musl/pachaos/build/build-smokes.sh" "${SEED0ROOT_ELF}"
  DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/main.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/live_console.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/live_console.h"
    "${REPO_ROOT}/pack/scripts/prepare_live_console_assets.sh"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/usb_input_smoke.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/linux_services.h"
    "${REPO_ROOT}/userland/filed/include/filed/bootstrap.h"
    "${REPO_ROOT}/userland/filed/include/filed/flags.h"
    "${REPO_ROOT}/userland/filed/include/filed/payload.h"
    "${REPO_ROOT}/userland/filed/include/filed/ipc_protocol.h"
    "${REPO_ROOT}/userland/storage/include/storage/bootstrap.h"
    "${REPO_ROOT}/userland/storage/include/storage/stack.h"
    "${REPO_ROOT}/userland/storage/include/storage/stack.def"
    "${REPO_ROOT}/userland/lpr_supervisor/include/lpr_supervisor/boot_config.h"
    "${REPO_ROOT}/userland/lpr_supervisor/include/lpr_supervisor/ipc_protocol.h"
    "${REPO_ROOT}/userland/personality/include/personality/linux_lpr.h"
    "${REPO_ROOT}/userland/personality/include/personality/lpr_manifest.h"
    "${REPO_ROOT}/userland/personality/src/lpr_manifest.c"
    "${REPO_ROOT}/userland/libipc/src/ipc.c"
    "${REPO_ROOT}/userland/libipc/include/pacha/ipc.h"
    "${REPO_ROOT}/userland/libipc/include/pacha/launch.h"
    "${REPO_ROOT}/userland/libipc/src/status.c"
    "${REPO_ROOT}/userland/libipc/include/pacha/status.h"
    "${REPO_ROOT}/userland/libpacha/include/pacha/abi.h"
    "${REPO_ROOT}/userland/libpacha/include/pacha/live_bootstrap.h"
    "${REPO_ROOT}/userland/libpacha/include/pacha/root_handoff.h"
    "${REPO_ROOT}/userland/netd/include/netd/boot_config.h"
    "${REPO_ROOT}/userland/unixd/include/unixd/ipc_protocol.h"
    "${REPO_ROOT}/userland/unixd/include/unixd/poll.h"
    "${REPO_ROOT}/userland/unixd/include/unixd/transport.h"
    "${REPO_ROOT}/userland/unixd/include/unixd/transfer.h"
    "${REPO_ROOT}/userland/termd/include/termd/boot_config.h"
    "${REPO_ROOT}/userland/gpud/include/gpud/boot_config.h"
    "${REPO_ROOT}/userland/usbd/include/usbd/boot_config.h"
    "${REPO_ROOT}/userland/inputd/include/inputd/boot_config.h"
    "${REPO_ROOT}/userland/libpacha/src/syscall.c"
    "${REPO_ROOT}/userland/libcapsule/src/capsule.c"
    "${REPO_ROOT}/userland/libcapsule/include/pacha/capsule.h"
    "${REPO_ROOT}/musl/pachaos/build/build-smokes.sh"
    "${REPO_ROOT}/musl/pachaos/include/pachaos/abi.h"
    "${REPO_ROOT}/musl/upstream/arch/pachaos/syscall_arch.h"
  WORKING_DIRECTORY "${REPO_ROOT}"
  VERBATIM
)

add_custom_target(seed0root ALL DEPENDS "${SEED0ROOT_ELF}")
