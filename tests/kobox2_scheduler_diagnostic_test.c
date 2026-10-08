/* SPDX-License-Identifier: MIT */
#include "../userland/kobox2_adapter/scheduler_diagnostic.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char *const fields[] = {" owner_available=", " owner=", " owner_fd=",
    " irq_depth=", " owner_waiting=", " handoff_released=", " mask=", " deferred=",
    " permit=", " frame_valid=", " frame_sequence=", " rip=", " rsp=", " frame_mask=",
    " wait_word=", " wait_expected=", " timer_lock=", " timer_arms=", " timer_expiries=",
    " notify_enter=", " notify_return=", " callback_enter=", " callback_return=",
    " sequence=", " pending_tick=", " pending_resched=", " pending_call=",
    " pending_device=", " pending_control=", " pending_vm=", " owner_sequence=",
    " rdi=", " rsi=", " rax=", " rbx=", " stack_valid=",
    " stack0=", " stack1=", " stack2=", " stack3=",
    " stack4=", " stack5=", " stack6=", " stack7=",
    " stack8=", " stack9=", " stack10=", " stack11=",
    " stack12=", " stack13=", " stack14=", " stack15="};
static unsigned seen[sizeof(fields) / sizeof(*fields)], calls, rejected;
static size_t longest;

static int mock_log(const char *record, size_t bytes) {
    /* Faithful rejection is essential: the old formatter's 1 KiB call could
     * appear to work in a permissive mock while native LOG silently failed. */
    if (bytes > 256) { ++rejected; return 1; }
    assert(bytes && record[bytes - 1] == '\n');
    char copy[257];
    memcpy(copy, record, bytes);
    copy[bytes] = 0;
    assert(strncmp(copy, "[PH_SCHED] cpu=0xffffffffffffffff ns=0xffffffffffffffff group=", 58) == 0);
    for (size_t i = 0; i < sizeof(fields) / sizeof(*fields); ++i) {
        const char *position = strstr(copy, fields[i]);
        if (position) {
            assert(strncmp(position + strlen(fields[i]), "0xffffffffffffffff", 18) == 0);
            ++seen[i];
        }
    }
    if (bytes > longest) longest = bytes;
    ++calls;
    return 0;
}

static int failed_log(const char *record, size_t bytes) {
    (void)record;
    assert(bytes <= 256);
    return 6;
}

static void stack_bounds(void) {
    uint64_t stack[32], fault[32], words[PH_SCHED_STACK_WORDS];
    for (unsigned i = 0; i < 32; ++i) { stack[i] = 10+i; fault[i] = 100+i; }
    assert(ph_sched_stack_words((uintptr_t)stack, stack, fault, sizeof(stack), words));
    for (unsigned i = 0; i < PH_SCHED_STACK_WORDS; ++i) assert(words[i] == stack[i]);
    assert(ph_sched_stack_words((uintptr_t)(stack+16), stack, fault, sizeof(stack), words));
    for (unsigned i = 0; i < PH_SCHED_STACK_WORDS; ++i) assert(words[i] == stack[16+i]);
    assert(ph_sched_stack_words((uintptr_t)fault, NULL, fault, sizeof(fault), words));
    for (unsigned i = 0; i < PH_SCHED_STACK_WORDS; ++i) assert(words[i] == fault[i]);
    assert(!ph_sched_stack_words((uintptr_t)(stack+17), stack, NULL, sizeof(stack), words));
    assert(!ph_sched_stack_words((uintptr_t)stack-8, stack, NULL, sizeof(stack), words));
    assert(!ph_sched_stack_words((uintptr_t)stack+1, stack, NULL, sizeof(stack), words));
    assert(!ph_sched_stack_words((uintptr_t)stack, NULL, NULL, sizeof(stack), words));
    assert(!ph_sched_stack_words((uintptr_t)stack, stack, NULL, 127, words));
    /* Invalid extents must be rejected before any arbitrary dereference. */
    assert(!ph_sched_stack_words(UINTPTR_MAX-7, (void *)(UINTPTR_MAX-7), NULL, 128, words));
    assert(!ph_sched_stack_words(0, stack, fault, sizeof(stack), words));
    for (unsigned i = 0; i < PH_SCHED_STACK_WORDS; ++i) assert(words[i] == 0);
}

int main(void) {
    stack_bounds();
    uint64_t values[sizeof(fields) / sizeof(*fields)];
    for (size_t i = 0; i < sizeof(values) / sizeof(*values); ++i) values[i] = UINT64_MAX;
    assert(ph_sched_records(UINT64_MAX, UINT64_MAX, fields, values,
        sizeof(values) / sizeof(*values), mock_log) == 0);
    for (size_t i = 0; i < sizeof(seen) / sizeof(*seen); ++i) assert(seen[i] == 1);
    assert(calls > 1 && longest <= 256 && rejected == 0);
    char overlong[257] = {0};
    assert(mock_log(overlong, sizeof(overlong)) == 1 && rejected == 1);
    assert(ph_sched_records(UINT64_MAX, UINT64_MAX, fields, values,
        sizeof(values) / sizeof(*values), failed_log) == 6);
    printf("scheduler diagnostic: %zu UINT64_MAX fields preserved, %u records, maximum %zu bytes, native rejection propagated / owned-stack bounds PASS\n",
        sizeof(fields) / sizeof(*fields) + 2, calls, longest);
    return 0;
}
