#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

struct record {
    uint32_t key;
    char name[32];
    char payload[48];
};

static unsigned long long now_ns(void)
{
#if defined(__x86_64__) || defined(__i386__)
    static int calibrated;
    static unsigned long long base_cycles;
    static unsigned long long base_ns;
    static double ns_per_cycle;
    unsigned int lo = 0;
    unsigned int hi = 0;

    if (!calibrated) {
        struct timespec ts0;
        struct timespec ts1;
        unsigned long long c0;
        unsigned long long c1;
        unsigned long long t0;
        unsigned long long t1;

        if (clock_gettime(CLOCK_MONOTONIC, &ts0) != 0) {
            return 0;
        }
        __asm__ __volatile__("lfence; rdtsc" : "=a"(lo), "=d"(hi) :: "memory");
        c0 = ((unsigned long long)hi << 32) | (unsigned long long)lo;
        t0 = (unsigned long long)ts0.tv_sec * 1000000000ull + (unsigned long long)ts0.tv_nsec;
        do {
            if (clock_gettime(CLOCK_MONOTONIC, &ts1) != 0) {
                return 0;
            }
            t1 = (unsigned long long)ts1.tv_sec * 1000000000ull + (unsigned long long)ts1.tv_nsec;
        } while (t1 - t0 < 20000000ull);
        __asm__ __volatile__("lfence; rdtsc" : "=a"(lo), "=d"(hi) :: "memory");
        c1 = ((unsigned long long)hi << 32) | (unsigned long long)lo;
        if (c1 <= c0 || t1 <= t0) {
            return t1;
        }
        base_cycles = c1;
        base_ns = t1;
        ns_per_cycle = (double)(t1 - t0) / (double)(c1 - c0);
        calibrated = 1;
    }

    __asm__ __volatile__("lfence; rdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    const unsigned long long cycles = ((unsigned long long)hi << 32) | (unsigned long long)lo;
    const unsigned long long delta_cycles = cycles - base_cycles;
    return base_ns + (unsigned long long)((double)delta_cycles * ns_per_cycle);
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec;
#endif
}

static void metric_avg(
    const char *name,
    unsigned long long total_ns,
    unsigned long long iterations)
{
    const unsigned long long avg = iterations == 0 ? 0 : total_ns / iterations;
    printf(
        "[libc-mix-bench] metric op=%s iterations=%llu total_ns=%llu avg_ns=%llu\n",
        name,
        iterations,
        total_ns,
        avg);
    fflush(stdout);
}

static void metric_total(const char *name, unsigned long long total)
{
    printf("[libc-mix-bench] metric op=%s total=%llu\n", name, total);
    fflush(stdout);
}

static int fail(const char *what)
{
    fprintf(stderr, "[libc-mix-bench] %s failed errno=%d\n", what, errno);
    return 1;
}

static void stage(const char *name)
{
    printf("[libc-mix-bench] stage %s\n", name);
    fflush(stdout);
}

static int compare_record_key(const void *a, const void *b)
{
    const struct record *ra = (const struct record *)a;
    const struct record *rb = (const struct record *)b;
    if (ra->key < rb->key) return -1;
    if (ra->key > rb->key) return 1;
    return strcmp(ra->name, rb->name);
}

static void fill_pattern(unsigned char *buffer, size_t size, unsigned int seed)
{
    for (size_t i = 0; i < size; ++i) {
        seed = seed * 1103515245u + 12345u;
        buffer[i] = (unsigned char)('A' + ((seed >> 16) % 26));
    }
}

static int prepare_file(const char *path)
{
    unsigned char block[4096];
    fill_pattern(block, sizeof(block), 0x1234u);

    FILE *f = fopen(path, "w+");
    if (f == NULL) return fail("fopen prepare");
    if (fwrite(block, 1, sizeof(block), f) != sizeof(block)) {
        fclose(f);
        return fail("fwrite prepare");
    }
    if (fflush(f) != 0) {
        fclose(f);
        return fail("fflush prepare");
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return fail("fseek prepare");
    }
    unsigned char check[32];
    if (fread(check, 1, sizeof(check), f) != sizeof(check)) {
        fclose(f);
        return fail("fread prepare");
    }
    if (memcmp(check, block, sizeof(check)) != 0) {
        fclose(f);
        fprintf(stderr, "[libc-mix-bench] prepare verify failed\n");
        return 1;
    }
    if (fclose(f) != 0) return fail("fclose prepare");
    return 0;
}

static int prepare_text_file(const char *path)
{
    FILE *f = fopen(path, "w+");
    if (f == NULL) return fail("fopen prepare_text");
    for (int i = 0; i < 128; ++i) {
        if (fprintf(f, "line-%03d key=%08x payload=abcdefghijklmnopqrstuvwxyz\n",
                i,
                (unsigned)(i * 2654435761u)) < 0)
        {
            fclose(f);
            return fail("fprintf prepare_text");
        }
    }
    if (fflush(f) != 0) {
        fclose(f);
        return fail("fflush prepare_text");
    }
    if (fclose(f) != 0) return fail("fclose prepare_text");
    return 0;
}

static int bench_path_metadata_hot(const char *path)
{
    const unsigned long long iterations = 256;
    unsigned long long access_ns = 0;
    unsigned long long stat_ns = 0;
    unsigned long long lstat_ns = 0;
    unsigned long long fstatat_ns = 0;
    unsigned long long getcwd_ns = 0;
    unsigned long long realpath_ns = 0;
    unsigned long long readlink_ns = 0;
    unsigned long long checksum = 0;

    const unsigned long long t0 = now_ns();
    for (unsigned long long i = 0; i < iterations; ++i) {
        unsigned long long p0 = now_ns();
        if (access(path, F_OK) != 0) return fail("access path_metadata");
        unsigned long long p1 = now_ns();
        access_ns += p1 - p0;

        struct stat st;
        p0 = now_ns();
        if (stat(path, &st) != 0 || st.st_size <= 0) return fail("stat path_metadata");
        p1 = now_ns();
        stat_ns += p1 - p0;
        checksum += (unsigned long long)st.st_size;

        p0 = now_ns();
        if (lstat(path, &st) != 0 || st.st_size <= 0) return fail("lstat path_metadata");
        p1 = now_ns();
        lstat_ns += p1 - p0;

        p0 = now_ns();
        if (fstatat(AT_FDCWD, path, &st, 0) != 0 || st.st_size <= 0) {
            return fail("fstatat path_metadata");
        }
        p1 = now_ns();
        fstatat_ns += p1 - p0;

        char cwd[PATH_MAX];
        p0 = now_ns();
        if (getcwd(cwd, sizeof(cwd)) == NULL || cwd[0] != '/') return fail("getcwd path_metadata");
        p1 = now_ns();
        getcwd_ns += p1 - p0;
        checksum += (unsigned long long)cwd[0];

        char resolved[PATH_MAX];
        p0 = now_ns();
        if (realpath(path, resolved) == NULL || resolved[0] != '/') return fail("realpath path_metadata");
        p1 = now_ns();
        realpath_ns += p1 - p0;
        checksum += (unsigned long long)resolved[0];

        char linkbuf[PATH_MAX];
        errno = 0;
        p0 = now_ns();
        ssize_t link_len = readlink(path, linkbuf, sizeof(linkbuf));
        p1 = now_ns();
        readlink_ns += p1 - p0;
        if (link_len >= 0 || errno != EINVAL) return fail("readlink regular path_metadata");
    }

    metric_avg("mixed_path_metadata_hot", now_ns() - t0, iterations);
    metric_avg("mixed_path_metadata_hot.access_f_ok", access_ns, iterations);
    metric_avg("mixed_path_metadata_hot.stat", stat_ns, iterations);
    metric_avg("mixed_path_metadata_hot.lstat", lstat_ns, iterations);
    metric_avg("mixed_path_metadata_hot.fstatat", fstatat_ns, iterations);
    metric_avg("mixed_path_metadata_hot.getcwd", getcwd_ns, iterations);
    metric_avg("mixed_path_metadata_hot.realpath", realpath_ns, iterations);
    metric_avg("mixed_path_metadata_hot.readlink_regular_einval", readlink_ns, iterations);
    metric_total("mixed_path_metadata_checksum", checksum);
    return 0;
}

static int bench_openat_relative_hot(const char *dir_path, const char *name)
{
    const unsigned long long iterations = 256;
    unsigned long long open_dir_ns = 0;
    unsigned long long fstatat_ns = 0;
    unsigned long long openat_ns = 0;
    unsigned long long read_ns = 0;
    unsigned long long close_file_ns = 0;
    unsigned long long close_dir_ns = 0;
    unsigned long long checksum = 0;
    unsigned char buffer[64];

    const unsigned long long t0 = now_ns();
    for (unsigned long long i = 0; i < iterations; ++i) {
        unsigned long long p0 = now_ns();
        int dirfd = open(dir_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        unsigned long long p1 = now_ns();
        open_dir_ns += p1 - p0;
        if (dirfd < 0) return fail("open dir openat_relative");

        struct stat st;
        p0 = now_ns();
        if (fstatat(dirfd, name, &st, 0) != 0 || st.st_size <= 0) {
            close(dirfd);
            return fail("fstatat openat_relative");
        }
        p1 = now_ns();
        fstatat_ns += p1 - p0;

        p0 = now_ns();
        int fd = openat(dirfd, name, O_RDONLY | O_CLOEXEC);
        p1 = now_ns();
        openat_ns += p1 - p0;
        if (fd < 0) {
            close(dirfd);
            return fail("openat openat_relative");
        }

        p0 = now_ns();
        ssize_t got = read(fd, buffer, sizeof(buffer));
        p1 = now_ns();
        read_ns += p1 - p0;
        if (got != (ssize_t)sizeof(buffer)) {
            close(fd);
            close(dirfd);
            return fail("read openat_relative");
        }
        checksum += buffer[i & (sizeof(buffer) - 1)];

        p0 = now_ns();
        if (close(fd) != 0) {
            close(dirfd);
            return fail("close file openat_relative");
        }
        p1 = now_ns();
        close_file_ns += p1 - p0;

        p0 = now_ns();
        if (close(dirfd) != 0) return fail("close dir openat_relative");
        p1 = now_ns();
        close_dir_ns += p1 - p0;
    }

    metric_avg("mixed_openat_relative_hot", now_ns() - t0, iterations);
    metric_avg("mixed_openat_relative_hot.open_dir", open_dir_ns, iterations);
    metric_avg("mixed_openat_relative_hot.fstatat", fstatat_ns, iterations);
    metric_avg("mixed_openat_relative_hot.openat", openat_ns, iterations);
    metric_avg("mixed_openat_relative_hot.read", read_ns, iterations);
    metric_avg("mixed_openat_relative_hot.close_file", close_file_ns, iterations);
    metric_avg("mixed_openat_relative_hot.close_dir", close_dir_ns, iterations);
    metric_total("mixed_openat_relative_checksum", checksum);
    return 0;
}

static int bench_open_stat_io(const char *path)
{
    const unsigned long long iterations = 256;
    unsigned char read_buffer[96];
    unsigned char write_buffer[32];
    unsigned long long open_ns = 0;
    unsigned long long fstat_ns = 0;
    unsigned long long pread_ns = 0;
    unsigned long long pwrite_ns = 0;
    unsigned long long close_ns = 0;
    fill_pattern(write_buffer, sizeof(write_buffer), 0x4455u);

    const unsigned long long t0 = now_ns();
    for (unsigned long long i = 0; i < iterations; ++i) {
        unsigned long long p0 = now_ns();
        int fd = open(path, O_RDWR | O_CLOEXEC);
        unsigned long long p1 = now_ns();
        open_ns += p1 - p0;
        if (fd < 0) return fail("open open_stat_io");
        struct stat st;
        p0 = now_ns();
        if (fstat(fd, &st) != 0 || st.st_size < 512) {
            close(fd);
            return fail("fstat open_stat_io");
        }
        p1 = now_ns();
        fstat_ns += p1 - p0;
        const off_t roff = (off_t)((i * 37u) & 2047u);
        p0 = now_ns();
        if (pread(fd, read_buffer, sizeof(read_buffer), roff) != (ssize_t)sizeof(read_buffer)) {
            close(fd);
            return fail("pread open_stat_io");
        }
        p1 = now_ns();
        pread_ns += p1 - p0;
        const off_t woff = (off_t)(256 + ((i * 17u) & 1023u));
        p0 = now_ns();
        if (pwrite(fd, write_buffer, sizeof(write_buffer), woff) != (ssize_t)sizeof(write_buffer)) {
            close(fd);
            return fail("pwrite open_stat_io");
        }
        p1 = now_ns();
        pwrite_ns += p1 - p0;
        p0 = now_ns();
        if (close(fd) != 0) return fail("close open_stat_io");
        p1 = now_ns();
        close_ns += p1 - p0;
    }
    metric_avg("mixed_open_stat_pread_pwrite_close", now_ns() - t0, iterations);
    metric_avg("mixed_open_stat_pread_pwrite_close.open", open_ns, iterations);
    metric_avg("mixed_open_stat_pread_pwrite_close.fstat", fstat_ns, iterations);
    metric_avg("mixed_open_stat_pread_pwrite_close.pread", pread_ns, iterations);
    metric_avg("mixed_open_stat_pread_pwrite_close.pwrite", pwrite_ns, iterations);
    metric_avg("mixed_open_stat_pread_pwrite_close.close", close_ns, iterations);
    return 0;
}

static int bench_stdio_loop(const char *path)
{
    const unsigned long long iterations = 128;
    unsigned char buffer[128];
    unsigned long long fopen_ns = 0;
    unsigned long long fread_ns = 0;
    unsigned long long fwrite_ns = 0;
    unsigned long long fflush_ns = 0;
    unsigned long long fclose_ns = 0;
    fill_pattern(buffer, sizeof(buffer), 0x7788u);

    const unsigned long long t0 = now_ns();
    for (unsigned long long i = 0; i < iterations; ++i) {
        unsigned long long p0 = now_ns();
        FILE *f = fopen(path, "r+");
        unsigned long long p1 = now_ns();
        fopen_ns += p1 - p0;
        if (f == NULL) return fail("fopen stdio_loop");
        if (fseek(f, (long)((i * 29u) & 1023u), SEEK_SET) != 0) {
            fclose(f);
            return fail("fseek stdio_loop read");
        }
        p0 = now_ns();
        if (fread(buffer, 1, 64, f) != 64) {
            fclose(f);
            return fail("fread stdio_loop");
        }
        p1 = now_ns();
        fread_ns += p1 - p0;
        if (fseek(f, (long)(1536 + ((i * 19u) & 1023u)), SEEK_SET) != 0) {
            fclose(f);
            return fail("fseek stdio_loop write");
        }
        p0 = now_ns();
        if (fwrite(buffer, 1, 64, f) != 64) {
            fclose(f);
            return fail("fwrite stdio_loop");
        }
        p1 = now_ns();
        fwrite_ns += p1 - p0;
        if ((i & 15u) == 0 && fflush(f) != 0) {
            fclose(f);
            return fail("fflush stdio_loop");
        }
        if ((i & 15u) == 0) {
            fflush_ns += now_ns() - p1;
        }
        p0 = now_ns();
        if (fclose(f) != 0) return fail("fclose stdio_loop");
        p1 = now_ns();
        fclose_ns += p1 - p0;
    }
    metric_avg("mixed_stdio_fopen_fread_fwrite_fclose", now_ns() - t0, iterations);
    metric_avg("mixed_stdio_fopen_fread_fwrite_fclose.fopen", fopen_ns, iterations);
    metric_avg("mixed_stdio_fopen_fread_fwrite_fclose.fread", fread_ns, iterations);
    metric_avg("mixed_stdio_fopen_fread_fwrite_fclose.fwrite", fwrite_ns, iterations);
    metric_avg("mixed_stdio_fopen_fread_fwrite_fclose.fflush", fflush_ns, iterations);
    metric_avg("mixed_stdio_fopen_fread_fwrite_fclose.fclose", fclose_ns, iterations);
    return 0;
}

static int bench_stdio_hot(const char *text_path, const char *sink_path)
{
    const unsigned long long iterations = 128;
    unsigned long long fopen_read_ns = 0;
    unsigned long long fgetc_ns = 0;
    unsigned long long rewind_ns = 0;
    unsigned long long fgets_ns = 0;
    unsigned long long fseek_ns = 0;
    unsigned long long ftell_ns = 0;
    unsigned long long fclose_read_ns = 0;
    unsigned long long fopen_write_ns = 0;
    unsigned long long fputc_ns = 0;
    unsigned long long fputs_ns = 0;
    unsigned long long fprintf_ns = 0;
    unsigned long long snprintf_ns = 0;
    unsigned long long fflush_ns = 0;
    unsigned long long fclose_write_ns = 0;
    unsigned long long checksum = 0;
    char line[160];
    char formatted[160];

    const unsigned long long t0 = now_ns();
    for (unsigned long long i = 0; i < iterations; ++i) {
        unsigned long long p0 = now_ns();
        FILE *rf = fopen(text_path, "r");
        unsigned long long p1 = now_ns();
        fopen_read_ns += p1 - p0;
        if (rf == NULL) return fail("fopen read stdio_hot");

        p0 = now_ns();
        for (int j = 0; j < 32; ++j) {
            int c = fgetc(rf);
            if (c == EOF) {
                fclose(rf);
                return fail("fgetc stdio_hot");
            }
            checksum += (unsigned long long)(unsigned char)c;
        }
        p1 = now_ns();
        fgetc_ns += p1 - p0;

        p0 = now_ns();
        rewind(rf);
        p1 = now_ns();
        rewind_ns += p1 - p0;

        p0 = now_ns();
        if (fgets(line, sizeof(line), rf) == NULL) {
            fclose(rf);
            return fail("fgets stdio_hot");
        }
        p1 = now_ns();
        fgets_ns += p1 - p0;
        checksum += (unsigned long long)line[0];

        p0 = now_ns();
        if (fseek(rf, (long)((i * 23u) & 511u), SEEK_SET) != 0) {
            fclose(rf);
            return fail("fseek stdio_hot");
        }
        p1 = now_ns();
        fseek_ns += p1 - p0;

        p0 = now_ns();
        long pos = ftell(rf);
        p1 = now_ns();
        ftell_ns += p1 - p0;
        if (pos < 0) {
            fclose(rf);
            return fail("ftell stdio_hot");
        }
        checksum += (unsigned long long)pos;

        p0 = now_ns();
        if (fclose(rf) != 0) return fail("fclose read stdio_hot");
        p1 = now_ns();
        fclose_read_ns += p1 - p0;

        p0 = now_ns();
        FILE *wf = fopen(sink_path, "a");
        p1 = now_ns();
        fopen_write_ns += p1 - p0;
        if (wf == NULL) return fail("fopen write stdio_hot");

        p0 = now_ns();
        if (fputc('A' + (int)(i & 15u), wf) == EOF) {
            fclose(wf);
            return fail("fputc stdio_hot");
        }
        p1 = now_ns();
        fputc_ns += p1 - p0;

        p0 = now_ns();
        if (fputs(" stdio-hot ", wf) == EOF) {
            fclose(wf);
            return fail("fputs stdio_hot");
        }
        p1 = now_ns();
        fputs_ns += p1 - p0;

        p0 = now_ns();
        if (fprintf(wf, "round=%llu checksum=%llu\n", i, checksum) < 0) {
            fclose(wf);
            return fail("fprintf stdio_hot");
        }
        p1 = now_ns();
        fprintf_ns += p1 - p0;

        p0 = now_ns();
        int n = snprintf(formatted, sizeof(formatted), "fmt:%llu:%llu:%s", i, checksum, line);
        p1 = now_ns();
        snprintf_ns += p1 - p0;
        if (n <= 0 || n >= (int)sizeof(formatted)) {
            fclose(wf);
            return fail("snprintf stdio_hot");
        }

        if ((i & 15u) == 0) {
            p0 = now_ns();
            if (fflush(wf) != 0) {
                fclose(wf);
                return fail("fflush stdio_hot");
            }
            p1 = now_ns();
            fflush_ns += p1 - p0;
        }

        p0 = now_ns();
        if (fclose(wf) != 0) return fail("fclose write stdio_hot");
        p1 = now_ns();
        fclose_write_ns += p1 - p0;
    }

    metric_avg("mixed_stdio_hot", now_ns() - t0, iterations);
    metric_avg("mixed_stdio_hot.fopen_read", fopen_read_ns, iterations);
    metric_avg("mixed_stdio_hot.fgetc32", fgetc_ns, iterations);
    metric_avg("mixed_stdio_hot.rewind", rewind_ns, iterations);
    metric_avg("mixed_stdio_hot.fgets", fgets_ns, iterations);
    metric_avg("mixed_stdio_hot.fseek", fseek_ns, iterations);
    metric_avg("mixed_stdio_hot.ftell", ftell_ns, iterations);
    metric_avg("mixed_stdio_hot.fclose_read", fclose_read_ns, iterations);
    metric_avg("mixed_stdio_hot.fopen_append", fopen_write_ns, iterations);
    metric_avg("mixed_stdio_hot.fputc", fputc_ns, iterations);
    metric_avg("mixed_stdio_hot.fputs", fputs_ns, iterations);
    metric_avg("mixed_stdio_hot.fprintf", fprintf_ns, iterations);
    metric_avg("mixed_stdio_hot.snprintf", snprintf_ns, iterations);
    metric_avg("mixed_stdio_hot.fflush_every16", fflush_ns, iterations);
    metric_avg("mixed_stdio_hot.fclose_write", fclose_write_ns, iterations);
    metric_total("mixed_stdio_hot_checksum", checksum);
    return 0;
}

static int bench_stdio_buffered_write_hot(const char *sink_path)
{
    const unsigned long long iterations = 512;
    unsigned long long fputc_ns = 0;
    unsigned long long fputs_ns = 0;
    unsigned long long fprintf_ns = 0;
    unsigned long long snprintf_ns = 0;
    unsigned long long fflush_ns = 0;
    unsigned long long checksum = 0;
    char formatted[160];

    FILE *wf = fopen(sink_path, "w+");
    if (wf == NULL) return fail("fopen stdio_buffered_write_hot");
    const unsigned long long t0 = now_ns();
    for (unsigned long long i = 0; i < iterations; ++i) {
        unsigned long long p0 = now_ns();
        if (fputc('a' + (int)(i % 26u), wf) == EOF) {
            fclose(wf);
            return fail("fputc stdio_buffered_write_hot");
        }
        unsigned long long p1 = now_ns();
        fputc_ns += p1 - p0;

        p0 = now_ns();
        if (fputs(" buffered-write ", wf) == EOF) {
            fclose(wf);
            return fail("fputs stdio_buffered_write_hot");
        }
        p1 = now_ns();
        fputs_ns += p1 - p0;

        p0 = now_ns();
        if (fprintf(wf, "round=%llu\n", i) < 0) {
            fclose(wf);
            return fail("fprintf stdio_buffered_write_hot");
        }
        p1 = now_ns();
        fprintf_ns += p1 - p0;

        p0 = now_ns();
        int n = snprintf(formatted, sizeof(formatted), "formatted-%llu-%llu", i, checksum);
        p1 = now_ns();
        snprintf_ns += p1 - p0;
        if (n <= 0 || n >= (int)sizeof(formatted)) {
            fclose(wf);
            return fail("snprintf stdio_buffered_write_hot");
        }
        checksum += (unsigned long long)n;
    }
    unsigned long long p0 = now_ns();
    if (fflush(wf) != 0) {
        fclose(wf);
        return fail("fflush stdio_buffered_write_hot");
    }
    unsigned long long p1 = now_ns();
    fflush_ns += p1 - p0;
    if (fclose(wf) != 0) return fail("fclose stdio_buffered_write_hot");

    metric_avg("mixed_stdio_buffered_write_hot", now_ns() - t0, iterations);
    metric_avg("mixed_stdio_buffered_write_hot.fputc", fputc_ns, iterations);
    metric_avg("mixed_stdio_buffered_write_hot.fputs", fputs_ns, iterations);
    metric_avg("mixed_stdio_buffered_write_hot.fprintf", fprintf_ns, iterations);
    metric_avg("mixed_stdio_buffered_write_hot.snprintf", snprintf_ns, iterations);
    metric_avg("mixed_stdio_buffered_write_hot.fflush_once", fflush_ns, iterations);
    metric_total("mixed_stdio_buffered_write_checksum", checksum);
    return 0;
}

static int bench_vector_io(const char *path)
{
    const unsigned long long iterations = 256;
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) return fail("open vector_io");

    char a[32];
    char b[48];
    char c[64];
    fill_pattern((unsigned char *)a, sizeof(a), 0x1010u);
    fill_pattern((unsigned char *)b, sizeof(b), 0x2020u);
    fill_pattern((unsigned char *)c, sizeof(c), 0x3030u);
    struct iovec writev_iov[3] = {
        {.iov_base = a, .iov_len = sizeof(a)},
        {.iov_base = b, .iov_len = sizeof(b)},
        {.iov_base = c, .iov_len = sizeof(c)},
    };
    char out_a[32];
    char out_b[48];
    char out_c[64];
    struct iovec readv_iov[3] = {
        {.iov_base = out_a, .iov_len = sizeof(out_a)},
        {.iov_base = out_b, .iov_len = sizeof(out_b)},
        {.iov_base = out_c, .iov_len = sizeof(out_c)},
    };

    const unsigned long long t0 = now_ns();
    for (unsigned long long i = 0; i < iterations; ++i) {
        const off_t off = (off_t)(512 + ((i * 13u) & 1023u));
        if (lseek(fd, off, SEEK_SET) != off) {
            close(fd);
            return fail("lseek vector write");
        }
        if (writev(fd, writev_iov, 3) != (ssize_t)(sizeof(a) + sizeof(b) + sizeof(c))) {
            close(fd);
            return fail("writev vector_io");
        }
        if (lseek(fd, off, SEEK_SET) != off) {
            close(fd);
            return fail("lseek vector read");
        }
        if (readv(fd, readv_iov, 3) != (ssize_t)(sizeof(out_a) + sizeof(out_b) + sizeof(out_c))) {
            close(fd);
            return fail("readv vector_io");
        }
        if (memcmp(a, out_a, sizeof(a)) != 0 ||
            memcmp(b, out_b, sizeof(b)) != 0 ||
            memcmp(c, out_c, sizeof(c)) != 0)
        {
            close(fd);
            fprintf(stderr, "[libc-mix-bench] vector io verify failed\n");
            return 1;
        }
    }
    metric_avg("mixed_lseek_writev_readv", now_ns() - t0, iterations);
    if (close(fd) != 0) return fail("close vector_io");
    return 0;
}

static int bench_dirent_loop(void)
{
    const unsigned long long iterations = 128;
    unsigned long long total_entries = 0;

    const unsigned long long t0 = now_ns();
    for (unsigned long long i = 0; i < iterations; ++i) {
        DIR *dir = opendir("/");
        if (dir == NULL) return fail("opendir dirent_loop");
        for (;;) {
            errno = 0;
            struct dirent *ent = readdir(dir);
            if (ent == NULL) {
                if (errno != 0) {
                    closedir(dir);
                    return fail("readdir dirent_loop");
                }
                break;
            }
            if (ent->d_name[0] != '\0') total_entries++;
        }
        if (closedir(dir) != 0) return fail("closedir dirent_loop");
    }
    metric_avg("mixed_opendir_readdir_closedir", now_ns() - t0, iterations);
    printf("[libc-mix-bench] metric op=mixed_dir_entries total=%llu avg=%llu\n",
        total_entries,
        iterations == 0 ? 0 : total_entries / iterations);
    fflush(stdout);
    return 0;
}

static int bench_alloc_sort_string(void)
{
    const unsigned long long iterations = 256;
    const size_t count = 256;
    unsigned long long checksum = 0;

    const unsigned long long t0 = now_ns();
    for (unsigned long long round = 0; round < iterations; ++round) {
        struct record *records = (struct record *)malloc(count * sizeof(*records));
        if (records == NULL) return fail("malloc records");
        for (size_t i = 0; i < count; ++i) {
            records[i].key = (uint32_t)(((count - i) * 2654435761u) ^ (uint32_t)round);
            snprintf(records[i].name, sizeof(records[i].name), "rec-%03zu-%03llu", i, round);
            snprintf(records[i].payload, sizeof(records[i].payload), "payload:%u:%s",
                records[i].key,
                records[i].name);
            checksum += strlen(records[i].payload);
        }
        qsort(records, count, sizeof(*records), compare_record_key);
        struct record needle = records[count / 2];
        struct record *found = (struct record *)bsearch(
            &needle,
            records,
            count,
            sizeof(*records),
            compare_record_key);
        if (found == NULL || strcmp(found->name, needle.name) != 0) {
            free(records);
            fprintf(stderr, "[libc-mix-bench] bsearch verify failed\n");
            return 1;
        }
        struct record *grown = (struct record *)realloc(records, (count + 16) * sizeof(*records));
        if (grown == NULL) {
            free(records);
            return fail("realloc records");
        }
        records = grown;
        memset(records + count, 0, 16 * sizeof(*records));
        checksum += records[count / 2].key & 0xffu;
        free(records);
    }
    metric_avg("mixed_malloc_string_qsort_bsearch_free", now_ns() - t0, iterations);
    printf("[libc-mix-bench] metric op=mixed_cpu_checksum total=%llu\n", checksum);
    fflush(stdout);
    return 0;
}

static int bench_memory_string_hot(void)
{
    const unsigned long long iterations = 512;
    const size_t bytes = 4096;
    unsigned long long malloc_free_ns = 0;
    unsigned long long calloc_free_ns = 0;
    unsigned long long realloc_ns = 0;
    unsigned long long memset_ns = 0;
    unsigned long long memcpy_ns = 0;
    unsigned long long memmove_ns = 0;
    unsigned long long strlen_ns = 0;
    unsigned long long strcmp_ns = 0;
    unsigned long long strchr_ns = 0;
    unsigned long long strstr_ns = 0;
    unsigned long long checksum = 0;

    unsigned char *src = (unsigned char *)malloc(bytes + 64);
    unsigned char *dst = (unsigned char *)malloc(bytes + 64);
    char *text = (char *)malloc(bytes + 64);
    if (src == NULL || dst == NULL || text == NULL) {
        free(src);
        free(dst);
        free(text);
        return fail("malloc memory_string_hot setup");
    }
    fill_pattern(src, bytes + 64, 0xa5a5u);
    for (size_t i = 0; i < bytes + 63; ++i) {
        text[i] = (char)('a' + (i % 26));
    }
    memcpy(text + 2048, "needle", 6);
    text[bytes + 63] = 0;

    const unsigned long long t0 = now_ns();
    for (unsigned long long i = 0; i < iterations; ++i) {
        unsigned long long p0 = now_ns();
        void *p = malloc(256 + (size_t)(i & 127u));
        unsigned long long p1 = now_ns();
        malloc_free_ns += p1 - p0;
        if (p == NULL) {
            free(src);
            free(dst);
            free(text);
            return fail("malloc memory_string_hot");
        }
        p0 = now_ns();
        free(p);
        p1 = now_ns();
        malloc_free_ns += p1 - p0;

        p0 = now_ns();
        p = calloc(8 + (size_t)(i & 7u), 32);
        p1 = now_ns();
        calloc_free_ns += p1 - p0;
        if (p == NULL) {
            free(src);
            free(dst);
            free(text);
            return fail("calloc memory_string_hot");
        }
        p0 = now_ns();
        free(p);
        p1 = now_ns();
        calloc_free_ns += p1 - p0;

        p = malloc(128);
        if (p == NULL) {
            free(src);
            free(dst);
            free(text);
            return fail("malloc realloc memory_string_hot");
        }
        p0 = now_ns();
        p = realloc(p, 512 + (size_t)(i & 255u));
        p1 = now_ns();
        realloc_ns += p1 - p0;
        if (p == NULL) {
            free(src);
            free(dst);
            free(text);
            return fail("realloc memory_string_hot");
        }
        free(p);

        p0 = now_ns();
        memset(dst, (int)(i & 255u), bytes);
        p1 = now_ns();
        memset_ns += p1 - p0;

        p0 = now_ns();
        memcpy(dst, src, bytes);
        p1 = now_ns();
        memcpy_ns += p1 - p0;

        p0 = now_ns();
        memmove(dst + 17, dst, bytes - 17);
        p1 = now_ns();
        memmove_ns += p1 - p0;

        p0 = now_ns();
        checksum += strlen(text);
        p1 = now_ns();
        strlen_ns += p1 - p0;

        p0 = now_ns();
        checksum += (strcmp(text, text) == 0);
        p1 = now_ns();
        strcmp_ns += p1 - p0;

        p0 = now_ns();
        checksum += strchr(text, 'z') != NULL;
        p1 = now_ns();
        strchr_ns += p1 - p0;

        p0 = now_ns();
        checksum += strstr(text, "needle") != NULL;
        p1 = now_ns();
        strstr_ns += p1 - p0;
    }

    free(src);
    free(dst);
    free(text);
    metric_avg("mixed_memory_string_hot", now_ns() - t0, iterations);
    metric_avg("mixed_memory_string_hot.malloc_free", malloc_free_ns, iterations);
    metric_avg("mixed_memory_string_hot.calloc_free", calloc_free_ns, iterations);
    metric_avg("mixed_memory_string_hot.realloc", realloc_ns, iterations);
    metric_avg("mixed_memory_string_hot.memset4k", memset_ns, iterations);
    metric_avg("mixed_memory_string_hot.memcpy4k", memcpy_ns, iterations);
    metric_avg("mixed_memory_string_hot.memmove4k", memmove_ns, iterations);
    metric_avg("mixed_memory_string_hot.strlen4k", strlen_ns, iterations);
    metric_avg("mixed_memory_string_hot.strcmp_self", strcmp_ns, iterations);
    metric_avg("mixed_memory_string_hot.strchr", strchr_ns, iterations);
    metric_avg("mixed_memory_string_hot.strstr", strstr_ns, iterations);
    metric_total("mixed_memory_string_checksum", checksum);
    return 0;
}

static int bench_env_hot(void)
{
    const unsigned long long iterations = 512;
    unsigned long long getenv_ns = 0;
    unsigned long long setenv_ns = 0;
    unsigned long long unsetenv_ns = 0;
    unsigned long long checksum = 0;

    if (setenv("PACHAOS_LIBC_MIX_BASE", "base", 1) != 0) return fail("setenv env_hot setup");
    const unsigned long long t0 = now_ns();
    for (unsigned long long i = 0; i < iterations; ++i) {
        unsigned long long p0 = now_ns();
        const char *value = getenv("PACHAOS_LIBC_MIX_BASE");
        unsigned long long p1 = now_ns();
        getenv_ns += p1 - p0;
        if (value == NULL) return fail("getenv env_hot");
        checksum += (unsigned long long)value[0];

        char buf[48];
        int n = snprintf(buf, sizeof(buf), "value-%llu", i);
        if (n <= 0 || n >= (int)sizeof(buf)) return fail("snprintf env_hot");
        p0 = now_ns();
        if (setenv("PACHAOS_LIBC_MIX_DYNAMIC", buf, 1) != 0) return fail("setenv env_hot");
        p1 = now_ns();
        setenv_ns += p1 - p0;

        p0 = now_ns();
        if (unsetenv("PACHAOS_LIBC_MIX_DYNAMIC") != 0) return fail("unsetenv env_hot");
        p1 = now_ns();
        unsetenv_ns += p1 - p0;
    }

    metric_avg("mixed_env_hot", now_ns() - t0, iterations);
    metric_avg("mixed_env_hot.getenv", getenv_ns, iterations);
    metric_avg("mixed_env_hot.setenv", setenv_ns, iterations);
    metric_avg("mixed_env_hot.unsetenv", unsetenv_ns, iterations);
    metric_total("mixed_env_checksum", checksum);
    return 0;
}

int main(void)
{
    const char *path = "/tmp/libc_mix_bench.dat";
    const char *text_path = "/tmp/libc_mix_text.txt";
    const char *stdio_sink_path = "/tmp/libc_mix_stdio_sink.txt";
    const unsigned long long all0 = now_ns();

    stage("prepare");
    if (prepare_file(path) != 0) return 1;
    if (prepare_text_file(text_path) != 0) return 1;
    stage("path_metadata_hot");
    if (bench_path_metadata_hot(path) != 0) return 1;
    stage("openat_relative_hot");
    if (bench_openat_relative_hot("/tmp", "libc_mix_bench.dat") != 0) return 1;
    stage("open_stat_io");
    if (bench_open_stat_io(path) != 0) return 1;
    stage("stdio_loop");
    if (bench_stdio_loop(path) != 0) return 1;
    stage("stdio_hot");
    if (bench_stdio_hot(text_path, stdio_sink_path) != 0) return 1;
    stage("stdio_buffered_write_hot");
    if (bench_stdio_buffered_write_hot("/tmp/libc_mix_stdio_buffered_sink.txt") != 0) return 1;
    stage("vector_io");
    if (bench_vector_io(path) != 0) return 1;
    stage("dirent_loop");
    if (bench_dirent_loop() != 0) return 1;
    stage("alloc_sort_string");
    if (bench_alloc_sort_string() != 0) return 1;
    stage("memory_string_hot");
    if (bench_memory_string_hot() != 0) return 1;
    stage("env_hot");
    if (bench_env_hot() != 0) return 1;

    metric_avg("mixed_total", now_ns() - all0, 1);
    printf("[libc-mix-bench] ok\n");
    fflush(stdout);
    return 0;
}
