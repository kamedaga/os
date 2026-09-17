/* SPDX-License-Identifier: MIT */
#include "device.h"
#include "host.h"
#include <errno.h>

int ph_device_init(struct ph_device *device, const struct ph_device_config *config) {
    if (!device || !config || device->generation || device->pci.config.generation ||
        device->dma.config.generation || device->irq.config.generation ||
        !config->image || !config->generation || !config->irq_cookie_sequence ||
        config->memory.size != sizeof(config->memory) ||
        config->memory.identity != KOBOX_LINUX_MEMORY_HOST_IDENTITY ||
        config->memory.operations != &ph_memory_ops ||
        config->memory.ram_backing != config->image ||
        config->memory.direct_map != config->image->direct ||
        config->image->ram_fd < 16 || config->memory.ram_size != PH_RAM_SIZE) return -EINVAL;
    struct ph_pci_config pci = {
        .device_fd = config->device_fd, .generation = config->generation,
        .segment = config->segment, .bus = config->bus, .devfn = config->devfn,
        .reservation = config->memory.vmalloc_base,
        .reservation_size = config->memory.vmalloc_size,
        .mappings = device->mmio_mappings, .mapping_capacity = PH_DEVICE_MMIO_MAPPINGS,
    };
    int result = ph_pci_init(&device->pci, &pci);
    if (result) return result;
    struct ph_dma_config dma = {
        .device_fd = config->device_fd, .native_device = device->pci.native_device,
        .generation = config->generation, .ram = config->memory.direct_map,
        .ram_fd = config->image->ram_fd,
        .ram_length = config->memory.ram_size,
        .aperture_start = config->aperture_start, .aperture_end = config->aperture_end,
        .mappings = device->dma_mappings, .mapping_capacity = PH_DEVICE_DMA_MAPPINGS,
    };
    result = ph_dma_init(&device->dma, &dma);
    if (result) goto release_pci;
    struct ph_irq_config irq = {
        .device_fd = config->device_fd, .native_device = device->pci.native_device,
        .generation = config->generation, .cpu_count = PH_CPU_COUNT,
        .cookie_sequence = config->irq_cookie_sequence,
    };
    result = ph_irq_init(&device->irq, &irq);
    if (result) {
        /* A failed stop cannot return backing ownership to the bootstrapper. */
        PH_OK(ph_dma_revoke(&device->dma, config->generation));
        PH_OK(ph_dma_destroy(&device->dma));
        goto release_pci;
    }
    device->generation = config->generation;
    return 0;
release_pci:
    PH_OK(ph_pci_revoke(&device->pci, config->generation));
    PH_OK(ph_pci_destroy(&device->pci));
    return result;
}

int ph_device_finish(struct ph_device *device, uint64_t generation) {
    if (!device || !generation || device->generation != generation) return -ESTALE;
    for (size_t i = 0; i < PH_DEVICE_MMIO_MAPPINGS; ++i)
        if (device->mmio_mappings[i].length) return -EBUSY;
    for (size_t i = 0; i < PH_DEVICE_DMA_MAPPINGS; ++i)
        if (device->dma_mappings[i].length) return -EBUSY;
    for (size_t i = 0; i < PH_IRQ_ROUTES; ++i)
        if (device->irq.slots[i].route.cookie) return -EBUSY;
    PH_OK(ph_irq_revoke(&device->irq, generation));
    int result = ph_dma_revoke(&device->dma, generation);
    if (result) return result;
    PH_OK(ph_pci_revoke(&device->pci, generation));
    PH_OK(ph_irq_destroy(&device->irq));
    PH_OK(ph_dma_destroy(&device->dma));
    PH_OK(ph_pci_destroy(&device->pci));
    device->generation = 0;
    return 0;
}
