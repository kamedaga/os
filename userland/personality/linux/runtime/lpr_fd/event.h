#pragma once
#include "table.h"
#include <unixd/poll.h>

enum { LPR_EVENT_COUNTER_BYTES = 4096u };

static inline uint64_t *lpr_event_counter(const lpr_event_backend_t *event)
{
    if (event->subtype == LPR_EVENT_BACKEND_EVENTFD && event->counter_map)
        return (uint64_t *)(uintptr_t)event->counter_map;
    return (uint64_t *)(uintptr_t)&event->counter;
}

int lpr_event_counter_create(lpr_event_backend_t *event, uint64_t initial);
int lpr_event_counter_map(lpr_event_backend_t *event);
int64_t lpr_event_counter_unmap(lpr_event_backend_t *event);
int64_t lpr_event_counter_close(lpr_event_backend_t *event);

/* Private words in the existing shared counter page. Progress must follow the
 * OFD through dup/fork/SCM_RIGHTS, rather than a process-local descriptor. */
static inline void lpr_event_counter_publish(lpr_event_backend_t *event, uint32_t events)
{
    if (!event->counter_map) return;
    uint64_t *state = (uint64_t *)(uintptr_t)event->counter_map;
    if (events & 1u) __atomic_add_fetch(&state[1], 1, __ATOMIC_RELEASE);
    if (events & 4u) __atomic_add_fetch(&state[2], 1, __ATOMIC_RELEASE);
}

static inline uint32_t lpr_event_counter_poll_sequence(
    const lpr_event_backend_t *event, uint32_t events, struct unix_poll_sequence *sequence)
{
    *sequence = (struct unix_poll_sequence){0};
    if (event->counter_map) {
        const uint64_t *state = (const uint64_t *)(uintptr_t)event->counter_map;
        sequence->source = 1;
        /* Sample progress before level, so new progress cannot be consumed
         * together with a level sampled before the corresponding counter CAS. */
        sequence->read = __atomic_load_n(&state[1], __ATOMIC_ACQUIRE);
        sequence->write = __atomic_load_n(&state[2], __ATOMIC_ACQUIRE);
    }
    uint64_t counter = __atomic_load_n(lpr_event_counter(event), __ATOMIC_ACQUIRE);
    return ((counter ? 1u : 0u) | (counter < UINT64_MAX - 1u ? 4u : 0u)) & events;
}
