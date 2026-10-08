/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "../userland/installer/menu.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

struct execution { int calls; size_t target; struct installer_plan plan; };

struct disk_read_context {
    uint64_t esp_offset, root_offset;
    int fat32, ext4;
};

static void put16(uint8_t *p, uint16_t n) {
    p[0] = (uint8_t)n; p[1] = (uint8_t)(n >> 8);
}

static void put32(uint8_t *p, uint32_t n) {
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(n >> (8 * i));
}

static int read_disk(void *context, uint64_t offset, void *buffer, size_t bytes) {
    struct disk_read_context *disk = context;
    if (bytes != 4096) return -EIO;
    memset(buffer, 0, bytes);
    uint8_t *p = buffer;
    if (offset == disk->esp_offset ||
        offset == disk->esp_offset + 6 * 4096) {
        put16(p + 11, 4096);
        p[13] = 1; put16(p + 14, 32); p[16] = 2;
        put32(p + 32, 131072); put32(p + 36, 1024);
        put32(p + 44, 2); put16(p + 48, 1); put16(p + 50, 6);
        memcpy(p + 82, "FAT32", 5);
        p[510] = 0x55; p[511] = 0xaa;
        if (!disk->fat32 && offset == disk->esp_offset + 6 * 4096)
            p[12] = 0;
        return 0;
    }
    if (offset == disk->esp_offset + 4096) {
        put32(p, 0x41615252); put32(p + 484, 0x61417272);
        p[510] = 0x55; p[511] = 0xaa;
        return 0;
    }
    if (offset != disk->root_offset) return -EIO;
    uint8_t *sb = p + 1024;
    sb[56] = 0x53; sb[57] = 0xef;
    sb[4] = 10;
    sb[96] = disk->ext4 ? 0x40 : 0;
    return 0;
}

static int execute(void *context, size_t target,
    const struct installer_plan *plan, installer_progress progress,
    void *progress_context) {
    struct execution *record = context;
    record->calls++;
    record->target = target;
    record->plan = *plan;
    if (progress) {
        progress(progress_context, 1, "Checking");
        progress(progress_context, 50, "Copying");
        progress(progress_context, 100, "Complete");
    }
    return 0;
}

struct key_input { const unsigned char *cursor; };

static int read_key(void *context, uint64_t timeout_ns) {
    (void)timeout_ns;
    struct key_input *input = context;
    if (*input->cursor == 1) {
        ++input->cursor;
        return -ETIMEDOUT;
    }
    if (!*input->cursor) return -EIO;
    return *input->cursor++;
}

static int run(const char *input_text, const struct installer_disk *disks,
    size_t count, size_t media, struct execution *record,
    char *output, size_t output_bytes) {
    struct key_input input = {.cursor = (const unsigned char *)input_text};
    FILE *out = tmpfile();
    assert(out);
    int result = installer_ui_run(read_key, &input, out,
        disks, count, media, 24, 80, execute, record);
    rewind(out);
    size_t n = fread(output, 1, output_bytes - 1, out);
    output[n] = 0;
    fclose(out);
    return result;
}

int main(void) {
    struct installer_gpt boot = {.install_media = 1};
    boot.disk_guid[0] = 1;
    struct installer_disk disks[2] = {
        {.name = "sda", .model = "Install USB", .resource_id = 11,
         .generation = 1, .bytes = UINT64_C(16)<<30,
         .diskseq = 10, .major = 8, .minor = 0,
         .logical_block = 512, .gpt = &boot},
        {.name = "nvme0n1", .model = "Test SSD", .resource_id = 12,
         .generation = 2, .bytes = UINT64_C(8)<<30,
         .diskseq = 77, .major = 259, .minor = 0,
         .logical_block = 4096, .blank_verified = 1},
    };
    struct execution record = {0};
    /* Full-screen ANSI redraws intentionally overwrite prior screens on a
     * terminal, but the memory-backed test stream retains every redraw. */
    char output[32768];
    assert(run("\033[C\r", disks, 2, 0, &record, output,
        sizeof(output)) == INSTALLER_UI_SHELL);
    assert(strstr(output, "[<Install>]") && strstr(output, "<Shell>") &&
        !record.calls);
    /* Esc from disk selection returns to the welcome page; the following
     * right arrow selects Shell. Byte 1 is the scripted escape timeout. */
    assert(run("i\033\001\033[C\r", disks, 2, 0, &record, output,
        sizeof(output)) == INSTALLER_UI_SHELL);
    assert(!record.calls);
    assert(run("i\r\rWRONG\r", disks, 2, 0, &record, output,
        sizeof(output)) == -EIO);
    assert(strstr(output, "ERASE ENTIRE DISK") &&
        strstr(output, "Test SSD") && !record.calls);
    assert(run("i\r\rERASE 1\r", disks, 2, 0,
        &record, output, sizeof(output)) == INSTALLER_UI_DONE);
    assert(strstr(output, "Confirmation accepted") &&
        strstr(output, "Copying") && strstr(output, "100%"));
    assert(record.calls == 1 && record.target == 1 &&
        record.plan.kind == INSTALLER_LAYOUT_GUIDED &&
        record.plan.root_filesystem == INSTALLER_FS_EXT4 &&
        record.plan.logical_block == 4096);
    struct installer_gpt existing = {.first_usable_lba = 34,
        .last_usable_lba = (UINT64_C(8) << 30) / 4096 - 34,
        .partition_count = 2};
    existing.disk_guid[0] = 8;
    const uint8_t esp_type[16] = {0x28,0x73,0x2a,0xc1,0x1f,0xf8,0xd2,0x11,
        0xba,0x4b,0x00,0xa0,0xc9,0x3e,0xc9,0x3b};
    const uint8_t root_type[16] = {0xaf,0x3d,0xc6,0x0f,0x83,0x84,0x72,0x47,
        0x8e,0x79,0x3d,0x69,0xd8,0x47,0x7d,0xe4};
    memcpy(existing.partitions[0].type_guid, esp_type, 16);
    memcpy(existing.partitions[1].type_guid, root_type, 16);
    existing.partitions[0].unique_guid[0] = 1;
    existing.partitions[1].unique_guid[0] = 2;
    existing.partitions[0].first_lba = 256;
    existing.partitions[0].last_lba = 131327;
    existing.partitions[1].first_lba = 131328;
    existing.partitions[1].last_lba = 500000;
    disks[1].gpt = &existing;
    struct disk_read_context root = {
        .esp_offset = existing.partitions[0].first_lba * UINT64_C(4096),
        .root_offset = existing.partitions[1].first_lba * UINT64_C(4096),
        .fat32 = 1, .ext4 = 0,
    };
    disks[1].read_at = read_disk;
    disks[1].read_context = &root;
    assert(run("i\rm\r\r\rERASE 1\r", disks, 2, 0,
        &record, output, sizeof(output)) == INSTALLER_UI_DONE);
    assert(record.calls == 2 && record.plan.kind == INSTALLER_LAYOUT_MANUAL &&
        !record.plan.format_esp && record.plan.format_root &&
        record.plan.root_filesystem == INSTALLER_FS_EXT4);
    assert(installer_plan_revalidate(&record.plan, 77,
        UINT64_C(8)<<30, 4096, &existing) == 0);
    root.ext4 = 1;
    root.fat32 = 0;
    assert(run("i\rm\r\r\rERASE 1\r", disks, 2, 0,
        &record, output, sizeof(output)) == -EINVAL);
    assert(record.calls == 2); /* Damaged backup BPB blocks reuse too. */
    root.fat32 = 1;
    assert(run("i\rm\r\r\rERASE 1\r", disks, 2, 0,
        &record, output, sizeof(output)) == INSTALLER_UI_DONE);
    assert(record.calls == 3 && record.plan.format_root &&
        record.plan.root_filesystem == INSTALLER_FS_EXT4);
    existing.partitions[1].unique_guid[0] = 4;
    assert(installer_plan_revalidate(&record.plan, 77,
        UINT64_C(8)<<30, 4096, &existing) == -ESTALE);
    disks[1].gpt = &boot;
    assert(run("i\r\rERASE 1\r", disks, 2, 0,
        &record, output, sizeof(output)) == -EEXIST);
    assert(record.calls == 3);
    puts("installer menu safety checks passed");
    return 0;
}
