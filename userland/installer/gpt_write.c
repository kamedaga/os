/* SPDX-License-Identifier: MIT */
#include "gpt_write.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

enum { GPT_ENTRIES = 128, GPT_ENTRY_BYTES = 128, GPT_HEADER_BYTES = 92 };

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

static int valid_guid(const uint8_t guid[16]) {
    for (unsigned i = 0; i < 16; ++i) if (guid[i]) return 1;
    return 0;
}

static void make_entry(uint8_t *entry, const uint8_t type[16],
    const uint8_t unique[16], struct installer_extent extent,
    const char *name) {
    memcpy(entry, type, 16);
    memcpy(entry + 16, unique, 16);
    put64(entry + 32, extent.first_lba);
    put64(entry + 40, extent.last_lba);
    for (size_t i = 0; name[i] && i < 36; ++i)
        entry[56 + 2 * i] = (uint8_t)name[i];
}

static void make_header(uint8_t *header, uint32_t logical_block,
    uint64_t current_lba, uint64_t backup_lba, uint64_t first_usable,
    uint64_t last_usable, uint64_t table_lba,
    const uint8_t disk_guid[16], uint32_t table_crc) {
    memset(header, 0, logical_block);
    memcpy(header, "EFI PART", 8);
    put32(header + 8, 0x10000);
    put32(header + 12, GPT_HEADER_BYTES);
    put64(header + 24, current_lba);
    put64(header + 32, backup_lba);
    put64(header + 40, first_usable);
    put64(header + 48, last_usable);
    memcpy(header + 56, disk_guid, 16);
    put64(header + 72, table_lba);
    put32(header + 80, GPT_ENTRIES);
    put32(header + 84, GPT_ENTRY_BYTES);
    put32(header + 88, table_crc);
    put32(header + 16, crc32_bytes(header, GPT_HEADER_BYTES));
}

int installer_write_guided_gpt(const struct installer_plan *plan,
    uint64_t diskseq, uint64_t disk_bytes, uint32_t logical_block,
    int blank_verified, const struct installer_new_guids *new_guids,
    installer_read_at read_at, installer_write_at write_at,
    installer_flush flush, void *context,
    installer_gpt_progress progress, void *progress_context) {
    if (!plan || !new_guids || !read_at || !write_at || !flush ||
        plan->kind != INSTALLER_LAYOUT_GUIDED ||
        plan->esp_filesystem != INSTALLER_FS_FAT32 ||
        plan->root_filesystem != INSTALLER_FS_EXT4 ||
        !plan->format_esp || !plan->format_root ||
        !valid_guid(new_guids->disk) || !valid_guid(new_guids->esp) ||
        !valid_guid(new_guids->root) ||
        !memcmp(new_guids->disk, new_guids->esp, 16) ||
        !memcmp(new_guids->disk, new_guids->root, 16) ||
        !memcmp(new_guids->esp, new_guids->root, 16)) return -EINVAL;

    struct installer_gpt current = {0};
    int old_status = installer_read_gpt(read_at, context, disk_bytes,
        logical_block, &current);
    if (old_status && (old_status != -EINVAL || !blank_verified))
        return old_status;
    if (installer_plan_revalidate(plan, diskseq, disk_bytes, logical_block,
            old_status ? NULL : &current)) return -ESTALE;
    struct installer_plan expected = {0};
    int status = installer_guided_plan(diskseq, disk_bytes, logical_block,
        plan->target_guid, &expected);
    if (status) return status;
    if (memcmp(&expected.esp, &plan->esp, sizeof(plan->esp)) ||
        memcmp(&expected.root, &plan->root, sizeof(plan->root)) ||
        memcmp(&expected.erase, &plan->erase, sizeof(plan->erase)))
        return -EINVAL;

    const uint64_t sectors = disk_bytes / logical_block;
    const uint64_t table_sectors = (GPT_ENTRIES * GPT_ENTRY_BYTES +
        logical_block - 1) / logical_block;
    const uint64_t first_usable = 2 + table_sectors;
    const uint64_t last_usable = sectors - table_sectors - 2;
    const size_t table_bytes = (size_t)table_sectors * logical_block;
    uint8_t *table = calloc(1, table_bytes);
    uint8_t *header = malloc(logical_block);
    uint8_t *verify = malloc(table_bytes > logical_block ?
        table_bytes : logical_block);
    if (!table || !header || !verify) {
        free(table); free(header); free(verify); return -ENOMEM;
    }
    make_entry(table, installer_esp_type_guid, new_guids->esp,
        plan->esp, "PachaOS ESP");
    make_entry(table + GPT_ENTRY_BYTES, installer_linux_fs_type_guid,
        new_guids->root, plan->root, "PachaOS root");
    uint32_t table_crc = crc32_bytes(table, GPT_ENTRIES * GPT_ENTRY_BYTES);
    const uint64_t backup_lba = sectors - 1;
    const uint64_t backup_table_lba = backup_lba - table_sectors;
    if (progress) progress(progress_context, 27,
        "Writing the backup GPT table (16 KiB)");
    status = write_at(context, backup_table_lba * logical_block,
        table, table_bytes);
    if (!status) {
        if (progress) progress(progress_context, 27,
            "Writing the backup GPT header");
        make_header(header, logical_block, backup_lba, 1, first_usable,
            last_usable, backup_table_lba, new_guids->disk, table_crc);
        status = write_at(context, backup_lba * logical_block,
            header, logical_block);
    }
    if (!status) {
        if (progress) progress(progress_context, 28,
            "Flushing the backup GPT");
        status = flush(context);
    }
    if (!status) {
        if (progress) progress(progress_context, 28,
            "Writing the primary GPT table (16 KiB)");
        status = write_at(context, 2 * (uint64_t)logical_block,
            table, table_bytes);
    }
    if (!status) {
        if (progress) progress(progress_context, 28,
            "Writing the primary GPT header and protective MBR");
        make_header(header, logical_block, 1, backup_lba, first_usable,
            last_usable, 2, new_guids->disk, table_crc);
        status = write_at(context, logical_block, header, logical_block);
    }
    if (!status) {
        memset(header, 0, logical_block);
        header[0x1be + 4] = 0xee;
        put32(header + 0x1be + 8, 1);
        put32(header + 0x1be + 12, sectors - 1 > UINT32_MAX ?
            UINT32_MAX : (uint32_t)(sectors - 1));
        header[510] = 0x55;
        header[511] = 0xaa;
        status = write_at(context, 0, header, logical_block);
    }
    if (!status) {
        if (progress) progress(progress_context, 29,
            "Flushing the primary GPT");
        status = flush(context);
    }
    if (!status) {
        /* installer_read_gpt validates the primary copy.  Verify the backup
         * independently as well so a high-LBA transport/write bug can never
         * produce a superficially successful but unrecoverable install. */
        if (progress) progress(progress_context, 29,
            "Reading back the backup GPT");
        status = read_at(context, backup_table_lba * logical_block,
            verify, table_bytes);
        if (!status && memcmp(verify, table, table_bytes))
            status = -EBADMSG;
    }
    if (!status) {
        make_header(header, logical_block, backup_lba, 1, first_usable,
            last_usable, backup_table_lba, new_guids->disk, table_crc);
        status = read_at(context, backup_lba * logical_block,
            verify, logical_block);
        if (!status && memcmp(verify, header, logical_block))
            status = -EBADMSG;
    }
    if (!status) {
        if (progress) progress(progress_context, 29,
            "Reading back the primary GPT and partition layout");
        struct installer_gpt verified = {0};
        status = installer_read_gpt(read_at, context, disk_bytes,
            logical_block, &verified);
        if (!status && (verified.install_media ||
            !installer_guid_equal(verified.disk_guid, new_guids->disk) ||
            verified.partition_count != 2 ||
            !installer_guid_equal(verified.partitions[0].type_guid,
                installer_esp_type_guid) ||
            !installer_guid_equal(verified.partitions[1].type_guid,
                installer_linux_fs_type_guid) ||
            !installer_guid_equal(verified.partitions[0].unique_guid, new_guids->esp) ||
            !installer_guid_equal(verified.partitions[1].unique_guid, new_guids->root) ||
            verified.partitions[0].first_lba != plan->esp.first_lba ||
            verified.partitions[0].last_lba != plan->esp.last_lba ||
            verified.partitions[1].first_lba != plan->root.first_lba ||
            verified.partitions[1].last_lba != plan->root.last_lba)) status = -EBADMSG;
        if (!status) {
            status = read_at(context, 0, header, logical_block);
            if (!status && (header[510] != 0x55 || header[511] != 0xaa ||
                header[0x1be + 4] != 0xee)) status = -EBADMSG;
        }
    }
    free(table);
    free(header);
    free(verify);
    return status;
}
