/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef TEST_NATIVE
/* No native FS here: thread words go through test_current_task(). */
#define PH_TLS_TEST_HOOKS 1
#include "../userland/kobox2_adapter/host.h"
static struct ph_task task, other, *selected = &task;
static struct ph_fiber task_fiber, other_fiber;
static struct ph_task *test_current_task(void) { return selected; }
struct ph_task *ph_test_current_task(void) { return test_current_task(); }
#define ph_current_task test_current_task
#include "../userland/kobox2_adapter/native.c"
#else
#include "../kobox2/linux-sandbox/kobox/host/posix/host.h"
#endif

struct fp_bank { unsigned char bytes[512]; } __attribute__((aligned(16)));
static struct fp_bank seed, nested_seed, clobber;
static uint64_t saved_mask;
static unsigned deliveries;
extern int notification_fp_invoke(int (*fn)(uint64_t), uint64_t arg,
                                  const struct fp_bank *, struct fp_bank *, struct fp_bank *);

static void make_bank(struct fp_bank *bank, unsigned salt)
{
    memset(bank, 0, sizeof(*bank));
    /* All eight x87 slots are valid 80-bit normal numbers, TOP=0. */
    bank->bytes[0] = 0x7f;
    bank->bytes[1] = 0x0b;
    bank->bytes[2] = 0x25;
    bank->bytes[4] = 0xff;
    bank->bytes[6] = (unsigned char)salt;
    bank->bytes[7] = 1;
    bank->bytes[8] = (unsigned char)salt;
    bank->bytes[9] = 0x56;
    bank->bytes[10] = 0x12;
    bank->bytes[16] = (unsigned char)salt;
    bank->bytes[17] = 0x67;
    bank->bytes[18] = 0x23;
    bank->bytes[24] = 0xa5;
    bank->bytes[25] = 0x7f;
    for (unsigned i = 0; i < 8; ++i) {
        bank->bytes[32 + i * 16] = (unsigned char)(salt + i);
        bank->bytes[39 + i * 16] = 0x80;
        bank->bytes[40 + i * 16] = 0xff;
        bank->bytes[41 + i * 16] = 0x3f;
    }
    for (unsigned i = 160; i < 416; ++i)
        bank->bytes[i] = (unsigned char)(salt + i);
}

static void same_bank(const struct fp_bank *a, const struct fp_bank *b)
{
    /* Reserved FXSAVE bytes are unspecified. Check every architectural bank
     * field, all eight x87 payloads, and all sixteen full-width XMM values. */
    assert(!memcmp(a->bytes, b->bytes, 5));
    assert(!memcmp(a->bytes + 6, b->bytes + 6, 18));
    assert(!memcmp(a->bytes + 24, b->bytes + 24, 4));
    for (unsigned i = 0; i < 8; ++i)
        assert(!memcmp(a->bytes + 32 + i * 16, b->bytes + 32 + i * 16, 10));
    assert(!memcmp(a->bytes + 160, b->bytes + 160, 256));
}

#ifdef TEST_NATIVE
static __attribute__((noinline)) KOBOX_GPR_LEAF int save_call(uint64_t arg)
{
    (void)arg;
    return ph_notifications_save(&saved_mask);
}
static __attribute__((noinline)) KOBOX_GPR_LEAF int restore_call(uint64_t arg)
{
    return ph_notifications_restore(arg);
}
_Noreturn void ph_fail(const char *file, unsigned line, uint64_t result)
{
    fprintf(stderr, "%s:%u result=%llu\n", file, line, (unsigned long long)result);
    abort();
}
#else
static int save_call(uint64_t arg)
{
    return kobox_posix_notifications_save(arg ? &saved_mask : NULL);
}
static int restore_call(uint64_t arg)
{
    return kobox_posix_notifications_restore(arg);
}
#endif

static int checked_call(int (*fn)(uint64_t), uint64_t arg,
                        const struct fp_bank *input)
{
    struct fp_bank observed = {0};
    struct fp_bank installed = {0};
    int result = notification_fp_invoke(fn, arg, input, &observed, &installed);
    same_bank(input, &installed);
    same_bank(input, &observed);
    return result;
}

#ifdef TEST_NATIVE
void ph_dispatch_notifications(void)
{
    struct fp_bank entry = {0};
    asm volatile("fxsaveq %0" : "=m"(entry) : : "memory");
    assert(entry.bytes[0] == 0x7f && entry.bytes[1] == 3 && !entry.bytes[4]);
    assert(entry.bytes[24] == 0x80 && entry.bytes[25] == 0x1f);
    for (unsigned i = 160; i < 416; ++i)
        assert(!entry.bytes[i]);
    ++deliveries;
    if (deliveries == 1) {
        /* A nested callback owns its own stack bank, not a per-CPU scratch.
         * Change task identity and yield before its inner protected entry. */
        selected = &other;
        atomic_store(&other.notification_deferred, 1);
        assert(!checked_call(restore_call, 0, &nested_seed));
        selected = &task;
    }
    sched_yield();
#ifdef TEST_NATIVE_IRQ
    /* Model a logical CPU change while the preserving entry is suspended.
     * Its stack bank must survive; the enable caller must not reuse old CPU. */
    selected->fiber->logical_cpu ^= 1;
#endif
    asm volatile("fxrstorq %0" : : "m"(clobber) : "memory");
}
#else
static void notification_handler(int signo)
{
    assert(signo == SIGRTMIN);
    ++deliveries;
    uint64_t mask;
    assert(!kobox_posix_notifications_save(&mask));
    assert(!kobox_posix_notifications_restore(mask));
    asm volatile("fxrstorq %0" : : "m"(clobber) : "memory");
}
#endif

int main(void)
{
    make_bank(&seed, 0x31);
    make_bank(&nested_seed, 0x61);
    make_bank(&clobber, 0x91);
#ifdef TEST_NATIVE
    task.fiber = &task_fiber;
    other.fiber = &other_fiber;
    assert(!checked_call(save_call, 1, &seed));
    assert(saved_mask == 0 && atomic_load(&task.notification_mask) == PH_NOTIFICATION_MASK);
    assert(!checked_call(save_call, 1, &seed) && saved_mask == PH_NOTIFICATION_MASK);
    assert(!checked_call(restore_call, PH_NOTIFICATION_MASK, &seed));
    assert(!checked_call(restore_call, 0, &seed) && !deliveries);
    atomic_store(&task.notification_deferred, 1);
    assert(!checked_call(restore_call, PH_NOTIFICATION_MASK, &seed) && !deliveries);
    assert(atomic_load(&task.notification_deferred) == 1);
    assert(!checked_call(restore_call, 0, &seed) && deliveries == 2);
    assert(!atomic_load(&task.notification_mask) && !atomic_load(&task.notification_deferred));
    assert(!atomic_load(&other.notification_deferred));
    puts("native notification FP: PASS atomic/deferred/nested/yield, x87/MXCSR/all XMM");
#else
    struct sigaction action = {.sa_handler = notification_handler};
    sigemptyset(&action.sa_mask);
    assert(!sigaction(SIGRTMIN, &action, NULL));
    assert(checked_call(save_call, 0, &seed) == EINVAL);
    assert(checked_call(restore_call, UINT64_MAX, &seed) == EINVAL);
    assert(!checked_call(save_call, 1, &seed) && !saved_mask);
    assert(!checked_call(save_call, 1, &seed) && saved_mask);
    assert(!raise(SIGRTMIN) && !deliveries);
    assert(!checked_call(restore_call, 0, &seed) && deliveries == 1);
    assert(!checked_call(save_call, 1, &seed) && !saved_mask);
    assert(!checked_call(restore_call, saved_mask, &seed));
    puts("POSIX notification FP: PASS no BOOT_RUNTIME, errors/libc/deferred/nested, x87/MXCSR/all XMM");
#endif
    return 0;
}
