/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/fs.h>
#include <linux/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include "../kobox2/linux-sandbox/kobox/boot/fs_cache_matrix.h"

struct native_counts { uint64_t reads, read_sectors, writes, write_sectors; };
struct native_sample {
    struct native_counts counts;
    uint64_t wall;
    unsigned int phase, trial, sample;
    int cold;
};
static struct native_sample native_samples[96];
struct native_backend {
    unsigned char *buffer;
    const char *fs;
    int sync_fd, stat_fd;
    struct native_counts previous;
    unsigned int sample_count;
    int checkpoint_cold;
    uint64_t checkpoints;
};

static struct native_counts native_disk_counts(struct native_backend *b)
{
    char data[8192], name[32];
    unsigned int major, minor;
    uint64_t v[7];
    ssize_t length = pread(b->stat_fd, data, sizeof(data) - 1, 0);
    if (length <= 0) abort();
    data[length] = 0;
    char *line = data;
    while (line && *line) {
        int count = sscanf(line, "%u %u %31s %" SCNu64 " %" SCNu64 " %" SCNu64
            " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64,
            &major, &minor, name, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6]);
        if (count == 10 && !strcmp(name, "nvme0n1p2"))
            return (struct native_counts){v[0], v[2], v[4], v[6]};
        line = strchr(line, '\n');
        if (line) line++;
    }
    abort();
}
static void native_observe(void *context, unsigned int phase, const char *cache,
    unsigned int trial, unsigned int sample, int before, uint64_t ns)
{
    struct native_backend *b = context;
    if (strcmp(b->fs, "ext4") || (phase != 7 && phase != 12) ||
        (cache[0] == 'w' && (sample < 8 || sample >= 12))) return;
    struct native_counts now = native_disk_counts(b);
    if (before) { b->previous = now; return; }
    if (b->sample_count >= 96) abort();
    struct native_sample *r = &native_samples[b->sample_count++];
    *r = (struct native_sample){.phase = phase, .trial = trial, .sample = sample,
        .cold = cache[0] == 'c', .wall = ns,
        .counts = {now.reads - b->previous.reads, now.read_sectors - b->previous.read_sectors,
                   now.writes - b->previous.writes, now.write_sectors - b->previous.write_sectors}};
}
static void native_dump(struct native_backend *b)
{
    for (unsigned int i = 0; i < b->sample_count; i++) {
        struct native_sample *r = &native_samples[i];
        printf("FS_LINUX_COLD_DIAG phase=%s cache=%s trial=%u sample=%u wall_ns=%" PRIu64
            " reads=%" PRIu64 " writes=%" PRIu64 " read_sectors=%" PRIu64 " write_sectors=%" PRIu64 "\n",
            matrix_phases[r->phase], r->cold ? "cold" : "warm", r->trial, r->sample, r->wall,
            r->counts.reads, r->counts.writes, r->counts.read_sectors, r->counts.write_sectors);
    }
}

static int64_t native_call(void *context, struct matrix_call call, uint64_t *handle)
{
    struct native_backend *b = context;
    long result;
    switch (call.op) {
    case KB2_FILESYSTEM_OP_OPENAT2: {
        struct { uint64_t flags, mode, resolve; } how = {call.flags, call.mode, 0};
        result = syscall(SYS_openat2, AT_FDCWD, call.path, &how, sizeof(how));
        if (result < 0) return -errno;
        *handle = result;
        return 0;
    }
    case KB2_FILESYSTEM_OP_CLOSE: result = close(call.handle); break;
    case KB2_FILESYSTEM_OP_STATX: {
        struct statx stat;
        result = syscall(SYS_statx, AT_FDCWD, call.path, 0, STATX_BASIC_STATS, &stat);
        break;
    }
    case KB2_FILESYSTEM_OP_MKDIRAT: result = mkdirat(AT_FDCWD, call.path, call.mode); break;
    case KB2_FILESYSTEM_OP_UNLINKAT: result = unlinkat(AT_FDCWD, call.path, call.flags); break;
    case KB2_FILESYSTEM_OP_RENAMEAT2:
        result = syscall(SYS_renameat2, AT_FDCWD, call.path, AT_FDCWD, call.second, 0); break;
    case KB2_FILESYSTEM_OP_SYMLINKAT: result = symlinkat(call.path, AT_FDCWD, call.second); break;
    case KB2_FILESYSTEM_OP_READLINKAT:
        result = readlinkat(AT_FDCWD, call.path, (char *)b->buffer, call.length); break;
    case KB2_FILESYSTEM_OP_PREAD:
        result = pread(call.handle, b->buffer, call.length, call.offset);
        if (result > 0 && call.verify)
            for (long i = 0; i < result; i++) if (b->buffer[i] != 0x5a) return -EIO;
        break;
    case KB2_FILESYSTEM_OP_PWRITE:
        memset(b->buffer, 0x5a, call.length);
        result = pwrite(call.handle, b->buffer, call.length, call.offset); break;
    case KB2_FILESYSTEM_OP_FSYNC: result = fsync(call.handle); break;
    case KB2_FILESYSTEM_OP_GETDENTS64: {
        result = syscall(SYS_getdents64, call.handle, b->buffer, call.length);
        if (result < 0) break;
        size_t offset = 0;
        unsigned int entries = 0;
        while (offset < (size_t)result) {
            uint16_t length;
            if ((size_t)result - offset < 20) return -EIO;
            memcpy(&length, b->buffer + offset + 16, 2);
            if (length < 20 || length > (size_t)result - offset) return -EIO;
            offset += length;
            entries++;
        }
        return entries;
    }
    default: return -EINVAL;
    }
    return result < 0 ? -errno : result;
}
static int native_cold(void *context)
{
    struct native_backend *b = context;
    int fd, error = 0;
    if (syncfs(b->sync_fd)) return -errno;
    if (b->checkpoint_cold) {
        /* Commit completion alone can leave checkpoint references pinning
         * metadata. Match the sandbox's full freeze/thaw before eviction. */
        if (ioctl(b->sync_fd, FIFREEZE, 0)) return -errno;
        if (ioctl(b->sync_fd, FITHAW, 0)) return -errno;
        b->checkpoints++;
    }
    for (unsigned int pass = 0; !error && pass < 4; pass++) {
        fd = open("/proc/sys/vm/drop_caches", O_WRONLY);
        if (fd < 0) return -errno;
        if (write(fd, "3\n", 2) != 2) error = -errno ?: -EIO;
        if (close(fd) && !error) error = -errno;
    }
    return error;
}
static uint64_t native_clock(void *unused)
{
    (void)unused;
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) abort();
    return (uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}
static void native_report(void *context, const char *phase, const char *cache,
    unsigned int trial, uint64_t ops, uint64_t ns)
{
    struct native_backend *b = context;
    printf("FS_CACHE_MATRIX fs=%s phase=%s cache=%s trial=%u ops=%" PRIu64 " ns=%" PRIu64 "\n",
           b->fs, phase, cache, trial, ops, ns);
    fflush(stdout);
}
int main(int argc, char **argv)
{
    int disk = argc == 2 && !strcmp(argv[1], "--guest-cold");
    if (!disk && !(argc == 3 && !strcmp(argv[1], "--warm-only"))) return 2;
    /* Never drop the host's caches in a local smoke test. The cold mode needs
     * an explicit VM-only marker supplied by the dedicated guest runner. */
    if (disk && access("/run/kobox-cache-matrix-guest", F_OK)) return 2;
    struct native_backend backend = {.fs = disk ? "ext4" : "host", .sync_fd = -1,
        .stat_fd = -1, .checkpoint_cold = disk};
    backend.buffer = malloc(1048576);
    if (!backend.buffer) return 1;
    memset(backend.buffer, 0x5a, 1048576);
    if (disk) {
        struct rlimit limit = {RLIM_INFINITY, RLIM_INFINITY};
        if (setrlimit(RLIMIT_MEMLOCK, &limit) || mlockall(MCL_CURRENT | MCL_FUTURE)) return 1;
        backend.sync_fd = open("/", O_RDONLY | O_DIRECTORY);
        if (backend.sync_fd < 0) return 1;
    }
    struct matrix m = {.context = &backend, .call = native_call,
        .cold = native_cold, .clock = native_clock, .report = native_report};
    if (disk && getenv("FS_CACHE_DIAGNOSTIC") && !strcmp(getenv("FS_CACHE_DIAGNOSTIC"), "1")) {
        backend.stat_fd = open("/proc/diskstats", O_RDONLY);
        if (backend.stat_fd < 0) return 1;
        m.observe = native_observe;
    }
    if (disk) puts("FS_CACHE_POLICY cold=checkpoint_then_drop drop_passes=4");
    int error = matrix_run(&m, disk ? "" : argv[2], disk);
    native_dump(&backend);
    if (!error && disk) {
        backend.fs = "tmpfs";
        error = matrix_run(&m, "/tmp", 0);
    }
    if (backend.sync_fd >= 0) close(backend.sync_fd);
    if (backend.stat_fd >= 0) close(backend.stat_fd);
    free(backend.buffer);
    if (disk) printf("FS_CACHE_CHECKPOINT prepared=%" PRIu64 "\n", backend.checkpoints);
    printf("FS_CACHE_MATRIX_DONE status=%d\n", error);
    return !!error;
}
