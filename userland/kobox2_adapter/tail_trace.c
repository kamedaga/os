/* SPDX-License-Identifier: MIT */
#include "host.h"
#include "tail_trace.h"
#include "scheduler_diagnostic.h"

/* Diagnostic storage is append-only: a callback can outlive a window or
 * migrate, so resetting/reusing a slot at each console marker would race it. */
static struct tail_event {
    uint64_t begin, end, object;
    unsigned kind, cpu;
    atomic_uint ready;
    bool dumped;
} events[2048];
static atomic_uint enabled, count, dropped;
static atomic_ullong window_start;

static inline KOBOX_GPR_LEAF uint64_t ticks(void) {
    uint32_t lo, hi;
    __asm__ volatile("lfence; rdtsc; lfence" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | lo;
}

KOBOX_GPR_LEAF uint64_t ph_tail_begin(void) {
    return atomic_load_explicit(&enabled, memory_order_relaxed) ? ticks() : 0;
}

KOBOX_GPR_LEAF void ph_tail_end(unsigned kind, uintptr_t object, uint64_t begin) {
    if (!begin) return;
    uint64_t end = ticks();
    /* Raw TSC threshold, not CPU cycles or assumed microseconds. Offline
     * analysis calibrates it against each recorded FS request's wall time. */
    if ((kind <= 2 && end - begin < 200000) || !atomic_load_explicit(&enabled, memory_order_acquire) ||
        begin < atomic_load_explicit(&window_start, memory_order_relaxed)) return;
    unsigned slot = atomic_fetch_add_explicit(&count, 1, memory_order_relaxed);
    if (slot >= sizeof(events) / sizeof(events[0])) {
        atomic_fetch_add_explicit(&dropped, 1, memory_order_relaxed);
        return;
    }
    events[slot].begin = begin;
    events[slot].end = end;
    events[slot].object = object;
    events[slot].kind = kind;
    events[slot].cpu = (unsigned)ph_current_cpu();
    atomic_store_explicit(&events[slot].ready, 1, memory_order_release);
}

static bool contains(const char *text, size_t length, const char *marker) {
    size_t n = strlen(marker);
    for (size_t i = 0; i + n <= length; i++)
        if (!memcmp(text + i, marker, n)) return true;
    return false;
}

void ph_tail_console(const char *text, size_t length) {
    /* Existing benchmark console markers delimit isolated pre-READY work;
     * this avoids adding a production host callback or transport ABI. */
    if (contains(text, length, "FS_TAIL_WINDOW begin=1")) {
        atomic_store_explicit(&window_start, ticks(), memory_order_relaxed);
        atomic_store_explicit(&enabled, 1, memory_order_release);
    } else if (contains(text, length, "FS_TAIL_WINDOW begin=0")) {
        atomic_store_explicit(&enabled, 0, memory_order_release);
        unsigned n = atomic_load_explicit(&count, memory_order_acquire);
        for (unsigned i = 0; i < n && i < sizeof(events) / sizeof(events[0]); i++) {
            struct tail_event *e = &events[i];
            if (e->dumped || !atomic_load_explicit(&e->ready, memory_order_acquire)) continue;
            e->dumped = true;
            char line[PH_SCHED_LOG_BYTES] = "PH_TAIL";
            size_t used = sizeof("PH_TAIL") - 1;
            PH_CHECK(!ph_sched_field(line, &used, " kind=", e->kind));
            PH_CHECK(!ph_sched_field(line, &used, " begin=", e->begin));
            PH_CHECK(!ph_sched_field(line, &used, " end=", e->end));
            PH_CHECK(!ph_sched_field(line, &used, " object=", e->object));
            PH_CHECK(!ph_sched_field(line, &used, " cpu=", e->cpu));
            line[used++] = '\n';
            PH_OK(pacha_syscall2(1, (uintptr_t)line, used));
        }
        char line[PH_SCHED_LOG_BYTES] = "PH_TAIL_STATUS";
        size_t used = sizeof("PH_TAIL_STATUS") - 1;
        PH_CHECK(!ph_sched_field(line, &used, " count=", n));
        PH_CHECK(!ph_sched_field(line, &used, " dropped=", atomic_load(&dropped)));
        line[used++] = '\n';
        PH_OK(pacha_syscall2(1, (uintptr_t)line, used));
    }
}
