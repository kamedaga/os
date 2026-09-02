cmake_minimum_required(VERSION 3.16)

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

project(termd C ASM)

set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(TERMD_ELF "${CMAKE_BINARY_DIR}/termd.elf")

set(LINUX_TTY_SOURCES
  "${REPO_ROOT}/third_party/linux-6.8-tty/Makefile"
  "${REPO_ROOT}/third_party/linux-6.8-tty/drivers/tty/tty_io.c"
  "${REPO_ROOT}/third_party/linux-6.8-tty/drivers/tty/tty.h"
  "${REPO_ROOT}/third_party/linux-6.8-tty/drivers/tty/n_tty.c"
  "${REPO_ROOT}/third_party/linux-6.8-tty/drivers/tty/tty_ioctl.c"
  "${REPO_ROOT}/third_party/linux-6.8-tty/drivers/tty/tty_ldisc.c"
  "${REPO_ROOT}/third_party/linux-6.8-tty/drivers/tty/tty_buffer.c"
  "${REPO_ROOT}/third_party/linux-6.8-tty/drivers/tty/tty_port.c"
  "${REPO_ROOT}/third_party/linux-6.8-tty/drivers/tty/tty_mutex.c"
  "${REPO_ROOT}/third_party/linux-6.8-tty/drivers/tty/tty_ldsem.c"
  "${REPO_ROOT}/third_party/linux-6.8-tty/drivers/tty/tty_baudrate.c"
  "${REPO_ROOT}/third_party/linux-6.8-tty/drivers/tty/tty_jobctrl.c"
  "${REPO_ROOT}/third_party/linux-6.8-tty/drivers/tty/n_null.c"
  "${REPO_ROOT}/third_party/linux-6.8-tty/drivers/tty/pty.c"
  "${REPO_ROOT}/third_party/linux-6.8-tty/drivers/tty/hvc/hvc_console.c"
  "${REPO_ROOT}/third_party/linux-6.8-tty/drivers/tty/hvc/hvc_console.h"
  "${REPO_ROOT}/third_party/linux-6.8-tty/drivers/virtio/virtio.c"
  "${REPO_ROOT}/third_party/linux-6.8-tty/drivers/virtio/virtio_ring.c"
  "${REPO_ROOT}/third_party/linux-6.8-tty/drivers/virtio/virtio_pci_common.c"
  "${REPO_ROOT}/third_party/linux-6.8-tty/drivers/virtio/virtio_pci_common.h"
  "${REPO_ROOT}/third_party/linux-6.8-tty/drivers/virtio/virtio_pci_modern_dev.c"
  "${REPO_ROOT}/third_party/linux-6.8-tty/drivers/virtio/virtio_pci_legacy_dev.c"
  "${REPO_ROOT}/third_party/linux-6.8-tty/drivers/virtio/virtio_pci_modern.c"
  "${REPO_ROOT}/third_party/linux-6.8-tty/drivers/virtio/virtio_pci_legacy.c"
  "${REPO_ROOT}/third_party/linux-6.8-tty/drivers/char/virtio_console.c"
  "${REPO_ROOT}/third_party/linux-6.8-tty/fs/devpts/inode.c"
  "${REPO_ROOT}/third_party/linux-6.8-tty/kmodule/core_entry.c"
)

set(KOBOX_RUNTIME_SOURCES
  "${REPO_ROOT}/userland/libipc/src/ipc.c"
  "${REPO_ROOT}/userland/libfiled_client/src/module_image.c"
  "${REPO_ROOT}/userland/libipc/src/status.c"
  "${REPO_ROOT}/userland/libcapsule/src/capsule.c"
  "${REPO_ROOT}/userland/libpacha/src/syscall.c"
  "${REPO_ROOT}/userland/libpacha/src/trace.c"
  "${REPO_ROOT}/_kobox/src/device/device_backend.c"
  "${REPO_ROOT}/_kobox/src/device/pachaos_capsule/device_pachaos_capsule.c"
  "${REPO_ROOT}/_kobox/src/device/pachaos_capsule/pachaos_dma_arena.c"
  "${REPO_ROOT}/_kobox/src/host_interface/interface.c"
  "${REPO_ROOT}/_kobox/src/host_interface/linux_interface_common.c"
  "${REPO_ROOT}/_kobox/src/host_interface/linux_ipc/interface_linux_ipc.c"
  "${REPO_ROOT}/_kobox/src/loader/elf_reader.c"
  "${REPO_ROOT}/_kobox/src/loader/module_loader.c"
  "${REPO_ROOT}/_kobox/src/loader/symbol_registry.c"
  "${REPO_ROOT}/_kobox/src/platform/platform.c"
  "${REPO_ROOT}/_kobox/src/linux_subsystem/ata/ata.c"
  "${REPO_ROOT}/_kobox/src/linux_subsystem/ata/ata_symbols.c"
  "${REPO_ROOT}/_kobox/src/linux_subsystem/block/block.c"
  "${REPO_ROOT}/_kobox/src/linux_subsystem/block/block_symbols.c"
  "${REPO_ROOT}/_kobox/src/linux_subsystem/dma/dma.c"
  "${REPO_ROOT}/_kobox/src/linux_subsystem/dma/dma_symbols.c"
  "${REPO_ROOT}/_kobox/src/linux_subsystem/fs/fs.c"
  "${REPO_ROOT}/_kobox/src/linux_subsystem/fs/fs_symbols.c"
  "${REPO_ROOT}/_kobox/src/linux_subsystem/fs/kernel_object_registry.c"
  "${REPO_ROOT}/_kobox/src/linux_subsystem/input/input.c"
  "${REPO_ROOT}/_kobox/src/linux_subsystem/input/input_symbols.c"
  "${REPO_ROOT}/_kobox/src/linux_subsystem/kvm/kvm_symbols.c"
  "${REPO_ROOT}/_kobox/src/linux_subsystem/net/net_device.c"
  "${REPO_ROOT}/_kobox/src/linux_subsystem/net/net_symbols.c"
  "${REPO_ROOT}/_kobox/src/linux_subsystem/nvme/nvme.c"
  "${REPO_ROOT}/_kobox/src/linux_subsystem/pci/pci.c"
  "${REPO_ROOT}/_kobox/src/linux_subsystem/pci/pci_symbols.c"
  "${REPO_ROOT}/_kobox/src/linux_subsystem/scsi/scsi.c"
  "${REPO_ROOT}/_kobox/src/linux_subsystem/security/security_symbols.c"
  "${REPO_ROOT}/_kobox/src/linux_subsystem/sound/sound_symbols.c"
  "${REPO_ROOT}/_kobox/src/linux_subsystem/usb/config.c"
  "${REPO_ROOT}/_kobox/src/linux_subsystem/usb/port_state.c"
  "${REPO_ROOT}/_kobox/src/linux_subsystem/usb/storage.c"
  "${REPO_ROOT}/_kobox/src/linux_subsystem/usb/synthetic.c"
  "${REPO_ROOT}/_kobox/src/linux_subsystem/usb/usb.c"
  "${REPO_ROOT}/_kobox/src/linux_subsystem/usb/usb_symbols.c"
  "${REPO_ROOT}/_kobox/src/linux_personality/linux_context.c"
  "${REPO_ROOT}/_kobox/src/linux_personality/linux_call.c"
  "${REPO_ROOT}/_kobox/src/linux_personality/linux_block.c"
  "${REPO_ROOT}/_kobox/src/linux_personality/linux_core_symbols.c"
  "${REPO_ROOT}/_kobox/src/linux_personality/linux_device.c"
  "${REPO_ROOT}/_kobox/src/linux_personality/linux_dma.c"
  "${REPO_ROOT}/_kobox/src/linux_personality/linux_dma_pool.c"
  "${REPO_ROOT}/_kobox/src/linux_personality/linux_hwmon.c"
  "${REPO_ROOT}/_kobox/src/linux_personality/linux_irq.c"
  "${REPO_ROOT}/_kobox/src/linux_personality/linux_memory.c"
  "${REPO_ROOT}/_kobox/src/linux_personality/linux_mmio.c"
  "${REPO_ROOT}/_kobox/src/linux_personality/linux_netdev.c"
  "${REPO_ROOT}/_kobox/src/linux_personality/linux_nvme.c"
  "${REPO_ROOT}/_kobox/src/linux_personality/linux_pci.c"
  "${REPO_ROOT}/_kobox/src/linux_personality/linux_platform.c"
  "${REPO_ROOT}/_kobox/src/linux_personality/linux_printk.c"
  "${REPO_ROOT}/_kobox/src/linux_personality/linux_scsi.c"
  "${REPO_ROOT}/_kobox/src/linux_personality/linux_stubs.c"
  "${REPO_ROOT}/_kobox/src/linux_personality/linux_stub_symbols.c"
  "${REPO_ROOT}/_kobox/src/linux_personality/linux_sync.c"
  "${REPO_ROOT}/_kobox/src/linux_personality/linux_usb.c"
  "${REPO_ROOT}/_kobox/src/linux_personality/linux_input.c"
  "${REPO_ROOT}/_kobox/src/linux_personality/linux_work.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/fops_smoke_stub.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/linux_tty_island.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/null_device_backend.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/termd_service.c"
)

string(REPLACE ";" " " TERMD_EXTRA_SOURCES_ENV "${KOBOX_RUNTIME_SOURCES}")

add_custom_command(
  OUTPUT "${TERMD_ELF}"
  COMMAND
    ${CMAKE_COMMAND} -E env
      "PACHAOS_MUSL_APP_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/src/main.c"
      "PACHAOS_MUSL_EXTRA_SOURCES=${TERMD_EXTRA_SOURCES_ENV}"
      "PACHAOS_MUSL_EXTRA_INCLUDE_DIRS=${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src ${REPO_ROOT}/userland/filed/include ${REPO_ROOT}/userland/libfiled_client/include ${REPO_ROOT}/userland/lpr_supervisor/include ${REPO_ROOT}/userland/libipc/include ${REPO_ROOT}/userland/libcapsule/include ${REPO_ROOT}/userland/libpacha/include ${REPO_ROOT}/_kobox/include ${REPO_ROOT}/_kobox/src ${REPO_ROOT}/third_party/linux-6.8-tty"
      "PACHAOS_MUSL_EXTRA_CFLAGS=-D__pachaos__ -DTERMD_WITH_KOBOX_TTY_ISLAND -Wno-unused-parameter -Wno-unused-variable -Wno-unused-function -Wno-sign-compare -Wno-pointer-sign -Wno-missing-field-initializers -Wno-deprecated-non-prototype"
      bash "${REPO_ROOT}/musl/pachaos/build/build-smokes.sh" "${TERMD_ELF}"
  DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/termd/boot_config.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/termd/ipc_protocol.h"
    "${REPO_ROOT}/userland/lpr_supervisor/include/lpr_supervisor/ipc_protocol.h"
    "${REPO_ROOT}/userland/libipc/include/pacha/status.h"
    "${REPO_ROOT}/userland/filed/include/filed/payload.h"
    "${REPO_ROOT}/userland/libfiled_client/include/filed_client/module_image.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/fops_smoke_stub.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/linux_tty_island.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/linux_tty_island.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/main.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/null_device_backend.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/null_device_backend.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/termd_service.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/termd_service.h"
    "${REPO_ROOT}/third_party/linux-6.8-tty/README.md"
    "${REPO_ROOT}/musl/pachaos/build/build-smokes.sh"
    ${KOBOX_RUNTIME_SOURCES}
    ${LINUX_TTY_SOURCES}
  WORKING_DIRECTORY "${REPO_ROOT}"
  VERBATIM
)

add_custom_target(termd ALL DEPENDS "${TERMD_ELF}")

add_custom_target(termd-linux-tty-ko
  COMMAND bash "${REPO_ROOT}/pack/scripts/build_linux_tty_ko.sh" "${REPO_ROOT}/.artifacts/kobox-linux-tty"
  DEPENDS
    "${REPO_ROOT}/pack/scripts/build_linux_tty_ko.sh"
    ${LINUX_TTY_SOURCES}
  WORKING_DIRECTORY "${REPO_ROOT}"
  VERBATIM
)
