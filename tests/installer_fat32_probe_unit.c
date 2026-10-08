/* SPDX-License-Identifier: MIT */
#define _XOPEN_SOURCE 700
#include "../userland/installer/fat32_probe.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static int read_at(void *context, uint64_t offset, void *buffer, size_t bytes) {
    int fd = *(int *)context;
    if (offset > INT64_MAX || bytes > INT64_MAX - offset) return -EINVAL;
    ssize_t got = pread(fd, buffer, bytes, (off_t)offset);
    return got == (ssize_t)bytes ? 0 : got < 0 ? -errno : -EIO;
}

int main(int argc, char **argv) {
    if (argc != 5 && argc != 6) return 2;
    const uint32_t block = (uint32_t)strtoul(argv[2], NULL, 10);
    const uint64_t first = strtoull(argv[3], NULL, 10);
    const uint64_t sectors = strtoull(argv[4], NULL, 10);
    if (!block || !sectors) return 2;
    int fd = open(argv[1], O_RDONLY);
    assert(fd >= 0);
    struct stat st;
    assert(fstat(fd, &st) == 0);
    int expected = argc == 6 ? atoi(argv[5]) : 0;
    assert(installer_probe_fat32(read_at, &fd, (uint64_t)st.st_size, block,
        first, first + sectors - 1) == expected);
    assert(installer_probe_fat32(read_at, &fd, (uint64_t)st.st_size, block,
        first, first + 4096 - 1) == -EINVAL);
    assert(close(fd) == 0);
    puts("FAT32 probe returned the expected result on mkfs.fat image");
    return 0;
}
