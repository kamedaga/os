/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "../userland/installer/gpt.h"
#include "../userland/installer/plan.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

enum { SECTOR = 512, SECTORS = 4096 };
static uint8_t image[SECTOR * SECTORS];

static void put32(uint8_t *p, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (i * 8));
}

static void put64(uint8_t *p, uint64_t value) {
    put32(p, (uint32_t)value);
    put32(p + 4, (uint32_t)(value >> 32));
}

static uint32_t crc32_bytes(const uint8_t *p, size_t length) {
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < length; ++i) {
        crc ^= p[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = crc >> 1 ^ (UINT32_C(0xedb88320) & -(crc & 1u));
    }
    return ~crc;
}

static void finish_crc(void) {
    uint8_t *header = image + SECTOR;
    uint8_t *backup = image + (SECTORS - 1) * SECTOR;
    put32(header + 88, crc32_bytes(image + 2 * SECTOR, 128 * 128));
    put32(header + 16, 0);
    put32(header + 16, crc32_bytes(header, 92));
    memcpy(backup, header, SECTOR);
    put64(backup + 24, SECTORS - 1);
    put64(backup + 32, 1);
    put64(backup + 72, SECTORS - 33);
    put32(backup + 16, 0);
    put32(backup + 16, crc32_bytes(backup, 92));
    memcpy(image + (SECTORS - 33) * SECTOR,
        image + 2 * SECTOR, 128 * 128);
}

static int read_at(void *context, uint64_t offset, void *buffer, size_t bytes) {
    (void)context;
    if (offset > sizeof(image) || bytes > sizeof(image) - offset) return -EIO;
    memcpy(buffer, image + offset, bytes);
    return 0;
}

static int read_file(void *context, uint64_t offset, void *buffer, size_t bytes) {
    int fd = *(int *)context;
    if (offset > INT64_MAX || bytes > INT64_MAX - offset) return -EINVAL;
    return pread(fd, buffer, bytes, (off_t)offset) == (ssize_t)bytes ? 0 : -EIO;
}

int main(int argc, char **argv) {
    if (argc == 2) {
        int fd = open(argv[1], O_RDONLY);
        assert(fd >= 0);
        struct stat statbuf;
        assert(fstat(fd, &statbuf) == 0 && statbuf.st_size > 0);
        struct installer_gpt disk = {0};
        assert(installer_read_gpt(read_file, &fd, statbuf.st_size, 512, &disk) == 0);
        assert(disk.install_media && disk.partition_count == 3);
        assert(close(fd) == 0);
        puts("installer image GPT marker verified");
        return 0;
    }
    assert(argc == 1);
    uint8_t *header = image + SECTOR;
    uint8_t *entry = image + 2 * SECTOR;
    memcpy(header, "EFI PART", 8);
    put32(header + 8, 0x10000);
    put32(header + 12, 92);
    put64(header + 24, 1);
    put64(header + 32, SECTORS - 1);
    put64(header + 40, 34);
    put64(header + 48, SECTORS - 34);
    header[56] = 1;
    put64(header + 72, 2);
    put32(header + 80, 128);
    put32(header + 84, 128);
    entry[0] = 2;
    entry[16] = 3;
    put64(entry + 32, 40);
    put64(entry + 40, 100);
    const char *marker = "pacha-install-data";
    for (size_t i = 0; marker[i]; ++i) entry[56 + i * 2] = marker[i];
    finish_crc();

    struct installer_gpt tables[2] = {0};
    size_t index = 99;
    assert(installer_read_gpt(read_at, NULL, sizeof(image), SECTOR, &tables[0]) == 0);
    assert(installer_read_gpt(read_at, NULL, sizeof(image) + SECTOR * 32,
        SECTOR, &tables[0]) == 0); /* image copied to larger USB */
    assert(tables[0].install_media == 1 && tables[0].partition_count == 1);
    assert(installer_find_media(tables, 2, &index) == 0 && index == 0);
    struct installer_plan plan = {0};
    assert(installer_guided_plan(7, UINT64_C(8) << 30, 512,
        tables[0].disk_guid, &plan) == 0);
    assert(plan.esp.first_lba == 2048 && plan.esp.last_lba == 1050623);
    assert(plan.root.first_lba == 1050624 && plan.root.last_lba < plan.erase.last_lba);
    assert(plan.esp_filesystem == INSTALLER_FS_FAT32 &&
        plan.root_filesystem == INSTALLER_FS_EXT4);
    assert(installer_plan_revalidate(&plan, 7, UINT64_C(8) << 30,
        512, &tables[0]) == -ESTALE); /* install media is never a target */
    tables[0].install_media = 0;
    assert(installer_plan_revalidate(&plan, 7, UINT64_C(8) << 30,
        512, &tables[0]) == 0);
    plan.root_filesystem = INSTALLER_FS_FAT32;
    assert(installer_plan_revalidate(&plan, 7, UINT64_C(8) << 30,
        512, &tables[0]) == -ESTALE);
    plan.root_filesystem = INSTALLER_FS_EXT4;
    assert(installer_plan_revalidate(&plan, 8, UINT64_C(8) << 30,
        512, &tables[0]) == -ESTALE);
    assert(installer_guided_plan(7, UINT64_C(1) << 30, 512,
        tables[0].disk_guid, &plan) == -EINVAL);
    assert(installer_guided_plan(7, UINT64_C(8) << 30, 4096,
        tables[0].disk_guid, &plan) == 0);
    assert(plan.esp.first_lba == 256 && plan.root.first_lba == 131328);
    uint8_t blank_guid[16] = {0};
    assert(installer_guided_plan(7, UINT64_C(8) << 30, 512,
        blank_guid, &plan) == 0);
    assert(installer_plan_revalidate(&plan, 7, UINT64_C(8) << 30,
        512, NULL) == 0);
    tables[0].install_media = 1;
    tables[1].install_media = 1;
    assert(installer_find_media(tables, 2, &index) == -EEXIST);
    tables[0].install_media = tables[1].install_media = 0;
    assert(installer_find_media(tables, 2, &index) == -ENODEV);
    image[2 * SECTOR + 56] ^= 1;
    assert(installer_read_gpt(read_at, NULL, sizeof(image), SECTOR, &tables[0]) == -EBADMSG);
    assert(installer_verify_blank(read_at, NULL, sizeof(image), SECTOR) == -EEXIST);
    memset(image, 0, sizeof(image));
    assert(installer_verify_blank(read_at, NULL, sizeof(image), SECTOR) == 0);
    image[sizeof(image) - SECTOR] = 1;
    assert(installer_verify_blank(read_at, NULL, sizeof(image), SECTOR) == -EEXIST);
    puts("installer GPT safety checks passed");
    return 0;
}
