#define _GNU_SOURCE

#include "../../../storage/include/storage/ext4_nvme_benchmark_spec.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static uint64_t benchmark_read_tsc(void)
{
    uint32_t lo = 0;
    uint32_t hi = 0;
    __asm__ __volatile__("lfence; rdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

static uint64_t benchmark_clock_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t benchmark_timer_base_ns;
static uint64_t benchmark_timer_base_tsc;
static double benchmark_timer_ns_per_cycle;

static int benchmark_timer_init(void)
{
    const uint64_t start_ns = benchmark_clock_ns();
    const uint64_t start_tsc = benchmark_read_tsc();
    uint64_t end_ns = start_ns;
    uint64_t end_tsc = start_tsc;
    while (end_ns - start_ns < 200000000ull) {
        end_ns = benchmark_clock_ns();
        end_tsc = benchmark_read_tsc();
    }
    if (start_ns == 0 || end_ns <= start_ns || end_tsc <= start_tsc) {
        return -1;
    }
    benchmark_timer_base_ns = end_ns;
    benchmark_timer_base_tsc = end_tsc;
    benchmark_timer_ns_per_cycle =
        (double)(end_ns - start_ns) / (double)(end_tsc - start_tsc);
    return 0;
}

static uint64_t benchmark_now_ns(void)
{
    const uint64_t now_tsc = benchmark_read_tsc();
    return benchmark_timer_base_ns +
        (uint64_t)((double)(now_tsc - benchmark_timer_base_tsc) *
            benchmark_timer_ns_per_cycle);
}

static void fill_block(unsigned char *block, size_t size)
{
    uint32_t state = 0x6b627831u;
    for (size_t i = 0; i < size; ++i) {
        state = state * 1664525u + 1013904223u;
        block[i] = (unsigned char)(state >> 24);
    }
}

static int fail(const char *phase)
{
    fprintf(stderr,
        "LINUX_EXT4_NVME_BENCH_FAIL phase=%s errno=%d\n",
        phase,
        errno);
    return 1;
}

static int join_path(char *out, size_t capacity, const char *root, const char *path)
{
    const int result = snprintf(out, capacity, "%s/%s", root, path);
    return result >= 0 && (size_t)result < capacity ? 0 : -1;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s ROOT\n", argv[0]);
        return 2;
    }

    if (benchmark_timer_init() != 0) {
        errno = EIO;
        return fail("timer");
    }

    const char *root = argv[1];
    unsigned char block[STORAGE_EXT4_NVME_BENCH_BLOCK_BYTES];
    char path[512];
    char old_paths[STORAGE_EXT4_NVME_BENCH_METADATA_FILES][512];
    char new_paths[STORAGE_EXT4_NVME_BENCH_METADATA_FILES][512];
    fill_block(block, sizeof(block));

    if (snprintf(
            path,
            sizeof(path),
            "%s/%s/%s",
            root,
            STORAGE_EXT4_NVME_BENCH_READ_DIR,
            STORAGE_EXT4_NVME_BENCH_READ_FILE) >= (int)sizeof(path))
    {
        return fail("cold_read_path");
    }

    printf("LINUX_EXT4_NVME_BENCH_START\n");
    fflush(stdout);

    uint64_t started = benchmark_now_ns();
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return fail("cold_read_open");
    }
    size_t read_bytes = 0;
    while (read_bytes < STORAGE_EXT4_NVME_BENCH_COLD_READ_BYTES) {
        const ssize_t got = pread(fd, block, sizeof(block), (off_t)read_bytes);
        if (got != (ssize_t)sizeof(block)) {
            close(fd);
            return fail("cold_read");
        }
        read_bytes += (size_t)got;
    }
    if (close(fd) != 0) {
        return fail("cold_read_close");
    }
    const uint64_t cold_read_ns = benchmark_now_ns() - started;
    printf("LINUX_EXT4_NVME_BENCH phase=cold_read bytes=%zu ns=%llu\n",
        read_bytes,
        (unsigned long long)cold_read_ns);

    char bench_dir[512];
    if (join_path(bench_dir, sizeof(bench_dir), root, STORAGE_EXT4_NVME_BENCH_DIR) != 0 ||
        mkdir(bench_dir, 0755) != 0)
    {
        return fail("mkdir");
    }
    char stream_path[512];
    if (join_path(
            stream_path,
            sizeof(stream_path),
            bench_dir,
            STORAGE_EXT4_NVME_BENCH_STREAM_FILE) != 0)
    {
        return fail("stream_path");
    }
    fd = open(stream_path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0644);
    if (fd < 0 || close(fd) != 0) {
        return fail("stream_create");
    }

    started = benchmark_now_ns();
    fd = open(stream_path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        return fail("write_open");
    }
    for (uint64_t block_index = 0;
         block_index < STORAGE_EXT4_NVME_BENCH_STREAM_BLOCKS;
         ++block_index)
    {
        if (pwrite(
                fd,
                block,
                sizeof(block),
                (off_t)(block_index * sizeof(block))) != (ssize_t)sizeof(block))
        {
            close(fd);
            return fail("write");
        }
    }
    if (fsync(fd) != 0 || close(fd) != 0) {
        return fail("write_sync");
    }
    const uint64_t write_sync_ns = benchmark_now_ns() - started;
    printf("LINUX_EXT4_NVME_BENCH phase=write_sync bytes=%u ns=%llu\n",
        STORAGE_EXT4_NVME_BENCH_STREAM_BLOCKS * STORAGE_EXT4_NVME_BENCH_BLOCK_BYTES,
        (unsigned long long)write_sync_ns);

    fd = open(stream_path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        return fail("overwrite_open");
    }
    uint64_t fsync_total_ns = 0;
    for (unsigned iteration = 0;
         iteration < STORAGE_EXT4_NVME_BENCH_FSYNC_ITERATIONS;
         ++iteration)
    {
        started = benchmark_now_ns();
        if (pwrite(
                fd,
                block,
                sizeof(block),
                (off_t)((uint64_t)iteration * sizeof(block))) != (ssize_t)sizeof(block) ||
            fsync(fd) != 0)
        {
            close(fd);
            return fail("overwrite_fsync");
        }
        fsync_total_ns += benchmark_now_ns() - started;
    }
    if (close(fd) != 0) {
        return fail("overwrite_close");
    }
    printf("LINUX_EXT4_NVME_BENCH phase=overwrite_fsync iterations=%u total_ns=%llu avg_ns=%llu\n",
        STORAGE_EXT4_NVME_BENCH_FSYNC_ITERATIONS,
        (unsigned long long)fsync_total_ns,
        (unsigned long long)(fsync_total_ns / STORAGE_EXT4_NVME_BENCH_FSYNC_ITERATIONS));

    for (unsigned i = 0; i < STORAGE_EXT4_NVME_BENCH_METADATA_FILES; ++i) {
        char name[16];
        snprintf(name, sizeof(name), "m%03u", i);
        if (join_path(old_paths[i], sizeof(old_paths[i]), bench_dir, name) != 0) {
            return fail("create_path");
        }
        snprintf(name, sizeof(name), "r%03u", i);
        if (join_path(new_paths[i], sizeof(new_paths[i]), bench_dir, name) != 0) {
            return fail("rename_path");
        }
    }

    uint64_t create_total_ns = 0;
    for (unsigned i = 0; i < STORAGE_EXT4_NVME_BENCH_METADATA_FILES; ++i) {
        started = benchmark_now_ns();
        fd = open(old_paths[i], O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0644);
        create_total_ns += benchmark_now_ns() - started;
        if (fd < 0 || close(fd) != 0) {
            return fail("create");
        }
    }
    printf("LINUX_EXT4_NVME_BENCH phase=create files=%u total_ns=%llu avg_ns=%llu\n",
        STORAGE_EXT4_NVME_BENCH_METADATA_FILES,
        (unsigned long long)create_total_ns,
        (unsigned long long)(create_total_ns / STORAGE_EXT4_NVME_BENCH_METADATA_FILES));

    uint64_t rename_total_ns = 0;
    for (unsigned i = 0; i < STORAGE_EXT4_NVME_BENCH_METADATA_FILES; ++i) {
        started = benchmark_now_ns();
        const int rename_status = rename(old_paths[i], new_paths[i]);
        rename_total_ns += benchmark_now_ns() - started;
        if (rename_status != 0) {
            return fail("rename");
        }
    }
    printf("LINUX_EXT4_NVME_BENCH phase=rename files=%u total_ns=%llu avg_ns=%llu\n",
        STORAGE_EXT4_NVME_BENCH_METADATA_FILES,
        (unsigned long long)rename_total_ns,
        (unsigned long long)(rename_total_ns / STORAGE_EXT4_NVME_BENCH_METADATA_FILES));

    uint64_t unlink_total_ns = 0;
    for (unsigned i = 0; i < STORAGE_EXT4_NVME_BENCH_METADATA_FILES; ++i) {
        started = benchmark_now_ns();
        const int unlink_status = unlink(new_paths[i]);
        unlink_total_ns += benchmark_now_ns() - started;
        if (unlink_status != 0) {
            return fail("unlink");
        }
    }
    printf("LINUX_EXT4_NVME_BENCH phase=unlink files=%u total_ns=%llu avg_ns=%llu\n",
        STORAGE_EXT4_NVME_BENCH_METADATA_FILES,
        (unsigned long long)unlink_total_ns,
        (unsigned long long)(unlink_total_ns / STORAGE_EXT4_NVME_BENCH_METADATA_FILES));

    const int root_fd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (root_fd < 0) {
        return fail("syncfs_open");
    }
    started = benchmark_now_ns();
    const int sync_status = syncfs(root_fd);
    const uint64_t syncfs_ns = benchmark_now_ns() - started;
    if (sync_status != 0) {
        close(root_fd);
        return fail("syncfs");
    }
    printf("LINUX_EXT4_NVME_BENCH phase=syncfs ns=%llu\n",
        (unsigned long long)syncfs_ns);

    if (unlink(stream_path) != 0 || rmdir(bench_dir) != 0 || syncfs(root_fd) != 0 ||
        close(root_fd) != 0)
    {
        return fail("cleanup");
    }

    printf("LINUX_EXT4_NVME_BENCH_DONE\n");
    fflush(stdout);
    return 0;
}
