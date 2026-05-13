#include "fat_backend.h"
#include "block_client.h"
#include "fs_server_abi.h"

enum {
    SYSCALL_LOG = 0x9,
    SYSCALL_WAIT_EVENT = 0x17,
    SYSCALL_ACCEPT_CAP_TRANSFER = 0x2A,
    SYSCALL_SIGNAL_ENDPOINT = 0x2C,
    SYSCALL_MAP_PAGE_ANYWHERE = 0x5C,
    SYSCALL_ACCEPT_IPC_BUFFER_TRANSFER = 0x61,
    SYSCALL_MAP_IPC_BUFFER_ANYWHERE = 0x62,
    SYSCALL_OK = 0,
    CAP_TRANSFER_ID_MIN = 0x1000,
    IPC_BUFFER_TOKEN_TAG = 0xA000000000000000ULL,
    IPC_BUFFER_TOKEN_MASK = 0x0FFFFFFFFFFFFFFFULL,
    ESP_CONFIG_VA = 0x3C002000,
    ESP_MAX_OBJECTS = 32,
    ESP_OBJECT_MOUNT = 1,
    ESP_OBJECT_DIR = 2,
    ESP_OBJECT_FILE = 3,
    ESP_OBJECT_OPEN_FILE = 4,
    ESP_TOKEN_TAG = 1ULL << 63,
    ESP_DIR_MODE = 0x4000,
    ESP_FILE_MODE = 0x8000,
};

static u64 cstr_len(const char *s) {
    u64 n = 0;
    while (s[n] != 0) n++;
    return n;
}

static void user_log_len(const char *message, u64 len) {
    u64 ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"((u64)SYSCALL_LOG),
          "D"((u64)message),
          "S"(len)
        : "rcx", "rdx", "r8", "r9", "r10", "r11", "memory");
    (void)ret;
}

static void user_log(const char *message) {
    user_log_len(message, cstr_len(message));
}

static u64 syscall2(u64 nr, u64 arg0, u64 arg1) {
    u64 ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(nr), "D"(arg0), "S"(arg1)
        : "rcx", "rdx", "r8", "r9", "r10", "r11", "memory");
    return ret;
}

static u64 map_page_anywhere(u64 paddr, u64 writable) {
    return syscall2(SYSCALL_MAP_PAGE_ANYWHERE, paddr, writable);
}

static int is_ipc_buffer_token(u64 token) {
    return (token & ~IPC_BUFFER_TOKEN_MASK) == IPC_BUFFER_TOKEN_TAG && (token & IPC_BUFFER_TOKEN_MASK) != 0;
}

static u64 accept_ipc_buffer_transfer(u64 transfer_id) {
    return syscall2(SYSCALL_ACCEPT_IPC_BUFFER_TRANSFER, transfer_id, 0);
}

static u64 map_ipc_buffer_anywhere(u64 token, u64 writable) {
    return syscall2(SYSCALL_MAP_IPC_BUFFER_ANYWHERE, token, writable);
}

static u32 load_le32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static u16 load_le16(const u8 *p) {
    return (u16)p[0] | ((u16)p[1] << 8);
}

static u64 load_le64(const u8 *p) {
    return (u64)load_le32(p) | ((u64)load_le32(p + 4) << 32);
}

static u64 wait_event(void) {
    return syscall2(SYSCALL_WAIT_EVENT, 1, 1);
}

static void selftest_fat_parser(void) {
    u8 sector[512];
    for (u64 i = 0; i < sizeof(sector); i++) sector[i] = 0;
    sector[510] = 0x55;
    sector[511] = 0xAA;
    sector[11] = 0x00;
    sector[12] = 0x02;
    sector[13] = 0x01;
    sector[14] = 0x01;
    sector[16] = 0x02;
    sector[17] = 0xE0;
    sector[19] = 0x40;
    sector[20] = 0x0B;
    sector[22] = 0x09;

    struct fat_bpb_info info;
    if (fat_parse_bpb(sector, &info)) {
        user_log("EspServer: FAT parser ready\n");
    } else {
        user_log("EspServer: FAT parser selftest failed\n");
    }
}

struct fat_dirent_view {
    u8 attr;
    u32 first_cluster;
    u32 file_size;
};

struct fat_object {
    u8 active;
    u8 kind;
    u16 reserved0;
    u32 first_cluster;
    u32 file_size;
};

struct fat_session {
    u8 active;
    u8 reserved0[7];
    u64 request_va;
    u64 response_va;
    u64 request_paddr;
    u64 response_paddr;
    u64 request_token;
    u64 response_token;
    u64 session_nonce;
    u64 last_completed_seq;
    u64 mount_token;
};

static struct block_client g_block_client;
static struct fat_bpb_info g_fat_info;
static u64 g_fat_start_lba;
static u8 g_fat_mounted;
static u8 g_sector[4096];
static u8 g_entries[4096];
static struct fat_session g_session;
static struct fat_object g_objects[ESP_MAX_OBJECTS];
static u64 g_next_object_id = 1;
static u64 g_endpoint_id;

static int short_name_equal(const u8 *entry_name, const char target[11]) {
    for (u32 i = 0; i < 11; i++) {
        if (entry_name[i] != (u8)target[i]) return 0;
    }
    return 1;
}

static u64 fat_cluster_lba(const struct fat_bpb_info *info, u64 volume_lba, u32 cluster) {
    return volume_lba + info->first_data_sector + ((u64)(cluster - 2) * info->sectors_per_cluster);
}

static int fat16_next_cluster(
    struct block_client *client,
    const struct fat_bpb_info *info,
    u64 volume_lba,
    u32 cluster,
    u8 *scratch,
    u64 scratch_len,
    u32 *out_next
) {
    const u32 fat_offset = cluster * 2;
    const u64 fat_lba = volume_lba + info->first_fat_sector + (fat_offset / info->bytes_per_sector);
    const u32 sector_offset = fat_offset % info->bytes_per_sector;
    if ((u64)sector_offset + 2 > scratch_len) return 0;
    if (!block_client_read_one(client, fat_lba, scratch, scratch_len)) return 0;
    *out_next = load_le16(scratch + sector_offset);
    return 1;
}

static int scan_directory_sector(
    const u8 *sector,
    u32 sector_bytes,
    const char target[11],
    struct fat_dirent_view *out
) {
    for (u32 off = 0; off + 32 <= sector_bytes; off += 32) {
        const u8 *entry = sector + off;
        if (entry[0] == 0x00) return -1;
        if (entry[0] == 0xE5) continue;

        const u8 attr = entry[11];
        if ((attr & 0x0F) == 0x0F) continue;
        if ((attr & 0x08) != 0) continue;
        if (!short_name_equal(entry, target)) continue;

        out->attr = attr;
        out->first_cluster = ((u32)load_le16(entry + 20) << 16) | load_le16(entry + 26);
        out->file_size = load_le32(entry + 28);
        return 1;
    }
    return 0;
}

static int fat_lookup_root(
    struct block_client *client,
    const struct fat_bpb_info *info,
    u64 volume_lba,
    const char target[11],
    u8 *scratch,
    u64 scratch_len,
    struct fat_dirent_view *out
) {
    for (u32 i = 0; i < info->root_dir_sectors; i++) {
        const u64 lba = volume_lba + info->first_root_dir_sector + i;
        if (!block_client_read_one(client, lba, scratch, scratch_len)) return 0;
        const int found = scan_directory_sector(scratch, info->bytes_per_sector, target, out);
        if (found != 0) return found > 0;
    }
    return 0;
}

static int fat_lookup_cluster_dir(
    struct block_client *client,
    const struct fat_bpb_info *info,
    u64 volume_lba,
    u32 first_cluster,
    const char target[11],
    u8 *scratch,
    u64 scratch_len,
    struct fat_dirent_view *out
) {
    u32 cluster = first_cluster;
    while (cluster >= 2 && cluster < 0xFFF8) {
        const u64 first_lba = fat_cluster_lba(info, volume_lba, cluster);
        for (u32 i = 0; i < info->sectors_per_cluster; i++) {
            if (!block_client_read_one(client, first_lba + i, scratch, scratch_len)) return 0;
            const int found = scan_directory_sector(scratch, info->bytes_per_sector, target, out);
            if (found != 0) return found > 0;
        }

        if (!fat16_next_cluster(client, info, volume_lba, cluster, scratch, scratch_len, &cluster)) {
            return 0;
        }
    }
    return 0;
}

static void probe_bootfs_file(
    struct block_client *client,
    const struct fat_bpb_info *info,
    u64 volume_lba,
    u8 *scratch,
    u64 scratch_len
) {
    if (info->type != FAT_TYPE_16) {
        user_log("EspServer: FAT lookup unsupported type\n");
        return;
    }

    char efi_name[11];
    char boot_name[11];
    char bootfs_name[11];
    struct fat_dirent_view efi;
    struct fat_dirent_view boot;
    struct fat_dirent_view bootfs;

    if (!fat_make_short_name("EFI", 3, efi_name) ||
        !fat_make_short_name("BOOT", 4, boot_name) ||
        !fat_make_short_name("BOOTFS.IMG", 10, bootfs_name))
    {
        user_log("EspServer: short name build failed\n");
        return;
    }

    if (!fat_lookup_root(client, info, volume_lba, efi_name, scratch, scratch_len, &efi) ||
        (efi.attr & 0x10) == 0)
    {
        user_log("EspServer: lookup /EFI failed\n");
        return;
    }
    user_log("EspServer: lookup /EFI ok\n");

    if (!fat_lookup_cluster_dir(client, info, volume_lba, efi.first_cluster, boot_name, scratch, scratch_len, &boot) ||
        (boot.attr & 0x10) == 0)
    {
        user_log("EspServer: lookup /EFI/BOOT failed\n");
        return;
    }
    user_log("EspServer: lookup /EFI/BOOT ok\n");

    if (!fat_lookup_cluster_dir(client, info, volume_lba, boot.first_cluster, bootfs_name, scratch, scratch_len, &bootfs) ||
        (bootfs.attr & 0x10) != 0)
    {
        user_log("EspServer: lookup /EFI/BOOT/BOOTFS.IMG failed\n");
        return;
    }
    user_log("EspServer: lookup /EFI/BOOT/BOOTFS.IMG ok\n");

    if (bootfs.first_cluster < 2) {
        user_log("EspServer: BOOTFS.IMG has no data cluster\n");
        return;
    }

    if (!block_client_read_one(client, fat_cluster_lba(info, volume_lba, bootfs.first_cluster), scratch, scratch_len)) {
        user_log("EspServer: BOOTFS.IMG first block read failed\n");
        return;
    }
    user_log("EspServer: BOOTFS.IMG first block read ok\n");
}

static void clear_page(u64 va) {
    volatile u64 *p = (volatile u64 *)va;
    for (u64 i = 0; i < 512; i++) p[i] = 0;
}

static void copy_to_volatile(volatile u8 *dst, const u8 *src, u64 len) {
    for (u64 i = 0; i < len; i++) dst[i] = src[i];
}

static void copy_from_volatile(const volatile u8 *src, u8 *dst, u64 len) {
    for (u64 i = 0; i < len; i++) dst[i] = src[i];
}

static u64 make_token(u64 object_id) {
    return ESP_TOKEN_TAG | object_id;
}

static struct fat_object *object_from_token(u64 token) {
    if ((token & ESP_TOKEN_TAG) == 0) return 0;
    u64 id = token & ~ESP_TOKEN_TAG;
    if (id == 0 || id >= ESP_MAX_OBJECTS) return 0;
    if (!g_objects[id].active) return 0;
    return &g_objects[id];
}

static u64 alloc_object(u8 kind, u32 first_cluster, u32 file_size) {
    for (u64 tries = 0; tries < ESP_MAX_OBJECTS; tries++) {
        u64 id = g_next_object_id;
        g_next_object_id++;
        if (g_next_object_id >= ESP_MAX_OBJECTS) g_next_object_id = 1;
        if (g_objects[id].active) continue;
        g_objects[id].active = 1;
        g_objects[id].kind = kind;
        g_objects[id].first_cluster = first_cluster;
        g_objects[id].file_size = file_size;
        return make_token(id);
    }
    return 0;
}

static int path_component_short_name(const u8 *name, u16 len, char out_name[11]) {
    char tmp[FS_MAX_PATH_BYTES];
    if (len == 0 || len >= FS_MAX_PATH_BYTES) return 0;
    for (u16 i = 0; i < len; i++) tmp[i] = (char)name[i];
    tmp[len] = 0;
    return fat_make_short_name(tmp, len, out_name);
}

static int lookup_child(
    u8 parent_kind,
    u32 parent_cluster,
    const u8 *name,
    u16 name_len,
    struct fat_dirent_view *out
) {
    char short_name[11];
    if (!path_component_short_name(name, name_len, short_name)) return 0;
    if (parent_kind == ESP_OBJECT_MOUNT) {
        return fat_lookup_root(&g_block_client, &g_fat_info, g_fat_start_lba, short_name, g_sector, sizeof(g_sector), out);
    }
    if (parent_kind == ESP_OBJECT_DIR) {
        return fat_lookup_cluster_dir(&g_block_client, &g_fat_info, g_fat_start_lba, parent_cluster, short_name, g_sector, sizeof(g_sector), out);
    }
    return 0;
}

static int resolve_path_from(
    struct fat_object *base,
    const u8 *path,
    u16 path_len,
    struct fat_dirent_view *out
) {
    u8 current_kind = base->kind;
    u32 current_cluster = base->first_cluster;
    u16 pos = 0;
    if (path_len > 0 && path[0] == '/') {
        current_kind = ESP_OBJECT_MOUNT;
        current_cluster = 0;
        pos = 1;
    }
    while (pos < path_len) {
        while (pos < path_len && path[pos] == '/') pos++;
        if (pos >= path_len) break;
        const u16 start = pos;
        while (pos < path_len && path[pos] != '/') pos++;
        const u16 len = pos - start;
        const int last = pos >= path_len;
        if (!lookup_child(current_kind, current_cluster, path + start, len, out)) return 0;
        if (last) return 1;
        if ((out->attr & 0x10) == 0) return -1;
        current_kind = ESP_OBJECT_DIR;
        current_cluster = out->first_cluster;
    }
    return 0;
}

static void write_response(
    u16 op,
    u64 seq,
    i32 status,
    u64 result_token,
    u64 file_bytes,
    u64 cursor_next,
    u8 object_kind,
    u16 inline_bytes
) {
    volatile struct fs_response_header *response = (volatile struct fs_response_header *)g_session.response_va;
    response->magic = FS_RESPONSE_MAGIC;
    response->version = FS_PROTOCOL_VERSION;
    response->op = op;
    response->status = status;
    response->result_flags = 0;
    response->result_token = result_token;
    response->file_bytes = file_bytes;
    response->cursor_next = cursor_next;
    response->inline_bytes = inline_bytes;
    response->object_kind = object_kind;
    response->reserved0 = 0;
    response->reserved1 = 0;
    response->arg0 = g_block_client.block_size;
    response->arg1 = g_block_client.capacity_blocks;
    __asm__ volatile("" ::: "memory");
    response->response_seq = seq;
}

static void reply_status(u16 op, u64 seq, i32 status) {
    clear_page(g_session.response_va);
    write_response(op, seq, status, 0, 0, 0, FS_OBJECT_NONE, 0);
}

static void reply_lookup(u16 op, u64 seq, u64 token, u8 kind, u64 file_size) {
    clear_page(g_session.response_va);
    write_response(op, seq, FS_STATUS_OK, token, file_size, 0, kind, 0);
}

static void reply_stat(u64 seq, struct fat_object *object) {
    clear_page(g_session.response_va);
    volatile struct fs_stat_record *record = (volatile struct fs_stat_record *)(g_session.response_va + FS_RESPONSE_HEADER_BYTES);
    const int is_dir = object->kind == ESP_OBJECT_MOUNT || object->kind == ESP_OBJECT_DIR;
    record->object_kind = is_dir ? FS_OBJECT_DIRECTORY : FS_OBJECT_FILE;
    record->size_bytes = is_dir ? 0 : object->file_size;
    record->mode_bits = is_dir ? ESP_DIR_MODE : ESP_FILE_MODE;
    record->mtime_unix_sec = 0;
    write_response(FS_OP_STAT, seq, FS_STATUS_OK, 0, record->size_bytes, 0, record->object_kind, sizeof(struct fs_stat_record));
}

static int read_file_bytes(struct fat_object *object, u64 offset, u32 requested, volatile u8 *out, u16 *out_len) {
    *out_len = 0;
    if (offset >= object->file_size || requested == 0) return 1;
    if (object->first_cluster < 2) return 1;
    if (g_fat_info.bytes_per_sector == 0 || g_fat_info.sectors_per_cluster == 0) return 0;

    u64 remaining_file = object->file_size - offset;
    u64 remaining = requested;
    if (remaining > FS_RESPONSE_PAYLOAD_BYTES) remaining = FS_RESPONSE_PAYLOAD_BYTES;
    if (remaining > remaining_file) remaining = remaining_file;

    const u64 cluster_bytes = (u64)g_fat_info.bytes_per_sector * g_fat_info.sectors_per_cluster;
    u64 cluster_skip = offset / cluster_bytes;
    u64 offset_in_cluster = offset % cluster_bytes;
    u32 cluster = object->first_cluster;
    while (cluster_skip > 0) {
        if (!fat16_next_cluster(&g_block_client, &g_fat_info, g_fat_start_lba, cluster, g_sector, sizeof(g_sector), &cluster)) return 0;
        if (cluster < 2 || cluster >= 0xFFF8) return 1;
        cluster_skip--;
    }

    u16 copied = 0;
    while (remaining > 0 && cluster >= 2 && cluster < 0xFFF8) {
        u32 sector_in_cluster = (u32)(offset_in_cluster / g_fat_info.bytes_per_sector);
        u32 sector_offset = (u32)(offset_in_cluster % g_fat_info.bytes_per_sector);
        while (sector_in_cluster < g_fat_info.sectors_per_cluster && remaining > 0) {
            const u64 lba = fat_cluster_lba(&g_fat_info, g_fat_start_lba, cluster) + sector_in_cluster;
            if (!block_client_read_one(&g_block_client, lba, g_sector, sizeof(g_sector))) return 0;
            u64 chunk = g_fat_info.bytes_per_sector - sector_offset;
            if (chunk > remaining) chunk = remaining;
            copy_to_volatile(out + copied, g_sector + sector_offset, chunk);
            copied += (u16)chunk;
            remaining -= chunk;
            sector_in_cluster++;
            sector_offset = 0;
        }
        offset_in_cluster = 0;
        if (remaining == 0) break;
        if (!fat16_next_cluster(&g_block_client, &g_fat_info, g_fat_start_lba, cluster, g_sector, sizeof(g_sector), &cluster)) return 0;
    }
    *out_len = copied;
    return 1;
}

static void handle_fs_request(void) {
    if (!g_session.active || !g_fat_mounted) return;
    volatile struct fs_request_header *request = (volatile struct fs_request_header *)g_session.request_va;
    if (request->magic != FS_REQUEST_MAGIC || request->version != FS_PROTOCOL_VERSION) return;
    const u64 seq = request->request_seq;
    if (seq == 0 || seq <= g_session.last_completed_seq) return;
    if (request->session_nonce != g_session.session_nonce) return;

    if (request->op == FS_OP_CONNECT) {
        reply_status(FS_OP_CONNECT, seq, FS_STATUS_BUSY);
    } else if (request->op == FS_OP_LOOKUP) {
        struct fat_object *base = object_from_token(request->object_token);
        if (!base) {
            reply_status(FS_OP_LOOKUP, seq, FS_STATUS_NOT_FOUND);
        } else if (request->path_bytes > FS_REQUEST_PAYLOAD_BYTES) {
            reply_status(FS_OP_LOOKUP, seq, FS_STATUS_INVALID);
        } else {
            u8 path[FS_MAX_PATH_BYTES];
            if (request->path_bytes > FS_MAX_PATH_BYTES) {
                reply_status(FS_OP_LOOKUP, seq, FS_STATUS_INVALID);
            } else {
                copy_from_volatile((const volatile u8 *)(g_session.request_va + FS_REQUEST_HEADER_BYTES), path, request->path_bytes);
                struct fat_dirent_view found;
                const int resolved = resolve_path_from(base, path, request->path_bytes, &found);
                if (resolved == -1) {
                    reply_status(FS_OP_LOOKUP, seq, FS_STATUS_NOT_DIR);
                } else if (resolved == 0) {
                    reply_status(FS_OP_LOOKUP, seq, FS_STATUS_NOT_FOUND);
                } else {
                    const int is_dir = (found.attr & 0x10) != 0;
                    const u64 token = alloc_object(is_dir ? ESP_OBJECT_DIR : ESP_OBJECT_FILE, found.first_cluster, found.file_size);
                    if (token == 0) reply_status(FS_OP_LOOKUP, seq, FS_STATUS_BUSY);
                    else reply_lookup(FS_OP_LOOKUP, seq, token, is_dir ? FS_OBJECT_DIRECTORY : FS_OBJECT_FILE, found.file_size);
                }
            }
        }
    } else if (request->op == FS_OP_OPEN) {
        struct fat_object *object = object_from_token(request->object_token);
        if (!object) reply_status(FS_OP_OPEN, seq, FS_STATUS_NOT_FOUND);
        else if (object->kind != ESP_OBJECT_FILE) reply_status(FS_OP_OPEN, seq, FS_STATUS_IS_DIR);
        else {
            const u64 token = alloc_object(ESP_OBJECT_OPEN_FILE, object->first_cluster, object->file_size);
            if (token == 0) reply_status(FS_OP_OPEN, seq, FS_STATUS_BUSY);
            else reply_lookup(FS_OP_OPEN, seq, token, FS_OBJECT_OPEN_FILE, object->file_size);
        }
    } else if (request->op == FS_OP_READ) {
        struct fat_object *object = object_from_token(request->object_token);
        if (!object) reply_status(FS_OP_READ, seq, FS_STATUS_NOT_FOUND);
        else if (object->kind != ESP_OBJECT_OPEN_FILE) reply_status(FS_OP_READ, seq, FS_STATUS_INVALID);
        else {
            u16 bytes = 0;
            clear_page(g_session.response_va);
            if (!read_file_bytes(object, request->offset, request->length, (volatile u8 *)(g_session.response_va + FS_RESPONSE_HEADER_BYTES), &bytes)) {
                write_response(FS_OP_READ, seq, FS_STATUS_IO_ERROR, 0, 0, 0, FS_OBJECT_NONE, 0);
            } else {
                write_response(FS_OP_READ, seq, FS_STATUS_OK, 0, object->file_size, request->offset + bytes, FS_OBJECT_OPEN_FILE, bytes);
            }
        }
    } else if (request->op == FS_OP_STAT) {
        struct fat_object *object = object_from_token(request->object_token);
        if (!object) reply_status(FS_OP_STAT, seq, FS_STATUS_NOT_FOUND);
        else reply_stat(seq, object);
    } else if (request->op == FS_OP_CLOSE) {
        struct fat_object *object = object_from_token(request->object_token);
        if (object && object->kind != ESP_OBJECT_MOUNT) object->active = 0;
        reply_status(FS_OP_CLOSE, seq, FS_STATUS_OK);
    } else if (request->op == FS_OP_STATFS) {
        clear_page(g_session.response_va);
        write_response(FS_OP_STATFS, seq, FS_STATUS_OK, 0, g_block_client.capacity_blocks, 0, FS_OBJECT_MOUNT, 0);
    } else if (request->op == FS_OP_CREATE || request->op == FS_OP_WRITE || request->op == FS_OP_UNLINK || request->op == FS_OP_RENAME) {
        reply_status(request->op, seq, FS_STATUS_NO_RIGHT);
    } else {
        reply_status(request->op, seq, FS_STATUS_NOT_SUPPORTED);
    }

    g_session.last_completed_seq = seq;
}

static void finish_connect_session(
    volatile struct fs_request_header *request,
    u64 request_va,
    u64 response_va,
    u64 request_paddr,
    u64 response_paddr,
    u64 request_token,
    u64 response_token
) {
    clear_page(response_va);
    g_session.active = 1;
    g_session.request_va = request_va;
    g_session.response_va = response_va;
    g_session.request_paddr = request_paddr;
    g_session.response_paddr = response_paddr;
    g_session.request_token = request_token;
    g_session.response_token = response_token;
    g_session.session_nonce = request->session_nonce;
    g_session.last_completed_seq = 0;
    const u64 mount_token = alloc_object(ESP_OBJECT_MOUNT, 0, 0);
    g_session.mount_token = mount_token;
    write_response(FS_OP_CONNECT, request->request_seq, FS_STATUS_OK, mount_token, 0, 0, FS_OBJECT_MOUNT, 0);
    g_session.last_completed_seq = request->request_seq;
}

static int handle_connect_token(u64 request_token) {
    const u64 request_va = map_ipc_buffer_anywhere(request_token, 0);
    if (request_va < 0x1000) return 0;

    volatile struct fs_request_header *request = (volatile struct fs_request_header *)request_va;
    if (request->magic != FS_REQUEST_MAGIC ||
        request->version != FS_PROTOCOL_VERSION ||
        request->op != FS_OP_CONNECT ||
        request->request_seq == 0 ||
        !is_ipc_buffer_token(request->arg0) ||
        request->session_nonce == 0)
    {
        user_log("EspServer: invalid ipc-buffer connect request\n");
        return 0;
    }
    const u64 response_token = request->arg0;
    const u64 response_va = map_ipc_buffer_anywhere(response_token, 1);
    if (response_va < 0x1000) return 0;
    finish_connect_session(request, request_va, response_va, 0, 0, request_token, response_token);
    user_log("EspServer: ipc-buffer session connect ok\n");
    return 1;
}

static void handle_connect_paddr_transfer(u64 transfer_id) {
    const u64 request_paddr = syscall2(SYSCALL_ACCEPT_CAP_TRANSFER, transfer_id, 0);
    if (request_paddr < 0x1000) return;
    const u64 request_va = map_page_anywhere(request_paddr, 0);
    if (request_va < 0x1000) return;

    volatile struct fs_request_header *request = (volatile struct fs_request_header *)request_va;
    if (request->magic != FS_REQUEST_MAGIC ||
        request->version != FS_PROTOCOL_VERSION ||
        request->op != FS_OP_CONNECT ||
        request->request_seq == 0 ||
        request->arg0 < 0x1000 ||
        request->session_nonce == 0)
    {
        user_log("EspServer: invalid connect request\n");
        return;
    }
    const u64 response_va = map_page_anywhere(request->arg0, 1);
    if (response_va < 0x1000) return;

    finish_connect_session(request, request_va, response_va, request_paddr, request->arg0, 0, 0);
    user_log("EspServer: session connect ok\n");
}

static void handle_connect_transfer(u64 transfer_id) {
    if (!g_fat_mounted) return;
    const u64 request_token = accept_ipc_buffer_transfer(transfer_id);
    if (is_ipc_buffer_token(request_token) && handle_connect_token(request_token)) return;
    handle_connect_paddr_transfer(transfer_id);
}

static void mount_from_block_service(void) {
    if (!block_client_connect(&g_block_client)) {
        user_log("EspServer: block connect pending\n");
        return;
    }
    user_log("EspServer: block connect ok\n");

    if (!block_client_identify(&g_block_client)) {
        user_log("EspServer: block identify failed\n");
        return;
    }
    user_log("EspServer: block identify ok\n");

    if (!block_client_read_one(&g_block_client, 1, g_sector, sizeof(g_sector))) {
        user_log("EspServer: GPT header read failed\n");
        return;
    }

    if (g_sector[0] == 'E' && g_sector[1] == 'F' && g_sector[2] == 'I' && g_sector[3] == ' ' &&
        g_sector[4] == 'P' && g_sector[5] == 'A' && g_sector[6] == 'R' && g_sector[7] == 'T')
    {
        static const u8 efi_system_guid[16] = {
            0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
            0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B,
        };
        const u64 entry_lba = load_le64(g_sector + 72);
        u32 entry_count = load_le32(g_sector + 80);
        const u32 entry_size = load_le32(g_sector + 84);
        if (entry_count > 32) entry_count = 32;
        if (entry_size < 128 || entry_size > 512) {
            user_log("EspServer: GPT entry size unsupported\n");
            return;
        }
        if (!block_client_read_one(&g_block_client, entry_lba, g_entries, sizeof(g_entries))) {
            user_log("EspServer: GPT entries read failed\n");
            return;
        }
        const u32 entries_per_block = (u32)(g_block_client.block_size / entry_size);
        if (entries_per_block == 0) {
            user_log("EspServer: GPT entries per block invalid\n");
            return;
        }
        if (entry_count > entries_per_block) entry_count = entries_per_block;
        for (u32 i = 0; i < entry_count; i++) {
            const u8 *entry = g_entries + ((u64)i * entry_size);
            int match = 1;
            for (u32 j = 0; j < 16; j++) {
                if (entry[j] != efi_system_guid[j]) {
                    match = 0;
                    break;
                }
            }
            if (match) {
                g_fat_start_lba = load_le64(entry + 32);
                break;
            }
        }
    }

    if (g_fat_start_lba == 0) {
        user_log("EspServer: no FAT partition in GPT\n");
        return;
    }

    if (!block_client_read_one(&g_block_client, g_fat_start_lba, g_sector, sizeof(g_sector))) {
        user_log("EspServer: FAT BPB read failed\n");
        return;
    }

    if (fat_parse_bpb(g_sector, &g_fat_info)) {
        g_fat_mounted = 1;
        user_log("EspServer: FAT volume mounted readonly\n");
        probe_bootfs_file(&g_block_client, &g_fat_info, g_fat_start_lba, g_sector, sizeof(g_sector));
    } else {
        user_log("EspServer: FAT BPB invalid\n");
    }
}

void esp_server_main(void) {
    user_log("EspServer: started\n");
    user_log("EspServer: fs_protocol ABI ready\n");
    volatile u64 *config = (volatile u64 *)ESP_CONFIG_VA;
    g_endpoint_id = config[0];
    selftest_fat_parser();
    mount_from_block_service();
    if (g_endpoint_id != 0) {
        user_log("EspServer: readonly ESP endpoint ready\n");
        config[2] = 1;
    } else {
        user_log("EspServer: readonly ESP endpoint missing\n");
    }
    for (;;) {
        const u64 received = wait_event();
        if (received >= CAP_TRANSFER_ID_MIN) handle_connect_transfer(received);
        handle_fs_request();
    }
}
