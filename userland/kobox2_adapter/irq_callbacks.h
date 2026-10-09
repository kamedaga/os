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

/* Before this CPU delivers into Linux: its IRQ state moves to Linux's
 * per-CPU words, which Linux then changes with single instructions. */
static int irq_bind(uint32_t index, void *state, void *deferred) {
    uint64_t mask;
    int result;

    PH_CHECK(index < PH_CPU_COUNT);
    /* No upcall may record a deferral between copying and switching. */
    PH_OK(ph_notifications_save(&mask));
    result = (int)kobox_machine_domain_bind_irq(&cpus[index].domain, state, deferred);
    PH_OK(ph_notifications_restore(mask));
    return result;
}

/* Linux enabled IRQs and found notifications deferred while disabled. The
 * preserving dispatcher finds this thread's current CPU under its own mask,
 * so a migration between Linux's check and this call cannot misdirect it. */
static KOBOX_GPR_LEAF int irq_deliver(void) {
    PH_OK(ph_notifications_dispatch_preserved());
    return 0;
}

static KOBOX_GPR_LEAF uint8_t irq_disabled(uint32_t index) {
    PH_CHECK(index < PH_CPU_COUNT);
    return atomic_load(cpus[index].domain.irq_depth) != 0;
}

#endif
