#include "lpr_filed_internal.h"
#include <personality/lpr_signal_stack.h>

__asm__(
    ".pushsection .text\n"
    ".hidden lpr_runtime_text_start\n"
    "lpr_runtime_text_start:\n"
    ".popsection\n");

enum {
    LPR_SIGNAL_FRAME_MAGIC = 0x5349474652414d45ull,
    LPR_SIGNAL_RED_ZONE = 128,
    LPR_GREG_R8 = 0,
    LPR_GREG_R9 = 1,
    LPR_GREG_R10 = 2,
    LPR_GREG_R11 = 3,
    LPR_GREG_R12 = 4,
    LPR_GREG_R13 = 5,
    LPR_GREG_R14 = 6,
    LPR_GREG_R15 = 7,
    LPR_GREG_RDI = 8,
    LPR_GREG_RSI = 9,
    LPR_GREG_RBP = 10,
    LPR_GREG_RBX = 11,
    LPR_GREG_RDX = 12,
    LPR_GREG_RAX = 13,
    LPR_GREG_RCX = 14,
    LPR_GREG_RSP = 15,
    LPR_GREG_RIP = 16,
    LPR_GREG_EFL = 17,
    LPR_GREG_CSGSFS = 18,
};

typedef struct lpr_pacha_trap_frame {
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t r11;
    uint64_t r10;
    uint64_t r9;
    uint64_t r8;
    uint64_t rbp;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rdx;
    uint64_t rcx;
    uint64_t rbx;
    uint64_t rax;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} lpr_pacha_trap_frame_t;

typedef struct lpr_pacha_signal_frame {
    uint64_t magic;
    uint64_t size;
    uint64_t signo;
    uint64_t xstate_features;
    lpr_pacha_trap_frame_t context;
    unsigned char x_state[832];
} lpr_pacha_signal_frame_t;

typedef struct lpr_linux_siginfo {
    int32_t si_signo;
    int32_t si_errno;
    int32_t si_code;
    uint32_t reserved0;
    uint64_t payload[14];
} lpr_linux_siginfo_t;

typedef struct lpr_linux_stack {
    uint64_t ss_sp;
    uint32_t ss_flags;
    uint32_t reserved0;
    uint64_t ss_size;
} lpr_linux_stack_t;

typedef struct lpr_linux_mcontext {
    uint64_t gregs[23];
    uint64_t fpregs;
    uint64_t reserved[8];
} lpr_linux_mcontext_t;

typedef struct lpr_linux_ucontext {
    uint64_t uc_flags;
    uint64_t uc_link;
    lpr_linux_stack_t uc_stack;
    lpr_linux_mcontext_t uc_mcontext;
    uint64_t uc_sigmask[16];
    unsigned char fpregs_mem[512];
} lpr_linux_ucontext_t;

typedef struct lpr_linux_signal_frame {
    uint64_t magic;
    uint64_t signo;
    uint64_t handler;
    uint64_t old_mask;
    lpr_pacha_signal_frame_t native;
    lpr_linux_siginfo_t info;
    lpr_linux_ucontext_t ucontext;
} lpr_linux_signal_frame_t;

_Static_assert(sizeof(lpr_pacha_trap_frame_t) == 160, "Pacha signal trap frame size");
_Static_assert(sizeof(lpr_pacha_signal_frame_t) == PACHAOS_PROCESS_SIGNAL_FRAME_SIZE, "Pacha signal frame size");
_Static_assert(LPR_NATIVE_SIGNAL_RED_ZONE_BYTES == LPR_SIGNAL_RED_ZONE, "native signal red zone size");
_Static_assert(LPR_NATIVE_SIGNAL_FRAME_BYTES == PACHAOS_PROCESS_SIGNAL_FRAME_SIZE, "native signal frame layout size");
_Static_assert(LPR_NATIVE_SIGNAL_RUNTIME_STACK_BYTES == PACHAOS_PROCESS_SIGNAL_RUNTIME_STACK_SIZE, "native signal runtime stack size");
_Static_assert(sizeof(lpr_linux_siginfo_t) == 128, "Linux siginfo size");
_Static_assert(sizeof(lpr_linux_mcontext_t) == 256, "Linux mcontext size");
_Static_assert(sizeof(lpr_linux_ucontext_t) == 936, "Linux ucontext size");
_Static_assert(offsetof(lpr_linux_signal_frame_t, handler) == 16, "signal handler offset");
_Static_assert(offsetof(lpr_linux_signal_frame_t, info) == 1056, "signal info offset");
_Static_assert(offsetof(lpr_linux_signal_frame_t, ucontext) == 1184, "signal ucontext offset");
_Static_assert(sizeof(lpr_linux_signal_frame_t) == 2120, "Linux signal frame size");
_Static_assert(
    PACHAOS_PROCESS_SIGNAL_RUNTIME_STACK_SIZE >
        sizeof(lpr_linux_signal_frame_t) + LPR_SIGNAL_RED_ZONE,
    "native signal runtime stack must not overlap the Linux signal frame");

extern void lpr_async_signal_entry(void);
extern void lpr_async_signal_restorer(void);
extern char lpr_runtime_text_start[];
extern char lpr_runtime_text_end[];

static int lpr_signal_sp_on_altstack(uint64_t rsp)
{
    if ((lpr_linux_altstack_flags & LPR_LINUX_SS_DISABLE) != 0 ||
        lpr_linux_altstack_sp == 0 || lpr_linux_altstack_size == 0 ||
        lpr_linux_altstack_sp > UINT64_MAX - lpr_linux_altstack_size)
    {
        return 0;
    }
    const uint64_t end = lpr_linux_altstack_sp + lpr_linux_altstack_size;
    return rsp >= lpr_linux_altstack_sp && rsp < end;
}

void lpr_linux_signal_runtime_init(void)
{
    lpr_signal_thread_state_prepare_current();
    if (lpr_linux_signal_runtime_registered) {
        return;
    }
    const int64_t status = lpr_pacha_syscall6(
        PACHAOS_SYSCALL_PROCESS_SIGNAL_CTL,
        PACHAOS_PROCESS_SIGNAL_CTL_REGISTER,
        (uint64_t)(uintptr_t)lpr_async_signal_entry,
        (uint64_t)(uintptr_t)lpr_runtime_text_start,
        (uint64_t)(uintptr_t)lpr_runtime_text_end,
        LPR_ZPOLINE_PAGE_VA,
        LPR_ZPOLINE_PAGE_VA + LPR_ZPOLINE_PAGE_SIZE);
    if (status == PACHAOS_SYSCALL_OK) {
        lpr_linux_signal_runtime_registered = 1;
    }
}

int64_t lpr_linux_sync_native_signal_mask(void)
{
    lpr_linux_signal_runtime_init();
    const int64_t status = lpr_pacha_syscall2(
        PACHAOS_SYSCALL_PROCESS_SIGNAL_CTL,
        PACHAOS_PROCESS_SIGNAL_CTL_SET_MASK,
        lpr_linux_signal_mask);
    return status == PACHAOS_SYSCALL_OK ? 0 : lpr_pacha_status_to_errno(status);
}

static _Noreturn void lpr_native_signal_return(lpr_pacha_signal_frame_t *frame)
{
    // SIGNAL_CTL_RETURN replaces the current native trap frame and therefore
    // abandons the current LPR dispatch stack instead of returning here.
    (void)lpr_pacha_syscall2(
        PACHAOS_SYSCALL_PROCESS_SIGNAL_CTL,
        PACHAOS_PROCESS_SIGNAL_CTL_RETURN,
        (uint64_t)(uintptr_t)frame);
    (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_PROCESS_EXIT, 128u + 11u);
    for (;;) {
    }
}

void lpr_linux_deliver_native_pending_frame(int64_t interrupted_result)
{
    struct lpr_linux_user_frame *interrupted_frame =
        (struct lpr_linux_user_frame *)lpr_current_linux_user_frame();
    if (interrupted_frame == 0) return;
    const uint64_t saved_rax = interrupted_frame->rax;
    interrupted_frame->rax = (uint64_t)interrupted_result;
    // A successful DELIVER_PENDING_FRAME redirects directly to the native
    // signal entry, so the interrupted dispatch and its stack-local frame
    // anchor are abandoned together.
    (void)lpr_pacha_syscall2(
        PACHAOS_SYSCALL_PROCESS_SIGNAL_CTL,
        PACHAOS_PROCESS_SIGNAL_CTL_DELIVER_PENDING_FRAME,
        (uint64_t)(uintptr_t)interrupted_frame);
    // Reached only when no signal frame was installed.
    interrupted_frame->rax = saved_rax;
}

static void lpr_signal_context_to_linux(
    const lpr_pacha_signal_frame_t *native,
    lpr_linux_ucontext_t *ucontext)
{
    const lpr_pacha_trap_frame_t *context = &native->context;
    uint64_t *gregs = ucontext->uc_mcontext.gregs;
    gregs[LPR_GREG_R8] = context->r8;
    gregs[LPR_GREG_R9] = context->r9;
    gregs[LPR_GREG_R10] = context->r10;
    gregs[LPR_GREG_R11] = context->r11;
    gregs[LPR_GREG_R12] = context->r12;
    gregs[LPR_GREG_R13] = context->r13;
    gregs[LPR_GREG_R14] = context->r14;
    gregs[LPR_GREG_R15] = context->r15;
    gregs[LPR_GREG_RDI] = context->rdi;
    gregs[LPR_GREG_RSI] = context->rsi;
    gregs[LPR_GREG_RBP] = context->rbp;
    gregs[LPR_GREG_RBX] = context->rbx;
    gregs[LPR_GREG_RDX] = context->rdx;
    gregs[LPR_GREG_RAX] = context->rax;
    gregs[LPR_GREG_RCX] = context->rcx;
    gregs[LPR_GREG_RSP] = context->rsp;
    gregs[LPR_GREG_RIP] = context->rip;
    gregs[LPR_GREG_EFL] = context->rflags;
    gregs[LPR_GREG_CSGSFS] = context->cs;
    ucontext->uc_mcontext.fpregs = (uint64_t)(uintptr_t)ucontext->fpregs_mem;
    lpr_memcpy(ucontext->fpregs_mem, native->x_state, sizeof(ucontext->fpregs_mem));
}

static void lpr_signal_context_from_linux(
    lpr_pacha_signal_frame_t *native,
    const lpr_linux_ucontext_t *ucontext)
{
    lpr_pacha_trap_frame_t *context = &native->context;
    const uint64_t *gregs = ucontext->uc_mcontext.gregs;
    context->r8 = gregs[LPR_GREG_R8];
    context->r9 = gregs[LPR_GREG_R9];
    context->r10 = gregs[LPR_GREG_R10];
    context->r11 = gregs[LPR_GREG_R11];
    context->r12 = gregs[LPR_GREG_R12];
    context->r13 = gregs[LPR_GREG_R13];
    context->r14 = gregs[LPR_GREG_R14];
    context->r15 = gregs[LPR_GREG_R15];
    context->rdi = gregs[LPR_GREG_RDI];
    context->rsi = gregs[LPR_GREG_RSI];
    context->rbp = gregs[LPR_GREG_RBP];
    context->rbx = gregs[LPR_GREG_RBX];
    context->rdx = gregs[LPR_GREG_RDX];
    context->rax = gregs[LPR_GREG_RAX];
    context->rcx = gregs[LPR_GREG_RCX];
    context->rsp = gregs[LPR_GREG_RSP];
    context->rip = gregs[LPR_GREG_RIP];
    context->rflags = gregs[LPR_GREG_EFL];
    lpr_memcpy(native->x_state, ucontext->fpregs_mem, sizeof(ucontext->fpregs_mem));
}

void *lpr_linux_async_signal_prepare(void *native_raw)
{
    lpr_pacha_signal_frame_t *native = (lpr_pacha_signal_frame_t *)native_raw;
    if (native == 0 || native->magic != PACHAOS_PROCESS_SIGNAL_FRAME_MAGIC ||
        native->size != PACHAOS_PROCESS_SIGNAL_FRAME_SIZE ||
        native->xstate_features != PACHAOS_PROCESS_SIGNAL_XSTATE_FEATURE_MASK ||
        native->signo == 0 || native->signo > LPR_LINUX_SIGNAL_MAX)
    {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_PROCESS_EXIT, 128u + 11u);
        for (;;) {
        }
    }

    const uint32_t sig = (uint32_t)native->signo;
    const uint64_t bit = lpr_linux_signal_bit(sig);
    uint64_t return_mask = lpr_linux_signal_mask;
    const int restore_wait_mask = lpr_linux_wait_restore_mask_active != 0;
    if (restore_wait_mask) {
        return_mask = lpr_linux_wait_restore_mask;
        lpr_linux_wait_restore_mask_active = 0;
    }
    if ((lpr_linux_signal_mask & bit) != 0) {
        lpr_linux_queue_signal(sig);
        if (restore_wait_mask) {
            lpr_linux_signal_mask = return_mask;
            if (lpr_linux_sync_native_signal_mask() != 0) {
                lpr_linux_exit_for_signal(11u);
            }
        }
        lpr_native_signal_return(native);
    }

    lpr_linux_sigaction_record_t *action = &lpr_linux_sigactions[sig];
    if (action->handler == LPR_LINUX_SIG_IGN ||
        (action->handler == LPR_LINUX_SIG_DFL && lpr_linux_default_signal_ignored(sig)))
    {
        if (restore_wait_mask) {
            lpr_linux_signal_mask = return_mask;
            if (lpr_linux_sync_native_signal_mask() != 0) {
                lpr_linux_exit_for_signal(11u);
            }
        }
        lpr_native_signal_return(native);
    }
    if (action->handler == LPR_LINUX_SIG_DFL) {
        if (lpr_linux_default_signal_stops(sig)) {
            lpr_linux_queue_signal(sig);
            if (restore_wait_mask) {
                lpr_linux_signal_mask = return_mask;
                if (lpr_linux_sync_native_signal_mask() != 0) {
                    lpr_linux_exit_for_signal(11u);
                }
            }
            lpr_native_signal_return(native);
        }
        lpr_linux_exit_for_signal(sig);
    }

    uint64_t stack_top = (uint64_t)(uintptr_t)native;
    const int already_on_altstack = lpr_signal_sp_on_altstack(native->context.rsp);
    const int altstack_enabled =
        (lpr_linux_altstack_flags & LPR_LINUX_SS_DISABLE) == 0 &&
        lpr_linux_altstack_sp != 0 &&
        lpr_linux_altstack_size >= sizeof(lpr_linux_signal_frame_t) + 16u &&
        lpr_linux_altstack_sp <= UINT64_MAX - lpr_linux_altstack_size;
    int use_altstack = 0;
    if ((action->flags & LPR_LINUX_SA_ONSTACK) != 0 && altstack_enabled && !already_on_altstack) {
        stack_top = lpr_linux_altstack_sp + lpr_linux_altstack_size;
        use_altstack = 1;
    } else {
        if (stack_top <= LPR_SIGNAL_RED_ZONE) {
            lpr_linux_exit_for_signal(11u);
        }
        stack_top -= LPR_SIGNAL_RED_ZONE;
    }
    if (stack_top <= sizeof(lpr_linux_signal_frame_t) + 8u) {
        lpr_linux_exit_for_signal(11u);
    }
    const uint64_t body_va =
        (stack_top - (uint64_t)sizeof(lpr_linux_signal_frame_t)) & ~15ull;
    const uint64_t return_slot_va = body_va - 8u;
    if (use_altstack && return_slot_va < lpr_linux_altstack_sp) {
        lpr_linux_exit_for_signal(11u);
    }

    lpr_linux_signal_frame_t *body =
        (lpr_linux_signal_frame_t *)(uintptr_t)body_va;
    lpr_memset(body, 0, sizeof(*body));
    body->magic = LPR_SIGNAL_FRAME_MAGIC;
    body->signo = sig;
    body->handler = action->handler;
    body->old_mask = return_mask;
    lpr_memcpy(&body->native, native, sizeof(body->native));
    body->info.si_signo = (int32_t)sig;
    body->info.si_code = 0;
    body->ucontext.uc_stack.ss_sp = lpr_linux_altstack_sp;
    body->ucontext.uc_stack.ss_size = lpr_linux_altstack_size;
    body->ucontext.uc_stack.ss_flags = lpr_linux_altstack_flags;
    if (use_altstack || already_on_altstack) {
        body->ucontext.uc_stack.ss_flags |= LPR_LINUX_SS_ONSTACK;
    } else if (!altstack_enabled) {
        body->ucontext.uc_stack.ss_flags |= LPR_LINUX_SS_DISABLE;
    }
    body->ucontext.uc_sigmask[0] = return_mask;
    lpr_signal_context_to_linux(&body->native, &body->ucontext);

    /* Linux libc restorers execute rt_sigreturn with a raw SYSCALL.  LPR
     * cannot delegate to that instruction: executable patching is not
     * guaranteed to discover standalone restorer stubs, and a missed stub
     * would enter the native Pacha syscall table.  Keep the application
     * restorer in the emulated sigaction record, but always return through
     * the LPR restorer that translates the Linux frame to the native one. */
    *(uint64_t *)(uintptr_t)return_slot_va =
        (uint64_t)(uintptr_t)lpr_async_signal_restorer;

    lpr_linux_signal_mask |= action->mask;
    if ((action->flags & LPR_LINUX_SA_NODEFER) == 0) {
        lpr_linux_signal_mask |= bit;
    }
    lpr_linux_signal_mask &= ~lpr_linux_unblockable_signal_mask();
    if (lpr_linux_sync_native_signal_mask() != 0) {
        lpr_linux_exit_for_signal(11u);
    }
    if ((action->flags & LPR_LINUX_SA_RESETHAND) != 0) {
        action->handler = LPR_LINUX_SIG_DFL;
    }
    return body;
}

_Noreturn void lpr_linux_rt_sigreturn_body(void *body_raw)
{
    lpr_linux_signal_frame_t *body = (lpr_linux_signal_frame_t *)body_raw;
    if (body == 0 || body->magic != LPR_SIGNAL_FRAME_MAGIC ||
        body->native.magic != PACHAOS_PROCESS_SIGNAL_FRAME_MAGIC ||
        body->native.size != PACHAOS_PROCESS_SIGNAL_FRAME_SIZE ||
        body->native.xstate_features != PACHAOS_PROCESS_SIGNAL_XSTATE_FEATURE_MASK)
    {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_PROCESS_EXIT, 128u + 11u);
        for (;;) {
        }
    }
    lpr_signal_context_from_linux(&body->native, &body->ucontext);
    lpr_linux_signal_mask =
        body->ucontext.uc_sigmask[0] & ~lpr_linux_unblockable_signal_mask();
    if (lpr_linux_sync_native_signal_mask() != 0) {
        lpr_linux_exit_for_signal(11u);
    }
    lpr_native_signal_return(&body->native);
}

_Noreturn void lpr_linux_rt_sigreturn_frame(const struct lpr_linux_user_frame *frame)
{
    if (frame == 0 || frame->rsp == 0) {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_PROCESS_EXIT, 128u + 11u);
        for (;;) {
        }
    }
    lpr_linux_rt_sigreturn_body((void *)(uintptr_t)frame->rsp);
}

int64_t lpr_linux_sigaltstack(uint64_t ss_raw, uint64_t old_ss_raw)
{
    uint64_t rsp = 0;
    const struct lpr_linux_user_frame *active_frame =
        lpr_current_linux_user_frame();
    if (active_frame != 0) {
        rsp = active_frame->rsp;
    }
    const int on_altstack = lpr_signal_sp_on_altstack(rsp);
    if (old_ss_raw != 0) {
        lpr_linux_stack_t *old_ss = (lpr_linux_stack_t *)(uintptr_t)old_ss_raw;
        old_ss->ss_sp = lpr_linux_altstack_sp;
        old_ss->ss_size = lpr_linux_altstack_size;
        old_ss->reserved0 = 0;
        old_ss->ss_flags = lpr_linux_altstack_flags;
        if (on_altstack) {
            old_ss->ss_flags |= LPR_LINUX_SS_ONSTACK;
        } else if (lpr_linux_altstack_sp == 0) {
            old_ss->ss_flags |= LPR_LINUX_SS_DISABLE;
        }
    }
    if (ss_raw == 0) {
        return 0;
    }
    if (on_altstack) {
        return -LPR_LINUX_EPERM;
    }
    const lpr_linux_stack_t *ss =
        (const lpr_linux_stack_t *)(uintptr_t)ss_raw;
    if ((ss->ss_flags & ~(LPR_LINUX_SS_DISABLE | LPR_LINUX_SS_AUTODISARM)) != 0) {
        return -LPR_LINUX_EINVAL;
    }
    if ((ss->ss_flags & LPR_LINUX_SS_DISABLE) != 0) {
        lpr_linux_altstack_sp = 0;
        lpr_linux_altstack_size = 0;
        lpr_linux_altstack_flags = LPR_LINUX_SS_DISABLE;
        return 0;
    }
    if (ss->ss_sp == 0 || ss->ss_size < LPR_LINUX_MINSIGSTKSZ ||
        ss->ss_sp > UINT64_MAX - ss->ss_size)
    {
        return -LPR_LINUX_ENOMEM;
    }
    lpr_linux_altstack_sp = ss->ss_sp;
    lpr_linux_altstack_size = ss->ss_size;
    lpr_linux_altstack_flags = ss->ss_flags & LPR_LINUX_SS_AUTODISARM;
    return 0;
}
