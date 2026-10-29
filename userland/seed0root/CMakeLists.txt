cmake_minimum_required(VERSION 3.16)
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/pachaos-native.toolchain.cmake")
endif()
project(seed0root C ASM)

set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
include("${REPO_ROOT}/userland/cmake/pachaos_native.cmake")
# Written by pack/scripts/prepare_live_console_assets.sh (the
# live_console_assets pacgo recipe).
set(LIVE_VTERM_BUILD "${REPO_ROOT}/.artifacts/third_party/libvterm-0.3.3/build")
set(LIVE_VTERM_SOURCES)
foreach(VTERM_UNIT encoding keyboard mouse parser pen screen state unicode vterm)
  list(APPEND LIVE_VTERM_SOURCES "${LIVE_VTERM_BUILD}/src/${VTERM_UNIT}.c")
endforeach()
option(SEED0ROOT_NATIVE_FD_GROWTH_TEST "Run native FD capacity probe after host full-workload marker (test image only)" OFF)
option(SEED0ROOT_UNIXD_CONTRACT_TEST "Run native/UNIX probes with netd stopped (test image only)" OFF)
option(SEED0ROOT_NATIVE_PRIMITIVES_TEST "Run native execution and clock primitives (test image only)" OFF)
option(SEED0ROOT_CREDENTIAL_TEST "Run Linux credential fork/exec probe (test image only)" OFF)
option(SEED0ROOT_ACCOUNT_LAUNCH_TEST "Direct service account and capability regression launches" OFF)
option(SEED0ROOT_REAL_SERVICE_TEST "Isolated D-Bus service-account acceptance launches" OFF)
option(SEED0ROOT_GPUD_RESTART_TEST "Force and verify one production gpud sandbox restart" OFF)
option(SEED0ROOT_LPR_THREAD_SIGNAL_TEST "Run LPR thread signals before GPU startup (test image only)" OFF)
option(SEED0ROOT_USB_HID_INPUT_TEST "Read QEMU USB HID events through inputd IPC (test image only)" OFF)
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
if(SEED0ROOT_NATIVE_FD_GROWTH_TEST)
  string(APPEND SEED0ROOT_TEST_CFLAGS " -DSEED0ROOT_NATIVE_FD_GROWTH_TEST=1")
endif()
if(SEED0ROOT_UNIXD_CONTRACT_TEST)
  string(APPEND SEED0ROOT_TEST_CFLAGS " -DSEED0ROOT_UNIXD_CONTRACT_TEST=1")
endif()
if(SEED0ROOT_NATIVE_PRIMITIVES_TEST)
  string(APPEND SEED0ROOT_TEST_CFLAGS " -DSEED0ROOT_NATIVE_PRIMITIVES_TEST=1")
endif()
if(SEED0ROOT_CREDENTIAL_TEST)
  string(APPEND SEED0ROOT_TEST_CFLAGS " -DSEED0ROOT_CREDENTIAL_TEST=1")
endif()
separate_arguments(SEED0ROOT_TEST_CFLAGS UNIX_COMMAND "${SEED0ROOT_TEST_CFLAGS}")

pachaos_native_executable(seed0root
  SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/main.c"
    "${REPO_ROOT}/userland/libpacha/src/log.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/live_console.c"
    "${REPO_ROOT}/userland/libipc/src/ipc.c"
    "${REPO_ROOT}/userland/libipc/src/status.c"
    "${REPO_ROOT}/userland/libpacha/src/syscall.c"
    "${REPO_ROOT}/userland/libcapsule/src/capsule.c"
    "${REPO_ROOT}/userland/personality/src/lpr_manifest.c"
    "${REPO_ROOT}/userland/libfiled_client/src/client.c"
    "${REPO_ROOT}/userland/filed/src/exec_payload.c"
    "${REPO_ROOT}/kobox2/protocol/src/filesystem.c"
    ${SEED0ROOT_USB_HID_TEST_SOURCE}
    ${LIVE_VTERM_SOURCES}
  INCLUDE_DIRECTORIES
    "${REPO_ROOT}/kobox2/protocol/include"
    "${REPO_ROOT}/kobox2/protocol/generated/include"
    "${REPO_ROOT}/userland/libfiled_client/include"
    "${REPO_ROOT}/userland/seed0root/src"
    "${LIVE_VTERM_BUILD}"
    "${LIVE_VTERM_BUILD}/include"
    "${REPO_ROOT}/userland/filed/include"
    "${REPO_ROOT}/userland/storage/include"
    "${REPO_ROOT}/userland/lpr_supervisor/include"
    "${REPO_ROOT}/userland/personality/include"
    "${REPO_ROOT}/userland/libipc/include"
    "${REPO_ROOT}/userland/libpacha/include"
    "${REPO_ROOT}/userland/libcapsule/include"
    "${REPO_ROOT}/userland/netd/include"
    "${REPO_ROOT}/userland/logd/include"
    "${REPO_ROOT}/userland/termd/include"
    "${REPO_ROOT}/userland/gpud/include"
    "${REPO_ROOT}/userland/usbd/include"
    "${REPO_ROOT}/userland/inputd/include"
    "${REPO_ROOT}/userland/unixd/include"
  COMPILE_OPTIONS
    ${SEED0ROOT_TEST_CFLAGS})
