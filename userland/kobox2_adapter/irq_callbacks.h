/* SPDX-License-Identifier: MIT */
#ifndef PACHA_KOBOX_IRQ_CALLBACKS_H
#define PACHA_KOBOX_IRQ_CALLBACKS_H

#include "arch/x86_64/fp_entry.h"

/* The operations table calls these with Linux's FP bank live. Only integer
 * CPU/domain checks may precede a dispatch; arbitrary C and migration run
 * inside the stack-local preserving entry shared with notification unmask.
 * Keep these entries shared with the focused full-bank contract test. */
static KOBOX_GPR_LEAF int irq_disable(uint32_t index) {
    PH_CHECK(index < PH_CPU_COUNT && ph_current_cpu() == (int)index);
    return (int)kobox_machine_domain_irq_disable(&cpus[index].domain);
}

static KOBOX_GPR_LEAF int irq_enable(uint32_t index) {
    bool dispatch_pending;

    PH_CHECK(index < PH_CPU_COUNT && ph_current_cpu() == (int)index);
    PH_OK(kobox_machine_domain_irq_enable(&cpus[index].domain, &dispatch_pending));
    /* The surrounding IRQ transaction masks native upcalls, so this domain
     * stays owned through the check. Publication after an empty check still
     * sends the owner a signal (or wakes its sequence wait); outer unmask or
     * the next owner entry drains it. Do not clear deferred hints here.
     * Stop must enter the preserving dispatcher even without pending counts.
     * Only this all-GPR empty path skips FP work; Linux entry retains it. */
    if (dispatch_pending &&
        (atomic_load_explicit(&cpus[index].domain.stop_requested, memory_order_acquire) ||
         kobox_machine_domain_pending(&cpus[index].domain)))
        PH_OK(ph_notifications_dispatch_preserved());
    return 0;
}

static KOBOX_GPR_LEAF uint8_t irq_disabled(uint32_t index) {
    PH_CHECK(index < PH_CPU_COUNT);
    return atomic_load(&cpus[index].domain.irq_depth) != 0;
}

#endif
