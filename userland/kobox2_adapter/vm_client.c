/* SPDX-License-Identifier: MIT */
/* Native access client. Linux lives only in the owning sandbox process. */
#include "vm_client_control.h"
#include "vm_fixture.h"
#include <pacha/ipc.h>
#include <pacha/syscall.h>
#include "lpr_linux_syscall.h"

static struct ph_vm_client_control *const control = (void *)PH_VM_CLIENT_CONTROL;
static unsigned char fault_stack[PH_VM_CLIENT_STACK_SIZE] __attribute__((aligned(4096)));
static uint64_t execution_sequence;

/* The assembly frontend saves these before entering any C code. C in this
 * client is built with general registers only, including native IPC helpers. */
_Alignas(16) unsigned char ph_vm_client_saved_fp[512];
uint64_t ph_vm_client_user_sp, ph_vm_client_user_flags, ph_vm_client_return_ip;
extern uint64_t ph_vm_client_linux_call(uint64_t number, const uint64_t arguments[6]);

extern void ph_vm_client_fault_entry(void);
extern void ph_vm_client_syscall_entry(void);

static _Noreturn void client_fail(unsigned line) {
    (void)pacha_syscall1(PACHA_PROCESS_SYSCALL_EXIT, 128 + line);
    __builtin_trap();
}

#define CLIENT_CHECK(condition) do { \
    if (!(condition)) { \
        client_fail(__LINE__); \
    } \
} while (0)

static void publish_event(enum ph_vm_client_event kind) {
    const uint64_t one = 1;

    if (control->autonomous) {
        execution_sequence = atomic_load_explicit(&control->command_sequence, memory_order_acquire);
    }
    control->event = kind;
    atomic_store_explicit(&control->event_sequence, execution_sequence, memory_order_release);
    CLIENT_CHECK(pacha_syscall3(PACHA_FD_SYSCALL_WRITE, PH_VM_CLIENT_EVENT_FD,
        (uintptr_t)&one, sizeof(one)) == sizeof(one));
}

static uint64_t await_command(void) {
    struct pacha_pollfd event = {
        .fd = PH_VM_CLIENT_KICK_FD,
        .events = PACHA_FD_EVENT_READABLE,
    };
    uint64_t count;

    for (;;) {
        event.revents = 0;
        long ready = pacha_syscall4(PACHA_FD_SYSCALL_WAIT_MANY,
            (uintptr_t)&event, 1, PACHA_FD_WAIT_FOREVER, 0);

        if (ready == PACHA_SYSCALL_ERR_NOT_READY && !event.revents) {
            continue;
        }
        CLIENT_CHECK(ready == 1 && event.revents == PACHA_FD_EVENT_READABLE);
        CLIENT_CHECK(pacha_syscall3(PACHA_FD_SYSCALL_READ, PH_VM_CLIENT_KICK_FD,
            (uintptr_t)&count, sizeof(count)) == sizeof(count));
        CLIENT_CHECK(count == 1);
        uint64_t sequence = atomic_load_explicit(&control->command_sequence, memory_order_acquire);

        CLIENT_CHECK(sequence == execution_sequence + 1);
        execution_sequence = sequence;
        return control->command;
    }
}

void ph_vm_client_fault(struct pacha_native_fault_frame *frame) {
    CLIENT_CHECK(frame->signal.magic == PACHA_PROCESS_SIGNAL_FRAME_MAGIC);
    CLIENT_CHECK(frame->signal.size == sizeof(*frame) && frame->signal.signo == 0);
    CLIENT_CHECK(frame->vector == 14);
    control->fault_address = frame->address;
    control->fault_ip = frame->signal.rip;
    control->fault_sp = frame->signal.rsp;
    control->fault_flags = frame->signal.rflags;
    control->fault_error = frame->error_code;
    control->fault_vector = frame->vector;
    struct pacha_native_signal_frame *saved = &frame->signal;

    control->registers = (struct ph_vm_client_registers) {
        .rax = saved->rax, .rbx = saved->rbx, .rcx = saved->rcx, .rdx = saved->rdx,
        .rsi = saved->rsi, .rdi = saved->rdi, .rbp = saved->rbp,
        .r8 = saved->r8, .r9 = saved->r9, .r10 = saved->r10, .r11 = saved->r11,
        .r12 = saved->r12, .r13 = saved->r13, .r14 = saved->r14, .r15 = saved->r15,
        .rip = saved->rip, .rsp = saved->rsp, .rflags = saved->rflags,
        .fs_base = control->linux_fs_base, .gs_base = control->linux_gs_base,
    };
    for (unsigned index = 0; index < sizeof(control->fp); ++index) {
        control->fp[index] = saved->xstate[index];
    }
    control->restore_fault = 0;
    publish_event(PH_VM_CLIENT_FAULT);

    /* The saved instruction, stack and FP state remain on the dedicated
     * native fault stack. Only the owning sandbox resolves and resumes it. */
    CLIENT_CHECK(await_command() == PH_VM_CLIENT_RESUME);
    if (!control->restore_fault) {
        return;
    }
    const struct ph_vm_client_registers *registers = &control->registers;

    saved->rax = registers->rax;
    saved->rbx = registers->rbx;
    saved->rcx = registers->rcx;
    saved->rdx = registers->rdx;
    saved->rsi = registers->rsi;
    saved->rdi = registers->rdi;
    saved->rbp = registers->rbp;
    saved->r8 = registers->r8;
    saved->r9 = registers->r9;
    saved->r10 = registers->r10;
    saved->r11 = registers->r11;
    saved->r12 = registers->r12;
    saved->r13 = registers->r13;
    saved->r14 = registers->r14;
    saved->r15 = registers->r15;
    saved->rip = registers->rip;
    saved->rsp = registers->rsp;
    saved->rflags = registers->rflags;
    for (unsigned index = 0; index < sizeof(control->fp); ++index) {
        saved->xstate[index] = control->fp[index];
    }
    /* FXSAVE supplied by the fixed core replaces x87/SSE only. Mark those
     * XSAVE components present without discarding the interrupted AVX bank. */
    saved->xstate[512] |= 3;
    CLIENT_CHECK(pacha_syscall1(PACHA_THREAD_SYSCALL_SET_FS_BASE, registers->fs_base) == 0);
    CLIENT_CHECK(pacha_syscall1(PACHA_THREAD_SYSCALL_SET_GS_BASE, registers->gs_base) == 0);
    control->linux_fs_base = registers->fs_base;
    control->linux_gs_base = registers->gs_base;
}

int64_t lpr_dispatch_syscall_frame(struct lpr_linux_user_frame *frame,
    uint64_t number, uint64_t a0, uint64_t a1, uint64_t a2,
    uint64_t a3, uint64_t a4, uint64_t a5) {
    CLIENT_CHECK(frame->rax == number && frame->rdi == a0 && frame->rsi == a1);
    CLIENT_CHECK(frame->rdx == a2 && frame->r10 == a3 && frame->r8 == a4 && frame->r9 == a5);
    CLIENT_CHECK(control->syscall_sequence != UINT64_MAX);
    frame->rsp = ph_vm_client_user_sp;
    frame->rflags = ph_vm_client_user_flags;
    frame->r11 = ph_vm_client_user_flags;
    struct ph_vm_client_registers *registers = &control->registers;

    *registers = (struct ph_vm_client_registers) {
        .rax = frame->rax, .rbx = frame->rbx, .rcx = frame->rcx, .rdx = frame->rdx,
        .rsi = frame->rsi, .rdi = frame->rdi, .rbp = frame->rbp,
        .r8 = frame->r8, .r9 = frame->r9, .r10 = frame->r10, .r11 = frame->r11,
        .r12 = frame->r12, .r13 = frame->r13, .r14 = frame->r14, .r15 = frame->r15,
        .rip = frame->rip, .rsp = frame->rsp, .rflags = frame->rflags,
        .fs_base = control->linux_fs_base, .gs_base = control->linux_gs_base,
    };
    for (unsigned index = 0; index < sizeof(control->fp); ++index) {
        control->fp[index] = ph_vm_client_saved_fp[index];
    }
    ++control->syscall_sequence;
    publish_event(PH_VM_CLIENT_SYSCALL_ENTRY);
    CLIENT_CHECK(await_command() == PH_VM_CLIENT_RESUME);

    /* Only the owning sandbox runs Linux dispatch. Native FD numbers and
     * native errors are never substituted for the Linux syscall result. */
    frame->rax = registers->rax;
    frame->rbx = registers->rbx;
    frame->rcx = registers->rcx;
    frame->rdx = registers->rdx;
    frame->rsi = registers->rsi;
    frame->rdi = registers->rdi;
    frame->rbp = registers->rbp;
    frame->r8 = registers->r8;
    frame->r9 = registers->r9;
    frame->r10 = registers->r10;
    frame->r11 = registers->r11;
    frame->r12 = registers->r12;
    frame->r13 = registers->r13;
    frame->r14 = registers->r14;
    frame->r15 = registers->r15;
    frame->rip = registers->rip;
    frame->rsp = registers->rsp;
    frame->rflags = registers->rflags;
    ph_vm_client_return_ip = registers->rip;
    CLIENT_CHECK(pacha_syscall1(PACHA_THREAD_SYSCALL_SET_FS_BASE, registers->fs_base) == 0);
    CLIENT_CHECK(pacha_syscall1(PACHA_THREAD_SYSCALL_SET_GS_BASE, registers->gs_base) == 0);
    control->linux_fs_base = registers->fs_base;
    control->linux_gs_base = registers->gs_base;
    __asm__ volatile("fxrstor64 %0" : : "m"(control->fp) : "memory");
    return (int64_t)frame->rax;
}

_Noreturn void ph_vm_client_main(void) {
    CLIENT_CHECK(pacha_syscall4(PACHA_PROCESS_SYSCALL_SIGNAL_CTL,
        PACHA_PROCESS_SIGNAL_CTL_REGISTER_FAULT, (uintptr_t)ph_vm_client_fault_entry,
        (uintptr_t)fault_stack, sizeof(fault_stack)) == 0);
    control->pid = pacha_syscall0(PACHA_RUNTIME_SYSCALL_GETPID);
    control->syscall_entry = (uintptr_t)ph_vm_client_syscall_entry;
    publish_event(PH_VM_CLIENT_READY);

    for (;;) {
        uint64_t command = await_command();

        if (command == PH_VM_CLIENT_PARK) {
            publish_event(PH_VM_CLIENT_PARKED);
            /* No active native wait/fault is retained here. The manager can
             * STOP/GET/SET this thread, or CONTINUE it for fixture commands. */
            while (atomic_load_explicit(&control->command_sequence,
                memory_order_acquire) == execution_sequence) {
                __asm__ volatile("pause" ::: "memory");
            }
            continue;
        }
        if (command == PH_VM_CLIENT_PROTECT) {
            control->result = pacha_syscall3(PACHA_VM_SYSCALL_MPROTECT,
                control->address, 4096, control->value);
            publish_event(PH_VM_CLIENT_DONE);
            continue;
        }
        if (command == PH_VM_CLIENT_SYSCALL) {
            const struct ph_vm_client_registers *registers = &control->registers;
            const uint64_t arguments[6] = {
                registers->rdi, registers->rsi, registers->rdx,
                registers->r10, registers->r8, registers->r9,
            };

            control->result = ph_vm_client_linux_call(registers->rax, arguments);
            publish_event(PH_VM_CLIENT_DONE);
            continue;
        }
        CLIENT_CHECK(command == PH_VM_CLIENT_ACCESS);
        volatile uint64_t *address = (void *)(uintptr_t)control->address;
        uint64_t value = control->value;
        uint64_t write = control->write;

        if (write >= 7 && write <= 11) {
            control->result = ph_vm_fixture_access(write, control->address, value);
            publish_event(PH_VM_CLIENT_DONE);
            continue;
        }
        if (write == 5 || write == 6) {
            if (write == 5) {
                __asm__ volatile("mov %%fs:0, %0" : "=r"(value) : : "memory");
            } else {
                __asm__ volatile("mov %%gs:0, %0" : "=r"(value) : : "memory");
            }
            control->result = value;
            publish_event(PH_VM_CLIENT_DONE);
            continue;
        }
        CLIENT_CHECK(write <= 1);
        if (write) {
            *address = value;
        }
        control->result = *address;
        publish_event(PH_VM_CLIENT_DONE);
    }
}
