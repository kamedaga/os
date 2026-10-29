#pragma once
#include "table.h"

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
