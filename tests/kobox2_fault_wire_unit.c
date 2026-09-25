/* SPDX-License-Identifier: MIT */
#include "../userland/kobox2_adapter/lifecycle_message.h"
#include "../userland/netd/include/netd/boot_config.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

int main(void) {
    const uint64_t operation = ph_lifecycle_fault_operation(14, 5, 1);
    assert(ph_lifecycle_is_fault_operation(operation));
    assert((operation >> 16 & 0xffffu) == 14);
    assert((operation >> 32 & 0xffffu) == 5);
    assert((operation >> 48 & 1u) == 1);
    assert(!ph_lifecycle_is_fault_operation(PH_LIFECYCLE_READY));
    assert(!ph_lifecycle_is_fault_operation(operation | (UINT64_C(1) << 49)));

    const uint64_t status = (uint32_t)-5 |
        ((uint64_t)netd_boot_fault_metadata(14, 5, 1) << 32);
    const unsigned metadata = (unsigned)(status >> 32);
    assert((int32_t)status == -5);
    assert((metadata & 0xffffu) == 5);
    assert(((metadata >> 16) & 0x7fffu) == 14);
    assert((metadata >> 31) == 1);
    assert(NETD_BOOT_FAULT_MAGIC != NETD_BOOT_STATUS_MAGIC);
    puts("KOBOX2_FAULT_WIRE_UNIT=PASS");
    return 0;
}
