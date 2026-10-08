/* SPDX-License-Identifier: MIT */
/* The adapter's real fiber switch and initial frame on the host. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#define PH_TLS_TEST_HOOKS 1
#include "../userland/kobox2_adapter/host.h"

static struct ph_task thread;
static struct ph_fiber main_fiber;
static struct ph_fiber *fiber;
static unsigned starts;
static uint64_t start_mxcsr, start_cw, start_rsp;
struct ph_task *ph_test_current_task(void) { return &thread; }
#define ph_current_task ph_test_current_task
#include "../userland/kobox2_adapter/native.c"

uint64_t probe_switch(uint64_t *save_rsp, uint64_t next_rsp, uint64_t *out);
void clobber_and_switch(uint64_t *save_rsp, uint64_t next_rsp);

_Noreturn void ph_fail(const char *file, unsigned line, uint64_t result) {
    fprintf(stderr, "%s:%u result=%llu\n", file, line, (unsigned long long)result);
    abort();
}
void ph_exception(struct pacha_native_fault_frame *frame) { (void)frame; abort(); }
void ph_dispatch_notifications(void) { abort(); }
void *ph_alloc(size_t size) {
    void *memory = aligned_alloc(PH_PAGE_SIZE, (size + PH_PAGE_SIZE - 1) & ~(PH_PAGE_SIZE - 1));
    assert(memory);
    /* Native ph_alloc uses fresh anonymous mappings; the fixture must retain
     * their zero-fill guarantee for origin/bound, not depend on malloc luck. */
    memset(memory, 0, size);
    return memory;
}
void ph_free(void *base, size_t size) { (void)size; free(base); }
long pacha_syscall3(uint64_t nr, uint64_t a, uint64_t b, uint64_t c) {
    assert(nr == PACHA_VM_SYSCALL_MPROTECT && a && b == PH_PAGE_SIZE && !c);
    return 0;
}

/* Origin-thread join paths; no origin thread exists in this test. */
long pacha_syscall1(uint64_t nr, uint64_t a) { (void)nr; (void)a; abort(); }
long pacha_syscall2(uint64_t nr, uint64_t a, uint64_t b) { (void)nr; (void)a; (void)b; abort(); }
long pacha_syscall4(uint64_t nr, uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)nr; (void)a; (void)b; (void)c; (void)d;
    abort();
}

static void *entry(void *argument) {
    uint32_t mxcsr;
    uint16_t cw;
    char marker;

    __asm__ volatile("stmxcsr %0; fnstcw %1" : "=m"(mxcsr), "=m"(cw));
    start_mxcsr = mxcsr;
    start_cw = cw;
    start_rsp = (uintptr_t)&marker;
    assert(argument == &starts && ph_current_fiber() == fiber);
    for (;;) {
        ++starts;
        fiber->state = PH_FIBER_SWITCHED;
        clobber_and_switch(&fiber->rsp, main_fiber.rsp);
    }
}

int main(void) {
    thread.self = &thread;
    thread.fiber = &main_fiber;
    fiber = ph_fiber_create(entry, &starts);
    assert(fiber->state == PH_FIBER_NEW && !(fiber->rsp & 15));
    assert(fiber->rsp - (uintptr_t)fiber->stack < fiber->stack_size);
    for (unsigned round = 0; round < 1000; ++round) {
        uint64_t out[8];

        /* ph_fiber_main requires the switched-to fiber to be current. */
        thread.fiber = fiber;
        fiber->state = PH_FIBER_RUNNING;
        probe_switch(&main_fiber.rsp, fiber->rsp, out);
        thread.fiber = &main_fiber;
        assert(starts == round + 1);
        assert(out[0] == 0x1111111111111111 && out[1] == 0x2222222222222222);
        assert(out[2] == 0x3333333333333333 && out[3] == 0x4444444444444444);
        assert(out[4] == 0x5555555555555555 && out[5] == 0x6666666666666666);
        /* MXCSR may keep sticky exception flags; compare the control bits. */
        assert((out[6] & ~UINT64_C(0x3f)) == 0x7f80 && out[7] == 0x0c7f);
    }
    /* A NEW fiber starts with the ABI default control words, on its own stack. */
    assert((start_mxcsr & ~UINT64_C(0x3f)) == 0x1f80 && start_cw == 0x037f);
    assert(start_rsp - (uintptr_t)fiber->stack < fiber->stack_size);
    fiber->state = PH_FIBER_DEAD;
    ph_fiber_destroy(fiber);
    puts("kobox2 fiber switch: PASS callee-saved GPRs, MXCSR/x87 CW, initial frame, 1000 round trips");
    return 0;
}
