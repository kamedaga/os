/* SPDX-License-Identifier: MIT */
#include "device_irq_queue.h"
#include <errno.h>
#include <string.h>

static int ready(const struct ph_irq_slot *slot) {
    return slot->route.cookie && slot->pending && !slot->masked && !slot->servicing;
}

int ph_irq_queue_init(struct ph_irq_queue *queue, struct ph_irq_slot *slots,
    size_t capacity, uint32_t cpu_count, uint64_t *cookie_sequence) {
    if (!queue || !slots || !capacity || capacity > UINT32_MAX ||
        capacity > SIZE_MAX / sizeof(*slots) || !cpu_count || cpu_count > 64 ||
        !cookie_sequence) return -EINVAL;
    memset(slots, 0, capacity * sizeof(*slots));
    *queue = (struct ph_irq_queue){
        .slots = slots, .capacity = capacity, .cpu_count = cpu_count,
        .cookie_sequence = cookie_sequence,
    };
    return 0;
}

int ph_irq_queue_reserve(struct ph_irq_queue *queue, uint32_t index,
    uint32_t count, uint32_t cpu) {
    if (!count || index >= queue->capacity || count > queue->capacity - index ||
        cpu >= queue->cpu_count) return -EINVAL;
    if (count > UINT64_MAX - *queue->cookie_sequence) return -EOVERFLOW;
    for (uint32_t i = 0; i < count; ++i)
        if (queue->slots[index + i].route.cookie) return -EBUSY;
    for (uint32_t i = 0; i < count; ++i) {
        queue->slots[index + i] = (struct ph_irq_slot){
            .route = {.hwirq = index + i, .cookie = ++*queue->cookie_sequence},
            .cpu = cpu, .masked = 1,
        };
    }
    return 0;
}

struct ph_irq_slot *ph_irq_queue_lookup(struct ph_irq_queue *queue,
    uint64_t hwirq, uint64_t cookie) {
    if (!cookie || hwirq >= queue->capacity) return NULL;
    struct ph_irq_slot *slot = &queue->slots[hwirq];
    return slot->route.cookie == cookie ? slot : NULL;
}

int ph_irq_queue_release(struct ph_irq_queue *queue, uint64_t hwirq, uint64_t cookie) {
    struct ph_irq_slot *slot = ph_irq_queue_lookup(queue, hwirq, cookie);
    if (!slot) return -ESTALE;
    if (!slot->masked || slot->servicing) return -EBUSY;
    /* Only the port's completed native retirement authorizes this call. */
    *slot = (struct ph_irq_slot){0};
    return 0;
}

int ph_irq_queue_mask(struct ph_irq_queue *queue, uint64_t hwirq, uint64_t cookie,
    unsigned int masked) {
    struct ph_irq_slot *slot = ph_irq_queue_lookup(queue, hwirq, cookie);
    if (masked > 1) return -EINVAL;
    if (!slot) return -ESTALE;
    slot->masked = masked;
    return 0;
}

int ph_irq_queue_raise(struct ph_irq_queue *queue, uint64_t hwirq, uint64_t cookie) {
    struct ph_irq_slot *slot = ph_irq_queue_lookup(queue, hwirq, cookie);
    if (!slot) return -ESTALE;
    slot->pending = 1;
    return 0;
}

int ph_irq_queue_ack(struct ph_irq_queue *queue, uint64_t hwirq, uint64_t cookie) {
    struct ph_irq_slot *slot = ph_irq_queue_lookup(queue, hwirq, cookie);
    if (!slot) return -ESTALE;
    if (!slot->servicing) return -EINVAL;
    slot->servicing = 0;
    return 0;
}

int ph_irq_queue_quiesce(struct ph_irq_queue *queue, uint64_t hwirq, uint64_t cookie) {
    struct ph_irq_slot *slot = ph_irq_queue_lookup(queue, hwirq, cookie);
    if (!slot) return -ESTALE;
    if (!slot->masked || slot->servicing) return -EBUSY;
    /* Native source/CPU drain must succeed before dropping queued events. */
    slot->pending = 0;
    return 0;
}

int ph_irq_queue_affinity(struct ph_irq_queue *queue, uint64_t hwirq, uint64_t cookie,
    uint32_t cpu) {
    struct ph_irq_slot *slot = ph_irq_queue_lookup(queue, hwirq, cookie);
    if (cpu >= queue->cpu_count) return -EINVAL;
    if (!slot) return -ESTALE;
    slot->cpu = cpu;
    return 0;
}

int ph_irq_queue_active(struct ph_irq_queue *queue, uint64_t hwirq, uint64_t cookie,
    unsigned int *active) {
    struct ph_irq_slot *slot = ph_irq_queue_lookup(queue, hwirq, cookie);
    if (!active) return -EINVAL;
    if (!slot) return -ESTALE;
    *active = slot->servicing || (slot->pending && !slot->masked);
    return 0;
}

int ph_irq_queue_next(struct ph_irq_queue *queue, uint32_t cpu,
    struct kobox_linux_irq_event *event) {
    if (!event || cpu >= queue->cpu_count) return -EINVAL;
    for (size_t checked = 0; checked < queue->capacity; ++checked) {
        const size_t index = queue->cursor;
        queue->cursor = (index + 1) % queue->capacity;
        struct ph_irq_slot *slot = &queue->slots[index];
        if (slot->cpu != cpu || !ready(slot)) continue;
        slot->pending = 0;
        slot->servicing = 1;
        *event = (struct kobox_linux_irq_event){
            .hwirq = slot->route.hwirq, .cookie = slot->route.cookie,
        };
        return 0;
    }
    return -EAGAIN;
}

uint64_t ph_irq_queue_ready_cpus(const struct ph_irq_queue *queue) {
    uint64_t cpus = 0;
    for (size_t i = 0; i < queue->capacity; ++i)
        if (ready(&queue->slots[i])) cpus |= UINT64_C(1) << queue->slots[i].cpu;
    return cpus;
}
