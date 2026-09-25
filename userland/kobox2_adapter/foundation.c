/* SPDX-License-Identifier: MIT */
/* Boot-only launcher: all verification runs after upstream Linux reaches PID 1. */
#include "host.h"
#include "foundation.h"
#include "boot.h"
#include "vm.h"
#include "boot/service_gate.h"
#include "boot/memory_gate.h"
#include "boot/wait_gate.h"
#include "boot/rcu_gate.h"
#include "boot/workqueue_gate.h"
#include "boot/cleanup_gate.h"

#ifndef PH_CLEANUP_ROUNDS
#define PH_CLEANUP_ROUNDS 1
#endif
_Static_assert(PH_CLEANUP_ROUNDS >= 1 && PH_CLEANUP_ROUNDS <= 100, "cleanup repeat bound");

static void (*gates_completed)(void *);
static void *gates_context;

extern void ph_verify_native_mapping(void);
#ifdef PH_DEVICE_GATE
extern void ph_device_gate_prepare(const struct kobox_linux_boot_layout *layout);
extern void ph_verify_device_gate(void);
#endif

struct foundation_gates {
    int (*boot)(struct kobox_linux_boot_report *);
    int (*memory)(struct kobox_linux_boot_memory_report *);
    int (*wait)(struct kobox_linux_wait_report *);
    int (*rcu)(struct kobox_linux_rcu_report *);
    int (*workqueue)(struct kobox_linux_workqueue_report *);
    int (*cleanup)(struct kobox_linux_cleanup_report *);
};

/* Copy the loader's address into a typed function pointer without aliasing it
 * through void **. All resolved symbols belong to the inspected core ELF. */
#define RESOLVE_GATE(function, symbol) do { \
    void *address = ph_image_lookup(&ph_core, symbol); \
    PH_CHECK(address != NULL); \
    memcpy(&(function), &address, sizeof(function)); \
} while (0)

static struct foundation_gates resolve_foundation_gates(void) {
    struct foundation_gates gates;

    RESOLVE_GATE(gates.boot, "kobox_linux_boot_verify");
    RESOLVE_GATE(gates.memory, "kobox_linux_boot_memory_verify");
    RESOLVE_GATE(gates.wait, "kobox_linux_wait_verify");
    RESOLVE_GATE(gates.rcu, "kobox_linux_rcu_verify");
    RESOLVE_GATE(gates.workqueue, "kobox_linux_workqueue_verify");
    RESOLVE_GATE(gates.cleanup, "kobox_linux_cleanup_verify");
    return gates;
}

static void log_task_failure(const struct kobox_linux_task_report *task) {
    ph_number("SMP current/percpu", task->current_percpu_ready);
    ph_number("SMP local switch", task->local_switch_ready);
    ph_number("SMP remote switch", task->remote_switch_ready);
    ph_number("SMP migration", task->migration_ready);
    ph_number("SMP affinity", task->affinity_ready);
    ph_number("SMP preempt disable", task->preempt_disable_ready);
    ph_number("SMP IRQ disable", task->irq_disable_ready);
    ph_number("SMP exit/join", task->exit_join_ready);
    for (unsigned cpu = 0; cpu < PH_CPU_COUNT; ++cpu) {
        ph_number("SMP CPU", cpu);
        ph_number("SMP hardirq entries", task->hardirq_entries[cpu]);
        ph_number("SMP idle IRQ entries", task->idle_irq_entries[cpu]);
        ph_number("SMP idle entries", task->idle_entries[cpu]);
        ph_number("SMP idle exits", task->idle_exits[cpu]);
        ph_number("SMP tick progress", task->tick_progress[cpu]);
        ph_number("SMP high resolution", task->high_resolution_ready[cpu]);
    }
}

static void run_boot_gate(const struct foundation_gates *gates,
    const struct kobox_linux_task_report *task) {
    struct kobox_linux_boot_report report = {.size = sizeof(report)};
    int result = gates->boot(&report);

    ph_number("boot phase", report.phase);
    ph_number("boot warnings", report.warnings);
    ph_number("SMP switches", task->context_switches);
    ph_number("SMP remote IPIs", task->remote_reschedule_ipis);
    if (result != 0) {
        log_task_failure(task);
    }
    PH_OK(result);
    PH_CHECK(report.warnings == 0 && task->logical_cpu_count == PH_CPU_COUNT);
    PH_CHECK(task->upstream_schedule_ready && task->upstream_try_to_wake_up_ready);
    PH_CHECK(task->current_percpu_ready && task->local_switch_ready && task->remote_switch_ready);
    PH_CHECK(task->migration_ready && task->affinity_ready && task->preempt_disable_ready);
    PH_CHECK(task->irq_disable_ready && task->exit_join_ready);
    PH_CHECK(task->high_resolution_ready[0] && task->high_resolution_ready[1]);
    ph_log("PACHA_KOBOX_BOOT_SMP=PASS\n");
}

#ifndef PH_FOCUSED_GATE
static void run_memory_gate(const struct foundation_gates *gates) {
    struct kobox_linux_boot_memory_report report = {.size = sizeof(report)};
    int result = gates->memory(&report);

    ph_number("memory phase", report.phase);
    ph_number("memory line", report.line);
    ph_number("memory aliases", report.alias_cases);
    ph_number("memory warnings", report.warnings);
    PH_OK(result);
    PH_CHECK(report.warnings == 0);
    ph_log("PACHA_KOBOX_MEMORY=PASS\n");
}

static void run_wait_gate(const struct foundation_gates *gates) {
    struct kobox_linux_wait_report report = {.size = sizeof(report)};
    int result = gates->wait(&report);

    ph_log(report.api);
    ph_log(report.scenario);
    ph_number("wait line", report.failure_line);
    ph_number("wait result", report.result);
    ph_number("wait cases", report.passed);
    ph_number("wait IRQs", report.irq_callbacks);
    ph_number("wait warnings", report.warnings);
    PH_OK(result);
    PH_CHECK(report.warnings == 0);
    ph_log("PACHA_KOBOX_WAIT=PASS\n");
}

static void run_rcu_gate(const struct foundation_gates *gates) {
    struct kobox_linux_rcu_report report = {.size = sizeof(report)};
    int result = gates->rcu(&report);

    ph_log(report.flavor);
    ph_log(report.scenario);
    ph_number("RCU line", report.failure_line);
    ph_number("RCU cases", report.passed);
    ph_number("RCU callbacks", report.callbacks);
    ph_number("RCU freed", report.reclaimed);
    ph_number("RCU warnings", report.warnings);
    PH_OK(result);
    PH_CHECK(report.warnings == 0);
    ph_log("PACHA_KOBOX_RCU=PASS\n");
}

static void run_workqueue_gate(const struct foundation_gates *gates) {
    struct kobox_linux_workqueue_report report = {.size = sizeof(report)};
    int result = gates->workqueue(&report);

    ph_log(report.queue);
    ph_log(report.scenario);
    ph_number("workqueue line", report.line);
    ph_number("workqueue cases", report.cases);
    ph_number("workqueue callbacks", report.callbacks);
    ph_number("workqueue warnings", report.warnings);
    PH_OK(result);
    PH_CHECK(report.warnings == 0);
    ph_log("PACHA_KOBOX_WORKQUEUE=PASS\n");
}

static void log_cleanup_failure(const struct kobox_linux_cleanup_report *report) {
    ph_number("hold phase", report->hold_phase);
    ph_number("hold work", report->hold_work_active);
    ph_number("hold delayed", report->hold_delayed_active);
    ph_number("probe timeout phase", report->probe_timeout_phase);
    ph_number("probe calls", report->probe_calls);
    ph_number("probe phase", report->probe_phase);
    ph_number("probe cleaner", report->probe_cleaner);
    ph_number("probe active", report->probe_active);
}

static void run_cleanup_gate(const struct foundation_gates *gates) {
    for (unsigned round = 0; round < PH_CLEANUP_ROUNDS; ++round) {
        struct kobox_linux_cleanup_report report = {.size = sizeof(report)};
        int result = gates->cleanup(&report);

        ph_number("cleanup round", round + 1);
        ph_log(report.scenario);
        ph_number("cleanup line", report.line);
        ph_number("cleanup CPU", report.cpu);
        ph_number("cleanup phase", report.phase);
        ph_number("cleanup errors", report.errors);
        ph_number("cleanup probes", report.probes);
        if (result != 0) {
            log_cleanup_failure(&report);
        }
        ph_number("cleanup cases", report.cases);
        ph_number("cleanup freed", report.freed);
        ph_number("cleanup warnings", report.warnings);
        PH_OK(result);
        PH_CHECK(report.warnings == 0);
        ph_log("PACHA_KOBOX_CLEANUP=PASS\n");
    }
}

#endif

static void kernel_main(const struct kobox_linux_task_report *task_report, void *context) {
    (void)context;
    struct foundation_gates gates = resolve_foundation_gates();

    run_boot_gate(&gates, task_report);
#ifndef PH_FOCUSED_GATE
    run_memory_gate(&gates);
    run_wait_gate(&gates);
    run_rcu_gate(&gates);
    run_workqueue_gate(&gates);
    run_cleanup_gate(&gates);

#ifdef PH_CHAPTER2_CORE
    ph_verify_chapter2_core();
#endif
#endif
#ifdef PH_VM_GATE
    ph_verify_vm_gate();
#endif
#ifdef PH_DEVICE_GATE
    ph_verify_device_gate();
#endif
#ifdef PH_FOCUSED_GATE
    ph_log("PACHA_KOBOX_FOCUSED=PASS\n");
#else
    ph_log("PACHA_KOBOX_FOUNDATION=PASS\n");
#endif
    if (gates_completed) gates_completed(gates_context);
}

static void (*gates_prepare)(const struct kobox_linux_boot_layout *, void *);

static void prepare_gates(const struct kobox_linux_boot_layout *layout, void *context) {
    ph_verify_native_mapping();
    if (gates_prepare) gates_prepare(layout, context);
#ifdef PH_DEVICE_GATE
    ph_device_gate_prepare(layout);
#endif
#ifdef PH_VM_GATE
    ph_vm_gate_prepare();
#endif
}

_Noreturn void ph_foundation_run(const void *core_file, size_t core_size,
    void (*completed)(void *), void *context) {
    ph_foundation_run_prepared(core_file, core_size, NULL, completed, context);
}

_Noreturn void ph_foundation_run_prepared(const void *core_file, size_t core_size,
    void (*prepare)(const struct kobox_linux_boot_layout *, void *),
    void (*completed)(void *), void *context) {
    ph_log("PACHA_KOBOX_FOUNDATION=START\n");
    gates_completed = completed;
    gates_context = context;
    gates_prepare = prepare;
    ph_boot_run(core_file, core_size, prepare_gates, kernel_main, context);
}
