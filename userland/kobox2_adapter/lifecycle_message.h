/* SPDX-License-Identifier: MIT */
#ifndef PACHA_KOBOX_LIFECYCLE_MESSAGE_H
#define PACHA_KOBOX_LIFECYCLE_MESSAGE_H

#include <stdint.h>

/* Native management messages after bootstrap FINISH, not GPU queue opcodes.
 * All carry the launch generation and no ancillary capabilities. READY has
 * correlation=0 and value=0 on success. A module-launch failure before READY
 * uses the same operation with a negative Linux errno in value and
 * correlation=(loaded module count << 1) | PCI bound. A fatal failure before
 * READY uses correlation=the first eight bytes of the source basename and
 * value=(source line << 32) | low 32 bits of the fatal result. QUIESCE carries
 * the opaque controller action token and value=0. An unhandled native fault
 * uses FAULT with vector in operation bits 16..31, error code in bits 32..47,
 * and a core-relative-IP flag in bit 48; correlation is the IP/offset and
 * value is the fault address. PROGRESS reports a hosted-Linux module-launch
 * phase in correlation and (signed status << 32 | module index) in value;
 * it does not claim readiness. No raw stack contents cross this channel.
 * READY means the selected module closure initialized, not DRM publication.
 * There is no "stopped" acknowledgement: the owner must observe PROCESS_WAIT
 * before completing QUIESCE, then separately revoke/reset device resources. */
enum ph_lifecycle_operation {
    PH_LIFECYCLE_READY = 0x100,
    PH_LIFECYCLE_PROGRESS = 0x101,
    PH_LIFECYCLE_QUIESCE = 0x102,
    PH_LIFECYCLE_FAULT = 0x103,
};

static inline uint64_t ph_lifecycle_fault_operation(unsigned vector,
    unsigned error_code, unsigned core_relative) {
    return PH_LIFECYCLE_FAULT | ((uint64_t)(uint16_t)vector << 16) |
        ((uint64_t)(uint16_t)error_code << 32) |
        ((uint64_t)(core_relative & 1u) << 48);
}

static inline int ph_lifecycle_is_fault_operation(uint64_t operation) {
    return (operation & 0xffffu) == PH_LIFECYCLE_FAULT && !(operation >> 49);
}

#endif
