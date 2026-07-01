#include "lpr_memory.h"
#include "lpr_linux_syscall.h"
#include "support/syscall.h"

#include <pachaos/abi.h>

enum {
    LPR_BRK_RESERVE_SIZE = 64ull * 1024ull * 1024ull,
};

static uint64_t g_brk_base;
static uint64_t g_brk_current;
static uint64_t g_brk_limit;

static uint64_t align_up_page(uint64_t value)
{
    const uint64_t mask = 4095ull;
    if (value > UINT64_MAX - mask) {
        return 0;
    }
    return (value + mask) & ~mask;
}

static int init_brk(void)
{
    if (g_brk_base != 0) {
        return 0;
    }
    const int64_t mapped = lpr_pacha_syscall6(
        PACHAOS_SYSCALL_MMAP,
        0,
        0,
        LPR_BRK_RESERVE_SIZE,
        PACHAOS_PROT_READ | PACHAOS_PROT_WRITE,
        PACHAOS_MMAP_PRIVATE | PACHAOS_MMAP_ANONYMOUS | PACHAOS_MMAP_NORESERVE,
        0);
    if (mapped < 4096) {
        return -1;
    }
    g_brk_base = (uint64_t)mapped;
    g_brk_current = g_brk_base;
    g_brk_limit = g_brk_base + LPR_BRK_RESERVE_SIZE;
    if (g_brk_limit < g_brk_base) {
        g_brk_base = 0;
        g_brk_current = 0;
        g_brk_limit = 0;
        return -1;
    }
    return 0;
}

int64_t lpr_linux_brk(uint64_t requested)
{
    if (init_brk() != 0) {
        return 0;
    }
    if (requested == 0) {
        return (int64_t)g_brk_current;
    }
    const uint64_t aligned = align_up_page(requested);
    if (aligned == 0 || requested < g_brk_base || aligned > g_brk_limit) {
        return (int64_t)g_brk_current;
    }
    g_brk_current = requested;
    return (int64_t)g_brk_current;
}
