cmake_minimum_required(VERSION 3.16)

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

project(netd C ASM)

set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(NETD_ELF "${CMAKE_BINARY_DIR}/netd.elf")
set(NETD_LIBUINET_ARCHIVE "${REPO_ROOT}/.artifacts/libuinet-pachaos/lib/libuinet.a")
set(NETD_LIBUINET_INCLUDE "${REPO_ROOT}/.artifacts/libuinet-pachaos/include")
set(NETD_MBEDTLS_ARCHIVE "${REPO_ROOT}/.artifacts/mbedtls-pachaos/lib/libmbedtls-pachaos.a")
set(NETD_MBEDTLS_INCLUDE "${REPO_ROOT}/.artifacts/mbedtls-pachaos/include")

set(NETD_SOURCES
  ${REPO_ROOT}/userland/libipc/src/ipc.c
  ${REPO_ROOT}/userland/libipc/src/status.c
  ${REPO_ROOT}/userland/libfiled_client/src/module_image.c
  ${REPO_ROOT}/userland/libcapsule/src/capsule.c
  ${REPO_ROOT}/userland/libpacha/src/syscall.c
  ${REPO_ROOT}/userland/libpacha/src/trace.c
  ${REPO_ROOT}/userland/gpud/launch.c
  ${REPO_ROOT}/userland/gpud/launch_native.c
  ${REPO_ROOT}/userland/gpud/arch/x86_64/launch_image.c
  ${REPO_ROOT}/userland/gpud/bootstrap.c
  ${REPO_ROOT}/userland/gpud/process.c
  ${REPO_ROOT}/userland/gpud/process_native.c
  ${REPO_ROOT}/userland/gpud/lifecycle.c
  ${REPO_ROOT}/userland/kobox2_adapter/ipc.c
  ${REPO_ROOT}/userland/kobox2_adapter/bootstrap.c
  ${REPO_ROOT}/kobox2/src/controller/controller.c
  ${REPO_ROOT}/kobox2/src/closure.c
  ${REPO_ROOT}/kobox2/protocol/src/closure_manifest.c
  ${REPO_ROOT}/kobox2/protocol/src/resource_grant.c
  ${REPO_ROOT}/kobox2/protocol/src/sha256.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/kobox2_frame_client.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/kobox2_package.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/kobox2_nic.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/dhcp.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/libuinet_backend.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/link.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/mbedtls_compat.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/metrics.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/netlink_socket.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/network_config.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/packet_io.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/socket_service.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/status_file.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/upper.c
)

string(REPLACE ";" " " NETD_SOURCES_ENV "${NETD_SOURCES}")

add_custom_command(
  OUTPUT "${NETD_LIBUINET_ARCHIVE}"
  COMMAND bash "${REPO_ROOT}/pack/scripts/build_libuinet_pachaos.sh"
  DEPENDS
    "${REPO_ROOT}/pack/scripts/build_libuinet_pachaos.sh"
    "${REPO_ROOT}/pack/scripts/download_libuinet.sh"
    "${REPO_ROOT}/pack/scripts/apply_libuinet_pachaos_overlay.sh"
    "${REPO_ROOT}/third_party/libuinet/pachaos_overlay/lib/libuinet/api_include/uinet_pachaos_api.h"
    "${REPO_ROOT}/third_party/libuinet/pachaos_overlay/lib/libuinet/uinet_if_pachaos.c"
    "${REPO_ROOT}/third_party/libuinet/pachaos_overlay/lib/libuinet/uinet_if_pachaos.h"
    "${REPO_ROOT}/third_party/libuinet/pachaos_overlay/lib/libuinet/uinet_pachaos_host.c"
  WORKING_DIRECTORY "${REPO_ROOT}"
  VERBATIM
)

add_custom_command(
  OUTPUT "${NETD_MBEDTLS_ARCHIVE}"
  COMMAND bash "${REPO_ROOT}/pack/scripts/build_mbedtls_pachaos.sh"
  DEPENDS
    "${REPO_ROOT}/pack/scripts/build_mbedtls_pachaos.sh"
    "${REPO_ROOT}/pack/scripts/download_mbedtls.sh"
  WORKING_DIRECTORY "${REPO_ROOT}"
  VERBATIM
)

add_custom_command(
  OUTPUT "${NETD_ELF}"
  COMMAND
    ${CMAKE_COMMAND} -E env
      "PACHAOS_MUSL_APP_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/src/main.c"
      "PACHAOS_MUSL_EXTRA_SOURCES=${NETD_SOURCES_ENV}"
      "PACHAOS_MUSL_EXTRA_LINK_INPUTS=${NETD_LIBUINET_ARCHIVE} ${NETD_MBEDTLS_ARCHIVE}"
      "PACHAOS_MUSL_EXTRA_INCLUDE_DIRS=${CMAKE_CURRENT_SOURCE_DIR}/include ${NETD_LIBUINET_INCLUDE} ${NETD_MBEDTLS_INCLUDE} ${REPO_ROOT}/userland/gpud ${REPO_ROOT}/userland/filed/include ${REPO_ROOT}/userland/libfiled_client/include ${REPO_ROOT}/userland/libipc/include ${REPO_ROOT}/userland/libcapsule/include ${REPO_ROOT}/userland/libpacha/include ${REPO_ROOT}/kobox2/include ${REPO_ROOT}/kobox2/linux-sandbox/kobox ${REPO_ROOT}/kobox2/protocol/include ${REPO_ROOT}/kobox2/protocol/generated/include"
      "PACHAOS_MUSL_EXTRA_CFLAGS=-D__pachaos__ -DNETD_WITH_LIBUINET -Wno-unused-parameter -Wno-unused-variable -Wno-unused-function -Wno-sign-compare -Wno-pointer-sign -Wno-missing-field-initializers -Wno-deprecated-non-prototype"
      bash "${REPO_ROOT}/musl/pachaos/build/build-smokes.sh" "${NETD_ELF}"
  DEPENDS
    "${NETD_LIBUINET_ARCHIVE}"
    "${REPO_ROOT}/userland/libpacha/include/pacha/abi.h"
    "${REPO_ROOT}/musl/pachaos/include/pachaos/abi.h"
    "${REPO_ROOT}/musl/upstream/arch/pachaos/syscall_arch.h"
    "${NETD_MBEDTLS_ARCHIVE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/main.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/netd_internal.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/kobox2_frame_client.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/kobox2_frame_client.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/kobox2_package.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/kobox2_package.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/kobox2_nic.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/kobox2_nic.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/dhcp.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/dhcp.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/libuinet_backend.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/libuinet_backend.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/link.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/link.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/mbedtls_compat.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/metrics.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/netlink_socket.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/netlink_socket.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/network_config.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/network_config.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/packet_io.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/socket_service.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/socket_service.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/status_file.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/status_file.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/upper.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/upper.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/netd/boot_config.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/netd/ipc_protocol.h"
    "${REPO_ROOT}/userland/filed/include/filed/flags.h"
    "${REPO_ROOT}/userland/filed/include/filed/ipc_protocol.h"
    "${REPO_ROOT}/userland/filed/include/filed/payload.h"
    "${REPO_ROOT}/userland/libfiled_client/include/filed_client/module_image.h"
    "${REPO_ROOT}/musl/pachaos/build/build-smokes.sh"
    ${NETD_SOURCES}
  WORKING_DIRECTORY "${REPO_ROOT}"
  VERBATIM
)

add_custom_target(netd ALL DEPENDS "${NETD_ELF}")
