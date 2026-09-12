/* SPDX-License-Identifier: MIT */
/* Gate selection/diagnostics belong to the launcher, not the VM transport. */
#include "host.h"
#include "bootfs.h"
#include "vm.h"
#include "arch/x86_64/user_layout.h"
#include "boot/vm_gate.h"
#include "boot/syscall_gate.h"
#include <errno.h>

#ifndef PH_VM_CASE
#define PH_VM_CASE 0
#endif

/* Fork/clone fixtures create their peer through Linux and need one root.
 * The other cases start two independent clients. CPU coverage is separate. */
#if defined(PH_SYSCALL_GATE) && PH_SYSCALL_GATE >= 4 && PH_SYSCALL_GATE <= 9
enum { PH_GATE_ROOT_CLIENTS = 1 };
#else
enum { PH_GATE_ROOT_CLIENTS = 2 };
#endif

static struct kobox_linux_vm_test vm_test;
static struct ph_vm_space *clients[2];
static struct kobox_linux_vm_host_operations checked_operations;
static bool fp_marked[2];
static atomic_uint fault_context_checks;
static atomic_uint fault_return_checks;
static atomic_uint syscall_context_checks;
static atomic_uint syscall_dead_context_checks;
#ifdef PH_EXEC_GATE
extern void ph_verify_exec_gate(const struct kobox_linux_vm_test *vm);
#endif

static int checked_syscall_event(void *space, struct kobox_linux_vm_event *event) {
    int result = ph_vm_ops.event(space, event);

    if (result || event->kind != KOBOX_VM_EVENT_SYSCALL) {
        return result;
    }
    struct kobox_x86_user_regs registers;
    struct kobox_x86_fp_state fp;

    PH_CHECK(event->sequence != 0);
    PH_CHECK(ph_vm_ops.snapshot(space, event->sequence - 1, &registers, &fp) == -ESTALE);
    PH_OK(ph_vm_ops.snapshot(space, event->sequence, &registers, &fp));
    PH_CHECK(registers.ip == event->user.ip && registers.sp == event->user.sp);
    PH_CHECK(ph_vm_ops.restore(space, event->sequence, &registers, &fp) == -EBUSY);
    PH_CHECK(ph_vm_ops.resume(space, event->sequence) == -EBUSY);
    return 0;
}

static int checked_syscall_return(void *space, uint64_t sequence, uint64_t syscall_sequence,
    const struct kobox_x86_user_regs *registers) {
    struct kobox_x86_user_regs snapshot;
    struct kobox_x86_fp_state fp;

    /* The common verifier also calls this after destroying the VM binding.
     * A reaped client rejects every generation with ESRCH, not ESTALE. Keep
     * that terminal-state check separate from the live-frame assertions. */
    int status = ph_vm_ops.snapshot(space, sequence, &snapshot, &fp);

    if (status == -ESRCH) {
        int result = ph_vm_ops.syscall_return(space, sequence, syscall_sequence, registers);

        PH_CHECK(result == -ESRCH);
        atomic_fetch_add(&syscall_dead_context_checks, 1);
        return result;
    }
    PH_OK(status);
    PH_CHECK(sequence != 0);
    PH_CHECK(ph_vm_ops.syscall_return(space, sequence - 1,
        syscall_sequence, registers) == -ESTALE);
    int result = ph_vm_ops.syscall_return(space, sequence, syscall_sequence, registers);

    if (result) {
        return result;
    }
    PH_CHECK(ph_vm_ops.syscall_return(space, sequence,
        syscall_sequence, registers) == -ESTALE);
    PH_OK(ph_vm_ops.snapshot(space, sequence, &snapshot, &fp));
    PH_CHECK(memcmp(&snapshot, registers, sizeof(snapshot)) == 0);
    /* Restoring after Linux supplied the result is valid; restoring before
     * that result would skip an unserviced syscall and is rejected above. */
    PH_CHECK(ph_vm_ops.restore(space, sequence - 1, &snapshot, &fp) == -ESTALE);
    PH_OK(ph_vm_ops.restore(space, sequence, &snapshot, &fp));
    atomic_fetch_add(&syscall_context_checks, 1);
    return 0;
}

static void verify_reaped_syscall_contexts(void) {
    struct kobox_x86_user_regs registers = {0};
    struct kobox_x86_fp_state fp = {0};

    /* FD verifiers do not all make the base verifier's post-close callback.
     * Check both actual tombstones here, after Linux dropped its bindings,
     * without depending on how many callbacks a particular verifier made. */
    for (unsigned index = 0; index < PH_GATE_ROOT_CLIENTS; ++index) {
        void *space = clients[index];

        PH_CHECK(ph_vm_ops.snapshot(space, 0, &registers, &fp) == -ESRCH);
        PH_CHECK(ph_vm_ops.syscall_return(space, 0, 0, &registers) == -ESRCH);
        PH_CHECK(ph_vm_ops.syscall_return(space, UINT64_MAX,
            UINT64_MAX, &registers) == -ESRCH);
        PH_CHECK(ph_vm_ops.restore(space, 0, &registers, &fp) == -ESRCH);
        PH_CHECK(ph_vm_ops.resume(space, 0) == -ESRCH);
    }
    ph_number("syscall terminal root clients", PH_GATE_ROOT_CLIENTS);
}

static int checked_event(void *space, struct kobox_linux_vm_event *event) {
    int result = ph_vm_ops.event(space, event);

    if (result || event->kind != KOBOX_VM_EVENT_FAULT) {
        return result;
    }
    unsigned client = space == clients[0] ? 0 : 1;
    struct kobox_x86_user_regs before, after;
    struct kobox_x86_fp_state fp, after_fp, invalid;
    uint64_t marker = UINT64_C(0x6b6f626f78324650) + client;
    uint64_t observed;

    PH_CHECK(space == clients[client]);
    PH_CHECK(event->sequence != 0);
    PH_CHECK(ph_vm_ops.snapshot(space, event->sequence - 1, &before, &fp) == -ESTALE);
    PH_OK(ph_vm_ops.snapshot(space, event->sequence, &before, &fp));
    PH_CHECK(before.ip == event->fault.ip && before.sp == event->fault.sp);
    PH_CHECK(before.orig_ax == UINT64_MAX);
    /* XMM15 is unused by the general-register-only native access client.
     * On the next fault this value must come from actual native XSAVE, not
     * from the previous host-side snapshot cache. */
    memcpy(&observed, fp.fxsave + 160 + 15 * 16, sizeof(observed));
    if (fp_marked[client]) {
        PH_CHECK(observed == marker);
        atomic_fetch_add(&fault_return_checks, 1);
    }
    invalid = fp;
    invalid.fxsave[27] |= 0x80; /* Reserved MXCSR bit 31. */
    PH_CHECK(ph_vm_ops.restore(space, event->sequence, &before, &invalid) == -EINVAL);
    PH_CHECK(ph_vm_ops.write_fpregs(space, event->sequence, &invalid) == -EINVAL);
    after = before;
    after.ip = UINT64_C(1) << 63;
    PH_CHECK(ph_vm_ops.restore(space, event->sequence, &after, &fp) == -EINVAL);
    PH_CHECK(ph_vm_ops.restore(space, event->sequence - 1, &before, &fp) == -ESTALE);
    PH_CHECK(ph_vm_ops.syscall_return(space, event->sequence, 0, &before) == -EBUSY);
    PH_OK(ph_vm_ops.snapshot(space, event->sequence, &after, &after_fp));
    PH_CHECK(memcmp(&before, &after, sizeof(before)) == 0);
    PH_CHECK(memcmp(&fp, &after_fp, sizeof(fp)) == 0);
    memcpy(fp.fxsave + 160 + 15 * 16, &marker, sizeof(marker));
    PH_OK(ph_vm_ops.restore(space, event->sequence, &before, &fp));
    fp_marked[client] = true;
    atomic_fetch_add(&fault_context_checks, 1);
    return 0;
}

static int notify_cpu(uint32_t cpu) {
    return ph_task_ops.cpu_notify(cpu, KOBOX_LINUX_TASK_VM_EVENT);
}

static int terminate_client(uint64_t pid) {
    for (unsigned index = 0; index < PH_GATE_ROOT_CLIENTS; ++index) {
        if (ph_vm_pid(clients[index]) == pid) {
            return ph_vm_terminate(clients[index]);
        }
    }
    return -ESRCH;
}

void ph_vm_gate_prepare(void) {
    size_t size;
    const void *file = ph_bootfs_file("/srv/kobox2/vm-client.elf", &size);

    vm_test = (struct kobox_linux_vm_test) {
        .size = sizeof(vm_test),
        .operations = &ph_vm_ops,
        .start = UINT64_C(0x4000000000),
        .length = 16 * PH_PAGE_SIZE,
        .probe = ph_vm_probe,
        .notify = notify_cpu,
        .terminate = terminate_client,
    };
#ifdef PH_EXEC_GATE
    vm_test.start = KOBOX_X86_USER_START;
    vm_test.length = KOBOX_X86_USER_END - KOBOX_X86_USER_START;
#endif
#if defined(PH_SYSCALL_GATE) && PH_SYSCALL_GATE == 10
    vm_test.length = 32 * PH_PAGE_SIZE;
#endif
#ifndef PH_SYSCALL_GATE
    (void)checked_syscall_event;
    (void)checked_syscall_return;
    (void)verify_reaped_syscall_contexts;
#if defined(PH_EXEC_GATE) || defined(PH_GEM_GATE)
    (void)checked_event;
    (void)checked_operations;
#else
    if (PH_VM_CASE == 0) {
        /* Additional machine-context assertions around the unchanged basic
         * verifier. No event is replaced, consumed early or fabricated. */
        checked_operations = ph_vm_ops;
        checked_operations.event = checked_event;
        vm_test.operations = &checked_operations;
    }
#endif
#else
    (void)checked_event;
#if PH_SYSCALL_GATE < 4
    checked_operations = ph_vm_ops;
    checked_operations.event = checked_syscall_event;
    checked_operations.syscall_return = checked_syscall_return;
    vm_test.operations = &checked_operations;
#else
    /* Autonomous children first return at sequence zero and contribute
     * calls not counted by the parent's instruction-driving verifier.
     * Use the real host operations; retain terminal checks for each root. */
    (void)checked_operations;
    (void)checked_syscall_event;
    (void)checked_syscall_return;
#endif
#endif
    switch (PH_VM_CASE) {
    case 0: break;
    case 1: vm_test.readonly_case = 1; break;
    case 2: vm_test.reuse_case = 1; break;
    case 3: vm_test.race_case = KOBOX_VM_RACE_IRQ; break;
    case 4: vm_test.race_case = KOBOX_VM_RACE_TRUNCATE; break;
    case 5: vm_test.race_case = KOBOX_VM_RACE_LATE_FAULT; break;
    case 6: vm_test.race_case = KOBOX_VM_RACE_PRESSURE_FAULT; break;
    case 7: vm_test.race_case = KOBOX_VM_RACE_EXIT_PUBLISH; break;
    case 8: vm_test.race_case = KOBOX_VM_RACE_EXIT; break;
    case 9: vm_test.lifetime_case = KOBOX_VM_LIFETIME_NORMAL; break;
    case 10: vm_test.lifetime_case = KOBOX_VM_LIFETIME_DEATH; break;
    case 11: vm_test.lifetime_case = KOBOX_VM_LIFETIME_ROLLBACK; break;
    default: PH_CHECK(false);
    }
    for (unsigned index = 0; index < PH_GATE_ROOT_CLIENTS; ++index) {
        clients[index] = ph_vm_create(file, size, vm_test.start, vm_test.length);
        vm_test.spaces[index] = clients[index];
        vm_test.pids[index] = ph_vm_pid(clients[index]);
        ph_number("native VM client PID", vm_test.pids[index]);
#ifdef PH_SYSCALL_GATE
        const uint64_t arguments[6] = {0};

        PH_CHECK(ph_vm_issue_syscall(clients[index], 0, arguments, 0) == -EPERM);
#endif
    }
    if (PH_GATE_ROOT_CLIENTS == 2) {
        PH_CHECK(vm_test.pids[0] != vm_test.pids[1]);
    }
}

void ph_verify_vm_gate(void) {
#ifdef PH_GEM_GATE
    extern void ph_verify_gem_gate(const struct kobox_linux_vm_test *vm);

    ph_verify_gem_gate(&vm_test);
    for (unsigned index = 0; index < PH_GATE_ROOT_CLIENTS; ++index) {
        ph_vm_destroy(clients[index]);
    }
    ph_log("PACHA_KOBOX_GEM=PASS\n");
    return;
#endif
#ifdef PH_EXEC_GATE
    ph_verify_exec_gate(&vm_test);
#else
#ifdef PH_SYSCALL_GATE
    static const char *const symbols[] = {
        "kobox_linux_syscall_verify",
        "kobox_linux_fd_transfer_verify",
        "kobox_linux_fd_exit_race_verify",
        "kobox_linux_fd_inheritance_verify",
        "kobox_linux_fork_verify",
        "kobox_linux_clone_verify",
        "kobox_linux_thread_verify",
        "kobox_linux_group_exit_wait_verify",
        "kobox_linux_group_exit_running_verify",
        "kobox_linux_group_exit_peer_verify",
        "kobox_linux_autonomous_client_verify",
    };
    int (*verify)(const struct kobox_syscall_test *, struct kobox_syscall_report *);
    struct kobox_syscall_test test = {
        .size = sizeof(test),
        .vm = &vm_test,
        .issue = PH_SYSCALL_GATE == 10 ? NULL : ph_vm_issue_syscall,
    };
    struct kobox_syscall_report report = {.size = sizeof(report)};

    PH_CHECK(PH_SYSCALL_GATE < sizeof(symbols) / sizeof(symbols[0]));
    void *address = ph_image_lookup(&ph_core, symbols[PH_SYSCALL_GATE]);

    PH_CHECK(address != NULL);
    memcpy(&verify, &address, sizeof(verify));
    int result = verify(&test, &report);

    ph_number("syscall case", PH_SYSCALL_GATE);
    ph_number("syscall line", report.line);
    ph_number("syscall number", report.number);
    ph_number("syscall returned", report.returned);
    ph_number("syscall calls", report.calls);
    ph_number("syscall faults", report.faults);
    ph_number("syscall TLS", report.tls);
    ph_number("syscall nested", report.nested);
    ph_number("syscall invalid", report.invalid);
    ph_number("syscall descriptors", report.descriptors);
    ph_number("syscall transfers", report.transfers);
    ph_number("syscall truncated", report.truncated);
    ph_number("syscall queued exit", report.queued_exit);
    ph_number("syscall rendezvous", report.rendezvous);
    ph_number("syscall race sent", report.race_sent);
    ph_number("syscall race rejected", report.race_rejected);
    ph_number("syscall inherited", report.inherited);
    ph_number("syscall COW", report.cow);
    ph_number("syscall binding rollbacks", report.binding_rollbacks);
    ph_number("syscall native forks", report.native_forks);
    ph_number("syscall shared clones", report.shared_clones);
    ph_number("syscall threads", report.threads);
    ph_number("syscall group exits", report.group_exits);
    ph_number("syscall autonomous", report.autonomous);
    ph_number("syscall exited", report.exited);
    ph_number("syscall reclaimed", report.reclaimed);
    ph_number("syscall CPUs", report.cpu_mask);
    ph_number("syscall warnings", report.warnings);
#else
    int (*verify)(const struct kobox_linux_vm_test *, struct kobox_linux_vm_report *);
    void *address = ph_image_lookup(&ph_core, "kobox_linux_vm_probe");
    struct kobox_linux_vm_report report = {.size = sizeof(report)};

    PH_CHECK(address != NULL);
    memcpy(&verify, &address, sizeof(verify));
    int result = verify(&vm_test, &report);

    ph_number("VM case", PH_VM_CASE);
    ph_number("VM phase", report.phase);
    ph_number("VM line", report.line);
    ph_number("VM result", report.result);
    ph_number("VM faults", report.faults);
    ph_number("VM accesses", report.accesses);
    ph_number("VM mm checks", report.mm_checks);
    ph_number("VM signals", report.signals);
    ph_number("VM reclaimed pages", report.reclaimed_pages);
    ph_number("VM revoked aliases", report.revoked_aliases);
    ph_number("VM publication races", report.publication_races);
    ph_number("VM publication IRQs", report.publication_irqs);
    ph_number("VM stale resumes", report.stale_resumes);
    ph_number("VM dead spaces", report.dead_spaces);
    ph_number("VM delayed faults", report.delayed_faults);
    ph_number("VM async exits", report.async_exits);
    ph_number("VM rollbacks", report.rollbacks);
    ph_number("VM mm reclaims", report.mm_reclaims);
    ph_number("VM task reclaims", report.task_reclaims);
    ph_number("VM file reclaims", report.file_reclaims);
    ph_number("VM inode reclaims", report.inode_reclaims);
    ph_number("VM folio reclaims", report.folio_reclaims);
    ph_number("VM warnings", report.warnings);
#endif
    PH_OK(result);
    PH_CHECK(report.warnings == 0);
#ifdef PH_SYSCALL_GATE
    ph_number("syscall context checks", atomic_load(&syscall_context_checks));
    ph_number("syscall dead context checks", atomic_load(&syscall_dead_context_checks));
#if PH_SYSCALL_GATE < 4
    PH_CHECK(atomic_load(&syscall_context_checks) == report.calls);
#endif
    verify_reaped_syscall_contexts();
    ph_log("PACHA_KOBOX_SYSCALL_CONTEXT=PASS\n");
#endif
#ifndef PH_SYSCALL_GATE
    if (PH_VM_CASE == 0) {
        ph_number("fault context checks", atomic_load(&fault_context_checks));
        ph_number("fault FP returns", atomic_load(&fault_return_checks));
        PH_CHECK(atomic_load(&fault_context_checks) >= 2);
        PH_CHECK(atomic_load(&fault_return_checks) != 0);
        ph_log("PACHA_KOBOX_VM_FAULT_CONTEXT=PASS\n");
    }
#endif

#endif /* PH_EXEC_GATE */

    /* The common verifier has joined its Linux tasks and dropped bindings.
     * Only now may native tombstones and their producers be destroyed. */
    for (unsigned index = 0; index < PH_GATE_ROOT_CLIENTS; ++index) {
        ph_vm_destroy(clients[index]);
        clients[index] = NULL;
    }
#ifdef PH_EXEC_GATE
    ph_log("PACHA_KOBOX_EXEC=PASS\n");
#elif defined(PH_SYSCALL_GATE)
    ph_log("PACHA_KOBOX_SYSCALL=PASS\n");
#else
    ph_log("PACHA_KOBOX_VM=PASS\n");
#endif
}
