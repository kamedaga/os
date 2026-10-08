/* SPDX-License-Identifier: MIT */
#include "execute.h"
#include "ext4_probe.h"
#include "fat32_probe.h"

#include <pacha/ipc.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

enum { GPT_HEADER_BYTES = 92, GPT_ENTRY_COUNT = 128,
    GPT_ENTRY_BYTES = 128 };

static void report_progress(installer_progress progress, void *context,
    unsigned percent, const char *phase) {
    if (progress) progress(context, percent, phase);
}

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
        (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t get64(const uint8_t *p) {
    return get32(p) | (uint64_t)get32(p + 4) << 32;
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

static int same_disk(const kb2_block_disk_t *a, const kb2_block_disk_t *b) {
    return a->diskseq == b->diskseq && a->bytes == b->bytes &&
        a->major == b->major && a->minor == b->minor &&
        a->logical_block_size == b->logical_block_size &&
        a->physical_block_size == b->physical_block_size &&
        a->removable == b->removable && a->read_only == b->read_only &&
        !memcmp(a->name, b->name, sizeof(a->name));
}

static int refresh_target(struct installer_block_target *target) {
    uint64_t cursor = 0;
    do {
        kb2_block_disk_t disks[KB2_BLOCK_DEVICE_MAX_DISKS];
        size_t count = 0;
        uint64_t next = 0;
        int status = installer_block_list(target->source, cursor, disks,
            KB2_BLOCK_DEVICE_MAX_DISKS, &count, &next);
        if (status) return status;
        for (size_t i = 0; i < count; ++i)
            if (same_disk(&target->disk, &disks[i])) {
                target->generation = target->source->generation;
                return 0;
            }
        cursor = next;
    } while (cursor);
    return -ESTALE;
}

static int wait_for_restart(struct installer_block_target *target,
    uint64_t old_generation) {
    const uint64_t rights = PACHA_FD_RIGHT_READ | PACHA_FD_RIGHT_WAIT |
        PACHA_FD_RIGHT_POLL | PACHA_FD_RIGHT_CLOSE;
    int timer = pacha_timerfd_create(UINT64_C(100000000),
        UINT64_C(100000000), rights, 0);
    if (timer < 16) return timer < 0 ? timer : -EIO;
    int status = -ETIMEDOUT;
    for (unsigned attempt = 0; attempt < 300; ++attempt) {
        int found = refresh_target(target);
        if (target->source->generation != old_generation) {
            status = found == -ESTALE ? refresh_target(target) : found;
            break;
        }
        if (found && found != -ESTALE) { status = found; break; }
        struct pacha_pollfd event = {.fd = timer,
            .events = PACHA_FD_EVENT_READABLE};
        uint64_t expirations = 0;
        if (pacha_fd_wait_many(&event, 1, PACHA_FD_WAIT_FOREVER) != 1 ||
            pacha_fd_read(timer, &expirations, sizeof(expirations)) !=
                (long)sizeof(expirations)) { status = -EIO; break; }
    }
    (void)pacha_fd_close(timer);
    return status;
}

static int read_at(void *context, uint64_t offset,
    void *bytes, size_t length) {
    struct installer_block_target *target = context;
    uint8_t *cursor = bytes;
    while (length) {
        size_t chunk = length > KB2_BLOCK_DEVICE_MAX_TRANSFER ?
            KB2_BLOCK_DEVICE_MAX_TRANSFER : length;
        int status = installer_block_read(target, offset, cursor, chunk);
        if (status) return status;
        offset += chunk;
        cursor += chunk;
        length -= chunk;
    }
    return 0;
}

static int write_at(void *context, uint64_t offset,
    const void *bytes, size_t length) {
    struct installer_block_target *target = context;
    const uint8_t *cursor = bytes;
    while (length) {
        size_t chunk = length > KB2_BLOCK_DEVICE_MAX_TRANSFER ?
            KB2_BLOCK_DEVICE_MAX_TRANSFER : length;
        int status = installer_block_write(target, offset, cursor, chunk);
        if (status) return status;
        offset += chunk;
        cursor += chunk;
        length -= chunk;
    }
    return 0;
}

static int flush(void *context) {
    return installer_block_flush(context);
}

static int verify_guided_backup_gpt(struct installer_block_target *target) {
    const uint32_t block = target->disk.logical_block_size;
    const uint64_t bytes = target->disk.bytes;
    if (block < 512 || block > 4096 || (block & (block - 1)) ||
        bytes < 3 * (uint64_t)block || bytes % block) return -EINVAL;
    const uint64_t sectors = bytes / block;
    const size_t table_bytes = GPT_ENTRY_COUNT * GPT_ENTRY_BYTES;
    const uint64_t table_sectors = (table_bytes + block - 1) / block;
    uint8_t *header = malloc(block);
    uint8_t *table = malloc((size_t)table_sectors * block);
    if (!header || !table) {
        free(header); free(table); return -ENOMEM;
    }
    int status = read_at(target, bytes - block, header, block);
    if (!status && (memcmp(header, "EFI PART", 8) ||
            get32(header + 12) != GPT_HEADER_BYTES ||
            get64(header + 24) != sectors - 1 ||
            get64(header + 32) != 1 ||
            get64(header + 72) != sectors - 1 - table_sectors ||
            get32(header + 80) != GPT_ENTRY_COUNT ||
            get32(header + 84) != GPT_ENTRY_BYTES))
        status = -EBADMSG;
    uint32_t expected_header_crc = 0, expected_table_crc = 0;
    if (!status) {
        expected_header_crc = get32(header + 16);
        expected_table_crc = get32(header + 88);
        memset(header + 16, 0, 4);
        if (crc32_bytes(header, GPT_HEADER_BYTES) != expected_header_crc)
            status = -EBADMSG;
    }
    if (!status) status = read_at(target,
        (sectors - 1 - table_sectors) * (uint64_t)block,
        table, (size_t)table_sectors * block);
    if (!status && crc32_bytes(table, table_bytes) != expected_table_crc)
        status = -EBADMSG;
    free(header);
    free(table);
    return status;
}

static int new_guids(struct installer_new_guids *guids) {
    if (!guids || pacha_getrandom(guids, sizeof(*guids), 0) !=
        (long)sizeof(*guids)) return -EIO;
    /* GPT stores UUID fields in mixed-endian byte order, but version and
     * variant bits are at these positions in the on-disk representation. */
    uint8_t *values[3] = {guids->disk, guids->esp, guids->root};
    for (unsigned i = 0; i < 3; ++i) {
        values[i][7] = (values[i][7] & 0x0f) | 0x40;
        values[i][8] = (values[i][8] & 0x3f) | 0x80;
    }
    return 0;
}

int installer_execute_plan(void *context, size_t target_index,
    const struct installer_plan *plan, installer_progress progress,
    void *progress_context) {
    struct installer_execution *execution = context;
    if (!execution || !plan || target_index >= execution->count ||
        target_index == execution->media_index ||
        !execution->tools.preflight ||
        (!execution->tools.run_plan &&
         (!execution->tools.format_fat32 || !execution->tools.format_ext4 ||
          !execution->tools.deploy_os || !execution->tools.verify)))
        return -ENOSYS;
    /* mke2fs -d performs an atomic fresh population without mounting an
     * untrusted filesystem in the privileged native installer. Updating an
     * existing ext4 tree is deliberately unavailable until a bounded ext4
     * file API exists; reject it before preflight, ARM, or any disk write. */
    if (!plan->format_root) return -EOPNOTSUPP;
    report_progress(progress, progress_context, 1,
        "Verifying installation media");
    int status = execution->tools.preflight(execution->tools.context,
        progress, progress_context);
    if (status) return status; /* No disk write before all tools/payload exist. */
    report_progress(progress, progress_context, 20,
        "Revalidating the selected disk");
    struct installer_catalog_entry *entry = &execution->entries[target_index];
    struct installer_block_target target = entry->target;
    if (target.disk.read_only || !target.disk.diskseq ||
        plan->target_diskseq != target.disk.diskseq ||
        plan->target_bytes != target.disk.bytes ||
        plan->logical_block != target.disk.logical_block_size)
        return -ESTALE;
    status = refresh_target(&target);
    if (status) return status;
    struct installer_gpt fresh = {0};
    status = installer_read_gpt(read_at, &target, target.disk.bytes,
        target.disk.logical_block_size, &fresh);
    if (status == -EINVAL && entry->display.blank_verified)
        status = installer_verify_blank(read_at, &target, target.disk.bytes,
            target.disk.logical_block_size);
    else if (!status && fresh.install_media)
        return -EACCES;
    if (status) return status;
    status = installer_plan_revalidate(plan, target.disk.diskseq,
        target.disk.bytes, target.disk.logical_block_size,
        entry->display.gpt ? &fresh : NULL);
    if (status) return status;
    if (!plan->format_esp) {
        status = installer_probe_fat32(read_at, &target,
            target.disk.bytes, target.disk.logical_block_size,
            plan->esp.first_lba, plan->esp.last_lba);
        if (status) return status;
    }
    if (!plan->format_root) {
        status = installer_probe_ext4(read_at, &target,
            target.disk.bytes, target.disk.logical_block_size,
            plan->root.first_lba, plan->root.last_lba);
        if (status) return status;
    }
    report_progress(progress, progress_context, 24,
        "Granting temporary write access to the selected NVMe");
    status = installer_block_arm(&target);
    if (status) return status;
    const uint64_t armed_generation = target.generation;
    /* ARM restarts the sandbox. A generation change is expected, but the
     * namespace key must remain identical before any write is attempted. */
    report_progress(progress, progress_context, 25,
        "Waiting for the NVMe storage service to restart");
    status = wait_for_restart(&target, armed_generation);
    if (status) goto disarm;
    report_progress(progress, progress_context, 26,
        "NVMe write path is ready");
    if (plan->kind == INSTALLER_LAYOUT_GUIDED) {
        report_progress(progress, progress_context, 27,
            "Writing the backup and primary GPT");
        struct installer_new_guids guids;
        status = new_guids(&guids);
        if (!status) status = installer_write_guided_gpt(plan,
            target.disk.diskseq, target.disk.bytes,
            target.disk.logical_block_size, entry->display.blank_verified,
            &guids, read_at, write_at, flush, &target,
            progress, progress_context);
        if (status) goto disarm;
        report_progress(progress, progress_context, 29,
            "GPT written, flushed, and read back");
    }
    report_progress(progress, progress_context, 30,
        "Reading installation payload");
    if (execution->tools.run_plan) {
        status = execution->tools.run_plan(execution->tools.context,
            &target, plan, progress, progress_context);
    } else {
        if (plan->format_esp) {
            report_progress(progress, progress_context, 48,
                "Formatting the EFI System Partition");
            status = execution->tools.format_fat32(execution->tools.context,
                &target, plan->esp, plan->logical_block);
        }
        if (!status && plan->format_root) {
            report_progress(progress, progress_context, 58,
                "Formatting the ext4 root filesystem");
            status = execution->tools.format_ext4(execution->tools.context,
                &target, plan->root, plan->logical_block);
        }
        if (!status) {
            report_progress(progress, progress_context, 78,
                "Copying PachaOS files");
            status = execution->tools.deploy_os(
                execution->tools.context, &target, plan);
        }
        if (!status) status = installer_block_flush(&target);
        if (!status) {
            report_progress(progress, progress_context, 94,
                "Reading back installed files");
            status = execution->tools.verify(
                execution->tools.context, &target, plan);
        }
    }
    if (!status) {
        report_progress(progress, progress_context, 97,
            "Verifying FAT32 geometry for UEFI");
        /* mkfs.fat may return success for an undersized FAT32 cluster count;
         * file readback alone does not prove firmware can mount the ESP. */
        status = installer_probe_fat32(read_at, &target,
            target.disk.bytes, target.disk.logical_block_size,
            plan->esp.first_lba, plan->esp.last_lba);
    }
    if (!status && plan->kind == INSTALLER_LAYOUT_GUIDED) {
        report_progress(progress, progress_context, 98,
            "Verifying the backup GPT");
        status = verify_guided_backup_gpt(&target);
    }
disarm:
    {
        if (!status) report_progress(progress, progress_context, 99,
            "Flushing data and revoking write access");
        const uint64_t active_generation = target.source->generation;
        int revoked = installer_block_disarm(&target);
        if (revoked == -ESTALE && !refresh_target(&target))
            revoked = installer_block_disarm(&target);
        if (!revoked)
            revoked = wait_for_restart(&target, active_generation);
        if (revoked && target.source->channel_fd >= 16) {
            /* The provider revokes on control-channel loss even if the
             * explicit DISARM reply/restart cannot be observed. */
            (void)pacha_fd_close(target.source->channel_fd);
            target.source->channel_fd = -1;
        }
        if (!status && revoked) status = revoked;
    }
    if (!status) report_progress(progress, progress_context, 100,
        "Installation verified");
    return status;
}
