/* SPDX-License-Identifier: MIT */
#ifndef PACHA_KOBOX_DEVICE_PCI_H
#define PACHA_KOBOX_DEVICE_PCI_H

#include "boot/pci_host.h"
#include <pacha/capsule.h>

struct ph_pci_mapping {
    uintptr_t address;
    size_t length;
    int fd;
};

struct ph_pci_config {
    int device_fd;
    uint64_t generation;
    /* Local Linux topology, not a hardware address or additional authority.
     * Every config transaction still addresses only device_fd. */
    uint32_t segment, bus, devfn;
    void *reservation;
    size_t reservation_size;
    struct ph_pci_mapping *mappings;
    size_t mapping_capacity;
};

struct ph_pci {
    struct kobox_linux_pci_host host;
    struct ph_pci_config config;
    struct pacha_capsule_bar_info bars[KOBOX_PCI_MEMORY_WINDOWS];
    uint64_t native_device;
    unsigned int admitted;
};

/* Sandbox-local native PCI port. No controller code is linked here.
 * Initialize a zeroed object before boot; config, mapping storage and device
 * FD remain caller-owned. Linux serializes config and MMIO callbacks with its
 * respective locks. Stop all callers before revoke/destroy; revoke closes new
 * admission but unmap remains available until the last lease is retired.
 * No allocations, waits, or Linux reentry occur in leaf callbacks. Native
 * REPLACE_EXISTING must retain the underlying unbacked PROT_NONE reservation
 * throughout the MMIO lease, failed derive, and last close. A two-syscall
 * munmap/map or close/reserve fallback would open an allocator race. */
int ph_pci_init(struct ph_pci *pci, const struct ph_pci_config *config);
int ph_pci_revoke(struct ph_pci *pci, uint64_t generation);
int ph_pci_destroy(struct ph_pci *pci);

#endif
