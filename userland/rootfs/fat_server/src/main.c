#include "fs_protocol.h"

#if defined(__clang__)
#define FAT_NOINLINE_NOOPT __attribute__((noinline, optnone))
#else
#define FAT_NOINLINE_NOOPT __attribute__((noinline))
#endif

enum {
    SYSCALL_ALLOC_PAGE = 0x1,
    SYSCALL_LOG = 0x9,
    SYSCALL_MAP_PAGE = 0x2,
    SYSCALL_WAIT_EVENT = 0x17,
    SYSCALL_GRANT_CAP_ON_ENDPOINT = 0x24,
    SYSCALL_INSTALL_ENDPOINT = 0x26,
    SYSCALL_ACCEPT_CAP_TRANSFER = 0x2A,
    SYSCALL_SHARE_CAP = 0x2B,
    SYSCALL_SIGNAL_ENDPOINT = 0x2C,
    SYSCALL_GET_PROCESS_SLOT = 0x2E,
    SYSCALL_OK = 0,
    SYSCALL_ERR_ENDPOINT = 9,
    PAGE_RIGHT_CPU_READ = 0x1,
    PAGE_RIGHT_CPU_WRITE = 0x2,
    CAP_TRANSFER_ID_MIN = 0x1000,
    FAT_CONFIG_VA = 0x3C002000,
    FAT_SERVICE_REGISTRY_VA = 0x3C2C0000,
    FAT_REQUEST_VA = 0x27000000,
    FAT_RESPONSE_VA = 0x27001000,
    FAT_BLOCK_REQUEST_VA = 0x27100000,
    FAT_BLOCK_RESPONSE_VA = 0x27101000,
    FAT_REPLY_ENDPOINT_ID = 0xE8,
    FAT_TOKEN_TAG = 1ULL << 63,
    FAT_ROOT_OBJECT_ID = 1,
    FAT_PROBE_OBJECT_ID = 2,
    FAT_SEED2_FILE_OBJECT_ID = 3,
    FAT_SEED2_OPEN_OBJECT_ID = 4,
    FAT_ROOTFS_VFS_FILE_OBJECT_ID = 5,
    FAT_ROOTFS_VFS_OPEN_OBJECT_ID = 6,
    FAT_STARTUP_MANIFEST_FILE_OBJECT_ID = 7,
    FAT_STARTUP_MANIFEST_OPEN_OBJECT_ID = 8,
    FAT_DYNAMIC_OBJECT_ID_BASE = 0x100,
    FAT_DYNAMIC_OPEN_OBJECT_ID_BASE = 0x100000,
    FAT_MAX_DYNAMIC_OBJECTS = 64,
    FAT_ATTR_DIRECTORY = 0x10,
    FAT_ATTR_LONG_NAME = 0x0F,
    FAT_DIR_MODE = 0x4000,
    FAT_FILE_MODE = 0x8000,
    SERVICE_REGISTRY_MAGIC = 0x53525643,
    SERVICE_REGISTRY_VERSION = 1,
    SERVICE_KIND_BLOCK = 4,
    SERVICE_FLAG_PROCESS_SLOT_COMPAT = 1,
    SERVICE_REGISTRY_MAX_ENTRIES = 12,
    BLOCK_REQUEST_MAGIC = 0x514B4C42,
    BLOCK_RESPONSE_MAGIC = 0x524B4C42,
    BLOCK_PROTOCOL_VERSION = 1,
    BLOCK_OP_CONNECT = 1,
    BLOCK_OP_READ_BLOCKS = 3,
    BLOCK_STATUS_OK = 0,
    BLOCK_REQUEST_HEADER_BYTES = 72,
    BLOCK_RESPONSE_HEADER_BYTES = 56,
    BLOCK_RESPONSE_PAYLOAD_BYTES = 4096 - BLOCK_RESPONSE_HEADER_BYTES,
};

struct service_entry {
    u64 kind;
    u64 process_slot;
    u64 endpoint_id;
    u64 flags;
};

struct service_registry_page {
    u64 magic;
    u64 version;
    u64 entry_count;
    u64 reserved0;
    struct service_entry entries[SERVICE_REGISTRY_MAX_ENTRIES];
};

struct block_request_header {
    u32 magic;
    u16 version;
    u16 op;
    u64 request_seq;
    u64 object_token;
    u64 block_index;
    u32 block_count;
    u32 flags;
    u16 inline_bytes;
    u16 reserved0;
    u32 reserved1;
    u64 arg0;
    u64 arg1;
    u64 session_nonce;
};

struct block_response_header {
    u32 magic;
    u16 version;
    u16 op;
    u64 response_seq;
    i32 status;
    u32 result_flags;
    u64 result_token;
    u16 inline_bytes;
    u8 object_kind;
    u8 reserved0;
    u32 reserved1;
    u64 arg0;
    u64 arg1;
};

struct fat_session {
    u8 active;
    u8 reserved0[7];
    u64 request_va;
    u64 response_va;
    u64 request_paddr;
    u64 response_paddr;
    u64 reply_endpoint_id;
    u64 session_nonce;
    u64 last_completed_seq;
    u64 root_token;
};

struct block_session {
    u8 active;
    u8 reserved0[7];
    u64 endpoint_id;
    u64 process_slot;
    u64 request_va;
    u64 response_va;
    u64 request_paddr;
    u64 response_paddr;
    u64 session_nonce;
    u64 root_token;
    u64 block_size;
    u64 capacity_blocks;
    u64 next_seq;
};

struct fat_bpb_info {
    u8 valid;
    u8 sectors_per_cluster;
    u8 num_fats;
    u8 reserved0;
    u16 bytes_per_sector;
    u16 reserved_sector_count;
    u32 fat_size_sectors;
    u32 root_cluster;
    u32 first_fat_sector;
    u32 first_data_sector;
    u64 total_sectors;
};

static struct fat_session g_session;
static struct block_session g_block;
static struct fat_bpb_info g_bpb;
static u8 g_volume_sector0[4096];
static u8 g_sector_scratch[4096];
static u64 g_endpoint_id;
static u64 g_volume_start_block;
static u32 g_seed2_start_cluster;
static u32 g_seed2_size_bytes;
static u32 g_rootfs_vfs_start_cluster;
static u32 g_rootfs_vfs_size_bytes;
static u32 g_startup_manifest_start_cluster;
static u32 g_startup_manifest_size_bytes;

struct fat_cached_file {
    u64 file_object_id;
    u64 open_object_id;
    const char *path;
    const char *ready_log;
    u32 *start_cluster;
    u32 *size_bytes;
    u32 cached_cluster_index;
    u32 cached_cluster;
};

static struct fat_cached_file g_cached_files[] = {
    { FAT_SEED2_FILE_OBJECT_ID, FAT_SEED2_OPEN_OBJECT_ID, "/sbin/seed2.elf", "[fat_server] FatServer: /sbin/seed2.elf ready\n", &g_seed2_start_cluster, &g_seed2_size_bytes, 0, 0 },
    { FAT_ROOTFS_VFS_FILE_OBJECT_ID, FAT_ROOTFS_VFS_OPEN_OBJECT_ID, "/srv/rootfs_vfs.elf", "[fat_server] FatServer: /srv/rootfs_vfs.elf ready\n", &g_rootfs_vfs_start_cluster, &g_rootfs_vfs_size_bytes, 0, 0 },
    { FAT_STARTUP_MANIFEST_FILE_OBJECT_ID, FAT_STARTUP_MANIFEST_OPEN_OBJECT_ID, "/sys/startup_manifest.txt", "[fat_server] FatServer: /sys/startup_manifest.txt ready\n", &g_startup_manifest_start_cluster, &g_startup_manifest_size_bytes, 0, 0 },
};

struct fat_dir_entry_view {
    char name[128];
    u16 name_len;
    u8 attr;
    u32 first_cluster;
    u32 size_bytes;
};

struct fat_dynamic_object {
    u8 used;
    u8 attr;
    u16 path_len;
    u32 first_cluster;
    u32 size_bytes;
    u32 cached_cluster_index;
    u32 cached_cluster;
    char path[128];
};

static struct fat_dynamic_object g_dynamic_objects[FAT_MAX_DYNAMIC_OBJECTS];
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
        : "a"((u64)SYSCALL_LOG), "D"((u64)message), "S"(len)
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

static u64 syscall0(u64 nr) {
    u64 ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(nr)
        : "rcx", "rdx", "r8", "r9", "r10", "r11", "memory");
    return ret;
}

static u64 syscall3(u64 nr, u64 arg0, u64 arg1, u64 arg2) {
    u64 ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(nr), "D"(arg0), "S"(arg1), "d"(arg2)
        : "rcx", "r8", "r9", "r10", "r11", "memory");
    return ret;
}

static u64 wait_event(void) {
    return syscall2(SYSCALL_WAIT_EVENT, 1, 1);
}

static u64 wait_event_poll(void) {
    return syscall2(SYSCALL_WAIT_EVENT, 0, 1);
}

static void clear_page(u64 va) {
    volatile u64 *p = (volatile u64 *)va;
    for (u64 i = 0; i < 512; i++) p[i] = 0;
}

static int is_cap_token(u64 token) {
    return (token & FAT_TOKEN_TAG) != 0 && (token & ~FAT_TOKEN_TAG) != 0;
}

static u64 root_token(void) {
    return FAT_TOKEN_TAG | FAT_ROOT_OBJECT_ID;
}

static u64 token_from_object_id(u64 object_id) {
    return FAT_TOKEN_TAG | object_id;
}

static u64 object_id_from_token(u64 token) {
    if ((token & FAT_TOKEN_TAG) == 0) return 0;
    const u64 object_id = token & ~FAT_TOKEN_TAG;
    if (object_id == FAT_ROOT_OBJECT_ID ||
        object_id == FAT_PROBE_OBJECT_ID ||
        object_id == FAT_SEED2_FILE_OBJECT_ID ||
        object_id == FAT_SEED2_OPEN_OBJECT_ID ||
        object_id == FAT_ROOTFS_VFS_FILE_OBJECT_ID ||
        object_id == FAT_ROOTFS_VFS_OPEN_OBJECT_ID ||
        object_id == FAT_STARTUP_MANIFEST_FILE_OBJECT_ID ||
        object_id == FAT_STARTUP_MANIFEST_OPEN_OBJECT_ID) return object_id;
    if (object_id >= FAT_DYNAMIC_OBJECT_ID_BASE &&
        object_id < FAT_DYNAMIC_OBJECT_ID_BASE + FAT_MAX_DYNAMIC_OBJECTS)
        return object_id;
    if (object_id >= FAT_DYNAMIC_OPEN_OBJECT_ID_BASE &&
        object_id < FAT_DYNAMIC_OPEN_OBJECT_ID_BASE + FAT_MAX_DYNAMIC_OBJECTS)
        return object_id;
    return 0;
}

static struct fat_dynamic_object *dynamic_object_by_id(u64 object_id);
static int is_dynamic_open_object_id(u64 object_id);

static int is_root_token(u64 token) {
    return token == root_token();
}

static int is_dir_token(u64 token) {
    const u64 object_id = object_id_from_token(token);
    if (object_id == FAT_ROOT_OBJECT_ID || object_id == FAT_PROBE_OBJECT_ID) return 1;
    struct fat_dynamic_object *object = dynamic_object_by_id(object_id);
    return object && !is_dynamic_open_object_id(object_id) && ((object->attr & FAT_ATTR_DIRECTORY) != 0);
}

static int is_root_path(const volatile u8 *path, u16 len) {
    if (len == 0) return 1;
    if (len == 1 && path[0] == '/') return 1;
    if (len == 1 && path[0] == '.') return 1;
    return 0;
}

static int path_equals(const volatile u8 *path, u16 len, const char *name, u16 name_len) {
    if (len != name_len) return 0;
    for (u16 i = 0; i < len; i++) {
        if (path[i] != (u8)name[i]) return 0;
    }
    return 1;
}

static int path_equals_cstr(const volatile u8 *path, u16 len, const char *name) {
    return path_equals(path, len, name, (u16)cstr_len(name));
}

static struct fat_cached_file *cached_file_by_file_object_id(u64 object_id) {
    for (u64 i = 0; i < sizeof(g_cached_files) / sizeof(g_cached_files[0]); i++) {
        if (g_cached_files[i].file_object_id == object_id) return &g_cached_files[i];
    }
    return 0;
}

static struct fat_cached_file *cached_file_by_open_object_id(u64 object_id) {
    for (u64 i = 0; i < sizeof(g_cached_files) / sizeof(g_cached_files[0]); i++) {
        if (g_cached_files[i].open_object_id == object_id) return &g_cached_files[i];
    }
    return 0;
}

static struct fat_cached_file *cached_file_by_any_object_id(u64 object_id) {
    struct fat_cached_file *file = cached_file_by_file_object_id(object_id);
    if (file) return file;
    return cached_file_by_open_object_id(object_id);
}

static struct fat_cached_file *cached_file_by_path(const volatile u8 *path, u16 len) {
    for (u64 i = 0; i < sizeof(g_cached_files) / sizeof(g_cached_files[0]); i++) {
        if (path_equals_cstr(path, len, g_cached_files[i].path)) return &g_cached_files[i];
        if (len > 0 && path[0] != '/' && path_equals_cstr(path, len, g_cached_files[i].path + 1)) return &g_cached_files[i];
    }
    return 0;
}

static int cached_file_ready(const struct fat_cached_file *file) {
    return file && *file->start_cluster >= 2 && *file->size_bytes != 0;
}

static u64 dynamic_slot_from_object_id(u64 object_id) {
    if (object_id >= FAT_DYNAMIC_OBJECT_ID_BASE &&
        object_id < FAT_DYNAMIC_OBJECT_ID_BASE + FAT_MAX_DYNAMIC_OBJECTS)
        return object_id - FAT_DYNAMIC_OBJECT_ID_BASE;
    if (object_id >= FAT_DYNAMIC_OPEN_OBJECT_ID_BASE &&
        object_id < FAT_DYNAMIC_OPEN_OBJECT_ID_BASE + FAT_MAX_DYNAMIC_OBJECTS)
        return object_id - FAT_DYNAMIC_OPEN_OBJECT_ID_BASE;
    return FAT_MAX_DYNAMIC_OBJECTS;
}

static int is_dynamic_open_object_id(u64 object_id) {
    return object_id >= FAT_DYNAMIC_OPEN_OBJECT_ID_BASE &&
        object_id < FAT_DYNAMIC_OPEN_OBJECT_ID_BASE + FAT_MAX_DYNAMIC_OBJECTS;
}

static struct fat_dynamic_object *dynamic_object_by_id(u64 object_id) {
    const u64 slot = dynamic_slot_from_object_id(object_id);
    if (slot >= FAT_MAX_DYNAMIC_OBJECTS || !g_dynamic_objects[slot].used) return 0;
    return &g_dynamic_objects[slot];
}

static u64 intern_dynamic_object(const char *path, u16 path_len, const struct fat_dir_entry_view *entry) {
    if (path_len == 0 || path_len >= 128) return 0;
    for (u64 i = 0; i < FAT_MAX_DYNAMIC_OBJECTS; i++) {
        if (!g_dynamic_objects[i].used || g_dynamic_objects[i].path_len != path_len) continue;
        int same = 1;
        for (u16 j = 0; j < path_len; j++) {
            if (g_dynamic_objects[i].path[j] != path[j]) {
                same = 0;
                break;
            }
        }
        if (same) return FAT_DYNAMIC_OBJECT_ID_BASE + i;
    }
    for (u64 i = 0; i < FAT_MAX_DYNAMIC_OBJECTS; i++) {
        if (g_dynamic_objects[i].used) continue;
        g_dynamic_objects[i].used = 1;
        g_dynamic_objects[i].attr = entry->attr;
        g_dynamic_objects[i].path_len = path_len;
        g_dynamic_objects[i].first_cluster = entry->first_cluster;
        g_dynamic_objects[i].size_bytes = entry->size_bytes;
        g_dynamic_objects[i].cached_cluster_index = 0;
        g_dynamic_objects[i].cached_cluster = entry->first_cluster;
        for (u16 j = 0; j < path_len; j++) g_dynamic_objects[i].path[j] = path[j];
        g_dynamic_objects[i].path[path_len] = 0;
        return FAT_DYNAMIC_OBJECT_ID_BASE + i;
    }
    return 0;
}

static u16 load_le16(const u8 *p) {
    return (u16)p[0] | ((u16)p[1] << 8);
}

static u32 load_le32(const u8 *p) {
    return (u32)p[0] |
        ((u32)p[1] << 8) |
        ((u32)p[2] << 16) |
        ((u32)p[3] << 24);
}

static int parse_fat32_bpb(void) {
    if (g_block.block_size < 512) return 0;
    if (g_volume_sector0[510] != 0x55 || g_volume_sector0[511] != 0xAA) return 0;

    const u16 bytes_per_sector = load_le16(&g_volume_sector0[11]);
    const u8 sectors_per_cluster = g_volume_sector0[13];
    const u16 reserved_sector_count = load_le16(&g_volume_sector0[14]);
    const u8 num_fats = g_volume_sector0[16];
    const u16 root_entry_count = load_le16(&g_volume_sector0[17]);
    const u16 total_sectors_16 = load_le16(&g_volume_sector0[19]);
    const u16 fat_size_16 = load_le16(&g_volume_sector0[22]);
    const u32 total_sectors_32 = load_le32(&g_volume_sector0[32]);
    const u32 fat_size_32 = load_le32(&g_volume_sector0[36]);
    const u32 root_cluster = load_le32(&g_volume_sector0[44]);

    if (bytes_per_sector != 512 && bytes_per_sector != 1024 && bytes_per_sector != 2048 && bytes_per_sector != 4096) return 0;
    if (bytes_per_sector != g_block.block_size) return 0;
    if (sectors_per_cluster == 0 || (sectors_per_cluster & (sectors_per_cluster - 1)) != 0) return 0;
    if (reserved_sector_count == 0 || num_fats == 0) return 0;
    if (root_entry_count != 0) return 0;
    if (total_sectors_16 != 0) return 0;
    if (fat_size_16 != 0) return 0;
    if (total_sectors_32 == 0 || fat_size_32 == 0 || root_cluster < 2) return 0;

    const u32 first_fat_sector = reserved_sector_count;
    const u32 first_data_sector = reserved_sector_count + (u32)num_fats * fat_size_32;
    if (first_data_sector >= total_sectors_32) return 0;

    g_bpb.valid = 1;
    g_bpb.sectors_per_cluster = sectors_per_cluster;
    g_bpb.num_fats = num_fats;
    g_bpb.bytes_per_sector = bytes_per_sector;
    g_bpb.reserved_sector_count = reserved_sector_count;
    g_bpb.fat_size_sectors = fat_size_32;
    g_bpb.root_cluster = root_cluster;
    g_bpb.first_fat_sector = first_fat_sector;
    g_bpb.first_data_sector = first_data_sector;
    g_bpb.total_sectors = total_sectors_32;
    return 1;
}

static int find_block_service(u64 *endpoint_id, u64 *process_slot) {
    volatile struct service_registry_page *registry = (volatile struct service_registry_page *)FAT_SERVICE_REGISTRY_VA;
    if (registry->magic != SERVICE_REGISTRY_MAGIC || registry->version != SERVICE_REGISTRY_VERSION) return 0;
    u64 count = registry->entry_count;
    if (count > SERVICE_REGISTRY_MAX_ENTRIES) count = SERVICE_REGISTRY_MAX_ENTRIES;
    for (u64 i = 0; i < count; i++) {
        volatile struct service_entry *entry = &registry->entries[i];
        if (entry->kind != SERVICE_KIND_BLOCK) continue;
        if (entry->endpoint_id == 0) return 0;
        if ((entry->flags & SERVICE_FLAG_PROCESS_SLOT_COMPAT) == 0 || entry->process_slot == 0) return 0;
        *endpoint_id = entry->endpoint_id;
        *process_slot = entry->process_slot;
        return 1;
    }
    return 0;
}

static u64 make_block_session_nonce(u64 request_paddr, u64 response_paddr, u64 endpoint_id, u64 process_slot) {
    u64 nonce = request_paddr ^
        ((response_paddr << 17) | (response_paddr >> 47)) ^
        ((endpoint_id << 7) | (endpoint_id >> 57)) ^
        process_slot ^
        0x517cc1b727220a95ULL;
    return nonce == 0 ? 1 : nonce;
}

static int install_block_endpoint(void) {
    if (g_block.endpoint_id == 0 || g_block.process_slot == 0) return 0;
    return syscall3(SYSCALL_INSTALL_ENDPOINT, 0, g_block.endpoint_id, g_block.process_slot) == SYSCALL_OK;
}

static int grant_block_response_page(void) {
    u64 ret = syscall3(
        SYSCALL_GRANT_CAP_ON_ENDPOINT,
        g_block.response_paddr,
        g_block.endpoint_id,
        PAGE_RIGHT_CPU_READ | PAGE_RIGHT_CPU_WRITE
    );
    if (ret == SYSCALL_OK) return 1;
    if (ret == SYSCALL_ERR_ENDPOINT && install_block_endpoint()) {
        ret = syscall3(
            SYSCALL_GRANT_CAP_ON_ENDPOINT,
            g_block.response_paddr,
            g_block.endpoint_id,
            PAGE_RIGHT_CPU_READ | PAGE_RIGHT_CPU_WRITE
        );
    }
    return ret == SYSCALL_OK;
}

static int share_block_request_page(void) {
    u64 ret = syscall2(SYSCALL_SHARE_CAP, g_block.request_paddr, g_block.endpoint_id);
    if (ret == SYSCALL_OK) return 1;
    if (ret == SYSCALL_ERR_ENDPOINT && install_block_endpoint()) {
        ret = syscall2(SYSCALL_SHARE_CAP, g_block.request_paddr, g_block.endpoint_id);
    }
    return ret == SYSCALL_OK;
}

static int signal_block_endpoint(void) {
    u64 ret = syscall2(SYSCALL_SIGNAL_ENDPOINT, g_block.endpoint_id, 0);
    if (ret == SYSCALL_OK) return 1;
    if (ret == SYSCALL_ERR_ENDPOINT && install_block_endpoint()) {
        ret = syscall2(SYSCALL_SIGNAL_ENDPOINT, g_block.endpoint_id, 0);
    }
    return ret == SYSCALL_OK;
}

static int wait_block_response(u64 expected_seq, u16 expected_op) {
    volatile struct block_response_header *response = (volatile struct block_response_header *)FAT_BLOCK_RESPONSE_VA;
    for (u64 i = 0; i < 512; i++) {
        if (response->response_seq == expected_seq) {
            return response->magic == BLOCK_RESPONSE_MAGIC &&
                response->version == BLOCK_PROTOCOL_VERSION &&
                response->op == expected_op;
        }
        (void)wait_event_poll();
    }
    return 0;
}

static int connect_block_service(void) {
    u64 endpoint_id = 0;
    u64 process_slot = 0;
    if (!find_block_service(&endpoint_id, &process_slot)) {
        user_log("[fat_server] FatServer: block service missing\n");
        return 0;
    }

    g_block.endpoint_id = endpoint_id;
    g_block.process_slot = process_slot;
    g_block.request_va = FAT_BLOCK_REQUEST_VA;
    g_block.response_va = FAT_BLOCK_RESPONSE_VA;
    g_block.request_paddr = syscall0(SYSCALL_ALLOC_PAGE);
    g_block.response_paddr = syscall0(SYSCALL_ALLOC_PAGE);
    if (g_block.request_paddr < 0x1000 || g_block.response_paddr < 0x1000) return 0;
    if (syscall3(SYSCALL_MAP_PAGE, FAT_BLOCK_REQUEST_VA, g_block.request_paddr, 1) != SYSCALL_OK) return 0;
    if (syscall3(SYSCALL_MAP_PAGE, FAT_BLOCK_RESPONSE_VA, g_block.response_paddr, 1) != SYSCALL_OK) return 0;
    if (!grant_block_response_page()) return 0;

    clear_page(FAT_BLOCK_REQUEST_VA);
    clear_page(FAT_BLOCK_RESPONSE_VA);
    const u64 self_process_slot = syscall0(SYSCALL_GET_PROCESS_SLOT);
    g_block.session_nonce = make_block_session_nonce(
        g_block.request_paddr,
        g_block.response_paddr,
        g_block.endpoint_id,
        self_process_slot
    );

    volatile struct block_request_header *request = (volatile struct block_request_header *)FAT_BLOCK_REQUEST_VA;
    request->magic = BLOCK_REQUEST_MAGIC;
    request->version = BLOCK_PROTOCOL_VERSION;
    request->op = BLOCK_OP_CONNECT;
    request->object_token = 0;
    request->block_index = 0;
    request->block_count = 0;
    request->flags = 0;
    request->inline_bytes = 0;
    request->reserved0 = 0;
    request->reserved1 = 0;
    request->arg0 = g_block.response_paddr;
    request->arg1 = self_process_slot;
    request->session_nonce = g_block.session_nonce;
    __asm__ volatile("" ::: "memory");
    request->request_seq = 1;

    if (!share_block_request_page()) return 0;
    if (!wait_block_response(1, BLOCK_OP_CONNECT)) return 0;

    volatile struct block_response_header *response = (volatile struct block_response_header *)FAT_BLOCK_RESPONSE_VA;
    if (response->status != BLOCK_STATUS_OK || !is_cap_token(response->result_token)) return 0;
    if (response->arg0 == 0 || response->arg1 == 0) return 0;
    g_block.root_token = response->result_token;
    g_block.block_size = response->arg0;
    g_block.capacity_blocks = response->arg1;
    g_block.next_seq = 2;
    g_block.active = 1;
    user_log("[fat_server] FatServer: block connect ok\n");
    return 1;
}

static int read_volume_sector0_probe(void) {
    if (!g_block.active || g_block.block_size == 0 || g_block.block_size > BLOCK_RESPONSE_PAYLOAD_BYTES) return 0;
    clear_page(FAT_BLOCK_REQUEST_VA);
    clear_page(FAT_BLOCK_RESPONSE_VA);
    const u64 seq = g_block.next_seq++;
    volatile struct block_request_header *request = (volatile struct block_request_header *)FAT_BLOCK_REQUEST_VA;
    request->magic = BLOCK_REQUEST_MAGIC;
    request->version = BLOCK_PROTOCOL_VERSION;
    request->op = BLOCK_OP_READ_BLOCKS;
    request->object_token = g_block.root_token;
    request->block_index = g_volume_start_block;
    request->block_count = 1;
    request->flags = 0;
    request->inline_bytes = 0;
    request->reserved0 = 0;
    request->reserved1 = 0;
    request->arg0 = 0;
    request->arg1 = 0;
    request->session_nonce = g_block.session_nonce;
    __asm__ volatile("" ::: "memory");
    request->request_seq = seq;

    if (!signal_block_endpoint()) return 0;
    if (!wait_block_response(seq, BLOCK_OP_READ_BLOCKS)) return 0;

    volatile struct block_response_header *response = (volatile struct block_response_header *)FAT_BLOCK_RESPONSE_VA;
    if (response->status != BLOCK_STATUS_OK) return 0;
    if (response->inline_bytes != g_block.block_size) return 0;
    volatile u8 *payload = (volatile u8 *)(FAT_BLOCK_RESPONSE_VA + BLOCK_RESPONSE_HEADER_BYTES);
    for (u64 i = 0; i < g_block.block_size; i++) g_volume_sector0[i] = payload[i];
    user_log("[fat_server] FatServer: volume sector0 read ok\n");
    return 1;
}

static FAT_NOINLINE_NOOPT void copy_block_payload_to_sector(u8 *out, u64 copy_bytes) {
    const volatile u8 *payload = (const volatile u8 *)(FAT_BLOCK_RESPONSE_VA + BLOCK_RESPONSE_HEADER_BYTES);
    for (u64 i = 0; i < copy_bytes; i++) {
        out[i] = payload[i];
    }
}

static u32 cluster_to_sector(u32 cluster) {
    if (cluster < 2) return 0;
    return g_bpb.first_data_sector + (cluster - 2) * (u32)g_bpb.sectors_per_cluster;
}

static int fat_cluster_is_eoc(u32 cluster) {
    return cluster >= 0x0FFFFFF8u;
}

static int fat_cluster_valid(u32 cluster) {
    return cluster >= 2 && cluster < 0x0FFFFFF0u;
}

static FAT_NOINLINE_NOOPT int read_volume_sector(u32 sector, u8 *out) {
    if (!g_block.active || g_block.block_size == 0 || g_block.block_size > BLOCK_RESPONSE_PAYLOAD_BYTES) return 0;
    const u64 copy_bytes = g_bpb.valid ? (u64)g_bpb.bytes_per_sector : g_block.block_size;
    if (copy_bytes == 0 || copy_bytes > BLOCK_RESPONSE_PAYLOAD_BYTES) return 0;
    clear_page(FAT_BLOCK_REQUEST_VA);
    clear_page(FAT_BLOCK_RESPONSE_VA);
    const u64 seq = g_block.next_seq++;
    volatile struct block_request_header *request = (volatile struct block_request_header *)FAT_BLOCK_REQUEST_VA;
    request->magic = BLOCK_REQUEST_MAGIC;
    request->version = BLOCK_PROTOCOL_VERSION;
    request->op = BLOCK_OP_READ_BLOCKS;
    request->object_token = g_block.root_token;
    request->block_index = g_volume_start_block + sector;
    request->block_count = 1;
    request->flags = 0;
    request->inline_bytes = 0;
    request->reserved0 = 0;
    request->reserved1 = 0;
    request->arg0 = 0;
    request->arg1 = 0;
    request->session_nonce = g_block.session_nonce;
    __asm__ volatile("" ::: "memory");
    request->request_seq = seq;

    if (!signal_block_endpoint()) return 0;
    if (!wait_block_response(seq, BLOCK_OP_READ_BLOCKS)) return 0;

    volatile struct block_response_header *response = (volatile struct block_response_header *)FAT_BLOCK_RESPONSE_VA;
    if (response->status != BLOCK_STATUS_OK || response->inline_bytes < copy_bytes || response->inline_bytes > BLOCK_RESPONSE_PAYLOAD_BYTES) return 0;
    copy_block_payload_to_sector(out, copy_bytes);
    return 1;
}

static int read_fat_next_cluster(u32 cluster, u32 *next_out) {
    if (!g_bpb.valid || !fat_cluster_valid(cluster)) return 0;
    const u32 fat_offset = cluster * 4u;
    const u32 fat_sector = g_bpb.first_fat_sector + (fat_offset / g_bpb.bytes_per_sector);
    const u32 fat_sector_offset = fat_offset % g_bpb.bytes_per_sector;
    if (fat_sector_offset + 4 > g_bpb.bytes_per_sector) return 0;
    if (!read_volume_sector(fat_sector, g_sector_scratch)) return 0;
    const u32 next = load_le32(&g_sector_scratch[fat_sector_offset]) & 0x0FFFFFFFu;
    if (fat_cluster_is_eoc(next)) {
        *next_out = 0;
        return 1;
    }
    if (!fat_cluster_valid(next)) return 0;
    *next_out = next;
    return 1;
}

static int seek_cluster_cached(
    u32 start_cluster,
    u32 target_index,
    u32 *cached_index,
    u32 *cached_cluster,
    u32 *cluster_out
) {
    if (!fat_cluster_valid(start_cluster)) return 0;
    u32 index = 0;
    u32 cluster = start_cluster;
    if (cached_index && cached_cluster && fat_cluster_valid(*cached_cluster) && *cached_index <= target_index) {
        index = *cached_index;
        cluster = *cached_cluster;
    }
    while (index < target_index) {
        u32 next = 0;
        if (!read_fat_next_cluster(cluster, &next)) return 0;
        if (next == 0) return 0;
        cluster = next;
        index++;
    }
    if (cached_index && cached_cluster) {
        *cached_index = index;
        *cached_cluster = cluster;
    }
    *cluster_out = cluster;
    return 1;
}

static int lfn_char_eq(u8 actual, u8 expected) {
    if (actual >= 'A' && actual <= 'Z') actual = (u8)(actual + 32);
    if (expected >= 'A' && expected <= 'Z') expected = (u8)(expected + 32);
    return actual == expected;
}

static u16 read_lfn_one_entry(const u8 *entry, u8 *name, u16 base, u16 max_len) {
    static const u8 offsets[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
    u16 end = base;
    for (u16 i = 0; i < 13 && base + i < max_len; i++) {
        const u16 ch = load_le16(&entry[offsets[i]]);
        if (ch == 0 || ch == 0xFFFF) break;
        name[base + i] = (u8)ch;
        end = base + i + 1;
    }
    return end;
}

static int name_equals(const u8 *actual, u16 actual_len, const char *expected) {
    const u16 expected_len = (u16)cstr_len(expected);
    if (actual_len != expected_len) return 0;
    for (u16 i = 0; i < actual_len; i++) {
        if (!lfn_char_eq(actual[i], (u8)expected[i])) return 0;
    }
    return 1;
}

static u16 read_short_name(const u8 *entry, char *out, u16 out_len) {
    u16 len = 0;
    for (u16 i = 0; i < 8 && entry[i] != ' '; i++) {
        if (len + 1 >= out_len) return 0;
        u8 ch = entry[i];
        if (ch >= 'A' && ch <= 'Z') ch = (u8)(ch + 32);
        out[len++] = (char)ch;
    }
    if (entry[8] != ' ') {
        if (len + 1 >= out_len) return 0;
        out[len++] = '.';
        for (u16 i = 8; i < 11 && entry[i] != ' '; i++) {
            if (len + 1 >= out_len) return 0;
            u8 ch = entry[i];
            if (ch >= 'A' && ch <= 'Z') ch = (u8)(ch + 32);
            out[len++] = (char)ch;
        }
    }
    out[len] = 0;
    return len;
}

static void fill_dir_entry_view(const u8 *entry, const u8 *lfn_name, u16 lfn_len, struct fat_dir_entry_view *out) {
    const u32 hi = load_le16(&entry[20]);
    const u32 lo = load_le16(&entry[26]);
    out->attr = entry[11];
    out->first_cluster = (hi << 16) | lo;
    out->size_bytes = load_le32(&entry[28]);
    if (lfn_len != 0) {
        u16 name_len = lfn_len;
        if (name_len >= sizeof(out->name)) name_len = sizeof(out->name) - 1;
        for (u16 i = 0; i < name_len; i++) out->name[i] = (char)lfn_name[i];
        out->name[name_len] = 0;
        out->name_len = name_len;
    } else {
        out->name_len = read_short_name(entry, out->name, sizeof(out->name));
    }
}

static int read_dir_visible_entry(u32 dir_cluster, u64 cursor, struct fat_dir_entry_view *out) {
    if (!g_bpb.valid || dir_cluster < 2) return 0;
    u8 lfn_name[128];
    u16 lfn_len = 0;
    u64 visible_index = 0;
    u32 cluster = dir_cluster;
    for (u32 cluster_guard = 0; cluster_guard < 65536 && fat_cluster_valid(cluster); cluster_guard++) {
        const u32 first_sector = cluster_to_sector(cluster);
        for (u32 sector_offset = 0; sector_offset < g_bpb.sectors_per_cluster; sector_offset++) {
            if (!read_volume_sector(first_sector + sector_offset, g_sector_scratch)) return 0;
            for (u32 off = 0; off < g_bpb.bytes_per_sector; off += 32) {
                const u8 *entry = &g_sector_scratch[off];
                if (entry[0] == 0x00) return 0;
                if (entry[0] == 0xE5) {
                    lfn_len = 0;
                    continue;
                }
                const u8 attr = entry[11];
                if (attr == FAT_ATTR_LONG_NAME) {
                    const u8 seq = entry[0] & 0x1F;
                    if ((entry[0] & 0x40) != 0) lfn_len = 0;
                    if (seq != 0) {
                        const u16 end = read_lfn_one_entry(entry, lfn_name, (u16)(seq - 1) * 13, sizeof(lfn_name));
                        if (end > lfn_len) lfn_len = end;
                    }
                    continue;
                }
                if ((attr & 0x08) != 0) {
                    lfn_len = 0;
                    continue;
                }
                if (visible_index == cursor) {
                    fill_dir_entry_view(entry, lfn_name, lfn_len, out);
                    return out->name_len != 0;
                }
                visible_index++;
                lfn_len = 0;
            }
        }
        u32 next = 0;
        if (!read_fat_next_cluster(cluster, &next)) return 0;
        if (next == 0) return 0;
        cluster = next;
    }
    return 0;
}

static int find_child_in_directory(u32 dir_cluster, const char *name, struct fat_dir_entry_view *out) {
    for (u64 cursor = 0; cursor < 4096; cursor++) {
        struct fat_dir_entry_view entry;
        if (!read_dir_visible_entry(dir_cluster, cursor, &entry)) return 0;
        if (name_equals((const u8 *)entry.name, entry.name_len, name)) {
            *out = entry;
            return 1;
        }
    }
    return 0;
}

static int resolve_path_cstr(const char *path, u32 *cluster_out, u32 *size_out, u8 *attr_out) {
    if (!g_bpb.valid || !path || path[0] != '/') return 0;
    u32 dir_cluster = g_bpb.root_cluster;
    u32 found_cluster = dir_cluster;
    u32 found_size = 0;
    u8 found_attr = 0x10;
    u64 pos = 1;

    while (path[pos] != 0) {
        while (path[pos] == '/') pos++;
        if (path[pos] == 0) break;

        char component[128];
        u16 component_len = 0;
        while (path[pos] != 0 && path[pos] != '/') {
            if (component_len + 1 >= sizeof(component)) return 0;
            component[component_len++] = path[pos++];
        }
        component[component_len] = 0;

        struct fat_dir_entry_view entry;
        if (!find_child_in_directory(dir_cluster, component, &entry)) return 0;
        found_cluster = entry.first_cluster;
        found_size = entry.size_bytes;
        found_attr = entry.attr;
        while (path[pos] == '/') pos++;
        if (path[pos] != 0) {
            if ((found_attr & FAT_ATTR_DIRECTORY) == 0 || found_cluster < 2) return 0;
            dir_cluster = found_cluster;
        }
    }

    *cluster_out = found_cluster;
    *size_out = found_size;
    *attr_out = found_attr;
    return 1;
}

static int build_lookup_path(u64 base_token, const volatile u8 *path, u16 len, char *out, u16 out_len, u16 *written_out) {
    const u64 base_object_id = object_id_from_token(base_token);
    u16 written = 0;
    if (len == 0 || (len == 1 && (path[0] == '/' || path[0] == '.'))) {
        if (base_object_id == FAT_ROOT_OBJECT_ID) {
            out[0] = '/';
            out[1] = 0;
            *written_out = 1;
            return 1;
        }
        struct fat_dynamic_object *base = dynamic_object_by_id(base_object_id);
        if (!base || (base->attr & FAT_ATTR_DIRECTORY) == 0) return 0;
        for (u16 i = 0; i < base->path_len; i++) out[i] = base->path[i];
        out[base->path_len] = 0;
        *written_out = base->path_len;
        return 1;
    }

    if (len > 0 && path[0] == '/') {
        if (len >= out_len) return 0;
        for (u16 i = 0; i < len; i++) out[i] = (char)path[i];
        out[len] = 0;
        *written_out = len;
        return 1;
    }

    if (base_object_id == FAT_ROOT_OBJECT_ID) {
        if ((u32)len + 1 >= out_len) return 0;
        out[written++] = '/';
    } else {
        struct fat_dynamic_object *base = dynamic_object_by_id(base_object_id);
        if (!base || (base->attr & FAT_ATTR_DIRECTORY) == 0) return 0;
        if ((u32)base->path_len + 1 + len >= out_len) return 0;
        for (u16 i = 0; i < base->path_len; i++) out[written++] = base->path[i];
        if (written == 0 || out[written - 1] != '/') out[written++] = '/';
    }

    for (u16 i = 0; i < len; i++) out[written++] = (char)path[i];
    out[written] = 0;
    *written_out = written;
    return 1;
}

static u64 lookup_dynamic_path(u64 base_token, const volatile u8 *path, u16 len) {
    char full_path[128];
    u16 full_len = 0;
    if (!build_lookup_path(base_token, path, len, full_path, sizeof(full_path), &full_len)) return 0;
    if (full_len == 1 && full_path[0] == '/') return FAT_ROOT_OBJECT_ID;

    u32 cluster = 0;
    u32 size_bytes = 0;
    u8 attr = 0;
    if (!resolve_path_cstr(full_path, &cluster, &size_bytes, &attr)) return 0;

    struct fat_dir_entry_view entry;
    entry.attr = attr;
    entry.first_cluster = cluster;
    entry.size_bytes = size_bytes;
    entry.name_len = 0;
    return intern_dynamic_object(full_path, full_len, &entry);
}

static int probe_cached_file(struct fat_cached_file *file) {
    u32 cluster = 0;
    u32 size_bytes = 0;
    u8 attr = 0;
    if (!resolve_path_cstr(file->path, &cluster, &size_bytes, &attr)) return 0;
    if ((attr & FAT_ATTR_DIRECTORY) != 0 || cluster < 2 || size_bytes == 0) return 0;
    *file->start_cluster = cluster;
    *file->size_bytes = size_bytes;
    file->cached_cluster_index = 0;
    file->cached_cluster = cluster;
    struct fat_dir_entry_view entry;
    entry.attr = attr;
    entry.first_cluster = cluster;
    entry.size_bytes = size_bytes;
    entry.name_len = 0;
    (void)intern_dynamic_object(file->path, (u16)cstr_len(file->path), &entry);
    user_log(file->ready_log);
    return 1;
}

static FAT_NOINLINE_NOOPT u16 read_file_payload(
    u32 start_cluster,
    u32 size_bytes,
    u64 offset,
    u32 length,
    u32 *cached_cluster_index,
    u32 *cached_cluster
) {
    if (start_cluster < 2 || offset >= size_bytes) return 0;
    u64 remaining_file = (u64)size_bytes - offset;
    u32 bytes = length;
    if ((u64)bytes > remaining_file) bytes = (u32)remaining_file;
    if (bytes > FS_RESPONSE_PAYLOAD_BYTES) bytes = FS_RESPONSE_PAYLOAD_BYTES;
    if (bytes == 0) return 0;

    const u32 cluster_bytes = (u32)g_bpb.bytes_per_sector * (u32)g_bpb.sectors_per_cluster;
    u64 file_pos = offset;
    u32 copied = 0;
    volatile u8 *dst = (volatile u8 *)(FAT_RESPONSE_VA + FS_RESPONSE_HEADER_BYTES);
    while (copied < bytes) {
        const u32 cluster_index = (u32)(file_pos / cluster_bytes);
        const u32 within_cluster = (u32)(file_pos % cluster_bytes);
        const u32 sector_in_cluster = within_cluster / g_bpb.bytes_per_sector;
        const u32 within_sector = within_cluster % g_bpb.bytes_per_sector;
        u32 cluster = 0;
        if (!seek_cluster_cached(start_cluster, cluster_index, cached_cluster_index, cached_cluster, &cluster)) break;
        const u32 sector = cluster_to_sector(cluster) + sector_in_cluster;
        if (!read_volume_sector(sector, g_sector_scratch)) break;
        u32 chunk = (u32)g_bpb.bytes_per_sector - within_sector;
        if (chunk > bytes - copied) chunk = bytes - copied;
        for (u32 i = 0; i < chunk; i++) dst[copied + i] = g_sector_scratch[within_sector + i];
        copied += chunk;
        file_pos += chunk;
    }
    return (u16)copied;
}

static u16 read_cached_file_payload(struct fat_cached_file *file, u64 offset, u32 length) {
    if (!cached_file_ready(file)) return 0;
    return read_file_payload(
        *file->start_cluster,
        *file->size_bytes,
        offset,
        length,
        &file->cached_cluster_index,
        &file->cached_cluster
    );
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
    volatile struct fs_response_header *response = (volatile struct fs_response_header *)FAT_RESPONSE_VA;
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
    response->arg0 = 0;
    response->arg1 = 0;
    __asm__ volatile("" ::: "memory");
    response->response_seq = seq;
    if (g_session.reply_endpoint_id != 0) {
        (void)syscall2(SYSCALL_SIGNAL_ENDPOINT, g_session.reply_endpoint_id, 0);
    }
}

static void reply_status(u16 op, u64 seq, i32 status) {
    clear_page(FAT_RESPONSE_VA);
    write_response(op, seq, status, 0, 0, 0, FS_OBJECT_NONE, 0);
}

static void reply_root_lookup(u16 op, u64 seq) {
    clear_page(FAT_RESPONSE_VA);
    write_response(op, seq, FS_STATUS_OK, root_token(), 0, 0, FS_OBJECT_MOUNT, 0);
}

static void reply_dir_lookup(u16 op, u64 seq, u64 token) {
    clear_page(FAT_RESPONSE_VA);
    write_response(op, seq, FS_STATUS_OK, token, 0, 0, FS_OBJECT_DIRECTORY, 0);
}

static void reply_dir_stat(u64 seq, u64 token) {
    (void)token;
    clear_page(FAT_RESPONSE_VA);
    volatile struct fs_stat_record *record = (volatile struct fs_stat_record *)(FAT_RESPONSE_VA + FS_RESPONSE_HEADER_BYTES);
    record->object_kind = FS_OBJECT_DIRECTORY;
    record->size_bytes = 0;
    record->mode_bits = FAT_DIR_MODE;
    record->reserved1 = 0;
    record->mtime_unix_sec = 0;
    record->reserved2[0] = 0;
    record->reserved2[1] = 0;
    write_response(FS_OP_STAT, seq, FS_STATUS_OK, 0, 0, 0, FS_OBJECT_DIRECTORY, sizeof(struct fs_stat_record));
}

static void reply_file_stat(u64 seq, const struct fat_cached_file *file) {
    clear_page(FAT_RESPONSE_VA);
    volatile struct fs_stat_record *record = (volatile struct fs_stat_record *)(FAT_RESPONSE_VA + FS_RESPONSE_HEADER_BYTES);
    record->object_kind = FS_OBJECT_FILE;
    record->size_bytes = cached_file_ready(file) ? *file->size_bytes : 0;
    record->mode_bits = FAT_FILE_MODE;
    record->reserved1 = 0;
    record->mtime_unix_sec = 0;
    record->reserved2[0] = 0;
    record->reserved2[1] = 0;
    write_response(FS_OP_STAT, seq, FS_STATUS_OK, 0, record->size_bytes, 0, FS_OBJECT_FILE, sizeof(struct fs_stat_record));
}

static void reply_file_lookup(u16 op, u64 seq, const struct fat_cached_file *file) {
    clear_page(FAT_RESPONSE_VA);
    write_response(op, seq, FS_STATUS_OK, token_from_object_id(file->file_object_id), *file->size_bytes, 0, FS_OBJECT_FILE, 0);
}

static void reply_file_open(u16 op, u64 seq, const struct fat_cached_file *file) {
    clear_page(FAT_RESPONSE_VA);
    write_response(op, seq, FS_STATUS_OK, token_from_object_id(file->open_object_id), *file->size_bytes, 0, FS_OBJECT_OPEN_FILE, 0);
}

static void reply_file_read(u64 seq, struct fat_cached_file *file, u64 offset, u32 length) {
    clear_page(FAT_RESPONSE_VA);
    const u16 bytes = read_cached_file_payload(file, offset, length);
    write_response(FS_OP_READ, seq, FS_STATUS_OK, 0, *file->size_bytes, offset + bytes, FS_OBJECT_OPEN_FILE, bytes);
}

static void reply_dynamic_stat(u64 seq, u64 object_id, struct fat_dynamic_object *object) {
    clear_page(FAT_RESPONSE_VA);
    volatile struct fs_stat_record *record = (volatile struct fs_stat_record *)(FAT_RESPONSE_VA + FS_RESPONSE_HEADER_BYTES);
    const int is_dir = !is_dynamic_open_object_id(object_id) && ((object->attr & FAT_ATTR_DIRECTORY) != 0);
    record->object_kind = is_dir ? FS_OBJECT_DIRECTORY : FS_OBJECT_FILE;
    record->size_bytes = is_dir ? 0 : object->size_bytes;
    record->mode_bits = is_dir ? FAT_DIR_MODE : FAT_FILE_MODE;
    record->reserved1 = 0;
    record->mtime_unix_sec = 0;
    record->reserved2[0] = 0;
    record->reserved2[1] = 0;
    write_response(FS_OP_STAT, seq, FS_STATUS_OK, 0, record->size_bytes, 0, record->object_kind, sizeof(struct fs_stat_record));
}

static void reply_dynamic_lookup(u16 op, u64 seq, u64 object_id, struct fat_dynamic_object *object) {
    clear_page(FAT_RESPONSE_VA);
    const int is_dir = (object->attr & FAT_ATTR_DIRECTORY) != 0;
    write_response(
        op,
        seq,
        FS_STATUS_OK,
        token_from_object_id(object_id),
        is_dir ? 0 : object->size_bytes,
        0,
        is_dir ? FS_OBJECT_DIRECTORY : FS_OBJECT_FILE,
        0
    );
}

static void reply_dynamic_open(u16 op, u64 seq, u64 object_id, struct fat_dynamic_object *object) {
    clear_page(FAT_RESPONSE_VA);
    write_response(op, seq, FS_STATUS_OK, token_from_object_id(FAT_DYNAMIC_OPEN_OBJECT_ID_BASE + (object_id - FAT_DYNAMIC_OBJECT_ID_BASE)), object->size_bytes, 0, FS_OBJECT_OPEN_FILE, 0);
}

static void reply_dynamic_read(u64 seq, struct fat_dynamic_object *object, u64 offset, u32 length) {
    clear_page(FAT_RESPONSE_VA);
    const u16 bytes = read_file_payload(
        object->first_cluster,
        object->size_bytes,
        offset,
        length,
        &object->cached_cluster_index,
        &object->cached_cluster
    );
    write_response(FS_OP_READ, seq, FS_STATUS_OK, 0, object->size_bytes, offset + bytes, FS_OBJECT_OPEN_FILE, bytes);
}

static void reply_readdir(u64 seq, u64 token, u64 cursor) {
    clear_page(FAT_RESPONSE_VA);
    u32 dir_cluster = 0;
    char parent_path[128];
    u16 parent_len = 0;
    const u64 object_id = object_id_from_token(token);

    if (token == root_token()) {
        dir_cluster = g_bpb.root_cluster;
        parent_path[0] = '/';
        parent_path[1] = 0;
        parent_len = 1;
    } else {
        struct fat_dynamic_object *dir = dynamic_object_by_id(object_id);
        if (!dir || (dir->attr & FAT_ATTR_DIRECTORY) == 0) {
            write_response(FS_OP_READDIR, seq, FS_STATUS_NOT_FOUND, 0, 0, cursor, FS_OBJECT_NONE, 0);
            return;
        }
        dir_cluster = dir->first_cluster;
        parent_len = dir->path_len;
        for (u16 i = 0; i < parent_len; i++) parent_path[i] = dir->path[i];
        parent_path[parent_len] = 0;
    }

    struct fat_dir_entry_view entry;
    if (!read_dir_visible_entry(dir_cluster, cursor, &entry)) {
        write_response(FS_OP_READDIR, seq, FS_STATUS_END_OF_DIR, 0, 0, cursor, FS_OBJECT_DIRECTORY, 0);
        return;
    }

    char child_path[128];
    u16 child_len = 0;
    const u16 separator_bytes = (parent_len != 0 && parent_path[parent_len - 1] == '/') ? 0 : 1;
    if ((u32)parent_len + separator_bytes + entry.name_len >= sizeof(child_path)) {
        write_response(FS_OP_READDIR, seq, FS_STATUS_TOO_BIG, 0, 0, cursor, FS_OBJECT_DIRECTORY, 0);
        return;
    }
    for (u16 i = 0; i < parent_len && child_len + 1 < sizeof(child_path); i++) child_path[child_len++] = parent_path[i];
    if (child_len == 0 || child_path[child_len - 1] != '/') child_path[child_len++] = '/';
    for (u16 i = 0; i < entry.name_len && child_len + 1 < sizeof(child_path); i++) child_path[child_len++] = entry.name[i];
    child_path[child_len] = 0;

    const u64 child_object_id = intern_dynamic_object(child_path, child_len, &entry);
    if (child_object_id == 0) {
        write_response(FS_OP_READDIR, seq, FS_STATUS_BUSY, 0, 0, cursor, FS_OBJECT_DIRECTORY, 0);
        return;
    }
    const u8 object_kind = (entry.attr & FAT_ATTR_DIRECTORY) != 0 ? FS_OBJECT_DIRECTORY : FS_OBJECT_FILE;
    volatile struct fs_dirent_record *record = (volatile struct fs_dirent_record *)(FAT_RESPONSE_VA + FS_RESPONSE_HEADER_BYTES);
    record->next_cursor = cursor + 1;
    record->object_kind = object_kind;
    for (u8 i = 0; i < 7; i++) record->reserved0[i] = 0;
    record->name_bytes = entry.name_len;
    record->reserved1 = 0;
    record->reserved2 = 0;
    volatile u8 *payload = (volatile u8 *)(FAT_RESPONSE_VA + FS_RESPONSE_HEADER_BYTES + FS_DIRENT_RECORD_BYTES);
    for (u16 i = 0; i < entry.name_len; i++) payload[i] = (u8)entry.name[i];
    write_response(
        FS_OP_READDIR,
        seq,
        FS_STATUS_OK,
        token_from_object_id(child_object_id),
        entry.size_bytes,
        cursor + 1,
        object_kind,
        (u16)(FS_DIRENT_RECORD_BYTES + entry.name_len)
    );
}

static void handle_fs_request(void) {
    if (!g_session.active) return;
    volatile struct fs_request_header *request = (volatile struct fs_request_header *)FAT_REQUEST_VA;
    if (request->magic != FS_REQUEST_MAGIC || request->version != FS_PROTOCOL_VERSION) return;
    const u64 seq = request->request_seq;
    if (seq == 0 || seq <= g_session.last_completed_seq) return;
    if (request->session_nonce != g_session.session_nonce) return;

    if (request->op == FS_OP_CONNECT) {
        reply_status(FS_OP_CONNECT, seq, FS_STATUS_BUSY);
    } else if (request->op == FS_OP_LOOKUP) {
        if (!is_dir_token(request->object_token)) {
            reply_status(FS_OP_LOOKUP, seq, FS_STATUS_NOT_FOUND);
        } else if (request->path_bytes > FS_MAX_PATH_BYTES) {
            reply_status(FS_OP_LOOKUP, seq, FS_STATUS_INVALID);
        } else {
            const volatile u8 *path = (const volatile u8 *)(FAT_REQUEST_VA + FS_REQUEST_HEADER_BYTES);
            if (is_root_path(path, request->path_bytes)) {
                if (is_root_token(request->object_token)) reply_root_lookup(FS_OP_LOOKUP, seq);
                else reply_dir_lookup(FS_OP_LOOKUP, seq, request->object_token);
            } else {
                const u64 dynamic_id = lookup_dynamic_path(request->object_token, path, request->path_bytes);
                struct fat_dynamic_object *object = dynamic_object_by_id(dynamic_id);
                if (object) {
                    reply_dynamic_lookup(FS_OP_LOOKUP, seq, dynamic_id, object);
                } else {
                    const struct fat_cached_file *file = cached_file_by_path(path, request->path_bytes);
                    if (cached_file_ready(file)) reply_file_lookup(FS_OP_LOOKUP, seq, file);
                    else reply_status(FS_OP_LOOKUP, seq, FS_STATUS_NOT_FOUND);
                }
            }
        }
    } else if (request->op == FS_OP_STAT) {
        const u64 object_id = object_id_from_token(request->object_token);
        const struct fat_cached_file *file = cached_file_by_any_object_id(object_id);
        struct fat_dynamic_object *dynamic = dynamic_object_by_id(object_id);
        if (is_dir_token(request->object_token)) reply_dir_stat(seq, request->object_token);
        else if (cached_file_ready(file)) reply_file_stat(seq, file);
        else if (dynamic) reply_dynamic_stat(seq, object_id, dynamic);
        else reply_status(FS_OP_STAT, seq, FS_STATUS_NOT_FOUND);
    } else if (request->op == FS_OP_READDIR) {
        if (is_dir_token(request->object_token)) reply_readdir(seq, request->object_token, request->offset);
        else reply_status(FS_OP_READDIR, seq, FS_STATUS_NOT_FOUND);
    } else if (request->op == FS_OP_CLOSE) {
        reply_status(FS_OP_CLOSE, seq, FS_STATUS_OK);
    } else if (request->op == FS_OP_STATFS) {
        clear_page(FAT_RESPONSE_VA);
        write_response(FS_OP_STATFS, seq, FS_STATUS_OK, 0, 0, 0, FS_OBJECT_MOUNT, 0);
    } else if (request->op == FS_OP_OPEN || request->op == FS_OP_OPEN_EXEC) {
        const u64 object_id = object_id_from_token(request->object_token);
        const struct fat_cached_file *file = cached_file_by_file_object_id(object_id);
        struct fat_dynamic_object *dynamic = dynamic_object_by_id(object_id);
        if (is_dir_token(request->object_token)) reply_status(request->op, seq, FS_STATUS_IS_DIR);
        else if (cached_file_ready(file)) reply_file_open(request->op, seq, file);
        else if (dynamic && !is_dynamic_open_object_id(object_id) && (dynamic->attr & FAT_ATTR_DIRECTORY) == 0) reply_dynamic_open(request->op, seq, object_id, dynamic);
        else reply_status(request->op, seq, FS_STATUS_NOT_FOUND);
    } else if (request->op == FS_OP_READ) {
        const u64 object_id = object_id_from_token(request->object_token);
        struct fat_cached_file *file = cached_file_by_open_object_id(object_id);
        struct fat_dynamic_object *dynamic = dynamic_object_by_id(object_id);
        if (cached_file_ready(file)) reply_file_read(seq, file, request->offset, request->length);
        else if (dynamic && is_dynamic_open_object_id(object_id)) reply_dynamic_read(seq, dynamic, request->offset, request->length);
        else reply_status(request->op, seq, FS_STATUS_NOT_FOUND);
    } else if (request->op == FS_OP_CREATE || request->op == FS_OP_WRITE || request->op == FS_OP_UNLINK || request->op == FS_OP_RENAME) {
        reply_status(request->op, seq, FS_STATUS_NO_RIGHT);
    } else {
        reply_status(request->op, seq, FS_STATUS_NOT_SUPPORTED);
    }

    g_session.last_completed_seq = seq;
}

static void handle_connect_transfer(u64 transfer_id) {
    const u64 request_paddr = syscall2(SYSCALL_ACCEPT_CAP_TRANSFER, transfer_id, 0);
    if (request_paddr < 0x1000) return;
    if (syscall3(SYSCALL_MAP_PAGE, FAT_REQUEST_VA, request_paddr, 0) != SYSCALL_OK) return;

    volatile struct fs_request_header *request = (volatile struct fs_request_header *)FAT_REQUEST_VA;
    if (request->magic != FS_REQUEST_MAGIC ||
        request->version != FS_PROTOCOL_VERSION ||
        request->op != FS_OP_CONNECT ||
        request->request_seq == 0 ||
        request->arg0 < 0x1000 ||
        request->session_nonce == 0)
    {
        user_log("[fat_server] FatServer: invalid connect request\n");
        return;
    }
    if (syscall3(SYSCALL_MAP_PAGE, FAT_RESPONSE_VA, request->arg0, 1) != SYSCALL_OK) return;

    clear_page(FAT_RESPONSE_VA);
    g_session.active = 1;
    g_session.request_va = FAT_REQUEST_VA;
    g_session.response_va = FAT_RESPONSE_VA;
    g_session.request_paddr = request_paddr;
    g_session.response_paddr = request->arg0;
    g_session.reply_endpoint_id = syscall3(SYSCALL_INSTALL_ENDPOINT, 0, FAT_REPLY_ENDPOINT_ID, request->arg1) == SYSCALL_OK
        ? FAT_REPLY_ENDPOINT_ID
        : 0;
    g_session.session_nonce = request->session_nonce;
    g_session.last_completed_seq = 0;
    g_session.root_token = root_token();
    write_response(FS_OP_CONNECT, request->request_seq, FS_STATUS_OK, g_session.root_token, 0, 0, FS_OBJECT_MOUNT, 0);
    g_session.last_completed_seq = request->request_seq;
    user_log("[fat_server] FatServer: session connect ok\n");
}

void fat_server_main(void) {
    user_log("[fat_server] FatServer: started\n");
    volatile u64 *config = (volatile u64 *)FAT_CONFIG_VA;
    g_endpoint_id = config[0];
    g_volume_start_block = config[3];
    if (connect_block_service()) {
        if (!read_volume_sector0_probe()) {
            user_log("[fat_server] FatServer: volume sector0 read failed\n");
        } else if (parse_fat32_bpb()) {
            user_log("[fat_server] FatServer: fat32 bpb ok\n");
            for (u64 i = 0; i < sizeof(g_cached_files) / sizeof(g_cached_files[0]); i++) {
                if (!probe_cached_file(&g_cached_files[i])) {
                    user_log("[fat_server] FatServer: cached file missing\n");
                    user_log(g_cached_files[i].path);
                    user_log("\n");
                }
            }
        } else {
            user_log("[fat_server] FatServer: fat32 bpb invalid\n");
        }
    } else {
        user_log("[fat_server] FatServer: block connect failed\n");
    }
    if (g_endpoint_id != 0) {
        user_log("[fat_server] FatServer: endpoint ready\n");
        config[2] = 1;
    } else {
        user_log("[fat_server] FatServer: endpoint missing\n");
    }

    for (;;) {
        const u64 received = wait_event();
        if (received >= CAP_TRANSFER_ID_MIN) handle_connect_transfer(received);
        handle_fs_request();
    }
}
