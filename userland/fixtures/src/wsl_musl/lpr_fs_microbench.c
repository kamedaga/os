#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* The guest CLOCK_MONOTONIC ticks in 1 ms steps, so per-operation costs are
 * read from TSC calibrated against it over a full second. Each phase repeats
 * batches until at least one second of measured operations has accumulated. */

enum {
    BENCH_DEPTH_MAX = 8,
    BENCH_DIR_ENTRIES = 256,
    BENCH_FILE_BYTES = 1024 * 1024,
    BENCH_BLOCK = 4096,
};

static uint64_t read_tsc(void)
{
    uint32_t lo, hi;
    __asm__ __volatile__("lfence; rdtsc; lfence" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | lo;
}

static uint64_t clock_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static double ns_per_cycle;

static void calibrate(void)
{
    const uint64_t ns0 = clock_ns(), tsc0 = read_tsc();
    uint64_t ns1 = ns0, tsc1 = tsc0;
    while (ns1 - ns0 < 1000000000ull) {
        ns1 = clock_ns();
        tsc1 = read_tsc();
    }
    ns_per_cycle = (double)(ns1 - ns0) / (double)(tsc1 - tsc0);
    printf("FS_BENCH_CLOCK fs=%s tsc_hz=%llu clock_ns=%llu cycles=%llu\n",
        "calibration", (unsigned long long)(1000000000.0 / ns_per_cycle),
        (unsigned long long)(ns1 - ns0), (unsigned long long)(tsc1 - tsc0));
    fflush(stdout);
}

static uint64_t now_ns(void)
{
    return (uint64_t)((double)read_tsc() * ns_per_cycle);
}

static const char *label;
static unsigned scale;
/* Diagnostic mode brackets the unchanged measured loops. The host snapshots
 * counters while stdin is blocked; the default benchmark never takes this
 * path. Empty brackets quantify terminal/handshake notification overhead. */
static int notify_profile;
static unsigned profile_phase;
static void profile_boundary(int begin)
{
    if (!notify_profile) return;
    printf("FS_NOTIFY_BOUNDARY fs=%s phase=%u begin=%d\n", label, profile_phase, begin);
    fflush(stdout);
    int c;
    do { c = getchar(); } while (c != '\n' && c != EOF);
    if (c == EOF) exit(3);
    if (!begin) profile_phase++;
}

static void die(const char *phase)
{
    printf("FS_BENCH_FAIL fs=%s phase=%s errno=%d\n", label, phase, errno);
    fflush(stdout);
    exit(1);
}

static void report(const char *phase, uint64_t ops, uint64_t total_ns)
{
    printf("FS_BENCH fs=%s phase=%s ops=%llu total_ns=%llu ns_per_op=%llu\n",
        label, phase, (unsigned long long)ops, (unsigned long long)total_ns,
        (unsigned long long)(total_ns / ops));
    fflush(stdout);
}

/* Count only the operation batches, so untimed setup for create/unlink does
 * not inflate their results. Native Linux needs many more batches than LPR. */
#define TIMED_BEGIN(batch_ops) \
    profile_boundary(1); \
    uint64_t measured_ops = 0, measured_ns = 0; \
    do { \
        const uint64_t t0 = now_ns();
#define TIMED_END(phase, batch_ops) \
        measured_ns += now_ns() - t0; \
        measured_ops += (batch_ops); \
    } while (measured_ns < 1000000000ull); \
    report((phase), measured_ops, measured_ns); \
    profile_boundary(0)

static void path_at_depth(char *out, size_t cap, const char *base, unsigned depth, const char *leaf)
{
    size_t n = (size_t)snprintf(out, cap, "%s", base);
    for (unsigned i = 0; i < depth; ++i)
        n += (size_t)snprintf(out + n, cap - n, "/d%u", i);
    if (leaf)
        snprintf(out + n, cap - n, "/%s", leaf);
}

static void bench_stat(const char *base, unsigned depth, uint64_t ops)
{
    char path[512], phase[32];
    struct stat st;
    path_at_depth(path, sizeof(path), base, depth, "f");
    snprintf(phase, sizeof(phase), "stat_d%u", depth);
    TIMED_BEGIN(ops)
    for (uint64_t i = 0; i < ops; ++i)
        if (stat(path, &st) != 0) die("stat");
    TIMED_END(phase, ops);
}

static void bench_open(const char *base, unsigned depth, uint64_t ops)
{
    char path[512], phase[32];
    path_at_depth(path, sizeof(path), base, depth, "f");
    snprintf(phase, sizeof(phase), "open_close_d%u", depth);
    TIMED_BEGIN(ops)
    for (uint64_t i = 0; i < ops; ++i) {
        const int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0 || close(fd) != 0) die("open_close");
    }
    TIMED_END(phase, ops);
}

static void create_empty(const char *path)
{
    const int fd = open(path, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0644);
    if (fd < 0 || close(fd) != 0) die("create_close");
}

static unsigned read_directory(const char *path)
{
    DIR *dir = opendir(path);
    if (!dir) die("opendir");
    uint64_t seen[4] = {0};
    unsigned entries = 0, dots = 0;
    struct dirent *entry;
    errno = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            dots++;
            continue;
        }
        unsigned index;
        char extra, expected[32];
        if (sscanf(entry->d_name, "entry-%u%c", &index, &extra) != 1 || index >= BENCH_DIR_ENTRIES) {
            errno = EIO;
            die("readdir_name");
        }
        snprintf(expected, sizeof(expected), "entry-%04u", index);
        if (strcmp(entry->d_name, expected) != 0 || (seen[index / 64] & (1ull << (index % 64)))) {
            errno = EIO;
            die("readdir_duplicate");
        }
        seen[index / 64] |= 1ull << (index % 64);
        entries++;
    }
    if (errno || closedir(dir) != 0) die("readdir");
    /* The v1 backend omits dot entries. Check every actual filename, rather
     * than letting that semantic difference abort all remaining measurements.
     * Log the dot count explicitly so it is not hidden by the comparison. */
    if (entries != BENCH_DIR_ENTRIES || (dots != 0 && dots != 2)) {
        printf("FS_BENCH_DIRENTS fs=%s entries=%u dots=%u expected=%u\n", label, entries, dots, BENCH_DIR_ENTRIES);
        errno = EIO;
        die("readdir_count");
    }
    return dots;
}

static void bench_mutation(const char *base, unsigned depth, unsigned kind)
{
    enum { BATCH = 32 };
    static const char *names[] = {"create_close", "rename", "unlink"};
    char paths[BATCH][512], path[512], other[512], phase[32];
    for (unsigned j = 0; j < BATCH; ++j) {
        char leaf[32];
        snprintf(leaf, sizeof(leaf), "batch-%u", j);
        path_at_depth(paths[j], sizeof(paths[j]), base, depth, leaf);
    }
    path_at_depth(path, sizeof(path), base, depth, "f");
    path_at_depth(other, sizeof(other), base, depth, "g");
    uint64_t ops = 0, total = 0;
    do {
        if (kind == 2)
            for (unsigned j = 0; j < BATCH; ++j) create_empty(paths[j]);
        const uint64_t t0 = now_ns();
        for (unsigned j = 0; j < BATCH; ++j) {
            if (kind == 0) create_empty(paths[j]);
            else if (kind == 1) {
                if (rename(j % 2 ? other : path, j % 2 ? path : other) != 0) die("rename");
            } else if (unlink(paths[j]) != 0) die("unlink");
        }
        total += now_ns() - t0;
        ops += BATCH;
        if (kind == 0)
            for (unsigned j = 0; j < BATCH; ++j)
                if (unlink(paths[j]) != 0) die("create_cleanup");
    } while (total < 1000000000ull);
    snprintf(phase, sizeof(phase), "%s_d%u", names[kind], depth);
    report(phase, ops, total);
}

static void bench_readdir(const char *base, unsigned depth)
{
    char wide[512], path[512], phase[32];
    path_at_depth(wide, sizeof(wide), base, depth, "wide-depth");
    if (mkdir(wide, 0755) != 0) die("setup_readdir");
    for (unsigned j = 0; j < BENCH_DIR_ENTRIES; ++j) {
        snprintf(path, sizeof(path), "%s/entry-%04u", wide, j);
        create_empty(path);
    }
    snprintf(phase, sizeof(phase), "readdir_256_d%u", depth);
    const uint64_t ops = 4ull * scale;
    unsigned dots = 0;
    TIMED_BEGIN(ops)
    for (uint64_t i = 0; i < ops; ++i) {
        dots = read_directory(wide);
    }
    TIMED_END(phase, ops);
    printf("FS_BENCH_DIRENTS fs=%s phase=%s entries=%u dots=%u\n", label, phase, BENCH_DIR_ENTRIES, dots);
    for (unsigned j = 0; j < BENCH_DIR_ENTRIES; ++j) {
        snprintf(path, sizeof(path), "%s/entry-%04u", wide, j);
        if (unlink(path) != 0) die("cleanup_readdir_entry");
    }
    if (rmdir(wide) != 0) die("cleanup_readdir");
}

int main(int argc, char **argv)
{
    if (argc != 4 && (argc != 5 || (strcmp(argv[4], "ipc-mmap") && strcmp(argv[4], "notify-profile")))) {
        fprintf(stderr, "usage: %s LABEL DIR SCALE [ipc-mmap|notify-profile]\n", argv[0]);
        return 2;
    }
    label = argv[1];
    /* Keep the measured loops unchanged while avoiding unrelated mutation
     * phases during transport/snapshot iteration. Default remains full. */
    notify_profile = argc == 5 && !strcmp(argv[4], "notify-profile");
    const int focused = argc == 5;
    const char *root = argv[2];
    scale = (unsigned)strtoul(argv[3], NULL, 10);
    if (scale == 0 || scale > 1000) {
        fprintf(stderr, "SCALE must be 1..1000\n");
        return 2;
    }

    calibrate();
    char base[256], path[512], other[512];
    snprintf(base, sizeof(base), "%s/fsbench.%ld", root, (long)getpid());
    if (mkdir(base, 0755) != 0) die("mkdir_base");

    path_at_depth(path, sizeof(path), base, 0, "f");
    int fd = open(path, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0644);
    if (fd < 0 || close(fd) != 0) die("setup_leaf");
    for (unsigned d = 1; d <= BENCH_DEPTH_MAX; ++d) {
        path_at_depth(path, sizeof(path), base, d, NULL);
        if (mkdir(path, 0755) != 0) die("setup_dir");
        path_at_depth(path, sizeof(path), base, d, "f");
        fd = open(path, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0644);
        if (fd < 0 || close(fd) != 0) die("setup_leaf");
    }
    char wide[512];
    snprintf(wide, sizeof(wide), "%s/wide", base);
    if (mkdir(wide, 0755) != 0) die("setup_wide");
    for (unsigned i = 0; i < BENCH_DIR_ENTRIES; ++i) {
        snprintf(path, sizeof(path), "%s/entry-%04u", wide, i);
        fd = open(path, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0644);
        if (fd < 0 || close(fd) != 0) die("setup_wide_entry");
    }
    static unsigned char block[BENCH_BLOCK];
    memset(block, 0x5a, sizeof(block));
    char data[512];
    snprintf(data, sizeof(data), "%s/data.bin", base);
    fd = open(data, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0644);
    if (fd < 0) die("setup_data");
    for (unsigned off = 0; off < BENCH_FILE_BYTES; off += BENCH_BLOCK)
        if (pwrite(fd, block, BENCH_BLOCK, off) != BENCH_BLOCK) die("setup_data_write");
    if (fsync(fd) != 0 || close(fd) != 0) die("setup_data_sync");

    printf("FS_BENCH_START fs=%s scale=%u min_ns=1000000000\n", label, scale);
    fflush(stdout);

    profile_boundary(1);
    profile_boundary(0);
    const uint64_t meta = 1000ull * scale;
    bench_stat(base, 0, meta);
    if (!focused) {
    bench_stat(base, 4, meta);
    bench_stat(base, 8, meta);
    }
    bench_open(base, 0, meta);
    if (!focused) {
    bench_open(base, 4, meta);
    bench_open(base, 8, meta);

    for (unsigned d = 0; d <= BENCH_DEPTH_MAX; d += 4) bench_readdir(base, d);
    for (unsigned kind = 0; kind < 3; ++kind)
        for (unsigned d = 0; d <= BENCH_DEPTH_MAX; d += 4) bench_mutation(base, d, kind);

    {
        struct stat st;
        snprintf(path, sizeof(path), "%s/missing", base);
        TIMED_BEGIN(meta)
        for (uint64_t i = 0; i < meta; ++i)
            if (stat(path, &st) == 0 || errno != ENOENT) die("stat_missing");
        TIMED_END("stat_missing", meta);
    }
    {
        const uint64_t ops = 10ull * scale;
        unsigned dots = 0;
        TIMED_BEGIN(ops)
        for (uint64_t i = 0; i < ops; ++i) {
            dots = read_directory(wide);
        }
        TIMED_END("readdir_256", ops);
        printf("FS_BENCH_DIRENTS fs=%s phase=readdir_256 entries=%u dots=%u\n", label, BENCH_DIR_ENTRIES, dots);
    }
    {
        const uint64_t ops = 200ull * scale;
        snprintf(path, sizeof(path), "%s/c", base);
        TIMED_BEGIN(ops)
        for (uint64_t i = 0; i < ops; ++i) {
            fd = open(path, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0644);
            if (fd < 0 || close(fd) != 0 || unlink(path) != 0) die("create_unlink");
        }
        TIMED_END("create_unlink", ops);
    }
    {
        const uint64_t ops = 200ull * scale;
        snprintf(path, sizeof(path), "%s/m", base);
        TIMED_BEGIN(ops)
        for (uint64_t i = 0; i < ops; ++i)
            if (mkdir(path, 0755) != 0 || rmdir(path) != 0) die("mkdir_rmdir");
        TIMED_END("mkdir_rmdir", ops);
    }
    {
        const uint64_t ops = 200ull * scale;
        path_at_depth(path, sizeof(path), base, 0, "f");
        snprintf(other, sizeof(other), "%s/g", base);
        TIMED_BEGIN(ops)
        for (uint64_t i = 0; i < ops; ++i)
            if (rename(path, other) != 0 || rename(other, path) != 0) die("rename");
        TIMED_END("rename_pair", ops);
    }

    }
    fd = open(data, O_RDWR | O_CLOEXEC);
    if (fd < 0) die("data_open");
    {
        const uint64_t ops = 1000ull * scale;
        TIMED_BEGIN(ops)
        for (uint64_t i = 0; i < ops; ++i)
            if (pread(fd, block, BENCH_BLOCK, 0) != BENCH_BLOCK) die("pread_4k");
        TIMED_END("pread_4k_hot", ops);
    }
    {
        const uint64_t ops = 4ull * scale;
        TIMED_BEGIN(ops)
        for (uint64_t i = 0; i < ops; ++i)
            for (unsigned off = 0; off < BENCH_FILE_BYTES; off += BENCH_BLOCK)
                if (pread(fd, block, BENCH_BLOCK, off) != BENCH_BLOCK) die("read_1m");
        TIMED_END("read_1m_hot", ops);
    }
    {
        const uint64_t ops = 1000ull * scale;
        TIMED_BEGIN(ops)
        for (uint64_t i = 0; i < ops; ++i)
            if (pwrite(fd, block, BENCH_BLOCK, (off_t)((i % 256) * BENCH_BLOCK)) != BENCH_BLOCK)
                die("pwrite_4k");
        TIMED_END("pwrite_4k", ops);
    }
    if (!focused || notify_profile) {
        const uint64_t ops = 10ull * scale;
        TIMED_BEGIN(ops)
        for (uint64_t i = 0; i < ops; ++i)
            if (pwrite(fd, block, BENCH_BLOCK, (off_t)((i % 256) * BENCH_BLOCK)) != BENCH_BLOCK ||
                fsync(fd) != 0)
                die("pwrite_fsync");
        TIMED_END("pwrite_fsync", ops);
    }
    {
        /* Touch every page so the cost includes the file-to-memory path, not
         * only VMA creation. */
        const uint64_t ops = 4ull * scale;
        volatile unsigned sum = 0;
        TIMED_BEGIN(ops)
        for (uint64_t i = 0; i < ops; ++i) {
            unsigned char *map = mmap(NULL, BENCH_FILE_BYTES, PROT_READ, MAP_PRIVATE, fd, 0);
            if (map == MAP_FAILED) die("mmap");
            for (unsigned off = 0; off < BENCH_FILE_BYTES; off += BENCH_BLOCK)
                sum += map[off];
            if (munmap(map, BENCH_FILE_BYTES) != 0) die("munmap");
        }
        (void)sum;
        TIMED_END("mmap_1m_touch", ops);
    }
    if (close(fd) != 0) die("data_close");

    if (unlink(data) != 0) die("cleanup_data");
    for (unsigned i = 0; i < BENCH_DIR_ENTRIES; ++i) {
        snprintf(path, sizeof(path), "%s/entry-%04u", wide, i);
        if (unlink(path) != 0) die("cleanup_wide_entry");
    }
    if (rmdir(wide) != 0) die("cleanup_wide");
    for (unsigned d = BENCH_DEPTH_MAX + 1; d-- > 0;) {
        path_at_depth(path, sizeof(path), base, d, "f");
        if (unlink(path) != 0) die("cleanup_leaf");
        path_at_depth(path, sizeof(path), base, d, NULL);
        if (rmdir(path) != 0) die("cleanup_dir");
    }

    printf("FS_BENCH_DONE fs=%s\n", label);
    fflush(stdout);
    return 0;
}
