/* SPDX-License-Identifier: MIT */
#ifndef KOBOX2_ADAPTER_OWNER_WAIT_H
#define KOBOX2_ADAPTER_OWNER_WAIT_H

#include <stdatomic.h>
#include <stdbool.h>

struct ph_owner_wait {
    atomic_uint sequence;
    unsigned waiting;
};

/* The caller holds owner_lock and defers Linux notifications throughout each
 * helper. Register before unlocking: publication between unlock and futex wait
 * changes the expected sequence, so the waiter cannot sleep past that event.
 * Both domain entry and idle-owner waits must register, not just idle owners.
 */
static inline unsigned ph_owner_wait_begin(struct ph_owner_wait *wait) {
    unsigned observed = atomic_load_explicit(&wait->sequence, memory_order_acquire);
    ++wait->waiting;
    return observed;
}

static inline void ph_owner_wait_end(struct ph_owner_wait *wait) {
    --wait->waiting;
}

static inline bool ph_owner_wait_publish(struct ph_owner_wait *wait) {
    /* Always publish, including without a sleeper. Only the native wake syscall
     * is conditional; a registered waiter may not have entered futex wait yet.
     */
    atomic_fetch_add_explicit(&wait->sequence, 1, memory_order_release);
    return wait->waiting != 0;
}

#endif
