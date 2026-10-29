/* SPDX-License-Identifier: MIT */
/* Child address spaces make stop/kill ACK observable independently of local
 * FD close; the copied integer/assembly image uses no parent-private pointers. */
#include <stdint.h>
#include <stddef.h>
#include <pacha/ipc.h>
#include <pacha/syscall.h>

#define STR1(x) #x
#define STR(x) STR1(x)
#define PAGE 4096ul
#define CODE 0x56000000ul
#define CONTROL 0x57000000ul
#define STACK 0x58000000ul
#define RW (PACHA_PROT_READ | PACHA_PROT_WRITE)
#define LOAD(x) __atomic_load_n(&(x), __ATOMIC_ACQUIRE)
struct ww_control {
    uint64_t count[2], notifications[2], return_notifications[2], error;
    unsigned permits[2];
    uint64_t wakes[2], fs[2], gs[2], fp[2][2], return_status[2];
};
_Static_assert(offsetof(struct ww_control, permits) == 56, "child permits");
_Static_assert(offsetof(struct ww_control, fp) == 112, "child FP patterns");
_Static_assert(offsetof(struct pacha_native_signal_frame, xstate) == 192, "signal FP bank");
_Static_assert(offsetof(struct pacha_native_signal_frame, rip) == 152, "signal return RIP");
_Static_assert(offsetof(struct pacha_native_signal_frame, rax) == 144, "signal return RAX");
extern unsigned char ww_remote_blob[], ww_remote_worker1[], ww_remote_end[];

__asm__(".pushsection ww_remote_code,\"ax\",@progbits\n"
    ".global ww_remote_blob,ww_remote_worker1,ww_remote_end\n"
    "ww_remote_blob: xor %r13d,%r13d; jmp ww_remote_start\n"
    "ww_remote_worker1: mov $1,%r13d\n"
    "ww_remote_start: mov $0x57000000,%r12; mov %r13,%rbx; shl $4,%rbx\n"
    "movdqu 112(%r12,%rbx),%xmm15\n"
    "mov $1,%ebp; mov $" STR(PACHA_PROCESS_SIGNAL_CTL_REGISTER) ",%edi\n"
    "sub $64,%rsp; lea ww_remote_handler(%rip),%rax\n"
    "mov %rax,0(%rsp); mov %rax,8(%rsp); lea ww_remote_handler_end(%rip),%rax\n"
    "mov %rax,16(%rsp); movq $0,24(%rsp); movq $0,32(%rsp)\n"
    "movq $0,40(%rsp); movq $0,48(%rsp); movq $1,56(%rsp)\n"
    "mov %rsp,%rsi; mov $" STR(PACHA_PROCESS_SYSCALL_SIGNAL_CTL) ",%eax; syscall\n"
    "add $64,%rsp\n"
    "test %rax,%rax; jnz ww_remote_bad\n"
    "mov $2,%ebp; mov $" STR(PACHA_PROCESS_SIGNAL_CTL_SET_MASK) ",%edi; xor %esi,%esi\n"
    "mov $" STR(PACHA_PROCESS_SYSCALL_SIGNAL_CTL) ",%eax; syscall\n"
    "test %rax,%rax; jnz ww_remote_bad\n"
    "ww_remote_loop: mov $3,%ebp; mov %fs:0,%rax; cmp 80(%r12,%r13,8),%rax; jne ww_remote_bad\n"
    "mov $4,%ebp; mov %gs:0,%rax; cmp 96(%r12,%r13,8),%rax; jne ww_remote_bad\n"
    "mov $5,%ebp; movq %xmm15,%rax; cmp 112(%r12,%rbx),%rax; jne ww_remote_bad\n"
    "movdqa %xmm15,%xmm0; psrldq $8,%xmm0; movq %xmm0,%rax\n"
    "mov $6,%ebp; cmp 120(%r12,%rbx),%rax; jne ww_remote_bad\n"
    "cmpl $0,56(%r12,%r13,4); jne ww_remote_take\n"
    "lea 56(%r12,%r13,4),%rdi; xor %esi,%esi; mov $20,%edx\n"
    "xor %r10d,%r10d\n"
    "mov $" STR(PACHA_RUNTIME_SYSCALL_FUTEX_WAIT) ",%eax; syscall; jmp ww_remote_loop\n"
    "ww_remote_take: movl $0,56(%r12,%r13,4); mov $1,%r14d; sub %r13,%r14\n"
    "movl $1,56(%r12,%r14,4); addq $1,0(%r12,%r13,8)\n"
    "ww_remote_compound: lea 56(%r12,%r14,4),%rdi; lea 56(%r12,%r13,4),%rsi\n"
    "xor %r10d,%r10d; mov $20,%r8d\n"
    "xor %r9d,%r9d\n"
    "mov $" STR(PACHA_RUNTIME_SYSCALL_FUTEX_WAKE_WAIT) ",%eax; syscall\n"
    "ww_remote_resume: mov $8,%ebp; cmp $1,%rdx; jbe ww_remote_completed\n"
    /* Lifecycle admission can reject before any outbound wake. Only that
     * explicit sentinel permits retry; a committed count0/1 never does. */
    "cmp $-1,%rdx; jne ww_remote_bad\n"
    "cmp $" STR(PACHA_SYSCALL_ERR_NOT_READY) ",%rax; jne ww_remote_bad\n"
    "pause; jmp ww_remote_compound\n"
    "ww_remote_completed: test %rax,%rax; jz ww_remote_count\n"
    "cmp $" STR(PACHA_SYSCALL_ERR_NOT_READY) ",%rax; je ww_remote_count\n"
    "cmp $" STR(PACHA_FUTEX_WAIT_TIMED_OUT_STATUS) ",%rax; jne ww_remote_bad\n"
    "ww_remote_count:\n"
    "add %rdx,64(%r12,%r13,8); jmp ww_remote_loop\n"
    "ww_remote_bad: mov %rbp,48(%r12); pause; jmp ww_remote_bad\n"
    "ww_remote_handler: mov %rdi,%r15\n"
    "mov $9,%ebp; mov %fs:0,%rax; cmp 80(%r12,%r13,8),%rax; jne ww_remote_handler_bad\n"
    "mov $10,%ebp; mov %gs:0,%rax; cmp 96(%r12,%r13,8),%rax; jne ww_remote_handler_bad\n"
    "mov $11,%ebp; mov 592(%r15),%rax; cmp 112(%r12,%rbx),%rax; jne ww_remote_handler_bad\n"
    "mov $12,%ebp; mov 600(%r15),%rax; cmp 120(%r12,%rbx),%rax; jne ww_remote_handler_bad\n"
    "addq $1,16(%r12,%r13,8); lea ww_remote_resume(%rip),%rax\n"
    "cmp 152(%r15),%rax; jne ww_remote_handler_return\n"
    "cmpq $0,144(%r15); jne ww_remote_handler_return\n"
    "addq $1,32(%r12,%r13,8); jmp ww_remote_handler_return\n"
    "ww_remote_handler_bad: mov %rbp,48(%r12)\n"
    "ww_remote_handler_return: pxor %xmm15,%xmm15; mov %r15,%rsi\n"
    "ww_remote_signal_return:\n"
    "mov $" STR(PACHA_PROCESS_SIGNAL_CTL_RETURN) ",%edi\n"
    "mov $" STR(PACHA_PROCESS_SYSCALL_SIGNAL_CTL) ",%eax; syscall\n"
    /* STOP may block admission before signal-return restored anything. Keep
     * the frame intact and retry that NOT_READY result after CONTINUE. */
    "cmp $" STR(PACHA_SYSCALL_ERR_NOT_READY) ",%rax; je ww_remote_signal_return\n"
    "mov %rax,144(%r12,%r13,8); mov $20,%ebp; jmp ww_remote_bad\n"
    "ww_remote_handler_end:\nww_remote_end:\n.popsection\n");

static int failure(unsigned line)
{
    static const char prefix[] = "NATIVE_WAKE_WAIT_LIFECYCLE=FAIL line=";
    pacha_syscall2(1, (uintptr_t)prefix, sizeof(prefix) - 1);
    char digits[16]; unsigned n = 0;
    do { digits[n++] = '0' + line % 10; line /= 10; } while (line);
    for (unsigned i = 0; i < n / 2; ++i) {
        char c = digits[i]; digits[i] = digits[n-i-1]; digits[n-i-1] = c;
    }
    digits[n++] = '\n'; pacha_syscall2(1, (uintptr_t)digits, n);
    return 1;
}
#define CHECK(x) do { if (!(x)) return failure(__LINE__); } while (0)
static void diagnostic(struct ww_control *control) {
    const char *names[] = {"WW_DEBUG error=", "WW_DEBUG count0=", "WW_DEBUG count1=",
                          "WW_DEBUG notify0=", "WW_DEBUG notify1=", "WW_DEBUG return0=", "WW_DEBUG return1="};
    uint64_t values[] = {LOAD(control->error), LOAD(control->count[0]), LOAD(control->count[1]),
                        LOAD(control->notifications[0]), LOAD(control->notifications[1]),
                        LOAD(control->return_status[0]), LOAD(control->return_status[1])};
    for (unsigned i = 0; i < 7; ++i) {
        size_t length = 0; while (names[i][length]) ++length;
        pacha_syscall2(1, (uintptr_t)names[i], length);
        char digits[24]; unsigned n = 0; uint64_t value = values[i];
        do { digits[n++] = '0'+value%10; value /= 10; } while (value);
        for (unsigned j = 0; j < n/2; ++j) {
            char c = digits[j]; digits[j] = digits[n-j-1]; digits[n-j-1] = c;
        }
        digits[n++] = '\n'; pacha_syscall2(1, (uintptr_t)digits, n);
    }
}
static uint64_t now(void) {
    uint64_t value;
    return pacha_clock_read(1, &value) ? UINT64_MAX : value;
}
static int await_progress(struct ww_control *control, const uint64_t previous[2]) {
    uint64_t start = now(); CHECK(start != UINT64_MAX);
    while (LOAD(control->count[0]) <= previous[0]+64 || LOAD(control->count[1]) <= previous[1]+64) {
        if (LOAD(control->error) || now()-start >= 3000000000ull) {
            diagnostic(control); return failure(__LINE__);
        }
        __asm__ volatile("pause");
    }
    return 0;
}
static int unchanged(struct ww_control *control, const uint64_t counts[2], const uint64_t notifications[2]) {
    uint64_t start = now(); CHECK(start != UINT64_MAX);
    do {
        CHECK(!LOAD(control->error));
        for (unsigned i = 0; i < 2; ++i)
            CHECK(LOAD(control->count[i]) == counts[i] && LOAD(control->notifications[i]) == notifications[i]);
    } while (now()-start < 3000000);
    return 0;
}

int native_wake_wait_lifecycle_contract(void)
{
    const uint64_t vmo_rights = PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE |
        PACHA_FD_RIGHT_MAP_EXEC | PACHA_FD_RIGHT_CLOSE;
    const long code = pacha_syscall3(PACHA_FD_SYSCALL_VMO_CREATE, PAGE, vmo_rights, 0);
    CHECK(code >= 16);
    const long code_alias = pacha_syscall6(PACHA_VM_SYSCALL_MMAP, code, 0, PAGE, RW, PACHA_MMAP_SHARED, 0);
    CHECK(code_alias >= (long)PAGE);
    const size_t bytes = (uintptr_t)ww_remote_end-(uintptr_t)ww_remote_blob;
    CHECK(bytes && bytes <= PAGE);
    for (size_t i = 0; i < bytes; ++i) ((unsigned char *)(uintptr_t)code_alias)[i] = ww_remote_blob[i];
    const uint64_t process_rights = PACHA_FD_RIGHT_MAP_INTO | PACHA_FD_RIGHT_SPAWN |
        PACHA_FD_RIGHT_SET_CONTEXT | PACHA_FD_RIGHT_KILL | PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_CLOSE;
    for (unsigned generation = 0; generation < 4; ++generation) {
        const long process = pacha_syscall5(PACHA_PROCESS_SYSCALL_CREATE, 0, process_rights, 0, 0, 0);
        CHECK(process >= 16);
        const long control_fd = pacha_syscall3(PACHA_FD_SYSCALL_VMO_CREATE, PAGE, vmo_rights, 0);
        CHECK(control_fd >= 16);
        const long alias = pacha_syscall6(PACHA_VM_SYSCALL_MMAP, control_fd, 0, PAGE, RW, PACHA_MMAP_SHARED, 0);
        CHECK(alias >= (long)PAGE);
        struct ww_control *control = (void *)(uintptr_t)alias;
        for (size_t i = 0; i < sizeof(*control); ++i) ((unsigned char *)control)[i] = 0;
        control->permits[0] = 1;
        for (unsigned i = 0; i < 2; ++i) {
            control->fs[i] = 0x123456789abcd000ull+i;
            control->gs[i] = 0xfedcba9876543000ull+i;
            control->fp[i][0] = 0x1029384756abcd00ull+i;
            control->fp[i][1] = 0xfedcba6547382900ull+i;
        }
        CHECK(pacha_syscall6(PACHA_PROCESS_SYSCALL_MAP, process, code, CODE, PAGE,
            PACHA_PROT_READ | PACHA_PROT_EXEC, PACHA_PROCESS_MAP_SHARED) == CODE);
        CHECK(pacha_syscall6(PACHA_PROCESS_SYSCALL_MAP, process, control_fd, CONTROL, PAGE,
            RW, PACHA_PROCESS_MAP_SHARED) == CONTROL);
        CHECK(pacha_syscall6(PACHA_PROCESS_SYSCALL_MAP, process, 0, STACK, PAGE*8,
            RW, PACHA_PROCESS_MAP_PRIVATE | PACHA_PROCESS_MAP_ANONYMOUS) == STACK);
        uint64_t threads[2];
        for (unsigned i = 0; i < 2; ++i) {
            uint64_t entry = CODE + (i ? (uintptr_t)ww_remote_worker1-(uintptr_t)ww_remote_blob : 0);
            long thread = pacha_syscall6(PACHA_THREAD_SYSCALL_CREATE, process, entry, STACK+(i+1)*PAGE*4,
                0, 0, PACHA_FD_RIGHT_START | PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_INSPECT |
                PACHA_FD_RIGHT_SET_CONTEXT | PACHA_FD_RIGHT_PROCESS_SIGNAL);
            CHECK(thread >= 16); threads[i] = thread;
            struct pacha_thread_context context;
            CHECK(!pacha_syscall4(PACHA_THREAD_SYSCALL_CONTEXT, thread, PACHA_THREAD_CONTEXT_GET,
                (uintptr_t)&context, sizeof(context)));
            context.fs_base = CONTROL+80+i*8;
            context.gs_base = CONTROL+96+i*8;
            CHECK(!pacha_syscall4(PACHA_THREAD_SYSCALL_CONTEXT, thread, PACHA_THREAD_CONTEXT_SET,
                (uintptr_t)&context, sizeof(context)));
        }
        for (unsigned i = 0; i < 2; ++i) CHECK(!pacha_syscall1(PACHA_THREAD_SYSCALL_START, threads[i]));
        uint64_t counts[2] = {0,0}, notifications[2];
        CHECK(!await_progress(control, counts));
        /* RAX=0 and the exact compound-syscall resume RIP distinguish a peer
         * wake from a signal cancelling that receiver's pending wait. */
        uint64_t start = now();
        while (!LOAD(control->return_notifications[0]) || !LOAD(control->return_notifications[1])) {
            if (LOAD(control->error) || now()-start >= 3000000000ull) {
                diagnostic(control); return failure(__LINE__);
            }
            for (unsigned i = 0; i < 2; ++i)
                CHECK(!pacha_syscall2(PACHA_THREAD_SYSCALL_SIGNAL, threads[i], 12));
        }
        for (unsigned cycle = 0; cycle < 4; ++cycle) {
            CHECK(!pacha_syscall2(PACHA_PROCESS_SYSCALL_STOP, process, 19));
            for (unsigned i = 0; i < 2; ++i) {
                counts[i] = LOAD(control->count[i]); notifications[i] = LOAD(control->notifications[i]);
            }
            CHECK(!unchanged(control, counts, notifications));
            CHECK(!pacha_syscall2(PACHA_PROCESS_SYSCALL_CONTINUE, process, 0));
            CHECK(!await_progress(control, counts));
        }
        CHECK(LOAD(control->wakes[0]) && LOAD(control->wakes[1]));
        CHECK(!pacha_syscall2(PACHA_PROCESS_SYSCALL_KILL, process, 73));
        for (unsigned i = 0; i < 2; ++i) {
            counts[i] = LOAD(control->count[i]); notifications[i] = LOAD(control->notifications[i]);
        }
        CHECK(!unchanged(control, counts, notifications));
        uint64_t status[4];
        CHECK(!pacha_syscall2(PACHA_PROCESS_SYSCALL_WAIT, process, (uintptr_t)status));
        CHECK(status[0] == 3 && status[1] == 73);
        for (unsigned i = 0; i < 2; ++i) CHECK(!pacha_syscall1(PACHA_FD_SYSCALL_CLOSE, threads[i]));
        CHECK(!pacha_syscall1(PACHA_FD_SYSCALL_CLOSE, process));
        CHECK(!pacha_syscall2(PACHA_VM_SYSCALL_MUNMAP, alias, PAGE));
        CHECK(!pacha_syscall1(PACHA_FD_SYSCALL_CLOSE, control_fd));
    }
    CHECK(!pacha_syscall2(PACHA_VM_SYSCALL_MUNMAP, code_alias, PAGE));
    CHECK(!pacha_syscall1(PACHA_FD_SYSCALL_CLOSE, code));
    static const char passed[] = "NATIVE_WAKE_WAIT_RECEIVER_RETURN=PASS\nNATIVE_WAKE_WAIT_LIFECYCLE=PASS\n";
    pacha_syscall2(1, (uintptr_t)passed, sizeof(passed)-1);
    return 0;
}
