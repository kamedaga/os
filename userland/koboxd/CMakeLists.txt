cmake_minimum_required(VERSION 3.16)

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

project(koboxd C ASM)

set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(KOBOXD_ELF "${CMAKE_BINARY_DIR}/koboxd.elf")

set(KOBOXD_RUNTIME_SOURCES
  ${REPO_ROOT}/userland/libipc/src/ipc.c
  ${REPO_ROOT}/userland/libcapsule/src/capsule.c
  ${REPO_ROOT}/userland/libpacha/src/syscall.c
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
  ${CMAKE_CURRENT_SOURCE_DIR}/src/block_service.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/fs_backend.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/ipc_service.c
)

string(REPLACE ";" " " KOBOXD_RUNTIME_SOURCES_ENV "${KOBOXD_RUNTIME_SOURCES}")

add_custom_command(
  OUTPUT "${KOBOXD_ELF}"
  COMMAND
    ${CMAKE_COMMAND} -E env
      "PACHAOS_MUSL_APP_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/src/main.c"
      "PACHAOS_MUSL_EXTRA_SOURCES=${KOBOXD_RUNTIME_SOURCES_ENV}"
      "PACHAOS_MUSL_EXTRA_INCLUDE_DIRS=${CMAKE_CURRENT_SOURCE_DIR}/include ${REPO_ROOT}/userland/libipc/include ${REPO_ROOT}/userland/libcapsule/include ${REPO_ROOT}/userland/libpacha/include ${REPO_ROOT}/_kobox/include ${REPO_ROOT}/_kobox/src"
      "PACHAOS_MUSL_EXTRA_CFLAGS=-D__pachaos__ -Wno-unused-parameter -Wno-unused-variable -Wno-unused-function -Wno-sign-compare -Wno-pointer-sign -Wno-missing-field-initializers -Wno-deprecated-non-prototype"
      bash "${REPO_ROOT}/musl/pachaos/build/build-smokes.sh" "${KOBOXD_ELF}"
  DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/main.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/koboxd/ipc_protocol.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/block_service.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/block_service.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/fs_backend.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/fs_backend.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ipc_service.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ipc_service.h"
    "${REPO_ROOT}/musl/pachaos/build/build-smokes.sh"
    ${KOBOXD_RUNTIME_SOURCES}
  WORKING_DIRECTORY "${REPO_ROOT}"
  VERBATIM
)

add_custom_target(koboxd ALL DEPENDS "${KOBOXD_ELF}")
