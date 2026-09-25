/* SPDX-License-Identifier: MIT */
#ifndef PACHA_KOBOX_DEVICE_IRQ_QUEUE_H
#define PACHA_KOBOX_DEVICE_IRQ_QUEUE_H

#include "boot/irq_host.h"

struct ph_irq_slot {
    struct kobox_linux_irq_route route;
    uint32_t cpu;
    unsigned int masked;
    unsigned int pending;
    unsigned int servicing;
};

struct ph_irq_queue {
    struct ph_irq_slot *slots;
    size_t capacity;
    size_t cursor;
    uint32_t cpu_count;
    uint64_t *cookie_sequence;
};

/* Pure routing state, with no hardware or Linux calls. The native port holds
 * its lock and masks native notifications around every access. Slots and the
 * cookie sequence are caller-owned. The sequence must survive port teardown
 * and replacement; failure/rollback must not rewind it. One queue owner may
 * mutate that sequence at a time. Initialize before admitting routes; never
 * reinitialize a queue with live slots. Native route setup is committed before the
 * port exposes reserved slots or returns their identities to Linux.
 *
 * Pending hardware edges coalesce, like an interrupt-controller pending bit.
 * A taken event remains active until ack; mask cannot retract it. Pending
 * events migrate on affinity changes, while an executing handler stays active.
 * Send ready_cpus doorbells after releasing the port lock. Stale doorbells are
 * harmless: next rechecks current CPU, mask, servicing and cookie ownership. */
int ph_irq_queue_init(struct ph_irq_queue *queue, struct ph_irq_slot *slots,
    size_t capacity, uint32_t cpu_count, uint64_t *cookie_sequence);
int ph_irq_queue_reserve(struct ph_irq_queue *queue, uint32_t index,
    uint32_t count, uint32_t cpu);
struct ph_irq_slot *ph_irq_queue_lookup(struct ph_irq_queue *queue,
    uint64_t hwirq, uint64_t cookie);
int ph_irq_queue_release(struct ph_irq_queue *queue, uint64_t hwirq, uint64_t cookie);
int ph_irq_queue_mask(struct ph_irq_queue *queue, uint64_t hwirq, uint64_t cookie,
    unsigned int masked);
int ph_irq_queue_raise(struct ph_irq_queue *queue, uint64_t hwirq, uint64_t cookie);
int ph_irq_queue_ack(struct ph_irq_queue *queue, uint64_t hwirq, uint64_t cookie);
int ph_irq_queue_quiesce(struct ph_irq_queue *queue, uint64_t hwirq, uint64_t cookie);
int ph_irq_queue_affinity(struct ph_irq_queue *queue, uint64_t hwirq, uint64_t cookie,
    uint32_t cpu);
int ph_irq_queue_active(struct ph_irq_queue *queue, uint64_t hwirq, uint64_t cookie,
    unsigned int *active);
int ph_irq_queue_next(struct ph_irq_queue *queue, uint32_t cpu,
    struct kobox_linux_irq_event *event);
uint64_t ph_irq_queue_ready_cpus(const struct ph_irq_queue *queue);

#endif
