#include "lpr_memory.h"
#include "lpr_filed_internal.h"

#include <pachaos/abi.h>

enum {
    LPR_BRK_RESERVE_SIZE = 64ull * 1024ull * 1024ull,
};

static uint64_t align_up_page(uint64_t value)
{
    const uint64_t mask = 4095ull;
    if (value > UINT64_MAX - mask) {
        return 0;
    }
    return (value + mask) & ~mask;
}

static char *append_text(char *out, const char *end, const char *text)
{
    while (out < end && *text != '\0') {
        *out++ = *text++;
    }
    return out;
}

static char *append_u64(char *out, const char *end, uint64_t value)
{
    char digits[20];
    unsigned int count = 0;
    do {
        digits[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0 && count < sizeof(digits));
    while (out < end && count != 0) {
        *out++ = digits[--count];
    }
    return out;
}

static char *append_i64(char *out, const char *end, int64_t value)
{
    uint64_t magnitude = (uint64_t)value;
    if (value < 0) {
        if (out < end) {
            *out++ = '-';
        }
        magnitude = (~magnitude) + 1u;
    }
    return append_u64(out, end, magnitude);
}

static void log_brk_init_failure(int64_t status)
{
    char line[96];
    char *out = line;
    const char *end = line + sizeof(line);
    out = append_text(out, end, "[lpr] brk init failure status=");
    out = append_i64(out, end, status);
    if (out < end) {
        *out++ = '\n';
    }
    (void)lpr_pacha_syscall2(
        PACHAOS_SYSCALL_LOG,
        (uint64_t)(uintptr_t)line,
        (uint64_t)(out - line));
}

static void log_brk_limit(uint64_t requested, uint64_t aligned)
{
    char line[192];
    char *out = line;
    const char *end = line + sizeof(line);
    out = append_text(out, end, "[lpr] brk limit requested=");
    out = append_u64(out, end, requested);
    out = append_text(out, end, " aligned=");
    out = append_u64(out, end, aligned);
    out = append_text(out, end, " base=");
    out = append_u64(out, end, lpr_brk_base);
    out = append_text(out, end, " current=");
    out = append_u64(out, end, lpr_brk_current);
    out = append_text(out, end, " limit=");
    out = append_u64(out, end, lpr_brk_limit);
    if (out < end) {
        *out++ = '\n';
    }
    (void)lpr_pacha_syscall2(
        PACHAOS_SYSCALL_LOG,
        (uint64_t)(uintptr_t)line,
        (uint64_t)(out - line));
}

static int init_brk(void)
{
    if (lpr_brk_base != 0) {
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
        log_brk_init_failure(mapped);
        return -1;
    }
    lpr_brk_base = (uint64_t)mapped;
    lpr_brk_current = lpr_brk_base;
    lpr_brk_limit = lpr_brk_base + LPR_BRK_RESERVE_SIZE;
    if (lpr_brk_limit < lpr_brk_base) {
        lpr_brk_base = 0;
        lpr_brk_current = 0;
        lpr_brk_limit = 0;
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
        return (int64_t)lpr_brk_current;
    }
    const uint64_t aligned = align_up_page(requested);
    if (aligned == 0 || requested < lpr_brk_base || aligned > lpr_brk_limit) {
        log_brk_limit(requested, aligned);
        return (int64_t)lpr_brk_current;
    }
    lpr_brk_current = requested;
    return (int64_t)lpr_brk_current;
}
