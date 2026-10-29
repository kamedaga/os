/* SPDX-License-Identifier: MIT */
/* The production callbacks and actual native syscall, without libc/LPR. */
#include <stdint.h>
#include <stddef.h>
#include <pacha/ipc.h>
#include <pacha/syscall.h>
#include "../userland/kobox2_adapter/clock_callbacks.h"

struct fp_bank { unsigned char bytes[512]; } __attribute__((aligned(16)));
static struct fp_bank seed, nested_seed, clobber;
static uint64_t output;
static volatile uint64_t signal_value, signal_rip, signal_count;
static volatile int self_fd;
extern int notification_fp_invoke(int (*)(uint64_t), uint64_t,
    const struct fp_bank *, struct fp_bank *, struct fp_bank *);
static __attribute__((used, noinline)) KOBOX_GPR_LEAF int mono_call(uint64_t out) { return monotonic((void *)(uintptr_t)out); }
static KOBOX_GPR_LEAF int real_call(uint64_t out) { return realtime((void *)(uintptr_t)out); }
static KOBOX_GPR_LEAF int invalid_call(uint64_t out) { return clock_ns(99, (void *)(uintptr_t)out); }

static void log_line(const char *text) {
    size_t length = 0;
    while (text[length]) ++length;
    pacha_syscall2(1, (uintptr_t)text, length);
}
static void number(const char *label, uint64_t value) {
    log_line(label);
    char digits[24]; unsigned used = 0;
    do { digits[used++] = '0' + value % 10; value /= 10; } while (value);
    for (unsigned i = 0; i < used / 2; ++i) {
        char c = digits[i]; digits[i] = digits[used - i - 1]; digits[used - i - 1] = c;
    }
    digits[used++] = '\n';
    pacha_syscall2(1, (uintptr_t)digits, used);
}
static _Noreturn void fail(unsigned line) {
    number("NATIVE_CLOCK_FP=FAIL line=", line);
    pacha_syscall1(PACHA_PROCESS_SYSCALL_EXIT, 1);
    __builtin_trap();
}
#define CHECK(x) do { if (!(x)) fail(__LINE__); } while (0)
static void make_bank(struct fp_bank *bank, unsigned salt) {
    for (unsigned i = 0; i < 512; ++i) bank->bytes[i] = 0;
    bank->bytes[0] = 0x7f; bank->bytes[1] = 0x0b;
    bank->bytes[2] = 0x25; bank->bytes[4] = 0xff;
    bank->bytes[6] = salt; bank->bytes[7] = 1;
    bank->bytes[8] = salt; bank->bytes[9] = 0x56; bank->bytes[10] = 0x12;
    bank->bytes[16] = salt; bank->bytes[17] = 0x67; bank->bytes[18] = 0x23;
    bank->bytes[24] = 0xa5; bank->bytes[25] = 0x7f;
    for (unsigned i = 0; i < 8; ++i) {
        bank->bytes[32 + i * 16] = salt + i;
        bank->bytes[39 + i * 16] = 0x80;
        bank->bytes[40 + i * 16] = 0xff; bank->bytes[41 + i * 16] = 0x3f;
    }
    for (unsigned i = 160; i < 416; ++i) bank->bytes[i] = salt + i;
}
static void same_range(const struct fp_bank *a, const struct fp_bank *b, unsigned start, unsigned end) {
    for (unsigned i = start; i < end; ++i) CHECK(a->bytes[i] == b->bytes[i]);
}
static void same_bank(const struct fp_bank *a, const struct fp_bank *b) {
    same_range(a, b, 0, 5); same_range(a, b, 6, 28);
    for (unsigned i = 0; i < 8; ++i) same_range(a, b, 32 + i * 16, 42 + i * 16);
    same_range(a, b, 160, 416);
}
static int checked(int (*fn)(uint64_t), uint64_t argument, const struct fp_bank *bank) {
    struct fp_bank observed, installed;
    int status = notification_fp_invoke(fn, argument, bank, &observed, &installed);
    same_bank(bank, &installed); same_bank(bank, &observed);
    return status;
}

extern unsigned char __start_clock_fp_handlers[], __stop_clock_fp_handlers[];
extern unsigned char clock_fp_pending_begin[], clock_fp_pending_end[];
extern void clock_fp_signal_entry(void);
extern int clock_fp_pending_call(uint64_t);
__attribute__((used, noinline, section("clock_fp_handlers")))
static void clock_fp_signal_body(struct pacha_native_signal_frame *frame) {
    CHECK(frame->signo == 12 && frame->rax == 0 && frame->rdx != 0);
    signal_value = frame->rdx; signal_rip = frame->rip; ++signal_count;
    uint64_t nested_output;
    CHECK(!checked(mono_call, (uintptr_t)&nested_output, &nested_seed));
    CHECK(nested_output >= signal_value);
    asm volatile("fxrstorq %0" : : "m"(clobber) : "memory");
}
#define STR1(x) #x
#define STR(x) STR1(x)
__asm__(".pushsection clock_fp_handlers,\"ax\",@progbits\n"
    ".global clock_fp_signal_entry\nclock_fp_signal_entry:\n"
    "mov %rdi,%r12; call clock_fp_signal_body; mov %r12,%rsi\n"
    "mov $3,%edi; mov $" STR(PACHA_PROCESS_SYSCALL_SIGNAL_CTL) ",%eax; syscall; ud2\n"
    ".popsection\n.text\n.global clock_fp_pending_call\nclock_fp_pending_call:\n"
    "push %rbx; mov %rdi,%rbx\n.global clock_fp_pending_begin\nclock_fp_pending_begin:\n"
    "mov self_fd(%rip),%edi; mov $12,%esi; mov $" STR(PACHA_THREAD_SYSCALL_SIGNAL) ",%eax; syscall\n"
    "test %rax,%rax; jnz clock_fp_pending_end\n"
    "mov %rbx,%rdi; call mono_call\n.global clock_fp_pending_end\nclock_fp_pending_end:\n"
    "pop %rbx; ret\n");

int main(void) {
    make_bank(&seed, 0x31); make_bank(&nested_seed, 0x61); make_bank(&clobber, 0x91);
    for (unsigned i = 0; i < 128; ++i) {
        CHECK(!checked(mono_call, (uintptr_t)&output, &seed) && output);
        CHECK(!checked(real_call, (uintptr_t)&output, &seed) && output);
        output = UINT64_MAX;
        CHECK(checked(invalid_call, (uintptr_t)&output, &seed) == PACHA_SYSCALL_ERR_INVALID && !output);
    }
    log_line("NATIVE_CLOCK_FP_BANK=PASS\n");
    const uint64_t rights = PACHA_FD_RIGHT_PROCESS_SIGNAL | PACHA_FD_RIGHT_CLOSE;
    long fd = pacha_syscall4(PACHA_FD_SYSCALL_DUP, PACHA_THREAD_SELF_FD, 16, rights, 0);
    CHECK(fd >= 16); self_fd = fd;
    CHECK(!pacha_syscall6(PACHA_PROCESS_SYSCALL_SIGNAL_CTL, PACHA_PROCESS_SIGNAL_CTL_REGISTER,
        (uintptr_t)&(struct pacha_signal_registration){
            .entry = (uintptr_t)clock_fp_signal_entry,
            .inhibit_start = (uintptr_t)__start_clock_fp_handlers,
            .inhibit_end = (uintptr_t)__stop_clock_fp_handlers,
            .inhibit_secondary_start = (uintptr_t)clock_fp_pending_begin,
            .inhibit_secondary_end = (uintptr_t)clock_fp_pending_end,
            .scratch_base = 0,
            .scratch_size = 0,
            .flags = PACHA_SIGNAL_REGISTRATION_PROCESS_OWNER,
        }, 0, 0, 0, 0));
    CHECK(!pacha_syscall2(PACHA_PROCESS_SYSCALL_SIGNAL_CTL, PACHA_PROCESS_SIGNAL_CTL_SET_MASK, 0));
    CHECK(!checked(clock_fp_pending_call, (uintptr_t)&output, &seed));
    CHECK(signal_count == 1 && output == signal_value);
    number("CLOCK_FP_SYSCALL_ENTRY=", (uintptr_t)pacha_clock_read);
    number("CLOCK_FP_SIGNAL_RIP=", signal_rip);
    CHECK(!pacha_syscall1(PACHA_FD_SYSCALL_CLOSE, self_fd));
    log_line("NATIVE_CLOCK_FP_SIGNAL=PASS\nNATIVE_CLOCK_FP=PASS\n");
    return 0;
}
