/* Diagnostic only: exclusive main-thread syscall spans in fixed TSC bins.
 * No per-call logging, kernel ABI changes, or changes to Linux applications.
 * Dump after EXIT_GROUP; the host selects bins ending before first paint.
 * Time includes descheduling inside calls, not CPU time. Other threads are
 * deliberately excluded so their waits cannot be added to launch wall time.
 */
#if defined(LPR_GUI_PROFILE) && LPR_GUI_PROFILE
#include "lpr_gui_detail.h"
enum { LPR_GUI_BINS = 4096, LPR_GUI_KINDS = 40 };
#define LPR_GUI_BIN_CYCLES 36000000ull
static struct { uint64_t cycles, count; } lpr_gui_bins[LPR_GUI_BINS][LPR_GUI_KINDS];
static uint64_t lpr_gui_pid, lpr_gui_tid, lpr_gui_start, lpr_gui_start_ns;
static uint32_t lpr_gui_selected, lpr_gui_dumped;
static struct {
    uint64_t nr, ordinal, argument, flags, offset_valid, pread_active, size;
    char path[128];
} lpr_gui_files[128];
static unsigned lpr_gui_file_count;
static uint64_t lpr_gui_file_seen[2];
static struct {
    uint64_t start, end, fd, events, leaves, ready, timeout, status, handle, type;
} lpr_gui_waits[2048];
static struct { uint64_t tsc, fd, handle; char path[128]; } lpr_gui_peers[64];
static unsigned lpr_gui_wait_count, lpr_gui_peer_count;
static uint64_t lpr_gui_wait_dropped, lpr_gui_peer_dropped;

/* The native debug UART can interleave bytes from different CPUs. Measured
 * applications redirect stderr to /dev/hvc0; TERMD serializes each short
 * write there. This is diagnostic output, performed after the measured
 * interval except for the single start anchor. */
static void lpr_gui_emit(const char *line, uint64_t length)
{
    (void)lpr_linux_write(2, (uint64_t)(uintptr_t)line, length);
}

static void lpr_gui_log(const char *tag, const uint64_t *values, unsigned count)
{
    char line[256];
    char *out = line;
    const char *prefix = "[gui-prof] ";
    while (*prefix) *out++ = *prefix++;
    while (*tag) *out++ = *tag++;
    for (unsigned i = 0; i < count; ++i) {
        *out++ = ' ';
        char digits[20];
        unsigned n = 0;
        uint64_t v = values[i];
        do { digits[n++] = (char)('0' + v % 10u); v /= 10u; } while (v);
        while (n) *out++ = digits[--n];
    }
    *out++ = '\n';
    lpr_gui_emit(line, (uint64_t)(out - line));
}

static uint64_t lpr_gui_now_ns(void)
{
    struct pachaos_timespec ts = {0};
    if (lpr_pacha_clock_gettime(LPR_LINUX_CLOCK_MONOTONIC, &ts) != 0) return 0;
    return ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static void lpr_gui_init(void)
{
    const uint64_t pid = (uint64_t)lpr_linux_getpid();
    if (pid == lpr_gui_pid) return;
    lpr_gui_pid = pid;
    lpr_gui_selected = 0;
    lpr_gui_dumped = 0;
    lpr_gui_file_count = 0;
    lpr_gui_wait_count = lpr_gui_peer_count = 0;
    lpr_gui_wait_dropped = lpr_gui_peer_dropped = 0;
    lpr_gui_file_seen[0] = lpr_gui_file_seen[1] = 0;
    char path[256] = {0};
    const int64_t length = lpr_linux_readlinkat_to_buffer(
        (uint64_t)-100ll, (uint64_t)(uintptr_t)"/proc/self/exe", path, sizeof(path)-1u);
    if (length <= 0) return;
    const char *base = lpr_strrchr(path, '/');
    base = base ? base + 1 : path;
    if (lpr_strcmp(base, "gtk4-demo") == 0) lpr_gui_selected = 1;
    else if (lpr_strcmp(base, "xfce4-terminal") == 0) lpr_gui_selected = 2;
    else return;
    lpr_gui_tid = (uint64_t)lpr_linux_gettid();
    lpr_memset(lpr_gui_bins, 0, sizeof(lpr_gui_bins));
    lpr_gui_start_ns = lpr_gui_now_ns();
    lpr_gui_start = pacha_trace_read_tsc();
    const uint64_t values[] = {pid, lpr_gui_tid, lpr_gui_selected,
        lpr_gui_start, lpr_gui_start_ns, LPR_GUI_BIN_CYCLES};
    lpr_gui_log("begin", values, 6);
}

static unsigned lpr_gui_kind(uint64_t nr, uint64_t a3)
{
    switch (nr) {
    case 0: return 0; /* read */
    case 19: return 1; /* readv */
    case 17: case 295: return 2; /* pread */
    case 2: case 257: return 3; /* open */
    case 4: case 5: case 6: case 262: case 332: return 4; /* stat */
    case 8: return 5; /* lseek */
    case 3: return 6; /* close */
    case 9: return (a3 & 0x20u) ? 8 : 7; /* anonymous/file mmap */
    case 11: return 9; /* munmap */
    case 10: return 10; /* mprotect */
    case 1: case 18: case 20: case 296: return 11; /* writes */
    case 41: case 42: case 49: return 12; /* socket/connect/bind */
    case 44: case 46: case 307: return 13; /* send */
    case 45: case 47: case 299: return 14; /* receive */
    case 16: return 15; /* ioctl */
    case 7: case 23: case 232: case 270: case 271: case 281: case 441: return 16;
    case 202: return 17; /* futex, including waits */
    case 56: case 57: case 58: case 59: case 61: return 18; /* process */
    case 35: case 96: case 228: case 229: case 230: return 19; /* clock/sleep */
    case 21: case 89: case 217: case 267: case 269: return 20; /* path/dir */
    case 13: case 14: case 15: case 131: return 21; /* signal */
    case 12: case 25: case 28: return 22; /* other VM */
    default: return 23;
    }
}

static void lpr_gui_record(unsigned kind, uint64_t start, uint64_t end)
{
    if (end <= start || start < lpr_gui_start) return;
    uint64_t cursor = start - lpr_gui_start;
    const uint64_t limit = end - lpr_gui_start;
    unsigned first = 1;
    while (cursor < limit) {
        const uint64_t bin = cursor / LPR_GUI_BIN_CYCLES;
        if (bin >= LPR_GUI_BINS) break;
        uint64_t next = (bin + 1u) * LPR_GUI_BIN_CYCLES;
        if (next > limit) next = limit;
        lpr_gui_bins[bin][kind].cycles += next - cursor;
        lpr_gui_bins[bin][kind].count += first;
        first = 0;
        cursor = next;
    }
}

int lpr_gui_profile_current_thread(void)
{
    return lpr_gui_selected && !lpr_gui_dumped &&
        (uint64_t)lpr_linux_gettid() == lpr_gui_tid;
}

void lpr_gui_profile_span(unsigned kind, uint64_t start, uint64_t end)
{
    if (kind < LPR_GUI_KINDS - 24u) lpr_gui_record(24u + kind, start, end);
}

void lpr_gui_profile_file(uint64_t nr, uint64_t argument, uint64_t flags,
    uint64_t offset_valid, uint64_t pread_active, uint64_t size, const char *path)
{
    const unsigned slot = nr == 8 ? 0 : 1;
    const uint64_t ordinal = lpr_gui_file_seen[slot]++;
    if (lpr_gui_file_count >= 128u || (ordinal >= 3u && ordinal % 128u)) return;
    const unsigned i = lpr_gui_file_count++;
    lpr_gui_files[i].nr = nr;
    lpr_gui_files[i].ordinal = ordinal;
    lpr_gui_files[i].argument = argument;
    lpr_gui_files[i].flags = flags;
    lpr_gui_files[i].offset_valid = offset_valid;
    lpr_gui_files[i].pread_active = pread_active;
    lpr_gui_files[i].size = size;
    const unsigned n = (unsigned)lpr_strnlen(path, sizeof(lpr_gui_files[i].path)-1u);
    lpr_memcpy(lpr_gui_files[i].path, path, n);
    lpr_gui_files[i].path[n] = 0;
}

void lpr_gui_profile_peer(uint64_t fd, uint64_t handle, const char *path, unsigned length)
{
    if (lpr_gui_peer_count == 64u) { ++lpr_gui_peer_dropped; return; }
    const unsigned i = lpr_gui_peer_count++;
    lpr_gui_peers[i].tsc = pacha_trace_read_tsc();
    lpr_gui_peers[i].fd = fd;
    lpr_gui_peers[i].handle = handle;
    if (length > 127u) length = 127u;
    for (unsigned j = 0; j < length; ++j) {
        const unsigned char c = (unsigned char)path[j];
        lpr_gui_peers[i].path[j] = c >= 32 && c < 127 ? (char)c : '?';
    }
    lpr_gui_peers[i].path[length] = 0;
}

void lpr_gui_profile_wait(uint64_t start, uint64_t end, uint64_t fd,
    uint64_t events, uint64_t leaves, uint64_t ready, uint64_t timeout,
    int64_t status, uint64_t handle, uint64_t type)
{
    if (lpr_gui_wait_count == 2048u) { ++lpr_gui_wait_dropped; return; }
    const unsigned i = lpr_gui_wait_count++;
    lpr_gui_waits[i].start = start;
    lpr_gui_waits[i].end = end;
    lpr_gui_waits[i].fd = fd;
    lpr_gui_waits[i].events = events;
    lpr_gui_waits[i].leaves = leaves;
    lpr_gui_waits[i].ready = ready;
    lpr_gui_waits[i].timeout = timeout;
    lpr_gui_waits[i].status = (uint64_t)status;
    lpr_gui_waits[i].handle = handle;
    lpr_gui_waits[i].type = type;
}

static void lpr_gui_dump(void)
{
    if (lpr_gui_dumped) return;
    lpr_gui_dumped = 1;
    const uint64_t ns = lpr_gui_now_ns();
    const uint64_t tsc = pacha_trace_read_tsc();
    const uint64_t end[] = {lpr_gui_pid, tsc, ns};
    lpr_gui_log("end", end, 3);
    for (unsigned b = 0; b < LPR_GUI_BINS; ++b)
        for (unsigned k = 0; k < LPR_GUI_KINDS; ++k) {
            if (!lpr_gui_bins[b][k].cycles) continue;
            const uint64_t row[] = {lpr_gui_pid, b, k,
                lpr_gui_bins[b][k].cycles, lpr_gui_bins[b][k].count};
            lpr_gui_log(k < 24 ? "bin" : "detail", row, 5);
        }
    for (unsigned i = 0; i < lpr_gui_file_count; ++i) {
        const uint64_t row[] = {lpr_gui_pid, i, lpr_gui_files[i].nr,
            lpr_gui_files[i].ordinal, lpr_gui_files[i].argument,
            lpr_gui_files[i].flags, lpr_gui_files[i].offset_valid,
            lpr_gui_files[i].pread_active, lpr_gui_files[i].size};
        lpr_gui_log("file", row, 9);
        char line[256];
        char *out = line;
        const char *p = "[gui-path] ";
        while (*p) *out++ = *p++;
        p = lpr_gui_files[i].path;
        while (*p) *out++ = *p++;
        *out++ = '\n';
        lpr_gui_emit(line, (uint64_t)(out-line));
    }
    for (unsigned i = 0; i < lpr_gui_wait_count; ++i) {
        const uint64_t row[] = {lpr_gui_pid, lpr_gui_waits[i].start,
            lpr_gui_waits[i].end, lpr_gui_waits[i].fd, lpr_gui_waits[i].events,
            lpr_gui_waits[i].leaves, lpr_gui_waits[i].ready,
            lpr_gui_waits[i].timeout, lpr_gui_waits[i].status,
            lpr_gui_waits[i].handle, lpr_gui_waits[i].type};
        lpr_gui_log("wait", row, 11);
    }
    for (unsigned i = 0; i < lpr_gui_peer_count; ++i) {
        const uint64_t row[] = {lpr_gui_pid, lpr_gui_peers[i].tsc,
            lpr_gui_peers[i].fd, lpr_gui_peers[i].handle};
        lpr_gui_log("peer", row, 4);
        char line[160];
        char *out = line;
        const char *p = "[gui-peer] ";
        while (*p) *out++ = *p++;
        p = lpr_gui_peers[i].path;
        while (*p) *out++ = *p++;
        *out++ = '\n';
        lpr_gui_emit(line, (uint64_t)(out-line));
    }
    const uint64_t dropped[] = {lpr_gui_pid, lpr_gui_wait_dropped, lpr_gui_peer_dropped};
    lpr_gui_log("dropped", dropped, 3);
    lpr_gui_log("done", end, 1);
}
#endif
