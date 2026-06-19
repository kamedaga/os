#include "filed/bootstrap.h"
#include "vfs_core.h"
#include "pacha/ipc.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static int find_bootstrap_fd(char **argv, int *out_fd)
{
    if (argv == NULL || out_fd == NULL) {
        return -1;
    }
    *out_fd = -1;
    char **p = argv;
    while (*p != NULL) {
        p++;
    }
    p++;
    while (*p != NULL) {
        p++;
    }
    p++;

    uint64_t bootstrap_fd = 0;
    const uint64_t *auxv = (const uint64_t *)(const void *)p;
    for (unsigned i = 0; i < 64; i++) {
        const uint64_t type = auxv[i * 2u];
        const uint64_t value = auxv[i * 2u + 1u];
        if (type == 0) {
            break;
        }
        if (type == PACHA_AT_BOOTSTRAP_FD) {
            bootstrap_fd = value;
        }
    }
    if (bootstrap_fd < 16) {
        return -2;
    }
    *out_fd = (int)bootstrap_fd;
    return 0;
}

static int read_bootstrap_fd(int fd, filed_bootstrap_t *out_bootstrap)
{
    if (fd < 16 || out_bootstrap == NULL) {
        return -1;
    }
    const long got = pacha_fd_read(fd, out_bootstrap, sizeof(*out_bootstrap));
    if (got != (long)sizeof(*out_bootstrap)) {
        fprintf(stderr,
            "[filed] bootstrap fd read failed fd=%d got=%ld size=%llu\n",
            fd,
            got,
            (unsigned long long)sizeof(*out_bootstrap));
        return -2;
    }
    return 0;
}

static int validate_bootstrap(const filed_bootstrap_t *bootstrap, uint64_t bootstrap_size)
{
    if (bootstrap == NULL || bootstrap_size < sizeof(*bootstrap)) {
        return -1;
    }
    if (bootstrap->magic != FILED_BOOTSTRAP_MAGIC ||
        bootstrap->fs_backend_fd < 16)
    {
        fprintf(stderr,
            "[filed] bootstrap invalid magic=0x%llx fs_fd=%llu\n",
            (unsigned long long)bootstrap->magic,
            (unsigned long long)bootstrap->fs_backend_fd);
        return -2;
    }
    return 0;
}

int main(int argc, char **argv)
{
    (void)argc;

    struct timespec ts = {0};
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);

    int bootstrap_fd = -1;
    filed_bootstrap_t bootstrap;
    int status = find_bootstrap_fd(argv, &bootstrap_fd);
    if (status == 0) {
        status = read_bootstrap_fd(bootstrap_fd, &bootstrap);
    }
    if (status == 0) {
        status = validate_bootstrap(&bootstrap, sizeof(bootstrap));
    }
    if (status != 0) {
        return 2;
    }
    filed_vfs_t *vfs = calloc(1, sizeof(*vfs));
    if (vfs == NULL) {
        fprintf(stderr, "[filed] vfs allocation failed\n");
        return 3;
    }
    filed_vfs_init(vfs);

    status = filed_vfs_attach_root_backend(vfs, (int)bootstrap.fs_backend_fd);
    if (status != 0) {
        fprintf(stderr, "[filed] root backend attach failed status=%d\n", status);
        free(vfs);
        return 4;
    }
    status = filed_vfs_self_check(vfs);
    if (status != 0) {
        fprintf(stderr, "[filed] vfs self-check failed status=%d\n", status);
        free(vfs);
        return 5;
    }

    printf("[filed] ready\n");
    fflush(stdout);
    fflush(stderr);
    return 0;
}
