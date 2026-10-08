/* SPDX-License-Identifier: MIT */
#ifndef LPR_BOOT_DIAGNOSTIC_H
#define LPR_BOOT_DIAGNOSTIC_H

#if defined(LPR_BOOT_DIAGNOSTICS) && LPR_BOOT_DIAGNOSTICS
#include <stdint.h>
#include <stddef.h>

enum { LPR_BOOT_DIAGNOSTIC_BYTES = 256, LPR_BOOT_DIAGNOSTIC_RECORDS = 64 };
typedef struct lpr_boot_diagnostic_state {
    uint32_t records;
    uint32_t failed;
    uint32_t stdio_seen;
    uint32_t tty_active;
    uint32_t fs_seen;
    uint32_t fs_active;
    uint32_t fs_completed;
    uint32_t fd3_reads;
    int64_t last_log_status;
} lpr_boot_diagnostic_state_t;
extern lpr_boot_diagnostic_state_t lpr_boot_diagnostic_state;

static inline uint64_t lpr_boot_diagnostic_pid(void)
{
    /* ACTIVATE precedes GET_STATE. Only use the already validated manifest
     * identity here: a supervisor lookup would recurse into the RPC observed. */
    const uint64_t pid = lpr_linux_current_pid > 0 ?
        (uint64_t)lpr_linux_current_pid : lpr_process_manifest.linux_pid;
    return pid >= 1 && pid <= 16 ? pid : 0;
}

static inline void lpr_boot_diagnostic_text(char *line, size_t *length,
    const char *text, size_t maximum)
{
    while (text && *text && maximum-- && *length < LPR_BOOT_DIAGNOSTIC_BYTES - 1)
        line[(*length)++] = *text++;
}

static inline void lpr_boot_diagnostic_number(char *line, size_t *length,
    int64_t value)
{
    char digits[20];
    size_t count = 0;
    uint64_t magnitude = (uint64_t)value;
    if (value < 0) {
        lpr_boot_diagnostic_text(line, length, "-", 1);
        magnitude = ~magnitude + 1;
    }
    do {
        digits[count++] = (char)('0' + magnitude % 10);
        magnitude /= 10;
    } while (magnitude);
    while (count && *length < LPR_BOOT_DIAGNOSTIC_BYTES - 1)
        line[(*length)++] = digits[--count];
}

static inline void lpr_boot_diagnostic_emit(const char *stage,
    uint64_t a, uint64_t b, uint64_t c, int64_t status, int64_t raw_status)
{
    const uint64_t pid = lpr_boot_diagnostic_pid();
    if (!pid || __atomic_load_n(&lpr_boot_diagnostic_state.failed, __ATOMIC_RELAXED))
        return;
    uint32_t records = __atomic_load_n(&lpr_boot_diagnostic_state.records, __ATOMIC_RELAXED);
    do {
        if (records >= LPR_BOOT_DIAGNOSTIC_RECORDS) return;
    } while (!__atomic_compare_exchange_n(&lpr_boot_diagnostic_state.records,
        &records, records + 1, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
    char line[LPR_BOOT_DIAGNOSTIC_BYTES];
    size_t length = 0;
    lpr_boot_diagnostic_text(line, &length, "[LPR_BOOT] ", 11);
    lpr_boot_diagnostic_text(line, &length, stage, 40);
    lpr_boot_diagnostic_text(line, &length, " pid=", 5);
    lpr_boot_diagnostic_number(line, &length, (int64_t)pid);
    const char *labels[] = {" a=0x", " b=0x", " c=0x"};
    const uint64_t values[] = {a, b, c};
    for (unsigned i = 0; i < 3; ++i) {
        lpr_boot_diagnostic_text(line, &length, labels[i], 5);
        for (int shift = 60; shift >= 0; shift -= 4)
            line[length++] = "0123456789abcdef"[(values[i] >> shift) & 15];
    }
    lpr_boot_diagnostic_text(line, &length, " status=", 8);
    lpr_boot_diagnostic_number(line, &length, status);
    lpr_boot_diagnostic_text(line, &length, " raw=", 5);
    lpr_boot_diagnostic_number(line, &length, raw_status);
    line[length++] = '\n';
    /* Logging can fail independently. Never retry, allocate, call stdio, or
     * replace the result of the operation being observed with this status. */
    const int64_t log_status = lpr_pacha_syscall2(PACHAOS_SYSCALL_LOG,
        (uint64_t)(uintptr_t)line, length);
    __atomic_store_n(&lpr_boot_diagnostic_state.last_log_status, log_status, __ATOMIC_RELAXED);
    if (log_status)
        __atomic_store_n(&lpr_boot_diagnostic_state.failed, 1, __ATOMIC_RELAXED);
}

static inline int lpr_boot_diagnostic_stdio_begin(uint64_t fd, uint32_t operation)
{
    if (!lpr_boot_diagnostic_pid() || fd > 2 || operation > 3) return 0;
    const uint32_t bit = 1u << (fd * 2 + (operation & 1u));
    if (__atomic_fetch_or(&lpr_boot_diagnostic_state.stdio_seen, bit, __ATOMIC_RELAXED) & bit)
        return 0;
    __atomic_add_fetch(&lpr_boot_diagnostic_state.tty_active, 1, __ATOMIC_RELAXED);
    return 1;
}

static inline void lpr_boot_diagnostic_read_enter(uint32_t operation,
    uint64_t fd, uint8_t ops_id, uint64_t count)
{
    /* The pinned backend is the only positive evidence of what fd3 means.
     * Observe three entries, without a lookup, return log or additional RPC;
     * they share the existing per-image 64-record budget with bootstrap. */
    if (operation || fd != 3 || !lpr_boot_diagnostic_pid()) return;
    uint32_t reads = __atomic_load_n(&lpr_boot_diagnostic_state.fd3_reads, __ATOMIC_RELAXED);
    do {
        if (reads >= 3) return;
    } while (!__atomic_compare_exchange_n(&lpr_boot_diagnostic_state.fd3_reads,
        &reads, reads + 1, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
    lpr_boot_diagnostic_emit("fd3.read.enter", fd, ops_id, count, 0, 0);
}

static inline void lpr_boot_diagnostic_stdio_end(int observed)
{
    if (observed)
        __atomic_sub_fetch(&lpr_boot_diagnostic_state.tty_active, 1, __ATOMIC_RELAXED);
}

static inline int lpr_boot_diagnostic_tty_active(void)
{
    return __atomic_load_n(&lpr_boot_diagnostic_state.tty_active, __ATOMIC_RELAXED) != 0;
}

static inline void lpr_boot_diagnostic_fs_begin(void)
{
    /* The first endpoint lookup may precede call_impl, while HELLO/ATTACH
     * recurse through it. Keep that one bootstrap path visible until its
     * first two regular operations complete: EVENT_IDX failures can hide
     * behind a successful first request. Later file I/O stays unobserved. */
    if (lpr_boot_diagnostic_pid() &&
        !__atomic_exchange_n(&lpr_boot_diagnostic_state.fs_seen, 1, __ATOMIC_RELAXED))
        __atomic_store_n(&lpr_boot_diagnostic_state.fs_active, 1, __ATOMIC_RELAXED);
}

static inline int lpr_boot_diagnostic_fs_active(void)
{
    return __atomic_load_n(&lpr_boot_diagnostic_state.fs_active, __ATOMIC_RELAXED) != 0;
}

static inline void lpr_boot_diagnostic_fs_end(int regular_operation)
{
    if (regular_operation && lpr_boot_diagnostic_fs_active() &&
        __atomic_add_fetch(&lpr_boot_diagnostic_state.fs_completed, 1, __ATOMIC_RELAXED) >= 2)
        __atomic_store_n(&lpr_boot_diagnostic_state.fs_active, 0, __ATOMIC_RELAXED);
}

#define lpr_boot_diagnostic_fs_emit(stage, a, b, c, status, raw_status) do { \
    if (lpr_boot_diagnostic_fs_active()) \
        lpr_boot_diagnostic_emit(stage, a, b, c, status, raw_status); \
} while (0)
#else
#define lpr_boot_diagnostic_emit(stage, a, b, c, status, raw_status) ((void)0)
#define lpr_boot_diagnostic_stdio_begin(fd, operation) 0
#define lpr_boot_diagnostic_read_enter(operation, fd, ops_id, count) ((void)0)
#define lpr_boot_diagnostic_stdio_end(observed) ((void)0)
#define lpr_boot_diagnostic_tty_active() 0
#define lpr_boot_diagnostic_fs_begin() ((void)0)
#define lpr_boot_diagnostic_fs_active() 0
#define lpr_boot_diagnostic_fs_end(regular_operation) ((void)0)
#define lpr_boot_diagnostic_fs_emit(stage, a, b, c, status, raw_status) ((void)0)
#endif
#endif
