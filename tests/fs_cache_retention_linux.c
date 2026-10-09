/* SPDX-License-Identifier: GPL-2.0-only */
/* Dedicated VM diagnostic: vary journal checkpoint preparation, never the
 * timed operations. Reuse the baseline's exact fixture and FS operations. */
#define main cache_matrix_baseline_main
#include "fs_cache_matrix_linux.c"
#undef main
#include <linux/fs.h>
#include <sys/ioctl.h>

struct retention_sample {
    struct native_counts io;
    uint64_t wall;
    unsigned int trial, variant, phase, sample;
};
static struct retention_sample retention_samples[192];

static int retention_marker(unsigned variant, unsigned phase, unsigned trial,
    unsigned sample, const char *stage)
{
    if (!getenv("FS_RETENTION_TRACE")) return 0;
    char line[160];
    int length = snprintf(line, sizeof(line),
        "RETENTION variant=%u phase=%u trial=%u sample=%u stage=%s\n",
        variant, phase, trial, sample, stage);
    return write(4, line, length) == length ? 0 : -EIO;
}

static int retention_prepare(struct native_backend *b, int device, unsigned variant)
{
    if (syncfs(b->sync_fd)) return -errno;
    if (variant == 1 && syncfs(b->sync_fd)) return -errno;
    if (variant == 2) {
        /* Freeze flushes the journal's checkpoint state, unlike waiting only
         * for a commit. Always thaw before any measured mutation or error. */
        if (ioctl(b->sync_fd, FIFREEZE, 0)) return -errno;
        if (ioctl(b->sync_fd, FITHAW, 0)) return -errno;
    }
    if (variant == 3 && ioctl(device, BLKFLSBUF, 0)) return -errno;
    return native_cold(b);
}

int main(int argc, char **argv)
{
    struct stat device_stat, root_stat;
    /* FD 3 is a read-only block device supplied before chroot by the VM
     * fixture. Never infer a host device from a default path. */
    if (argc != 2 || strcmp(argv[1], "--guest-retention") ||
        access("/run/kobox-cache-matrix-guest", F_OK) ||
        fstat(3, &device_stat) || !S_ISBLK(device_stat.st_mode) ||
        stat("/", &root_stat) || root_stat.st_dev != device_stat.st_rdev) return 2;
    struct native_backend b = {.fs = "ext4", .sync_fd = open("/", O_RDONLY | O_DIRECTORY),
        .stat_fd = open("/proc/diskstats", O_RDONLY), .buffer = malloc(1048576)};
    struct rlimit limit = {RLIM_INFINITY, RLIM_INFINITY};
    if (b.sync_fd < 0 || b.stat_fd < 0 || !b.buffer || setrlimit(RLIMIT_MEMLOCK, &limit) ||
        mlockall(MCL_CURRENT)) return 1;
    memset(b.buffer, 0x5a, 1048576);
    struct matrix m = {.context = &b, .call = native_call, .cold = native_cold,
        .clock = native_clock, .report = native_report};
    unsigned int used = 0;
    int error = 0;
    for (unsigned int trial = 1; !error && trial <= 2; trial++) {
        for (unsigned int order = 0; !error && order < 4; order++) {
            unsigned int variant = trial == 1 ? order : 3 - order;
            error = matrix_setup(&m, "");
            if (error) break;
            for (unsigned int p = 0; !error && p < 2; p++) {
                unsigned int phase = p ? 12 : 7;
                for (unsigned int sample = 0; !error && sample < 12; sample++) {
                    error = retention_marker(variant, phase, trial, sample, "prepare");
                    if (!error) error = retention_prepare(&b, 3, variant);
                    if (!error) error = retention_marker(variant, phase, trial, sample, "execute");
                    if (error) break;
                    struct retention_sample *r = &retention_samples[used++];
                    *r = (struct retention_sample){.trial = trial, .variant = variant,
                        .phase = phase, .sample = sample};
                    struct native_counts before = native_disk_counts(&b);
                    uint64_t begin = native_clock(NULL);
                    error = matrix_operation(&m, phase);
                    r->wall = native_clock(NULL) - begin;
                    struct native_counts after = native_disk_counts(&b);
                    r->io = (struct native_counts){after.reads - before.reads,
                        after.read_sectors - before.read_sectors, after.writes - before.writes,
                        after.write_sectors - before.write_sectors};
                    if (!error) error = retention_marker(variant, phase, trial, sample, "complete");
                }
            }
            if (!error) error = matrix_cleanup(&m);
        }
    }
    static const char * const variants[] = {"drop", "extra_sync", "freeze", "blkflush"};
    for (unsigned int i = 0; i < used; i++) {
        struct retention_sample *r = &retention_samples[i];
        printf("FS_RETENTION variant=%s phase=%s trial=%u sample=%u wall_ns=%" PRIu64
            " reads=%" PRIu64 " read_sectors=%" PRIu64
            " writes=%" PRIu64 "\n", variants[r->variant], matrix_phases[r->phase], r->trial,
            r->sample, r->wall,
            r->io.reads, r->io.read_sectors, r->io.writes);
    }
    if (b.sync_fd >= 0) close(b.sync_fd);
    if (b.stat_fd >= 0) close(b.stat_fd);
    free(b.buffer);
    printf("FS_RETENTION_DONE status=%d samples=%u\n", error, used);
    return !!error;
}
