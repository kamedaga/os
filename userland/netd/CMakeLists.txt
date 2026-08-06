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

set(KOBOX_RUNTIME_SOURCES
  ${REPO_ROOT}/userland/libipc/src/ipc.c
  ${REPO_ROOT}/userland/libfiled_client/src/module_image.c
  ${REPO_ROOT}/userland/libcapsule/src/capsule.c
  ${REPO_ROOT}/userland/libpacha/src/syscall.c
  ${REPO_ROOT}/userland/libpacha/src/trace.c
  ${REPO_ROOT}/_kobox/src/device/device_backend.c
  ${REPO_ROOT}/_kobox/src/device/pachaos_capsule/device_pachaos_capsule.c
  ${REPO_ROOT}/_kobox/src/host_interface/interface.c
  ${REPO_ROOT}/_kobox/src/host_interface/linux_interface_common.c
  ${REPO_ROOT}/_kobox/src/host_interface/linux_ipc/interface_linux_ipc.c
  ${REPO_ROOT}/_kobox/src/loader/elf_reader.c
  ${REPO_ROOT}/_kobox/src/loader/module_loader.c
  ${REPO_ROOT}/_kobox/src/loader/symbol_registry.c
  ${REPO_ROOT}/_kobox/src/platform/platform.c
  ${REPO_ROOT}/_kobox/src/linux_subsystem/ata/ata.c
  ${REPO_ROOT}/_kobox/src/linux_subsystem/ata/ata_symbols.c
  ${REPO_ROOT}/_kobox/src/linux_subsystem/block/block.c
  ${REPO_ROOT}/_kobox/src/linux_subsystem/block/block_symbols.c
  ${REPO_ROOT}/_kobox/src/linux_subsystem/dma/dma.c
  ${REPO_ROOT}/_kobox/src/linux_subsystem/dma/dma_symbols.c
  ${REPO_ROOT}/_kobox/src/linux_subsystem/fs/fs.c
  ${REPO_ROOT}/_kobox/src/linux_subsystem/fs/fs_symbols.c
  ${REPO_ROOT}/_kobox/src/linux_subsystem/fs/kernel_object_registry.c
  ${REPO_ROOT}/_kobox/src/linux_subsystem/input/input.c
  ${REPO_ROOT}/_kobox/src/linux_subsystem/input/input_symbols.c
  ${REPO_ROOT}/_kobox/src/linux_subsystem/kvm/kvm_symbols.c
  ${REPO_ROOT}/_kobox/src/linux_subsystem/net/net_device.c
  ${REPO_ROOT}/_kobox/src/linux_subsystem/net/net_symbols.c
  ${REPO_ROOT}/_kobox/src/linux_subsystem/nvme/nvme.c
  ${REPO_ROOT}/_kobox/src/linux_subsystem/pci/pci.c
  ${REPO_ROOT}/_kobox/src/linux_subsystem/pci/pci_symbols.c
  ${REPO_ROOT}/_kobox/src/linux_subsystem/scsi/scsi.c
  ${REPO_ROOT}/_kobox/src/linux_subsystem/security/security_symbols.c
  ${REPO_ROOT}/_kobox/src/linux_subsystem/sound/sound_symbols.c
  ${REPO_ROOT}/_kobox/src/linux_subsystem/usb/config.c
  ${REPO_ROOT}/_kobox/src/linux_subsystem/usb/port_state.c
  ${REPO_ROOT}/_kobox/src/linux_subsystem/usb/storage.c
  ${REPO_ROOT}/_kobox/src/linux_subsystem/usb/synthetic.c
  ${REPO_ROOT}/_kobox/src/linux_subsystem/usb/usb.c
  ${REPO_ROOT}/_kobox/src/linux_subsystem/usb/usb_symbols.c
  ${REPO_ROOT}/_kobox/src/linux_personality/linux_context.c
  ${REPO_ROOT}/_kobox/src/linux_personality/linux_call.c
  ${REPO_ROOT}/_kobox/src/linux_personality/linux_block.c
  ${REPO_ROOT}/_kobox/src/linux_personality/linux_core_symbols.c
  ${REPO_ROOT}/_kobox/src/linux_personality/linux_device.c
  ${REPO_ROOT}/_kobox/src/linux_personality/linux_dma.c
  ${REPO_ROOT}/_kobox/src/linux_personality/linux_dma_pool.c
  ${REPO_ROOT}/_kobox/src/linux_personality/linux_hwmon.c
  ${REPO_ROOT}/_kobox/src/linux_personality/linux_irq.c
  ${REPO_ROOT}/_kobox/src/linux_personality/linux_memory.c
  ${REPO_ROOT}/_kobox/src/linux_personality/linux_mmio.c
  ${REPO_ROOT}/_kobox/src/linux_personality/linux_netdev.c
  ${REPO_ROOT}/_kobox/src/linux_personality/linux_nvme.c
  ${REPO_ROOT}/_kobox/src/linux_personality/linux_pci.c
  ${REPO_ROOT}/_kobox/src/linux_personality/linux_platform.c
  ${REPO_ROOT}/_kobox/src/linux_personality/linux_printk.c
  ${REPO_ROOT}/_kobox/src/linux_personality/linux_scsi.c
  ${REPO_ROOT}/_kobox/src/linux_personality/linux_stubs.c
  ${REPO_ROOT}/_kobox/src/linux_personality/linux_stub_symbols.c
  ${REPO_ROOT}/_kobox/src/linux_personality/linux_sync.c
  ${REPO_ROOT}/_kobox/src/linux_personality/linux_usb.c
  ${REPO_ROOT}/_kobox/src/linux_personality/linux_input.c
  ${REPO_ROOT}/_kobox/src/linux_personality/linux_work.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/device.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/fops_smoke_stub.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/driver.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/libuinet_backend.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/mbedtls_compat.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/metrics.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/module_stack.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/netlink_socket.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/packet_io.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/socket_service.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/unix_socket.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/upper.c
)

string(REPLACE ";" " " KOBOX_RUNTIME_SOURCES_ENV "${KOBOX_RUNTIME_SOURCES}")

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
      "PACHAOS_MUSL_EXTRA_SOURCES=${KOBOX_RUNTIME_SOURCES_ENV}"
      "PACHAOS_MUSL_EXTRA_LINK_INPUTS=${NETD_LIBUINET_ARCHIVE} ${NETD_MBEDTLS_ARCHIVE}"
      "PACHAOS_MUSL_EXTRA_INCLUDE_DIRS=${CMAKE_CURRENT_SOURCE_DIR}/include ${NETD_LIBUINET_INCLUDE} ${NETD_MBEDTLS_INCLUDE} ${REPO_ROOT}/userland/filed/include ${REPO_ROOT}/userland/libfiled_client/include ${REPO_ROOT}/userland/libipc/include ${REPO_ROOT}/userland/libcapsule/include ${REPO_ROOT}/userland/libpacha/include ${REPO_ROOT}/_kobox/include ${REPO_ROOT}/_kobox/src"
      "PACHAOS_MUSL_EXTRA_CFLAGS=-D__pachaos__ -DNETD_WITH_LIBUINET -Wno-unused-parameter -Wno-unused-variable -Wno-unused-function -Wno-sign-compare -Wno-pointer-sign -Wno-missing-field-initializers -Wno-deprecated-non-prototype"
      bash "${REPO_ROOT}/musl/pachaos/build/build-smokes.sh" "${NETD_ELF}"
  DEPENDS
    "${NETD_LIBUINET_ARCHIVE}"
    "${NETD_MBEDTLS_ARCHIVE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/main.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/netd_internal.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/device.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/fops_smoke_stub.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/driver.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/libuinet_backend.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/libuinet_backend.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/mbedtls_compat.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/metrics.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/module_stack.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/netlink_socket.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/netlink_socket.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/packet_io.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/socket_service.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/unix_socket.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/socket_service.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/upper.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/upper.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/netd/boot_config.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/netd/ipc_protocol.h"
    "${REPO_ROOT}/userland/filed/include/filed/flags.h"
    "${REPO_ROOT}/userland/filed/include/filed/ipc_protocol.h"
    "${REPO_ROOT}/userland/libfiled_client/include/filed_client/module_image.h"
    "${REPO_ROOT}/musl/pachaos/build/build-smokes.sh"
    ${KOBOX_RUNTIME_SOURCES}
  WORKING_DIRECTORY "${REPO_ROOT}"
  VERBATIM
)

add_custom_target(netd ALL DEPENDS "${NETD_ELF}")
