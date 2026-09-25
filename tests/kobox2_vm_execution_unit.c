/* SPDX-License-Identifier: MIT */
/* Native syscall sequencing tests; these do not replace actual STOP/XSAVE
 * conformance or the QEMU signal clients. No native syscall executes here. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../userland/kobox2_adapter/vm_internal.h"

static long native_two(long number, uint64_t first, uint64_t second);
static long native_four(long number, uint64_t first, uint64_t second,
    uint64_t third, uint64_t fourth);
#define pacha_syscall2 native_two
#define pacha_syscall4 native_four
#include "../userland/kobox2_adapter/vm_execution.c"
#undef pacha_syscall2
#undef pacha_syscall4

struct native_step {
    long number;
    long result;
    unsigned operation;
    bool publish_event;
};

static struct native_step steps[16];
static unsigned step_count, step_index;
static uint64_t time_now, time_step;
static struct ph_vm_space *active;
static struct pacha_thread_context input, installed;
static bool locked;

uint64_t ph_vm_lock_space(struct ph_vm_space *space) {
    assert(space == active && !locked);
    locked = true;
    return 0x1000;
}

void ph_vm_unlock_space(struct ph_vm_space *space, uint64_t mask) {
    assert(space == active && locked && mask == 0x1000);
    locked = false;
}

static struct native_step next_step(long number) {
    assert(step_index < step_count);
    struct native_step step = steps[step_index++];

    assert(step.number == number);
    if (step.publish_event) {
        atomic_store(&active->control->event_sequence,
            active->sequence + active->bootstrap_sequence);
    }
    return step;
}

static long native_two(long number, uint64_t first, uint64_t second) {
    struct native_step step = next_step(number);

    if (number == PACHA_RUNTIME_SYSCALL_NANOSLEEP) {
        const uint64_t *delay = (void *)(uintptr_t)first;

        assert(delay[0] == 0 && delay[1] == 1000 && second == 0);
    } else {
        assert(first == (uint64_t)active->process_fd);
        if (number == PACHA_PROCESS_SYSCALL_WAIT && !step.result) {
            uint64_t *status = (void *)(uintptr_t)second;

            status[0] = 3;
            status[1] = 0;
        }
    }
    return step.result;
}

static long native_four(long number, uint64_t first, uint64_t second,
    uint64_t third, uint64_t fourth) {
    struct native_step step = next_step(number);
    struct pacha_thread_context *context = (void *)(uintptr_t)third;

    assert(number == PACHA_THREAD_SYSCALL_CONTEXT);
    assert(first == (uint64_t)active->thread_fd && fourth == sizeof(*context));
    assert(second == step.operation);
    if (!step.result) {
        if (second == PACHA_THREAD_CONTEXT_GET) {
            *context = input;
        } else {
            installed = *context;
        }
    }
    return step.result;
}

static int monotonic(uint64_t *now) {
    *now = time_now;
    time_now += time_step;
    return 0;
}

const struct kobox_linux_task_host_operations ph_task_ops = {.monotonic_ns = monotonic};

_Noreturn void ph_fail(const char *file, unsigned line, uint64_t result) {
    fprintf(stderr, "%s:%u: %llu\n", file, line, (unsigned long long)result);
    abort();
}

static void prepare(struct ph_vm_space *space, struct ph_vm_client_control *control) {
    *control = (struct ph_vm_client_control){0};
    *space = (struct ph_vm_space) {
        .control = control, .start = 0x200000, .length = 0x10000,
        .process_fd = 80, .thread_fd = 81, .running = true, .user_started = true,
        .sequence = 9, .bootstrap_sequence = 4,
    };
    input = (struct pacha_thread_context){0};
    input.registers.rip = space->start + 128;
    input.registers.rsp = space->start + space->length - 16;
    input.registers.rax = 0x123456;
    input.registers.r12 = 0xabcdef;
    input.registers.rflags = 0x202;
    input.registers.cs = 0x23;
    input.registers.ss = 0x1b;
    input.fs_base = space->start + 256;
    input.gs_base = space->start + 512;
    for (unsigned i = 0; i < sizeof(input.xstate); ++i) {
        input.xstate[i] = (unsigned char)(i * 7);
    }
    uint32_t mxcsr = 0x1f80, mask = 0xffff;

    memcpy(input.xstate + 24, &mxcsr, sizeof(mxcsr));
    memcpy(input.xstate + 28, &mask, sizeof(mask));
    step_index = step_count = 0;
    time_now = 1;
    time_step = 1000;
    active = space;
}

static void expect(long number, long result, unsigned operation, bool event) {
    assert(step_count < sizeof(steps) / sizeof(steps[0]));
    steps[step_count++] = (struct native_step){number, result, operation, event};
}

static void stop_get(long result, bool event) {
    expect(PACHA_PROCESS_SYSCALL_STOP, 0, 0, false);
    expect(PACHA_THREAD_SYSCALL_CONTEXT, result, PACHA_THREAD_CONTEXT_GET, event);
}

static void captured_context(void) {
    struct ph_vm_space space;
    struct ph_vm_client_control control;

    prepare(&space, &control);
    stop_get(0, false);
    assert(!ph_vm_capture_running(&space));
    assert(step_index == step_count && space.native_stopped && !space.running);
    assert(!space.pending && !space.reaped);
    assert(space.captured_registers.ip == input.registers.rip);
    assert(space.captured_registers.sp == input.registers.rsp);
    assert(space.captured_registers.ax == input.registers.rax);
    assert(space.captured_registers.r12 == input.registers.r12);
    assert(space.captured_registers.fs_base == input.fs_base);
    assert(space.captured_registers.gs_base == input.gs_base);
    assert(space.captured_registers.orig_ax == UINT64_MAX);
    assert(!memcmp(&space.initial_context, &input, sizeof(input)));
    assert(!memcmp(space.captured_fp.fxsave, input.xstate, 512));

    struct kobox_x86_user_regs user = space.captured_registers;
    struct kobox_x86_fp_state fp = space.captured_fp;

    user.ax = 99;
    user.fs_base += 16;
    fp.fxsave[160] ^= 0xff;
    expect(PACHA_THREAD_SYSCALL_CONTEXT, 0, PACHA_THREAD_CONTEXT_SET, false);
    assert(!ph_vm_install_user_context(&space, &user, &fp));
    assert(step_index == step_count);
    assert(installed.registers.cs == input.registers.cs && installed.registers.ss == input.registers.ss);
    assert(installed.registers.rax == 99 && installed.fs_base == user.fs_base);
    assert(!memcmp(installed.xstate, fp.fxsave, 512));
    assert(installed.xstate[512] == (input.xstate[512] | 3));
    assert(!memcmp(installed.xstate + 513, input.xstate + 513, sizeof(input.xstate) - 513));

    /* Invalid user frames must never reach SET or change the shared control. */
    struct ph_vm_client_control before = control;

    user.ip = UINT64_C(1) << 63;
    assert(ph_vm_install_user_context(&space, &user, &fp) == -EINVAL);
    user = space.captured_registers;
    fp.fxsave[27] |= 0x80;
    assert(ph_vm_install_user_context(&space, &user, &fp) == -EINVAL);
    assert(step_index == step_count && !memcmp(&before, &control, sizeof(control)));
}

static void racing_event(void) {
    struct ph_vm_space space;
    struct ph_vm_client_control control;

    prepare(&space, &control);
    stop_get(0, true);
    expect(PACHA_PROCESS_SYSCALL_CONTINUE, 0, 0, false);
    assert(ph_vm_capture_running(&space) == -EBUSY);
    assert(step_index == step_count && space.running && !space.native_stopped);
    assert(atomic_load(&control.event_sequence) == space.sequence + space.bootstrap_sequence);
    assert(!space.pending && !space.reaped); /* Monitor remains the producer. */
}

static void transient_entry(void) {
    struct ph_vm_space space;
    struct ph_vm_client_control control;

    prepare(&space, &control);
    stop_get(PACHA_SYSCALL_ERR_NOT_READY, false);
    expect(PACHA_PROCESS_SYSCALL_CONTINUE, 0, 0, false);
    expect(PACHA_RUNTIME_SYSCALL_NANOSLEEP, 0, 0, false);
    stop_get(0, false);
    assert(!ph_vm_capture_running(&space));
    assert(step_index == step_count && space.native_stopped && !space.running);

    prepare(&space, &control);
    input.registers.rip = PH_VM_CLIENT_IMAGE_BASE;
    time_step = UINT64_C(5000000000);
    stop_get(0, false);
    expect(PACHA_PROCESS_SYSCALL_CONTINUE, 0, 0, false);
    assert(ph_vm_capture_running(&space) == -ETIMEDOUT);
    assert(step_index == step_count && space.running && !space.native_stopped);
}

static void terminal_races(void) {
    struct ph_vm_space space;
    struct ph_vm_client_control control;

    /* Native control calls report INVALID for a process which just died.
     * Only a successful native WAIT may turn that into terminal ESRCH. */
    prepare(&space, &control);
    expect(PACHA_PROCESS_SYSCALL_STOP, PACHA_SYSCALL_ERR_INVALID, 0, false);
    expect(PACHA_PROCESS_SYSCALL_WAIT, 0, 0, false);
    assert(ph_vm_capture_running(&space) == -ESRCH);
    assert(step_index == step_count && !space.reaped && !space.pending);

    prepare(&space, &control);
    stop_get(PACHA_SYSCALL_ERR_INVALID, false);
    expect(PACHA_PROCESS_SYSCALL_CONTINUE, PACHA_SYSCALL_ERR_INVALID, 0, false);
    expect(PACHA_PROCESS_SYSCALL_WAIT, 0, 0, false);
    assert(ph_vm_capture_running(&space) == -ESRCH);
    assert(step_index == step_count && !space.reaped && !space.pending);

    /* Do not conceal an invalid operation on a live process as an exit. */
    prepare(&space, &control);
    expect(PACHA_PROCESS_SYSCALL_STOP, PACHA_SYSCALL_ERR_INVALID, 0, false);
    expect(PACHA_PROCESS_SYSCALL_WAIT, PACHA_SYSCALL_ERR_NOT_READY, 0, false);
    assert(ph_vm_capture_running(&space) == -EINVAL);
    assert(step_index == step_count && !space.reaped && !space.pending);

    prepare(&space, &control);
    stop_get(0, false);
    assert(!ph_vm_capture_running(&space));
    struct ph_vm_client_control before = control;

    expect(PACHA_THREAD_SYSCALL_CONTEXT, PACHA_SYSCALL_ERR_INVALID,
        PACHA_THREAD_CONTEXT_SET, false);
    expect(PACHA_PROCESS_SYSCALL_WAIT, 0, 0, false);
    assert(ph_vm_install_user_context(&space, &space.captured_registers,
        &space.captured_fp) == -ESRCH);
    assert(step_index == step_count && !memcmp(&before, &control, sizeof(control)));
}

static void snapshot_rejected(struct ph_vm_space *space, uint64_t sequence, int result) {
    struct kobox_x86_user_regs registers, before;
    struct kobox_x86_fp_state fp, fp_before;

    memset(&before, 0xa5, sizeof(before));
    memset(&fp_before, 0x5a, sizeof(fp_before));
    registers = before;
    fp = fp_before;
    assert(ph_vm_snapshot(space, sequence, &registers, &fp) == result);
    assert(!memcmp(&before, &registers, sizeof(before)));
    assert(!memcmp(&fp_before, &fp, sizeof(fp)));
    assert(step_index == 0 && !locked);
}

static void snapshot_state_guards(void) {
    struct ph_vm_space space;
    struct ph_vm_client_control control;

    prepare(&space, &control);
    snapshot_rejected(&space, space.sequence - 1, -ESTALE);
    space.pending = true;
    snapshot_rejected(&space, space.sequence, -EBUSY);
    space.pending = false;
    space.user_started = false;
    snapshot_rejected(&space, space.sequence, -EBUSY);
    space.running = false;
    snapshot_rejected(&space, space.sequence, -EOPNOTSUPP);
    space.reaped = true;
    snapshot_rejected(&space, space.sequence, -ESRCH);
    snapshot_rejected(&space, UINT64_MAX, -ESRCH);

    prepare(&space, &control);
    stop_get(0, false);
    struct kobox_x86_user_regs registers;
    struct kobox_x86_fp_state fp;

    assert(!ph_vm_snapshot(&space, space.sequence, &registers, &fp));
    assert(!locked && space.native_stopped && !space.running);
    assert(registers.ip == input.registers.rip && registers.r12 == input.registers.r12);
    /* A second snapshot of the captured generation must not STOP/GET again. */
    assert(!ph_vm_snapshot(&space, space.sequence, &registers, &fp));
    assert(step_index == step_count && !locked);
}

int main(void) {
    captured_context();
    racing_event();
    transient_entry();
    terminal_races();
    snapshot_state_guards();
    puts("VM execution context, real-event priority and bounded capture unit PASS");
    return 0;
}
