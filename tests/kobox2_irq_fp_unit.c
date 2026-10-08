/* SPDX-License-Identifier: MIT */
/* Reuse the full architectural bank and assembly invocation harness. */
#define main notification_contract_main
#ifdef TEST_NATIVE
#define TEST_NATIVE_IRQ 1
#endif
#include "kobox2_notification_fp_unit.c"
#undef main

#ifdef TEST_NATIVE
/* Exercise the production entries, domain transitions and preserving shim.
 * Only the domain container and dispatch workload belong to the fixture. */
static struct { struct kobox_machine_domain domain; } cpus[PH_CPU_COUNT];
#include "../userland/kobox2_adapter/irq_callbacks.h"
static __attribute__((noinline)) KOBOX_GPR_LEAF int query_call(uint64_t cpu) { return irq_disabled(cpu); }
static __attribute__((noinline)) KOBOX_GPR_LEAF int disable_call(uint64_t cpu) { return irq_disable(cpu); }
static __attribute__((noinline)) KOBOX_GPR_LEAF int enable_call(uint64_t cpu) { return irq_enable(cpu); }
#else
#include "../kobox2/linux-sandbox/kobox/task/posix_machine.c"
#include "../kobox2/linux-sandbox/kobox/host/posix/core.c"
/* Keep the real loader's TLS entry out of line, as in its runtime host table.
 * Its linked code must not call an unprotected libc TLS resolver. */
static struct kobox_runtime_thread_state *(*volatile tls_entry)(void *) = thread_state;
static KOBOX_GPR_LEAF int state_call(uint64_t unused) { (void)unused; return tls_entry(NULL) != NULL; }
static int query_call(uint64_t cpu) { return host_cpu_irq_disabled(cpu); }
static int disable_call(uint64_t cpu) { return host_cpu_irq_disable(cpu); }
static int enable_call(uint64_t cpu) { return host_cpu_irq_enable(cpu); }

static void irq_notification(void *context, uint32_t cpu, enum kobox_posix_notification kind,
                             uint64_t count)
{
    (void)context;
    assert(cpu == 0);
    assert(kind == KOBOX_POSIX_NOTIFICATION_IRQ && count == 1);
    ++deliveries;
    assert(checked_call(query_call, 0, &nested_seed) == 1);
    assert(!checked_call(disable_call, 0, &nested_seed));
    assert(!checked_call(enable_call, 0, &nested_seed));
    sched_yield();
    asm volatile("fxrstorq %0" : : "m"(clobber) : "memory");
}
#endif

int main(void)
{
    assert(!notification_contract_main());
    deliveries = 0;
    make_bank(&seed, 0x31);
    make_bank(&nested_seed, 0x61);
    make_bank(&clobber, 0x91);
#ifdef TEST_NATIVE
    task_fiber.logical_cpu = other_fiber.logical_cpu = 0;
    assert(kobox_machine_domain_init(&cpus[0].domain) == KOBOX_MACHINE_OK);
    assert(kobox_machine_domain_init(&cpus[1].domain) == KOBOX_MACHINE_OK);
    atomic_store(&task.notification_mask, PH_NOTIFICATION_MASK);
    assert(checked_call(query_call, 0, &seed) == 0);
    assert(!checked_call(disable_call, 0, &seed));
    assert(!checked_call(disable_call, 0, &seed));
    assert(checked_call(query_call, 0, &seed) == 1);
    assert(!checked_call(enable_call, 0, &seed) && !deliveries);
    /* No pending count or stop: no dispatch is needed, and the GPR-only
     * entry must preserve every architectural bank field by itself. */
    assert(!checked_call(enable_call, 0, &seed) && !deliveries);
    assert(task_fiber.logical_cpu == 0 && !atomic_load(&cpus[0].domain.irq_depth));
    assert(!checked_call(disable_call, 0, &seed));
    atomic_store(&cpus[0].domain.pending[KOBOX_MACHINE_VM_EVENT], 1);
    assert(!checked_call(enable_call, 0, &seed) && deliveries == 2);
    atomic_store(&cpus[0].domain.pending[KOBOX_MACHINE_VM_EVENT], 0);
    assert(task_fiber.logical_cpu == 1 && !atomic_load(&cpus[0].domain.irq_depth));
    assert(checked_call(query_call, 1, &seed) == 0);
    /* Stop is not a pending-counter hint: the slow entry must still run
     * when every count is zero. The mock returns only to inspect the bank;
     * production's stop acknowledgement does not return. */
    atomic_store(&cpus[1].domain.stop_requested, true);
    assert(!checked_call(disable_call, 1, &seed));
    assert(!checked_call(enable_call, 1, &seed) && deliveries == 3);
    assert(task_fiber.logical_cpu == 0);
    atomic_store(&cpus[1].domain.stop_requested, false);
    /* A deferred native hint remains for the transaction's outer unmask. */
    atomic_store(&task.notification_deferred, 1);
    assert(!checked_call(disable_call, 0, &seed));
    assert(!checked_call(enable_call, 0, &seed) && deliveries == 3);
    assert(atomic_load(&task.notification_deferred) == 1);
    assert(!checked_call(restore_call, 0, &seed) && deliveries == 4);
    assert(task_fiber.logical_cpu == 1 && !atomic_load(&task.notification_deferred));
    atomic_store(&cpus[1].domain.irq_depth, UINT32_MAX);
    assert(checked_call(disable_call, 1, &seed) == KOBOX_MACHINE_OVERFLOW);
    assert(atomic_load(&cpus[1].domain.irq_depth) == UINT32_MAX);
    atomic_store(&cpus[1].domain.irq_depth, 0);
    puts("native IRQ FP: PASS empty/pending/stop/deferred, nested IRQ, dispatch/reentry/migration, full bank");
#else
    assert(checked_call(state_call, 0, &seed) == 1);
    assert(checked_call(disable_call, KOBOX_LINUX_MEMORY_LOGICAL_CPUS, &seed) == -1);
    assert(checked_call(enable_call, KOBOX_LINUX_MEMORY_LOGICAL_CPUS, &seed) == -1);
    assert(checked_call(query_call, KOBOX_LINUX_MEMORY_LOGICAL_CPUS, &seed) == 0);
    assert(checked_call(disable_call, 0, &seed) == EINVAL);
    assert(!kobox_posix_cpu_init(&cpus[0], 0, irq_notification, NULL));
    assert(!kobox_posix_cpu_enter(&cpus[0]));
    assert(checked_call(query_call, 0, &seed) == 0);
    assert(checked_call(enable_call, 0, &seed) == EINVAL);
    assert(!checked_call(disable_call, 0, &seed));
    assert(!checked_call(disable_call, 0, &seed));
    assert(!kobox_posix_cpu_notify(&cpus[0], KOBOX_POSIX_NOTIFICATION_IRQ));
    assert(!deliveries);
    assert(!checked_call(enable_call, 0, &seed) && !deliveries);
    assert(!checked_call(enable_call, 0, &seed) && deliveries == 1);
    assert(!checked_call(query_call, 0, &seed));
    assert(!kobox_posix_cpu_leave(&cpus[0]));
    assert(!kobox_posix_cpu_destroy(&cpus[0]));
    puts("POSIX IRQ FP: PASS table entries, errors, nested IRQ, libc/dispatch/reentry, full bank");
#endif
    return 0;
}
