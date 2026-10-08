/* SPDX-License-Identifier: MIT */
#include "gpt.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

enum { GPT_HEADER_MIN = 92, GPT_ENTRY_MIN = 128,
    GPT_TABLE_MAX_BYTES = 1024 * 1024 };

const uint8_t installer_esp_type_guid[16] = {
    0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
    0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b};
const uint8_t installer_linux_fs_type_guid[16] = {
    0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47,
    0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4};

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
        (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t le64(const uint8_t *p) {
    return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32;
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

static int all_zero(const uint8_t *p, size_t length) {
    for (size_t i = 0; i < length; ++i) if (p[i]) return 0;
    return 1;
}

int installer_verify_blank(installer_read_at read_at, void *context,
    uint64_t disk_bytes, uint32_t logical_block) {
    enum { PROBE_BYTES = 1024 * 1024 };
    if (!read_at || logical_block < 512 || logical_block > 4096 ||
        (logical_block & (logical_block - 1)) ||
        disk_bytes < 2 * (uint64_t)PROBE_BYTES ||
        disk_bytes % logical_block) return -EINVAL;
    uint8_t *buffer = malloc(PROBE_BYTES);
    if (!buffer) return -ENOMEM;
    int status = 0;
    const uint64_t positions[2] = {0, disk_bytes - PROBE_BYTES};
    for (unsigned region = 0; region < 2 && !status; ++region) {
        for (size_t at = 0; at < PROBE_BYTES; at += 65536) {
            status = read_at(context, positions[region] + at,
                buffer, 65536);
            if (!status && !all_zero(buffer, 65536)) status = -EEXIST;
            if (status) break;
        }
    }
    free(buffer);
    return status;
}

static int partition_name(const uint8_t *entry, size_t entry_bytes,
    char out[73]) {
    size_t length = 0;
    for (size_t i = 56; i + 1 < entry_bytes && length < 72; i += 2) {
        uint16_t value = (uint16_t)entry[i] | (uint16_t)entry[i + 1] << 8;
        if (!value) break;
        /* Non-ASCII names remain visible as '?'; the media marker is ASCII
         * and therefore cannot be matched through a lossy conversion. */
        out[length++] = value >= 32 && value <= 126 ? (char)value : '?';
    }
    out[length] = 0;
    return 0;
}

int installer_read_gpt(installer_read_at read_at, void *context,
    uint64_t disk_bytes, uint32_t logical_block, struct installer_gpt *out) {
    if (!read_at || !out || logical_block < 512 || logical_block > 4096 ||
        (logical_block & (logical_block - 1)) ||
        disk_bytes < (uint64_t)logical_block * 67 ||
        disk_bytes % logical_block) return -EINVAL;
    uint8_t *header = malloc(logical_block);
    if (!header) return -ENOMEM;
    int result = read_at(context, logical_block, header, logical_block);
    if (result) goto done;
    uint64_t sectors = disk_bytes / logical_block;
    uint32_t header_bytes = le32(header + 12);
    uint64_t backup_lba = le64(header + 32);
    uint64_t first = le64(header + 40), last = le64(header + 48);
    if (memcmp(header, "EFI PART", 8) || le32(header + 8) != 0x10000 ||
        header_bytes < GPT_HEADER_MIN || header_bytes > logical_block ||
        le64(header + 24) != 1 || !backup_lba || backup_lba >= sectors ||
        !first || first > last || last >= backup_lba) {
        result = -EINVAL; goto done;
    }
    uint32_t expected_header_crc = le32(header + 16);
    memset(header + 16, 0, 4);
    if (crc32_bytes(header, header_bytes) != expected_header_crc) {
        result = -EBADMSG; goto done;
    }
    /* A raw installer image may occupy only the beginning of a larger USB
     * stick. Its backup GPT stays at the end of the image until expanded.
     * Require that backup to be present and internally consistent instead
     * of incorrectly treating the remaining capacity as safe to erase. */
    uint8_t *backup = malloc(logical_block);
    if (!backup) { result = -ENOMEM; goto done; }
    result = read_at(context, backup_lba * logical_block, backup, logical_block);
    if (result) { free(backup); goto done; }
    uint32_t backup_bytes = le32(backup + 12);
    uint32_t backup_crc = le32(backup + 16);
    uint64_t backup_table_lba = le64(backup + 72);
    memset(backup + 16, 0, 4);
    if (memcmp(backup, "EFI PART", 8) || le32(backup + 8) != 0x10000 ||
        backup_bytes < GPT_HEADER_MIN || backup_bytes > logical_block ||
        le64(backup + 24) != backup_lba || le64(backup + 32) != 1 ||
        le64(backup + 40) != first || le64(backup + 48) != last ||
        memcmp(backup + 56, header + 56, 16) ||
        le32(backup + 80) != le32(header + 80) ||
        le32(backup + 84) != le32(header + 84) ||
        le32(backup + 88) != le32(header + 88) ||
        crc32_bytes(backup, backup_bytes) != backup_crc) {
        free(backup);
        result = -EBADMSG; goto done;
    }
    free(backup);
    uint64_t table_lba = le64(header + 72);
    uint32_t count = le32(header + 80), entry_bytes = le32(header + 84);
    uint64_t table_bytes = (uint64_t)count * entry_bytes;
    if (!first || first > last || last >= sectors - 1 ||
        !count || entry_bytes < GPT_ENTRY_MIN || entry_bytes > 1024 ||
        entry_bytes % 8 || !table_bytes ||
        table_bytes > GPT_TABLE_MAX_BYTES || !table_lba ||
        table_lba >= sectors ||
        (table_bytes + logical_block - 1) / logical_block > sectors - table_lba) {
        result = -EINVAL; goto done;
    }
    size_t aligned = (size_t)((table_bytes + logical_block - 1) &
        ~(uint64_t)(logical_block - 1));
    uint8_t *entries = malloc(aligned);
    if (!entries) { result = -ENOMEM; goto done; }
    result = read_at(context, table_lba * logical_block, entries, aligned);
    if (result) goto entries_done;
    if (crc32_bytes(entries, (size_t)table_bytes) != le32(header + 88)) {
        result = -EBADMSG; goto entries_done;
    }
    if (backup_table_lba < 2 || backup_table_lba >= backup_lba ||
        (uint64_t)(aligned / logical_block) > backup_lba - backup_table_lba) {
        result = -EINVAL; goto entries_done;
    }
    uint8_t *backup_entries = malloc(aligned);
    if (!backup_entries) { result = -ENOMEM; goto entries_done; }
    result = read_at(context, backup_table_lba * logical_block,
        backup_entries, aligned);
    if (!result && crc32_bytes(backup_entries, (size_t)table_bytes) !=
            le32(header + 88)) result = -EBADMSG;
    free(backup_entries);
    if (result) goto entries_done;
    struct installer_gpt parsed = {0};
    parsed.first_usable_lba = first;
    parsed.last_usable_lba = last;
    memcpy(parsed.disk_guid, header + 56, 16);
    if (all_zero(parsed.disk_guid, 16)) { result = -EINVAL; goto entries_done; }
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *entry = entries + (size_t)i * entry_bytes;
        if (all_zero(entry, 16)) continue;
        if (parsed.partition_count == PACHA_INSTALLER_MAX_PARTITIONS) {
            result = -E2BIG; goto entries_done;
        }
        struct installer_partition *partition =
            &parsed.partitions[parsed.partition_count++];
        memcpy(partition->type_guid, entry, 16);
        memcpy(partition->unique_guid, entry + 16, 16);
        partition->first_lba = le64(entry + 32);
        partition->last_lba = le64(entry + 40);
        if (all_zero(partition->unique_guid, 16) ||
            partition->first_lba < first || partition->last_lba > last ||
            partition->first_lba > partition->last_lba) {
            result = -EINVAL; goto entries_done;
        }
        partition_name(entry, entry_bytes, partition->name);
        if (!strcmp(partition->name, "pacha-install-data"))
            parsed.install_media = 1;
    }
    for (size_t i = 0; i < parsed.partition_count; ++i)
        for (size_t j = i + 1; j < parsed.partition_count; ++j)
            if (parsed.partitions[i].first_lba <= parsed.partitions[j].last_lba &&
                parsed.partitions[j].first_lba <= parsed.partitions[i].last_lba) {
                result = -EINVAL; goto entries_done;
            }
    *out = parsed;
entries_done:
    free(entries);
done:
    free(header);
    return result;
}

int installer_find_media(const struct installer_gpt *tables,
    size_t count, size_t *media_index) {
    if (!tables || !count || !media_index) return -EINVAL;
    size_t found = 0, index = 0;
    for (size_t i = 0; i < count; ++i) {
        if (tables[i].install_media) { found++; index = i; }
    }
    if (found != 1) return found ? -EEXIST : -ENODEV;
    *media_index = index;
    return 0;
}

int installer_guid_equal(const uint8_t a[16], const uint8_t b[16]) {
    return a && b && !memcmp(a, b, 16);
}

void installer_guid_text(const uint8_t guid[16], char out[37]) {
    static const char hex[] = "0123456789abcdef";
    unsigned positions[16] = {3, 2, 1, 0, 5, 4, 7, 6,
        8, 9, 10, 11, 12, 13, 14, 15};
    size_t at = 0;
    for (size_t i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out[at++] = '-';
        uint8_t value = guid[positions[i]];
        out[at++] = hex[value >> 4];
        out[at++] = hex[value & 15];
    }
    out[at] = 0;
}
