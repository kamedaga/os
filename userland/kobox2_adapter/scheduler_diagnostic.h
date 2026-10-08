/* SPDX-License-Identifier: MIT */
#ifndef PH_SCHEDULER_DIAGNOSTIC_H
#define PH_SCHEDULER_DIAGNOSTIC_H

#include <stddef.h>
#include <stdint.h>

/* Existing native LOG accepts at most 256 bytes. Keep every observation
 * independently labelled: concurrent Linux printk can interleave records. */
#define PH_SCHED_LOG_BYTES 256u
#define PH_SCHED_STACK_WORDS 16u

/* Only the interrupted task reads its own immutable stack allocations.
 * A native signal runs below its saved RSP, so these bounded words remain
 * owned by the suspended caller. They are raw words, not a generic unwind. */
static inline int ph_sched_stack_words(uint64_t rsp, const void *stack,
    const void *fault_stack, size_t stack_bytes,
    uint64_t words[PH_SCHED_STACK_WORDS]) {
    for (unsigned i = 0; i < PH_SCHED_STACK_WORDS; ++i) words[i] = 0;
    const size_t bytes = PH_SCHED_STACK_WORDS * sizeof(uint64_t);
    if (rsp > UINTPTR_MAX || (rsp & (sizeof(uint64_t) - 1)) ||
        stack_bytes < bytes) return 0;
    const void *bases[] = {stack, fault_stack};
    for (unsigned i = 0; i < sizeof(bases) / sizeof(*bases); ++i) {
        uintptr_t base = (uintptr_t)bases[i];
        if (!base || base > UINTPTR_MAX - stack_bytes || rsp < base ||
            rsp - base > stack_bytes - bytes) continue;
        const volatile uint64_t *saved = (const volatile uint64_t *)(uintptr_t)rsp;
        for (unsigned j = 0; j < PH_SCHED_STACK_WORDS; ++j) words[j] = saved[j];
        return 1;
    }
    return 0;
}

static inline int ph_sched_field(char *buffer, size_t *length,
    const char *label, uint64_t value) {
    size_t label_bytes = 0;
    while (label[label_bytes]) ++label_bytes;
    if (label_bytes > PH_SCHED_LOG_BYTES - 19 ||
        *length > PH_SCHED_LOG_BYTES - label_bytes - 19) return -1;
    for (size_t i = 0; i < label_bytes; ++i) buffer[(*length)++] = label[i];
    buffer[(*length)++] = '0';
    buffer[(*length)++] = 'x';
    for (int shift = 60; shift >= 0; shift -= 4)
        buffer[(*length)++] = "0123456789abcdef"[(value >> shift) & 15];
    return 0;
}

static inline int ph_sched_records(uint64_t cpu, uint64_t now,
    const char *const *labels, const uint64_t *values, size_t count,
    int (*write_record)(const char *, size_t)) {
    size_t cursor = 0;
    uint64_t group = 0;
    while (cursor < count) {
        char buffer[PH_SCHED_LOG_BYTES] = "[PH_SCHED]";
        size_t length = sizeof("[PH_SCHED]") - 1;
        if (ph_sched_field(buffer, &length, " cpu=", cpu) ||
            ph_sched_field(buffer, &length, " ns=", now) ||
            ph_sched_field(buffer, &length, " group=", group++)) return -1;
        size_t first = cursor;
        while (cursor < count && !ph_sched_field(buffer, &length,
                labels[cursor], values[cursor])) ++cursor;
        if (cursor == first) return -1;
        buffer[length++] = '\n';
        int status = write_record(buffer, length);
        if (status) return status;
    }
    return 0;
}

#endif
