/* Measurement-only extra compilation unit; no userland wire layout changes.
 * Use monotonic time shared with the app observer. Dump after its timed run. */
#include "../userland/personality/linux/runtime/lpr_filed_internal.h"
#include <unixd/transport.h>

struct flow_row { uint64_t values[9]; unsigned ready; };
_Static_assert(sizeof(struct flow_row) == 80, "binary trace row layout");
enum { FLOW_CAPACITY = 65536, FLOW_BLOCK_ROWS = 512,
       FLOW_BLOCK_BYTES = FLOW_BLOCK_ROWS * sizeof(struct flow_row) };
static struct flow_row *blocks[FLOW_CAPACITY / FLOW_BLOCK_ROWS];
static unsigned used, dumped;

static struct flow_row *row_at(unsigned index, int create)
{
    struct flow_row **slot = &blocks[index / FLOW_BLOCK_ROWS];
    struct flow_row *block = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
    if (!block && create) {
        int64_t address = lpr_pacha_syscall6(PACHAOS_SYSCALL_MMAP, 0, 0,
            FLOW_BLOCK_BYTES, PACHAOS_PROT_READ | PACHAOS_PROT_WRITE,
            PACHAOS_MMAP_PRIVATE | PACHAOS_MMAP_ANONYMOUS, 0);
        if (address < FLOW_BLOCK_BYTES) return 0;
        struct flow_row *candidate = (void *)(uintptr_t)address;
        if (__atomic_compare_exchange_n(slot, &block, candidate, 0,
                __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)) block = candidate;
        else (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP, address, FLOW_BLOCK_BYTES);
    }
    return block ? &block[index % FLOW_BLOCK_ROWS] : 0;
}

static int write_all(uint64_t fd, const void *data, uint64_t length)
{
    const unsigned char *cursor = data;
    while (length) {
        int64_t result = lpr_linux_write(fd, (uint64_t)(uintptr_t)cursor, length);
        if (result <= 0 || (uint64_t)result > length) return -1;
        cursor += result;
        length -= (uint64_t)result;
    }
    return 0;
}

static void output(uint64_t now, unsigned count, unsigned missing)
{
    /* Separate files avoid interleaving on the native serial logger. This
     * runs only after dumped is set, so file I/O cannot recursively trace. */
    char path[36];
    lpr_memcpy(path, "/root/lpr-flow-", 15);
    uint64_t pid = lpr_linux_current_pid;
    for (unsigned i = 0; i < 16; ++i)
        path[15+i] = "0123456789abcdef"[(pid >> (60-4*i)) & 15];
    lpr_memcpy(path+31, ".bin", 5);
    int64_t fd = lpr_linux_openat((uint64_t)-100, (uint64_t)(uintptr_t)path, 0x241, 0600);
    if (fd < 0) return;
    unsigned stored = count < FLOW_CAPACITY ? count : FLOW_CAPACITY;
    struct flow_row footer = { .values = {now, pid, 0, 'Z', count, missing}, .ready = 1 };
    int failed = 0;
    for (unsigned i = 0; i < stored && !failed; i += FLOW_BLOCK_ROWS) {
        unsigned n = stored-i < FLOW_BLOCK_ROWS ? stored-i : FLOW_BLOCK_ROWS;
        struct flow_row *block = row_at(i, 0);
        failed = !block || write_all(fd, block, n * sizeof(*block));
    }
    if (!failed && !write_all(fd, &footer, sizeof(footer)))
        (void)lpr_linux_fsync(fd);
    (void)lpr_linux_close(fd);
}

uint64_t lpr_flow_clock(void)
{
    if (__atomic_load_n(&dumped, __ATOMIC_ACQUIRE)) return 0;
    struct pachaos_timespec clock;
    if (lpr_pacha_clock_gettime(LPR_LINUX_CLOCK_MONOTONIC, &clock)) return 0;
    uint64_t now = clock.tv_sec * UINT64_C(1000000000) + clock.tv_nsec;
    if (now >= UINT64_C(115000000000)) {
        if (__atomic_exchange_n(&dumped, 1, __ATOMIC_ACQ_REL)) return 0;
        unsigned count = __atomic_load_n(&used, __ATOMIC_ACQUIRE), missing = 0;
        for (unsigned i = 0; i < count && i < FLOW_CAPACITY; ++i) {
            struct flow_row *r = row_at(i, 0);
            if (!r || !__atomic_load_n(&r->ready, __ATOMIC_ACQUIRE)) ++missing;
        }
        output(now, count, missing);
        return 0;
    }
    return now;
}

static void record(const uint64_t values[9])
{
    unsigned index = __atomic_fetch_add(&used, 1, __ATOMIC_RELAXED);
    if (index >= FLOW_CAPACITY) return;
    struct flow_row *r = row_at(index, 1);
    if (!r) return;
    for (unsigned i = 0; i < 9; ++i) r->values[i] = values[i];
    __atomic_store_n(&r->ready, 1, __ATOMIC_RELEASE);
}

void lpr_flow_record(unsigned op, uint64_t sender, const struct unix_tx *tx,
                     const struct unix_rx *rx, uint64_t detail)
{
    uint64_t now = lpr_flow_clock();
    if (now < UINT64_C(80000000000) || now >= UINT64_C(100000000000)) return;
    const uint64_t values[] = {now, lpr_linux_current_pid, lpr_linux_gettid(), op,
        sender, tx->generation, __atomic_load_n(&tx->published, __ATOMIC_ACQUIRE),
        __atomic_load_n(&rx->consumed, __ATOMIC_ACQUIRE), detail};
    record(values);
}

void lpr_drm_flow_record(uint64_t command, uint64_t file, uint64_t data,
                         uint64_t start, int64_t result)
{
    uint64_t end = lpr_flow_clock();
    /* Ten seconds suffice for the high-rate DRM path. Unix records span
     * twenty seconds, allowing complete read gaps around this interval. */
    if (start < UINT64_C(80000000000) || end >= UINT64_C(90000000000) || end < start) return;
    const uint64_t values[] = {end, lpr_linux_current_pid, lpr_linux_gettid(),
        'D', command, data, start, (uint64_t)result, file};
    record(values);
}
