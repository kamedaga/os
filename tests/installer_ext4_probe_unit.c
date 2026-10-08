/* SPDX-License-Identifier: MIT */
#include "../userland/installer/ext4_probe.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint8_t disk[128 * 1024];

static int read_at(void *context, uint64_t offset, void *buffer, size_t bytes) {
    (void)context;
    if (offset > sizeof(disk) || bytes > sizeof(disk) - offset) return -EIO;
    memcpy(buffer, disk + offset, bytes);
    return 0;
}

int main(void) {
    for (uint32_t block = 512; block <= 4096; block *= 8) {
        memset(disk, 0, sizeof(disk));
        uint64_t first = 2, last = sizeof(disk) / block - 1;
        uint8_t *sb = disk + first * block + 1024;
        sb[56] = 0x53; sb[57] = 0xef;
        sb[96] = 0x40; /* extents */
        sb[4] = 10;    /* nonzero block count */
        assert(installer_probe_ext4(read_at, NULL, sizeof(disk), block,
            first, last) == 0);
        sb[96] = 0;
        assert(installer_probe_ext4(read_at, NULL, sizeof(disk), block,
            first, last) == -EINVAL);
        sb[96] = 0x40; sb[56] = 0;
        assert(installer_probe_ext4(read_at, NULL, sizeof(disk), block,
            first, last) == -EINVAL);
    }
    puts("ext4 reuse probe passed");
    return 0;
}
