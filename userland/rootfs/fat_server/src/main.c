#include "fs_protocol.h"

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

static const char g_probe_name[] = "fat_probe";
static const u16 g_probe_name_len = 9;

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

static u64 cstr_len(const char *s) {
    u64 n = 0;
    while (s[n] != 0) n++;
    return n;
}

static void user_log_len(const char *message, u64 len) {
    u64 ret;
    __asm__ volatile(
        "syscall"
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
        object_id == FAT_SEED2_OPEN_OBJECT_ID) return object_id;
    return 0;
}

static int is_root_token(u64 token) {
    return token == root_token();
}

static int is_dir_token(u64 token) {
    const u64 object_id = object_id_from_token(token);
    return object_id == FAT_ROOT_OBJECT_ID || object_id == FAT_PROBE_OBJECT_ID;
}

static int is_file_token(u64 token) {
    return object_id_from_token(token) == FAT_SEED2_FILE_OBJECT_ID;
}

static int is_open_file_token(u64 token) {
    return object_id_from_token(token) == FAT_SEED2_OPEN_OBJECT_ID;
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
    volatile struct block_response_header *response = (volatile struct block_response_header *)g_block.response_va;
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
    if (syscall3(SYSCALL_MAP_PAGE, g_block.request_va, g_block.request_paddr, 1) != SYSCALL_OK) return 0;
    if (syscall3(SYSCALL_MAP_PAGE, g_block.response_va, g_block.response_paddr, 1) != SYSCALL_OK) return 0;
    if (!grant_block_response_page()) return 0;

    clear_page(g_block.request_va);
    clear_page(g_block.response_va);
    const u64 self_process_slot = syscall0(SYSCALL_GET_PROCESS_SLOT);
    g_block.session_nonce = make_block_session_nonce(
        g_block.request_paddr,
        g_block.response_paddr,
        g_block.endpoint_id,
        self_process_slot
    );

    volatile struct block_request_header *request = (volatile struct block_request_header *)g_block.request_va;
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

    volatile struct block_response_header *response = (volatile struct block_response_header *)g_block.response_va;
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
    clear_page(g_block.request_va);
    clear_page(g_block.response_va);
    const u64 seq = g_block.next_seq++;
    volatile struct block_request_header *request = (volatile struct block_request_header *)g_block.request_va;
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

    volatile struct block_response_header *response = (volatile struct block_response_header *)g_block.response_va;
    if (response->status != BLOCK_STATUS_OK) return 0;
    if (response->inline_bytes != g_block.block_size) return 0;
    volatile u8 *payload = (volatile u8 *)(g_block.response_va + BLOCK_RESPONSE_HEADER_BYTES);
    for (u64 i = 0; i < g_block.block_size; i++) g_volume_sector0[i] = payload[i];
    user_log("[fat_server] FatServer: volume sector0 read ok\n");
    return 1;
}

static u32 cluster_to_sector(u32 cluster) {
    if (cluster < 2) return 0;
    return g_bpb.first_data_sector + (cluster - 2) * (u32)g_bpb.sectors_per_cluster;
}

static int read_volume_sector(u32 sector, u8 *out) {
    if (!g_block.active || g_block.block_size == 0 || g_block.block_size > BLOCK_RESPONSE_PAYLOAD_BYTES) return 0;
    clear_page(g_block.request_va);
    clear_page(g_block.response_va);
    const u64 seq = g_block.next_seq++;
    volatile struct block_request_header *request = (volatile struct block_request_header *)g_block.request_va;
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

    volatile struct block_response_header *response = (volatile struct block_response_header *)g_block.response_va;
    if (response->status != BLOCK_STATUS_OK || response->inline_bytes != g_block.block_size) return 0;
    volatile u8 *payload = (volatile u8 *)(g_block.response_va + BLOCK_RESPONSE_HEADER_BYTES);
    for (u64 i = 0; i < g_block.block_size; i++) out[i] = payload[i];
    return 1;
}

static int lfn_char_eq(u8 actual, u8 expected) {
    if (actual >= 'A' && actual <= 'Z') actual = (u8)(actual + 32);
    if (expected >= 'A' && expected <= 'Z') expected = (u8)(expected + 32);
    return actual == expected;
}

static u16 read_lfn_one_entry(const u8 *entry, u8 *name, u16 max_len) {
    static const u8 offsets[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
    u16 count = 0;
    for (u16 i = 0; i < 13 && count < max_len; i++) {
        const u16 ch = load_le16(&entry[offsets[i]]);
        if (ch == 0 || ch == 0xFFFF) break;
        name[count++] = (u8)ch;
    }
    return count;
}

static int name_equals(const u8 *actual, u16 actual_len, const char *expected) {
    const u16 expected_len = (u16)cstr_len(expected);
    if (actual_len != expected_len) return 0;
    for (u16 i = 0; i < actual_len; i++) {
        if (!lfn_char_eq(actual[i], (u8)expected[i])) return 0;
    }
    return 1;
}

static int find_child_in_single_cluster(u32 dir_cluster, const char *name, u32 *child_cluster, u32 *size_bytes, u8 *attr_out) {
    if (!g_bpb.valid || dir_cluster < 2) return 0;
    const u32 first_sector = cluster_to_sector(dir_cluster);
    u8 lfn_name[128];
    u16 lfn_len = 0;
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
            if (attr == 0x0F) {
                if ((entry[0] & 0x1F) == 1) lfn_len = read_lfn_one_entry(entry, lfn_name, sizeof(lfn_name));
                continue;
            }
            if (lfn_len != 0 && name_equals(lfn_name, lfn_len, name)) {
                const u32 hi = load_le16(&entry[20]);
                const u32 lo = load_le16(&entry[26]);
                *child_cluster = (hi << 16) | lo;
                *size_bytes = load_le32(&entry[28]);
                *attr_out = attr;
                return 1;
            }
            lfn_len = 0;
        }
    }
    return 0;
}

static int probe_seed2_file(void) {
    u32 sbin_cluster = 0;
    u32 ignored_size = 0;
    u8 attr = 0;
    if (!find_child_in_single_cluster(g_bpb.root_cluster, "sbin", &sbin_cluster, &ignored_size, &attr)) return 0;
    if ((attr & 0x10) == 0) return 0;
    if (!find_child_in_single_cluster(sbin_cluster, "seed2.elf", &g_seed2_start_cluster, &g_seed2_size_bytes, &attr)) return 0;
    if ((attr & 0x10) != 0 || g_seed2_start_cluster < 2 || g_seed2_size_bytes == 0) return 0;
    user_log("[fat_server] FatServer: /sbin/seed2.elf ready\n");
    return 1;
}

static u16 read_seed2_payload(u64 offset, u32 length) {
    if (g_seed2_start_cluster < 2 || offset >= g_seed2_size_bytes) return 0;
    u64 remaining_file = (u64)g_seed2_size_bytes - offset;
    u32 bytes = length;
    if ((u64)bytes > remaining_file) bytes = (u32)remaining_file;
    if (bytes > FS_RESPONSE_PAYLOAD_BYTES) bytes = FS_RESPONSE_PAYLOAD_BYTES;
    if (bytes == 0) return 0;

    const u32 cluster_bytes = (u32)g_bpb.bytes_per_sector * (u32)g_bpb.sectors_per_cluster;
    u64 file_pos = offset;
    u32 copied = 0;
    volatile u8 *dst = (volatile u8 *)(g_session.response_va + FS_RESPONSE_HEADER_BYTES);
    while (copied < bytes) {
        const u32 cluster_index = (u32)(file_pos / cluster_bytes);
        const u32 within_cluster = (u32)(file_pos % cluster_bytes);
        const u32 sector_in_cluster = within_cluster / g_bpb.bytes_per_sector;
        const u32 within_sector = within_cluster % g_bpb.bytes_per_sector;
        const u32 sector = cluster_to_sector(g_seed2_start_cluster + cluster_index) + sector_in_cluster;
        if (!read_volume_sector(sector, g_sector_scratch)) break;
        u32 chunk = (u32)g_bpb.bytes_per_sector - within_sector;
        if (chunk > bytes - copied) chunk = bytes - copied;
        for (u32 i = 0; i < chunk; i++) dst[copied + i] = g_sector_scratch[within_sector + i];
        copied += chunk;
        file_pos += chunk;
    }
    return (u16)copied;
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
    response->arg0 = 0;
    response->arg1 = 0;
    __asm__ volatile("" ::: "memory");
    response->response_seq = seq;
    if (g_session.reply_endpoint_id != 0) {
        (void)syscall2(SYSCALL_SIGNAL_ENDPOINT, g_session.reply_endpoint_id, 0);
    }
}

static void reply_status(u16 op, u64 seq, i32 status) {
    clear_page(g_session.response_va);
    write_response(op, seq, status, 0, 0, 0, FS_OBJECT_NONE, 0);
}

static void reply_root_lookup(u16 op, u64 seq) {
    clear_page(g_session.response_va);
    write_response(op, seq, FS_STATUS_OK, root_token(), 0, 0, FS_OBJECT_MOUNT, 0);
}

static void reply_dir_lookup(u16 op, u64 seq, u64 token) {
    clear_page(g_session.response_va);
    write_response(op, seq, FS_STATUS_OK, token, 0, 0, FS_OBJECT_DIRECTORY, 0);
}

static void reply_dir_stat(u64 seq, u64 token) {
    (void)token;
    clear_page(g_session.response_va);
    volatile struct fs_stat_record *record = (volatile struct fs_stat_record *)(g_session.response_va + FS_RESPONSE_HEADER_BYTES);
    record->object_kind = FS_OBJECT_DIRECTORY;
    record->size_bytes = 0;
    record->mode_bits = FAT_DIR_MODE;
    record->reserved1 = 0;
    record->mtime_unix_sec = 0;
    record->reserved2[0] = 0;
    record->reserved2[1] = 0;
    write_response(FS_OP_STAT, seq, FS_STATUS_OK, 0, 0, 0, FS_OBJECT_DIRECTORY, sizeof(struct fs_stat_record));
}

static void reply_seed2_stat(u64 seq) {
    clear_page(g_session.response_va);
    volatile struct fs_stat_record *record = (volatile struct fs_stat_record *)(g_session.response_va + FS_RESPONSE_HEADER_BYTES);
    record->object_kind = FS_OBJECT_FILE;
    record->size_bytes = g_seed2_size_bytes;
    record->mode_bits = FAT_FILE_MODE;
    record->reserved1 = 0;
    record->mtime_unix_sec = 0;
    record->reserved2[0] = 0;
    record->reserved2[1] = 0;
    write_response(FS_OP_STAT, seq, FS_STATUS_OK, 0, g_seed2_size_bytes, 0, FS_OBJECT_FILE, sizeof(struct fs_stat_record));
}

static void reply_seed2_lookup(u16 op, u64 seq) {
    clear_page(g_session.response_va);
    write_response(op, seq, FS_STATUS_OK, token_from_object_id(FAT_SEED2_FILE_OBJECT_ID), g_seed2_size_bytes, 0, FS_OBJECT_FILE, 0);
}

static void reply_seed2_open(u16 op, u64 seq) {
    clear_page(g_session.response_va);
    write_response(op, seq, FS_STATUS_OK, token_from_object_id(FAT_SEED2_OPEN_OBJECT_ID), g_seed2_size_bytes, 0, FS_OBJECT_OPEN_FILE, 0);
}

static void reply_seed2_read(u64 seq, u64 offset, u32 length) {
    clear_page(g_session.response_va);
    const u16 bytes = read_seed2_payload(offset, length);
    write_response(FS_OP_READ, seq, FS_STATUS_OK, 0, g_seed2_size_bytes, offset + bytes, FS_OBJECT_OPEN_FILE, bytes);
}

static void reply_readdir(u64 seq, u64 token, u64 cursor) {
    clear_page(g_session.response_va);
    if (token != root_token()) {
        write_response(FS_OP_READDIR, seq, FS_STATUS_END_OF_DIR, 0, 0, cursor, FS_OBJECT_DIRECTORY, 0);
        return;
    }
    if (cursor != 0) {
        write_response(FS_OP_READDIR, seq, FS_STATUS_END_OF_DIR, 0, 0, cursor, FS_OBJECT_DIRECTORY, 0);
        return;
    }

    volatile struct fs_dirent_record *record = (volatile struct fs_dirent_record *)(g_session.response_va + FS_RESPONSE_HEADER_BYTES);
    record->next_cursor = 1;
    record->object_kind = FS_OBJECT_DIRECTORY;
    for (u8 i = 0; i < 7; i++) record->reserved0[i] = 0;
    record->name_bytes = g_probe_name_len;
    record->reserved1 = 0;
    record->reserved2 = 0;
    volatile u8 *payload = (volatile u8 *)(g_session.response_va + FS_RESPONSE_HEADER_BYTES + FS_DIRENT_RECORD_BYTES);
    for (u16 i = 0; i < g_probe_name_len; i++) payload[i] = (u8)g_probe_name[i];
    write_response(
        FS_OP_READDIR,
        seq,
        FS_STATUS_OK,
        token_from_object_id(FAT_PROBE_OBJECT_ID),
        0,
        1,
        FS_OBJECT_DIRECTORY,
        (u16)(FS_DIRENT_RECORD_BYTES + g_probe_name_len)
    );
}

static void handle_fs_request(void) {
    if (!g_session.active) return;
    volatile struct fs_request_header *request = (volatile struct fs_request_header *)g_session.request_va;
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
            const volatile u8 *path = (const volatile u8 *)(g_session.request_va + FS_REQUEST_HEADER_BYTES);
            if (is_root_path(path, request->path_bytes)) {
                if (is_root_token(request->object_token)) reply_root_lookup(FS_OP_LOOKUP, seq);
                else reply_dir_lookup(FS_OP_LOOKUP, seq, request->object_token);
            } else if (is_root_token(request->object_token)) {
                const volatile u8 *component = path;
                u16 component_len = request->path_bytes;
                if (component_len > 0 && component[0] == '/') {
                    component++;
                    component_len--;
                }
                if (path_equals(component, component_len, g_probe_name, g_probe_name_len)) {
                    reply_dir_lookup(FS_OP_LOOKUP, seq, token_from_object_id(FAT_PROBE_OBJECT_ID));
                } else if (path_equals(component, component_len, &"/sbin/seed2.elf"[1], 14) && g_seed2_start_cluster != 0) {
                    reply_seed2_lookup(FS_OP_LOOKUP, seq);
                } else if (request->path_bytes == 15 && path_equals(path, request->path_bytes, "/sbin/seed2.elf", 15) && g_seed2_start_cluster != 0) {
                    reply_seed2_lookup(FS_OP_LOOKUP, seq);
                } else {
                    reply_status(FS_OP_LOOKUP, seq, FS_STATUS_NOT_FOUND);
                }
            } else if (path_equals(path, request->path_bytes, g_probe_name, g_probe_name_len)) {
                reply_dir_lookup(FS_OP_LOOKUP, seq, token_from_object_id(FAT_PROBE_OBJECT_ID));
            }
            else reply_status(FS_OP_LOOKUP, seq, FS_STATUS_NOT_FOUND);
        }
    } else if (request->op == FS_OP_STAT) {
        if (is_dir_token(request->object_token)) reply_dir_stat(seq, request->object_token);
        else if (is_file_token(request->object_token) || is_open_file_token(request->object_token)) reply_seed2_stat(seq);
        else reply_status(FS_OP_STAT, seq, FS_STATUS_NOT_FOUND);
    } else if (request->op == FS_OP_READDIR) {
        if (is_dir_token(request->object_token)) reply_readdir(seq, request->object_token, request->offset);
        else reply_status(FS_OP_READDIR, seq, FS_STATUS_NOT_FOUND);
    } else if (request->op == FS_OP_CLOSE) {
        reply_status(FS_OP_CLOSE, seq, FS_STATUS_OK);
    } else if (request->op == FS_OP_STATFS) {
        clear_page(g_session.response_va);
        write_response(FS_OP_STATFS, seq, FS_STATUS_OK, 0, 0, 0, FS_OBJECT_MOUNT, 0);
    } else if (request->op == FS_OP_OPEN || request->op == FS_OP_OPEN_EXEC) {
        if (is_dir_token(request->object_token)) reply_status(request->op, seq, FS_STATUS_IS_DIR);
        else if (is_file_token(request->object_token)) reply_seed2_open(request->op, seq);
        else reply_status(request->op, seq, FS_STATUS_NOT_FOUND);
    } else if (request->op == FS_OP_READ) {
        if (is_open_file_token(request->object_token)) reply_seed2_read(seq, request->offset, request->length);
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
    user_log("[fat_server] FatServer: skeleton started\n");
    volatile u64 *config = (volatile u64 *)FAT_CONFIG_VA;
    g_endpoint_id = config[0];
    g_volume_start_block = config[3];
    if (connect_block_service()) {
        if (!read_volume_sector0_probe()) {
            user_log("[fat_server] FatServer: volume sector0 read failed\n");
        } else if (parse_fat32_bpb()) {
            user_log("[fat_server] FatServer: fat32 bpb ok\n");
            if (!probe_seed2_file()) user_log("[fat_server] FatServer: /sbin/seed2.elf missing\n");
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
