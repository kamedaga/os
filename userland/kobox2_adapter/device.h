/* SPDX-License-Identifier: MIT */
#ifndef PACHA_KOBOX_DEVICE_H
#define PACHA_KOBOX_DEVICE_H

#include "device_pci.h"
#include "device_dma.h"
#include "device_irq.h"
#include <pacha/abi.h>

#define PH_DEVICE_MMIO_MAPPINGS 64u
/* Each live mapping owns a native FD; inventory must not impose a lower limit. */
#define PH_DEVICE_DMA_MAPPINGS PACHA_FD_TABLE_LIMIT

struct ph_image;

struct ph_device_config {
    int device_fd;
    uint64_t generation;
    uint32_t segment, bus, devfn;
    uint64_t aperture_start, aperture_end;
    uint64_t *irq_cookie_sequence;
    struct ph_image *image;
    struct kobox_linux_memory_layout memory;
};

struct ph_device {
    uint64_t generation;
    struct ph_pci pci;
    struct ph_dma dma;
    struct ph_irq irq;
    struct ph_pci_mapping mmio_mappings[PH_DEVICE_MMIO_MAPPINGS];
    struct ph_dma_mapping dma_mappings[PH_DEVICE_DMA_MAPPINGS];
};

/* Bootstrap composition for one exclusively granted device. The caller owns
 * zeroed storage, the device FD, the inspected core image and its reserved
 * windows. The image's real registered RAM backs DMA; MMIO overlays only its
 * vmalloc reservation. No host FD or native layout is a wire representation.
 * Init is before Linux attachment, on a registered ph_task with core TLS and
 * the CPU notification machinery ready. Cookie storage survives replacements.
 *
 * Finish only AFTER Linux has stopped the driver, freed IRQs, detached its
 * DMA/IRQ ports and removed the PCI bus. Nonempty inventories reject finish;
 * it does not forcibly release live Linux ownership. Failed DMA drain retains
 * the device and backing. No new caller may race init/finish. */
int ph_device_init(struct ph_device *device, const struct ph_device_config *config);
int ph_device_finish(struct ph_device *device, uint64_t generation);

#endif
