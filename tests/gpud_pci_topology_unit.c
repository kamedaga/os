/* SPDX-License-Identifier: MIT */
#include "../userland/gpud/pci_topology.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    struct kobox_linux_pci_topology t = {.devfn = 16, .metadata_count = 2,
        .metadata = {
            {.devfn = 0, .config = {[0] = 0x86, [1] = 0x80, [2] = 0x14,
                [3] = 0x9a, [11] = 6, [0x48] = 1}},
            {.devfn = 248, .config = {[0] = 0x86, [1] = 0x80,
                [2] = 0x80, [3] = 0xa0, [10] = 1, [11] = 6}}}};
    assert(gpud_i915_topology_valid(&t));
    struct kobox_linux_pci_topology bad = t;
    bad.devfn = 0; assert(!gpud_i915_topology_valid(&bad));
    bad = t; bad.bus = 1; assert(!gpud_i915_topology_valid(&bad));
    bad = t; bad.metadata[0].devfn = 8; assert(!gpud_i915_topology_valid(&bad));
    bad = t; bad.metadata[1].devfn = 0; assert(!gpud_i915_topology_valid(&bad));
    bad = t; bad.metadata[1].config[10] = 0; assert(!gpud_i915_topology_valid(&bad));
    bad = t; bad.metadata[0].config[16] = 1; assert(!gpud_i915_topology_valid(&bad));
    bad = t; bad.metadata[0].config[48] = 1; assert(!gpud_i915_topology_valid(&bad));
    bad = t; bad.metadata[0].config[14] = 1; assert(!gpud_i915_topology_valid(&bad));
    bad = t; bad.metadata[1].config[0] = 0xff; assert(!gpud_i915_topology_valid(&bad));
    bad = t; bad.metadata_count = 3; assert(!gpud_i915_topology_valid(&bad));
    bad = t; bad.metadata_count = 1; assert(!gpud_i915_topology_valid(&bad));
    assert(!gpud_i915_topology_valid(NULL));
    puts("gpud i915 topology: distinct GPU/host/PCH, BAR-less read-only admission PASS");
    return 0;
}
