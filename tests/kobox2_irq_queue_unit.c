/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../userland/kobox2_adapter/device_irq_queue.h"

int main(void) {
    struct ph_irq_queue queue;
    struct ph_irq_slot slots[4];
    struct kobox_linux_irq_event event = {0};
    uint64_t sequence = 0;
    unsigned int active = 99;
    assert(!ph_irq_queue_init(&queue, slots, 4, 2, &sequence));
    assert(!ph_irq_queue_reserve(&queue, 0, 2, 0));
    const uint64_t first = slots[0].route.cookie;
    const uint64_t second = slots[1].route.cookie;
    assert(first && second > first && sequence == second);
    assert(slots[0].masked && slots[1].masked);
    assert(!ph_irq_queue_ready_cpus(&queue));
    assert(!ph_irq_queue_raise(&queue, 0, first));
    assert(!ph_irq_queue_raise(&queue, 0, first));
    assert(!ph_irq_queue_active(&queue, 0, first, &active) && !active);
    assert(ph_irq_queue_next(&queue, 0, &event) == -EAGAIN);
    assert(!ph_irq_queue_mask(&queue, 0, first, 0));
    assert(ph_irq_queue_ready_cpus(&queue) == 1);
    assert(!ph_irq_queue_active(&queue, 0, first, &active) && active);
    assert(ph_irq_queue_quiesce(&queue, 0, first) == -EBUSY);
    assert(!ph_irq_queue_affinity(&queue, 0, first, 1));
    assert(ph_irq_queue_ready_cpus(&queue) == 2);
    assert(ph_irq_queue_next(&queue, 0, &event) == -EAGAIN); /* stale CPU doorbell */
    assert(!ph_irq_queue_next(&queue, 1, &event));
    assert(event.hwirq == 0 && event.cookie == first);
    assert(!ph_irq_queue_mask(&queue, 0, first, 1));
    assert(!ph_irq_queue_active(&queue, 0, first, &active) && active);
    assert(ph_irq_queue_release(&queue, 0, first) == -EBUSY);
    assert(ph_irq_queue_quiesce(&queue, 0, first) == -EBUSY);
    assert(!ph_irq_queue_raise(&queue, 0, first)); /* edge while handler executes */
    assert(!ph_irq_queue_affinity(&queue, 0, first, 0));
    assert(!ph_irq_queue_mask(&queue, 0, first, 0));
    assert(!ph_irq_queue_ready_cpus(&queue));
    assert(ph_irq_queue_next(&queue, 0, &event) == -EAGAIN);
    assert(!ph_irq_queue_ack(&queue, 0, first));
    assert(ph_irq_queue_ready_cpus(&queue) == 1);
    assert(!ph_irq_queue_next(&queue, 0, &event));
    assert(!ph_irq_queue_ack(&queue, 0, first));
    assert(ph_irq_queue_ack(&queue, 0, first) == -EINVAL);
    assert(ph_irq_queue_next(&queue, 0, &event) == -EAGAIN); /* coalesced, not replayed */

    assert(!ph_irq_queue_raise(&queue, 0, first));
    assert(!ph_irq_queue_mask(&queue, 0, first, 1));
    assert(!ph_irq_queue_quiesce(&queue, 0, first));
    assert(!ph_irq_queue_mask(&queue, 0, first, 0));
    assert(ph_irq_queue_next(&queue, 0, &event) == -EAGAIN);
    assert(!ph_irq_queue_mask(&queue, 0, first, 1));
    assert(!ph_irq_queue_release(&queue, 0, first));
    assert(!ph_irq_queue_reserve(&queue, 0, 1, 1));
    const uint64_t replacement = slots[0].route.cookie;
    assert(replacement > second);
    struct ph_irq_slot unchanged = slots[0];
    assert(ph_irq_queue_raise(&queue, 0, first) == -ESTALE);
    assert(ph_irq_queue_ack(&queue, 0, first) == -ESTALE);
    assert(ph_irq_queue_release(&queue, 0, first) == -ESTALE);
    assert(ph_irq_queue_quiesce(&queue, 0, first) == -ESTALE);
    assert(ph_irq_queue_mask(&queue, 0, first, 0) == -ESTALE);
    assert(ph_irq_queue_affinity(&queue, 0, first, 0) == -ESTALE);
    assert(!memcmp(&unchanged, &slots[0], sizeof(unchanged)));

    const uint64_t saved_sequence = sequence;
    assert(ph_irq_queue_reserve(&queue, 0, 4, 1) == -EBUSY);
    assert(!slots[2].route.cookie && !slots[3].route.cookie && sequence == saved_sequence);
    assert(ph_irq_queue_reserve(&queue, 3, 2, 0) == -EINVAL);
    assert(ph_irq_queue_reserve(&queue, 2, 1, 2) == -EINVAL);
    assert(ph_irq_queue_mask(&queue, 0, replacement, 2) == -EINVAL);
    assert(ph_irq_queue_active(&queue, 0, replacement, NULL) == -EINVAL);
    assert(ph_irq_queue_next(&queue, 2, &event) == -EINVAL);
    assert(ph_irq_queue_next(&queue, 0, NULL) == -EINVAL);
    assert(!ph_irq_queue_release(&queue, 0, replacement));
    assert(!ph_irq_queue_release(&queue, 1, second));

    /* A replacement port uses the same durable counter, never starts at one. */
    assert(!ph_irq_queue_init(&queue, slots, 4, 2, &sequence));
    for (unsigned round = 0; round < 4096; ++round) {
        assert(!ph_irq_queue_reserve(&queue, 0, 4, round & 1));
        for (unsigned i = 0; i < 4; ++i) {
            const uint64_t cookie = slots[i].route.cookie;
            assert(cookie > replacement);
            assert(!ph_irq_queue_mask(&queue, i, cookie, 0));
            assert(!ph_irq_queue_raise(&queue, i, cookie));
        }
        for (unsigned i = 0; i < 4; ++i) {
            assert(!ph_irq_queue_next(&queue, round & 1, &event));
            assert(event.hwirq == i); /* round-robin scanning preserves fairness */
            assert(!ph_irq_queue_ack(&queue, event.hwirq, event.cookie));
            assert(!ph_irq_queue_mask(&queue, event.hwirq, event.cookie, 1));
            assert(!ph_irq_queue_release(&queue, event.hwirq, event.cookie));
        }
    }
    sequence = UINT64_MAX - 1;
    assert(ph_irq_queue_reserve(&queue, 0, 2, 0) == -EOVERFLOW);
    assert(sequence == UINT64_MAX - 1 && !slots[0].route.cookie);
    assert(!ph_irq_queue_reserve(&queue, 0, 1, 0));
    assert(slots[0].route.cookie == UINT64_MAX);
    assert(ph_irq_queue_reserve(&queue, 1, 1, 0) == -EOVERFLOW);
    puts("kobox2 IRQ routing-state unit: PASS (not native IRQ evidence)");
    return 0;
}
