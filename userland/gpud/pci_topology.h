/* SPDX-License-Identifier: MIT */
#ifndef PACHA_GPUD_PCI_TOPOLOGY_H
#define PACHA_GPUD_PCI_TOPOLOGY_H
#include "../../kobox2/linux-sandbox/kobox/boot/pci_metadata.h"

static inline int gpud_i915_topology_valid(const struct kobox_linux_pci_topology *t)
{
    if (!kobox_pci_metadata_valid(t) || t->bus != 0 || t->metadata_count != 2)
        return 0;
    for (uint32_t i = 0; i < t->metadata_count; ++i) {
        const struct kobox_linux_pci_metadata *m = &t->metadata[i];
        if (m->config[0] != 0x86 || m->config[1] != 0x80 ||
            (m->config[10] == 0 && m->devfn != 0))
            return 0;
    }
    return 1;
}
#endif
