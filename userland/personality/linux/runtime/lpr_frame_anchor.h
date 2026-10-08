/* SPDX-License-Identifier: MIT */
#ifndef LPR_FRAME_ANCHOR_H
#define LPR_FRAME_ANCHOR_H

#include <stdint.h>

/* Only the owned, frame-pointer-preserving LPR call chain is traversed. */
static inline const struct lpr_linux_user_frame *lpr_frame_anchor_find(uintptr_t cursor)
{
    for (unsigned depth = 0; depth < 64u; ++depth) {
        if (cursor == 0 || (cursor & (sizeof(uintptr_t) - 1u)) != 0) return 0;
        const uintptr_t next = *(const uintptr_t *)cursor;
        /* Native callers need not originate in the syscall trampoline. Their
         * frame chain can end in ordinary Linux stack data (e.g. argv). Do
         * not read a misaligned candidate: a string pointer near a stack
         * guard page can straddle that page even when the pointer is mapped. */
        if ((next & (sizeof(uintptr_t) - 1u)) != 0 || next <= cursor ||
            next - cursor > 1024u * 1024u) return 0;
        const uint64_t *anchor = (const uint64_t *)next;
        if (anchor[0] == UINT64_C(0x4c50524652414d45) &&
            anchor[2] == UINT64_C(0x454d41524652504c))
            return (const struct lpr_linux_user_frame *)(uintptr_t)anchor[1];
        cursor = next;
    }
    return 0;
}
#endif
