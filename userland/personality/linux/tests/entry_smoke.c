#include <stdint.h>
#include <personality/linux_lpr.h>

extern void lpr_syscall_entry(void);

struct captured_call {
    uint64_t nr;
    uint64_t a0;
    uint64_t a1;
    uint64_t a2;
    uint64_t a3;
    uint64_t a4;
    uint64_t a5;
};

static struct captured_call g_last;

int64_t lpr_dispatch_syscall(uint64_t nr,
                             uint64_t a0,
                             uint64_t a1,
                             uint64_t a2,
                             uint64_t a3,
                             uint64_t a4,
                             uint64_t a5) {
    g_last.nr = nr;
    g_last.a0 = a0;
    g_last.a1 = a1;
    g_last.a2 = a2;
    g_last.a3 = a3;
    g_last.a4 = a4;
    g_last.a5 = a5;
    return 0x123456789abcdef0ll;
}

static int expect(int condition) {
    return condition ? 0 : 1;
}

static int64_t call_lpr_fixed_args(void) {
    int64_t ret;
    __asm__ volatile(
        "mov $0x11, %%rax\n"
        "mov $0x22, %%rdi\n"
        "mov $0x33, %%rsi\n"
        "mov $0x44, %%rdx\n"
        "mov $0x55, %%r10\n"
        "mov $0x66, %%r8\n"
        "mov $0x77, %%r9\n"
        "call lpr_syscall_entry\n"
        : "=a"(ret)
        :
        : "rcx", "r11", "rdi", "rsi", "rdx", "r8", "r9", "r10", "memory");
    return ret;
}

int main(void) {
    if (expect(call_lpr_fixed_args() == (int64_t)0x123456789abcdef0ull)) return 1;
    if (expect(g_last.nr == 0x11)) return 1;
    if (expect(g_last.a0 == 0x22)) return 1;
    if (expect(g_last.a1 == 0x33)) return 1;
    if (expect(g_last.a2 == 0x44)) return 1;
    if (expect(g_last.a3 == 0x55)) return 1;
    if (expect(g_last.a4 == 0x66)) return 1;
    if (expect(g_last.a5 == 0x77)) return 1;
    return 0;
}
