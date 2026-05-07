#include "fs_protocol.h"

typedef unsigned long long u64;
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;

enum {
    SYSCALL_ALLOC_PAGE = 0x1,
    SYSCALL_MAP_PAGE = 0x2,
    SYSCALL_GRANT_CAP = 0x8,
    SYSCALL_LOG = 0x9,
    SYSCALL_ALLOC_MAP_PAGES = 0xC,
    SYSCALL_WAIT_EVENT = 0x17,
    SYSCALL_SPAWN_EXEC = 0x1D,
    SYSCALL_INSTALL_VM_OBJECT = 0x1E,
    SYSCALL_INSTALL_EXEC_IMAGE = 0x20,
    SYSCALL_GRANT_CAP_ON_ENDPOINT = 0x24,
    SYSCALL_INSTALL_ENDPOINT = 0x26,
    SYSCALL_SHARE_CAP = 0x2B,
    SYSCALL_SIGNAL_ENDPOINT = 0x2C,
    SYSCALL_GET_PROCESS_SLOT = 0x2E,
    SYSCALL_PUBLISH_SERVICE_ENDPOINT = 0x33,
    SYSCALL_GRANT_QUEUE_CAP = 0x23,
    SYSCALL_OK = 0,
    SYSCALL_ERR_ENDPOINT = 9,
    PAGE_RIGHT_CPU_READ = 0x1,
    PAGE_RIGHT_CPU_WRITE = 0x2,
    PAGE_RIGHT_GRANT = 0x8,
    ROOT_CONFIG_VA = 0x3C002000,
    DEVICE_CATALOG_VA = 0x3C030000,
    REQUEST_VA = 0x29000000,
    RESPONSE_VA = 0x29001000,
    VFS_REQUEST_VA = 0x29002000,
    VFS_RESPONSE_VA = 0x29003000,
    ROOTFS_VFS_IMAGE_VA = 0x29100000,
    SCHED_IMAGE_BASE_VA = 0x2B000000,
    ROOTFS_VFS_CONFIG_VA = 0x2A200000,
    SERVICE_REGISTRY_SOURCE_VA = 0x2A300000,
    DRIVER_CONFIG_BASE_VA = 0x2A400000,
    PROCESS_STANDARD_CONFIG_TARGET_VA = 0x3C002000,
    PROCESS_SERVICE_REGISTRY_SHADOW_VA = 0x3C2C0000,
    REPLY_ENDPOINT_ID = 0xEB,
    ROOTFS_VFS_ENDPOINT_ID = 0x90,
    EXEC_SERVICE_ENDPOINT_ID = 0x92,
    ROOT_CONSOLE_ENDPOINT_ID = 0x88,
    ROOT_NET_ENDPOINT_ID = 0x89,
    SERVICE_REGISTRY_MAGIC = 0x53525643,
    SERVICE_REGISTRY_VERSION = 1,
    SERVICE_REGISTRY_MAX_ENTRIES = 12,
    SERVICE_KIND_VFS = 2,
    SERVICE_KIND_FAT_FS = 9,
    SERVICE_KIND_CONSOLE = 10,
    SERVICE_KIND_NET = 11,
    SERVICE_FLAG_PROCESS_SLOT_COMPAT = 1,
    VM_OBJECT_TOKEN_TAG = 1ULL << 62,
    EXEC_IMAGE_TOKEN_TAG = (1ULL << 62) | (1ULL << 61),
    SPAWN_RESULT_TAG = 1ULL << 63,
    SPAWN_RESULT_PROCESS_MASK = 0xFFFFFFFFULL,
    SPAWN_FLAG_BOOTSTRAP_PAGE_WRITABLE = 1ULL << 0,
    SPAWN_FLAG_BOOTSTRAP_EXTENDED_DESCRIPTOR_TABLE = 1ULL << 2,
    SPAWN_FLAG_CHILD_BOOTSTRAP_OWNER = 1ULL << 3,
    SPAWN_FLAG_ALLOW_BOOTSTRAP_AP_PLACEMENT = 1ULL << 4,
    MAX_ROOTFS_VFS_PAGES = 256,
    MAX_SCHED_IMAGE_PAGES = 256,
    MAX_STARTUP_NODES = 24,
    MAX_PROVIDED_SERVICES = 32,
    DEVICE_CATALOG_MAGIC = 0x44455643,
    DEVICE_CATALOG_VERSION = 1,
    DEVICE_CATALOG_READY = 0x44564352,
    DEVICE_CATALOG_MAX_ENTRIES = 6,
    DEVICE_CATALOG_KIND_CONSOLE = 1,
    DEVICE_CATALOG_KIND_NET = 2,
    QUEUE_CAP_TAG_BASE = (1ULL << 62) | (1ULL << 60),
    QUEUE_CAP_KIND_SHIFT = 56,
    QUEUE_CAP_KIND_MASK = 0x0FULL << 56,
    QUEUE_CAP_KIND_IOMMU = 1,
    QUEUE_CAP_KIND_VIRTQUEUE = 2,
    QUEUE_CAP_KIND_COMMAND = 3,
    CONSOLE_CONFIG_MAGIC = 0x434F4E43,
    CONSOLE_CONFIG_VERSION = 1,
    CONSOLE_STATUS_READY = 0x43524459,
    CONSOLE_ENDPOINT_ID_INDEX = 2,
    CONSOLE_COMMON_PAGE_PADDR_INDEX = 3,
    CONSOLE_NOTIFY_PAGE_PADDR_INDEX = 4,
    CONSOLE_ISR_PAGE_PADDR_INDEX = 5,
    CONSOLE_DEVICE_PAGE_PADDR_INDEX = 6,
    CONSOLE_COMMON_PAGE_OFFSET_INDEX = 7,
    CONSOLE_NOTIFY_PAGE_OFFSET_INDEX = 8,
    CONSOLE_ISR_PAGE_OFFSET_INDEX = 9,
    CONSOLE_DEVICE_PAGE_OFFSET_INDEX = 10,
    CONSOLE_NOTIFY_OFF_MULTIPLIER_INDEX = 11,
    CONSOLE_IOMMU_TOKEN_INDEX = 12,
    CONSOLE_RX_QUEUE_SUBMIT_TOKEN_INDEX = 13,
    CONSOLE_RX_QUEUE_NOTIFY_TOKEN_INDEX = 14,
    CONSOLE_TX_QUEUE_SUBMIT_TOKEN_INDEX = 15,
    CONSOLE_TX_QUEUE_NOTIFY_TOKEN_INDEX = 16,
    CONSOLE_COMMAND_TOKEN_INDEX = 17,
    CONSOLE_RESOURCE_ID_INDEX = 18,
    CONSOLE_DRIVER_STATUS_INDEX = 19,
    NET_CONFIG_MAGIC = 0x4E455443,
    NET_CONFIG_VERSION = 1,
    NET_STATUS_READY = 0x4E524459,
    NET_STATUS_FAILED = 0x4E464149,
    NET_ENDPOINT_ID_INDEX = 2,
    NET_COMMON_PAGE_PADDR_INDEX = 3,
    NET_NOTIFY_PAGE_PADDR_INDEX = 4,
    NET_ISR_PAGE_PADDR_INDEX = 5,
    NET_DEVICE_PAGE_PADDR_INDEX = 6,
    NET_COMMON_PAGE_OFFSET_INDEX = 7,
    NET_NOTIFY_PAGE_OFFSET_INDEX = 8,
    NET_ISR_PAGE_OFFSET_INDEX = 9,
    NET_DEVICE_PAGE_OFFSET_INDEX = 10,
    NET_NOTIFY_OFF_MULTIPLIER_INDEX = 11,
    NET_IOMMU_TOKEN_INDEX = 12,
    NET_RX_QUEUE_SUBMIT_TOKEN_INDEX = 13,
    NET_RX_QUEUE_NOTIFY_TOKEN_INDEX = 14,
    NET_TX_QUEUE_SUBMIT_TOKEN_INDEX = 15,
    NET_TX_QUEUE_NOTIFY_TOKEN_INDEX = 16,
    NET_COMMAND_TOKEN_INDEX = 17,
    NET_RESOURCE_ID_INDEX = 18,
    NET_DRIVER_STATUS_INDEX = 19,
};

struct service_entry { u64 kind; u64 process_slot; u64 endpoint_id; u64 flags; };
struct service_registry_page {
    u64 magic;
    u64 version;
    u64 entry_count;
    u64 reserved0;
    struct service_entry entries[SERVICE_REGISTRY_MAX_ENTRIES];
};

struct bootstrap_page_descriptor { u64 source_va; u64 target_va; u64 flags; };
struct bootstrap_cap_descriptor { u64 source_token; u64 target_token_va; u64 rights_bits; u8 kind; u8 reserved[7]; };
struct bootstrap_descriptor_table {
    u16 page_count;
    u16 cap_count;
    u32 reserved0;
    struct bootstrap_page_descriptor pages[136];
    struct bootstrap_cap_descriptor caps[8];
};

struct backend_session {
    u8 active;
    u8 reserved0[7];
    u64 endpoint_id;
    u64 process_slot;
    u64 request_paddr;
    u64 response_paddr;
    u64 session_nonce;
    u64 root_token;
    u64 next_seq;
};

struct startup_node {
    char action[32];
    char name[48];
    char path[128];
    char label[48];
    char load[24];
    char after[48];
    char requires[48];
    char provides[48];
    u8 completed;
    u8 spawned;
    u64 child_slot;
};

struct device_catalog_entry {
    u64 present;
    u64 kind;
    u64 vendor_id;
    u64 device_id;
    u64 subsystem_id;
    u64 resource_id;
    u64 common_page_paddr;
    u64 notify_page_paddr;
    u64 isr_page_paddr;
    u64 device_page_paddr;
    u64 common_page_offset;
    u64 notify_page_offset;
    u64 isr_page_offset;
    u64 device_page_offset;
    u64 notify_off_multiplier;
    u64 iommu_token;
    u64 queue0_submit_token;
    u64 queue0_notify_token;
    u64 queue1_submit_token;
    u64 queue1_notify_token;
    u64 command_token;
};

struct device_catalog_page {
    u64 magic;
    u64 version;
    u64 entry_count;
    u64 reserved0;
    struct device_catalog_entry entries[DEVICE_CATALOG_MAX_ENTRIES];
};

static struct backend_session g_fat;
static struct backend_session g_vfs;
static struct startup_node g_startup_nodes[MAX_STARTUP_NODES];
static char g_provided_services[MAX_PROVIDED_SERVICES][48];
static u8 g_startup_manifest[4096];
static u32 g_startup_manifest_len;
static u64 g_rootfs_vfs_process_slot;
static u64 g_console_endpoint_id;
static u64 g_console_process_slot;
static u64 g_net_endpoint_id;
static u64 g_net_process_slot;
static u64 g_net_exec_token;
static u32 g_startup_node_count;
static u32 g_provided_service_count;
static u64 g_next_sched_image_va = SCHED_IMAGE_BASE_VA;
static u64 g_next_driver_config_va = DRIVER_CONFIG_BASE_VA;
static u64 g_device_catalog_va;

static u64 decode_spawn_process_slot(u64 value);

static u64 cstr_len(const char *s) { u64 n = 0; while (s[n] != 0) n++; return n; }

static int cstr_eq(const char *a, const char *b) {
    u64 i = 0;
    while (a[i] != 0 && b[i] != 0) {
        if (a[i] != b[i]) return 0;
        i++;
    }
    return a[i] == 0 && b[i] == 0;
}

static int cstr_empty(const char *s) {
    return s[0] == 0;
}

static int key_equals(const u8 *key, u16 key_len, const char *expected) {
    u16 i = 0;
    while (i < key_len && expected[i] != 0) {
        if ((char)key[i] != expected[i]) return 0;
        i++;
    }
    return i == key_len && expected[i] == 0;
}

static void copy_value(char *dst, u16 dst_len, const u8 *src, u16 src_len) {
    if (dst_len == 0) return;
    u16 n = src_len;
    if (n >= dst_len) n = dst_len - 1;
    for (u16 i = 0; i < n; i++) dst[i] = (char)src[i];
    dst[n] = 0;
}

static void user_log_len(const char *message, u64 len) {
    u64 ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"((u64)SYSCALL_LOG), "D"((u64)message), "S"(len) : "rcx", "rdx", "r8", "r9", "r10", "r11", "memory");
    (void)ret;
}

static void user_log(const char *message) { user_log_len(message, cstr_len(message)); }

static u64 syscall0(u64 nr) {
    u64 ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(nr) : "rcx", "rdx", "r8", "r9", "r10", "r11", "memory");
    return ret;
}

static u64 syscall2(u64 nr, u64 a0, u64 a1) {
    u64 ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(nr), "D"(a0), "S"(a1) : "rcx", "rdx", "r8", "r9", "r10", "r11", "memory");
    return ret;
}

static u64 syscall3(u64 nr, u64 a0, u64 a1, u64 a2) {
    u64 ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(nr), "D"(a0), "S"(a1), "d"(a2) : "rcx", "r8", "r9", "r10", "r11", "memory");
    return ret;
}

static u64 syscall4(u64 nr, u64 a0, u64 a1, u64 a2, u64 a3) {
    u64 ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(nr), "D"(a0), "S"(a1), "d"(a2), "c"(a3) : "r8", "r9", "r10", "r11", "memory");
    return ret;
}

static u64 wait_event_poll(void) { return syscall2(SYSCALL_WAIT_EVENT, 0, 1); }
static u64 wait_event(void) { return syscall2(SYSCALL_WAIT_EVENT, 1, 1); }
static void clear_page(u64 va) { volatile u64 *p = (volatile u64 *)va; for (u64 i = 0; i < 512; i++) p[i] = 0; }
static void clear_bytes(void *ptr, u64 bytes) { u8 *p = (u8 *)ptr; for (u64 i = 0; i < bytes; i++) p[i] = 0; }

static u64 encode_queue_cap(u64 kind, u64 token) {
    return QUEUE_CAP_TAG_BASE | (kind << QUEUE_CAP_KIND_SHIFT) | token;
}

static u64 decode_queue_cap(u64 value, u64 kind) {
    if ((value & QUEUE_CAP_TAG_BASE) != QUEUE_CAP_TAG_BASE) return 0;
    if (((value & QUEUE_CAP_KIND_MASK) >> QUEUE_CAP_KIND_SHIFT) != kind) return 0;
    return value & ~(QUEUE_CAP_TAG_BASE | QUEUE_CAP_KIND_MASK);
}

static void service_registry_init(void) {
    u64 paddr = 0;
    if (syscall4(SYSCALL_ALLOC_MAP_PAGES, SERVICE_REGISTRY_SOURCE_VA, 1, 1, (u64)&paddr) != SYSCALL_OK) {
        user_log("[seed2_root] service registry alloc failed\n");
        return;
    }
    clear_page(SERVICE_REGISTRY_SOURCE_VA);
    volatile struct service_registry_page *page = (volatile struct service_registry_page *)SERVICE_REGISTRY_SOURCE_VA;
    page->magic = SERVICE_REGISTRY_MAGIC;
    page->version = SERVICE_REGISTRY_VERSION;
}

static void service_registry_set(u64 kind, u64 process_slot, u64 endpoint_id) {
    volatile struct service_registry_page *page = (volatile struct service_registry_page *)SERVICE_REGISTRY_SOURCE_VA;
    if (page->magic != SERVICE_REGISTRY_MAGIC || page->version != SERVICE_REGISTRY_VERSION) return;
    for (u64 i = 0; i < page->entry_count && i < SERVICE_REGISTRY_MAX_ENTRIES; i++) {
        if (page->entries[i].kind != kind) continue;
        page->entries[i].process_slot = process_slot;
        page->entries[i].endpoint_id = endpoint_id;
        page->entries[i].flags = SERVICE_FLAG_PROCESS_SLOT_COMPAT;
        return;
    }
    if (page->entry_count >= SERVICE_REGISTRY_MAX_ENTRIES) return;
    const u64 index = page->entry_count++;
    page->entries[index].kind = kind;
    page->entries[index].process_slot = process_slot;
    page->entries[index].endpoint_id = endpoint_id;
    page->entries[index].flags = SERVICE_FLAG_PROCESS_SLOT_COMPAT;
}

static int install_fat_endpoint(void) {
    if (g_fat.endpoint_id == 0 || g_fat.process_slot == 0) return 0;
    return syscall3(SYSCALL_INSTALL_ENDPOINT, 0, g_fat.endpoint_id, g_fat.process_slot) == SYSCALL_OK;
}

static int grant_response_page(void) {
    u64 ret = syscall3(SYSCALL_GRANT_CAP_ON_ENDPOINT, g_fat.response_paddr, g_fat.endpoint_id, PAGE_RIGHT_CPU_READ | PAGE_RIGHT_CPU_WRITE);
    if (ret == SYSCALL_OK) return 1;
    if (ret == SYSCALL_ERR_ENDPOINT && install_fat_endpoint()) ret = syscall3(SYSCALL_GRANT_CAP_ON_ENDPOINT, g_fat.response_paddr, g_fat.endpoint_id, PAGE_RIGHT_CPU_READ | PAGE_RIGHT_CPU_WRITE);
    return ret == SYSCALL_OK;
}

static int share_request_page(void) {
    u64 ret = syscall2(SYSCALL_SHARE_CAP, g_fat.request_paddr, g_fat.endpoint_id);
    if (ret == SYSCALL_OK) return 1;
    if (ret == SYSCALL_ERR_ENDPOINT && install_fat_endpoint()) ret = syscall2(SYSCALL_SHARE_CAP, g_fat.request_paddr, g_fat.endpoint_id);
    return ret == SYSCALL_OK;
}

static int signal_fat(void) {
    u64 ret = syscall2(SYSCALL_SIGNAL_ENDPOINT, g_fat.endpoint_id, 0);
    if (ret == SYSCALL_OK) return 1;
    if (ret == SYSCALL_ERR_ENDPOINT && install_fat_endpoint()) ret = syscall2(SYSCALL_SIGNAL_ENDPOINT, g_fat.endpoint_id, 0);
    return ret == SYSCALL_OK;
}

static int wait_response(u64 expected_seq, u16 expected_op) {
    volatile struct fs_response_header *response = (volatile struct fs_response_header *)RESPONSE_VA;
    for (u64 i = 0; i < 4096; i++) {
        if (response->response_seq == expected_seq) return response->magic == FS_RESPONSE_MAGIC && response->version == FS_PROTOCOL_VERSION && response->op == expected_op;
        (void)wait_event_poll();
    }
    return 0;
}

static u64 make_nonce(u64 request_paddr, u64 response_paddr, u64 endpoint_id, u64 process_slot) {
    u64 nonce = request_paddr ^ ((response_paddr << 17) | (response_paddr >> 47)) ^ ((endpoint_id << 7) | (endpoint_id >> 57)) ^ process_slot ^ 0x5eed2002f5007ULL;
    return nonce == 0 ? 1 : nonce;
}

static int connect_fat(u64 endpoint_id, u64 process_slot) {
    g_fat.endpoint_id = endpoint_id;
    g_fat.process_slot = process_slot;
    if (endpoint_id == 0 || process_slot == 0) return 0;
    g_fat.request_paddr = syscall0(SYSCALL_ALLOC_PAGE);
    g_fat.response_paddr = syscall0(SYSCALL_ALLOC_PAGE);
    if (g_fat.request_paddr < 0x1000 || g_fat.response_paddr < 0x1000) return 0;
    if (syscall3(SYSCALL_MAP_PAGE, REQUEST_VA, g_fat.request_paddr, 1) != SYSCALL_OK) return 0;
    if (syscall3(SYSCALL_MAP_PAGE, RESPONSE_VA, g_fat.response_paddr, 1) != SYSCALL_OK) return 0;
    if (!grant_response_page()) return 0;

    clear_page(REQUEST_VA);
    clear_page(RESPONSE_VA);
    const u64 self_slot = syscall0(SYSCALL_GET_PROCESS_SLOT);
    g_fat.session_nonce = make_nonce(g_fat.request_paddr, g_fat.response_paddr, endpoint_id, self_slot);
    volatile struct fs_request_header *request = (volatile struct fs_request_header *)REQUEST_VA;
    request->magic = FS_REQUEST_MAGIC;
    request->version = FS_PROTOCOL_VERSION;
    request->op = FS_OP_CONNECT;
    request->arg0 = g_fat.response_paddr;
    request->arg1 = self_slot;
    request->session_nonce = g_fat.session_nonce;
    __asm__ volatile("" ::: "memory");
    request->request_seq = 1;

    if (!share_request_page()) return 0;
    if (!wait_response(1, FS_OP_CONNECT)) return 0;
    volatile struct fs_response_header *response = (volatile struct fs_response_header *)RESPONSE_VA;
    if (response->status != FS_STATUS_OK || response->result_token == 0) return 0;
    g_fat.root_token = response->result_token;
    g_fat.next_seq = 2;
    g_fat.active = 1;
    return 1;
}

static int fs_request(u16 op, u64 token, u64 offset, u32 length, const char *path) {
    const u16 path_len = path ? (u16)cstr_len(path) : 0;
    const u64 seq = g_fat.next_seq++;
    clear_page(REQUEST_VA);
    clear_page(RESPONSE_VA);
    volatile struct fs_request_header *request = (volatile struct fs_request_header *)REQUEST_VA;
    request->magic = FS_REQUEST_MAGIC;
    request->version = FS_PROTOCOL_VERSION;
    request->op = op;
    request->object_token = token;
    request->offset = offset;
    request->length = length;
    request->path_bytes = path_len;
    request->session_nonce = g_fat.session_nonce;
    volatile u8 *payload = (volatile u8 *)(REQUEST_VA + FS_REQUEST_HEADER_BYTES);
    for (u16 i = 0; i < path_len; i++) payload[i] = (u8)path[i];
    __asm__ volatile("" ::: "memory");
    request->request_seq = seq;
    if (!signal_fat()) return 0;
    return wait_response(seq, op);
}

static int alloc_pages_at(u64 base_va, u64 file_bytes, u64 max_pages) {
    const u64 pages = (file_bytes + 4095) / 4096;
    if (pages == 0 || pages > max_pages) return 0;
    for (u64 i = 0; i < pages; i++) {
        u64 paddr = 0;
        const u64 va = base_va + i * 4096;
        if (syscall4(SYSCALL_ALLOC_MAP_PAGES, va, 1, 1, (u64)&paddr) != SYSCALL_OK || paddr < 0x1000) return 0;
        clear_page(va);
    }
    return 1;
}

static int load_exec_from_fat(const char *path, u64 image_va, u64 *exec_token_out) {
    if (!fs_request(FS_OP_LOOKUP, g_fat.root_token, 0, 0, path)) return 0;
    volatile struct fs_response_header *response = (volatile struct fs_response_header *)RESPONSE_VA;
    if (response->status != FS_STATUS_OK || response->result_token == 0) return 0;
    const u64 file_token = response->result_token;
    if (!fs_request(FS_OP_OPEN_EXEC, file_token, 0, 0, 0)) return 0;
    response = (volatile struct fs_response_header *)RESPONSE_VA;
    if (response->status != FS_STATUS_OK || response->result_token == 0 || response->file_bytes == 0) return 0;
    const u64 open_token = response->result_token;
    const u64 file_bytes = response->file_bytes;
    if (!alloc_pages_at(image_va, file_bytes, MAX_SCHED_IMAGE_PAGES)) return 0;

    u64 offset = 0;
    while (offset < file_bytes) {
        u32 request_len = FS_RESPONSE_PAYLOAD_BYTES;
        if ((u64)request_len > file_bytes - offset) request_len = (u32)(file_bytes - offset);
        if (!fs_request(FS_OP_READ, open_token, offset, request_len, 0)) return 0;
        response = (volatile struct fs_response_header *)RESPONSE_VA;
        if (response->status != FS_STATUS_OK || response->inline_bytes == 0) return 0;
        volatile u8 *dst = (volatile u8 *)(image_va + offset);
        volatile u8 *src = (volatile u8 *)(RESPONSE_VA + FS_RESPONSE_HEADER_BYTES);
        for (u16 i = 0; i < response->inline_bytes; i++) dst[i] = src[i];
        offset += response->inline_bytes;
    }

    const u64 vm_token = syscall3(SYSCALL_INSTALL_VM_OBJECT, image_va, file_bytes, 1);
    if ((vm_token & VM_OBJECT_TOKEN_TAG) != VM_OBJECT_TOKEN_TAG) return 0;
    const u64 exec_token = syscall2(SYSCALL_INSTALL_EXEC_IMAGE, vm_token, 1);
    if ((exec_token & EXEC_IMAGE_TOKEN_TAG) != EXEC_IMAGE_TOKEN_TAG) return 0;
    *exec_token_out = exec_token;
    return 1;
}

static int load_text_from_fat(const char *path, u8 *buffer, u32 capacity, u32 *len_out) {
    if (!fs_request(FS_OP_LOOKUP, g_fat.root_token, 0, 0, path)) return 0;
    volatile struct fs_response_header *response = (volatile struct fs_response_header *)RESPONSE_VA;
    if (response->status != FS_STATUS_OK || response->result_token == 0) return 0;
    const u64 file_token = response->result_token;
    if (!fs_request(FS_OP_OPEN, file_token, 0, 0, 0)) return 0;
    response = (volatile struct fs_response_header *)RESPONSE_VA;
    if (response->status != FS_STATUS_OK || response->result_token == 0) return 0;
    if (response->file_bytes >= capacity) return 0;
    const u64 open_token = response->result_token;
    const u64 file_bytes = response->file_bytes;
    u64 offset = 0;
    while (offset < file_bytes) {
        u32 request_len = FS_RESPONSE_PAYLOAD_BYTES;
        if ((u64)request_len > file_bytes - offset) request_len = (u32)(file_bytes - offset);
        if (!fs_request(FS_OP_READ, open_token, offset, request_len, 0)) return 0;
        response = (volatile struct fs_response_header *)RESPONSE_VA;
        if (response->status != FS_STATUS_OK) return 0;
        if (response->inline_bytes == 0 && offset < file_bytes) return 0;
        volatile u8 *src = (volatile u8 *)(RESPONSE_VA + FS_RESPONSE_HEADER_BYTES);
        for (u16 i = 0; i < response->inline_bytes; i++) buffer[offset + i] = src[i];
        offset += response->inline_bytes;
    }
    buffer[file_bytes] = 0;
    *len_out = (u32)file_bytes;
    return 1;
}

static int install_vfs_endpoint(void) {
    if (g_vfs.endpoint_id == 0 || g_vfs.process_slot == 0) return 0;
    return syscall3(SYSCALL_INSTALL_ENDPOINT, 0, g_vfs.endpoint_id, g_vfs.process_slot) == SYSCALL_OK;
}

static int grant_vfs_response_page(void) {
    u64 ret = syscall3(SYSCALL_GRANT_CAP_ON_ENDPOINT, g_vfs.response_paddr, g_vfs.endpoint_id, PAGE_RIGHT_CPU_READ | PAGE_RIGHT_CPU_WRITE);
    if (ret == SYSCALL_OK) return 1;
    if (ret == SYSCALL_ERR_ENDPOINT && install_vfs_endpoint()) ret = syscall3(SYSCALL_GRANT_CAP_ON_ENDPOINT, g_vfs.response_paddr, g_vfs.endpoint_id, PAGE_RIGHT_CPU_READ | PAGE_RIGHT_CPU_WRITE);
    return ret == SYSCALL_OK;
}

static int share_vfs_request_page(void) {
    u64 ret = syscall2(SYSCALL_SHARE_CAP, g_vfs.request_paddr, g_vfs.endpoint_id);
    if (ret == SYSCALL_OK) return 1;
    if (ret == SYSCALL_ERR_ENDPOINT && install_vfs_endpoint()) ret = syscall2(SYSCALL_SHARE_CAP, g_vfs.request_paddr, g_vfs.endpoint_id);
    return ret == SYSCALL_OK;
}

static int signal_vfs(void) {
    u64 ret = syscall2(SYSCALL_SIGNAL_ENDPOINT, g_vfs.endpoint_id, 0);
    if (ret == SYSCALL_OK) return 1;
    if (ret == SYSCALL_ERR_ENDPOINT && install_vfs_endpoint()) ret = syscall2(SYSCALL_SIGNAL_ENDPOINT, g_vfs.endpoint_id, 0);
    return ret == SYSCALL_OK;
}

static int wait_vfs_response(u64 expected_seq, u16 expected_op) {
    volatile struct fs_response_header *response = (volatile struct fs_response_header *)VFS_RESPONSE_VA;
    for (u64 i = 0; i < 8192; i++) {
        if (response->response_seq == expected_seq) return response->magic == FS_RESPONSE_MAGIC && response->version == FS_PROTOCOL_VERSION && response->op == expected_op;
        (void)wait_event_poll();
    }
    return 0;
}

static int connect_vfs(u64 endpoint_id, u64 process_slot) {
    if (g_vfs.active) return 1;
    g_vfs.endpoint_id = endpoint_id;
    g_vfs.process_slot = process_slot;
    if (endpoint_id == 0 || process_slot == 0) return 0;
    g_vfs.request_paddr = syscall0(SYSCALL_ALLOC_PAGE);
    g_vfs.response_paddr = syscall0(SYSCALL_ALLOC_PAGE);
    if (g_vfs.request_paddr < 0x1000 || g_vfs.response_paddr < 0x1000) return 0;
    if (syscall3(SYSCALL_MAP_PAGE, VFS_REQUEST_VA, g_vfs.request_paddr, 1) != SYSCALL_OK) return 0;
    if (syscall3(SYSCALL_MAP_PAGE, VFS_RESPONSE_VA, g_vfs.response_paddr, 1) != SYSCALL_OK) return 0;
    if (!grant_vfs_response_page()) return 0;

    clear_page(VFS_REQUEST_VA);
    clear_page(VFS_RESPONSE_VA);
    const u64 self_slot = syscall0(SYSCALL_GET_PROCESS_SLOT);
    g_vfs.session_nonce = make_nonce(g_vfs.request_paddr, g_vfs.response_paddr, endpoint_id, self_slot);
    volatile struct fs_request_header *request = (volatile struct fs_request_header *)VFS_REQUEST_VA;
    request->magic = FS_REQUEST_MAGIC;
    request->version = FS_PROTOCOL_VERSION;
    request->op = FS_OP_CONNECT;
    request->arg0 = g_vfs.response_paddr;
    request->arg1 = self_slot;
    request->session_nonce = g_vfs.session_nonce;
    __asm__ volatile("" ::: "memory");
    request->request_seq = 1;

    if (!share_vfs_request_page()) return 0;
    if (!wait_vfs_response(1, FS_OP_CONNECT)) return 0;
    volatile struct fs_response_header *response = (volatile struct fs_response_header *)VFS_RESPONSE_VA;
    if (response->status != FS_STATUS_OK || response->result_token == 0) return 0;
    g_vfs.root_token = response->result_token;
    g_vfs.next_seq = 2;
    g_vfs.active = 1;
    return 1;
}

static int vfs_request(u16 op, u64 token, u64 offset, u32 length, const char *path) {
    const u16 path_len = path ? (u16)cstr_len(path) : 0;
    const u64 seq = g_vfs.next_seq++;
    clear_page(VFS_REQUEST_VA);
    clear_page(VFS_RESPONSE_VA);
    volatile struct fs_request_header *request = (volatile struct fs_request_header *)VFS_REQUEST_VA;
    request->magic = FS_REQUEST_MAGIC;
    request->version = FS_PROTOCOL_VERSION;
    request->op = op;
    request->object_token = token;
    request->offset = offset;
    request->length = length;
    request->path_bytes = path_len;
    request->session_nonce = g_vfs.session_nonce;
    volatile u8 *payload = (volatile u8 *)(VFS_REQUEST_VA + FS_REQUEST_HEADER_BYTES);
    for (u16 i = 0; i < path_len; i++) payload[i] = (u8)path[i];
    __asm__ volatile("" ::: "memory");
    request->request_seq = seq;
    if (!signal_vfs()) return 0;
    return wait_vfs_response(seq, op);
}

static int load_text_from_vfs(const char *path, u8 *buffer, u32 capacity, u32 *len_out) {
    if (!vfs_request(FS_OP_LOOKUP, g_vfs.root_token, 0, 0, path)) return 0;
    volatile struct fs_response_header *response = (volatile struct fs_response_header *)VFS_RESPONSE_VA;
    if (response->status != FS_STATUS_OK || response->result_token == 0) return 0;
    const u64 file_token = response->result_token;
    if (!vfs_request(FS_OP_OPEN, file_token, 0, 0, 0)) return 0;
    response = (volatile struct fs_response_header *)VFS_RESPONSE_VA;
    if (response->status != FS_STATUS_OK || response->result_token == 0) return 0;
    if (response->file_bytes >= capacity) return 0;
    const u64 open_token = response->result_token;
    const u64 file_bytes = response->file_bytes;
    u64 offset = 0;
    while (offset < file_bytes) {
        u32 request_len = FS_RESPONSE_PAYLOAD_BYTES;
        if ((u64)request_len > file_bytes - offset) request_len = (u32)(file_bytes - offset);
        if (!vfs_request(FS_OP_READ, open_token, offset, request_len, 0)) return 0;
        response = (volatile struct fs_response_header *)VFS_RESPONSE_VA;
        if (response->status != FS_STATUS_OK) return 0;
        if (response->inline_bytes == 0 && offset < file_bytes) return 0;
        volatile u8 *src = (volatile u8 *)(VFS_RESPONSE_VA + FS_RESPONSE_HEADER_BYTES);
        for (u16 i = 0; i < response->inline_bytes; i++) buffer[offset + i] = src[i];
        offset += response->inline_bytes;
    }
    buffer[file_bytes] = 0;
    *len_out = (u32)file_bytes;
    return 1;
}

static int load_exec_from_vfs(const char *path, u64 image_va, u64 *exec_token_out) {
    if (!vfs_request(FS_OP_LOOKUP, g_vfs.root_token, 0, 0, path)) return 0;
    volatile struct fs_response_header *response = (volatile struct fs_response_header *)VFS_RESPONSE_VA;
    if (response->status != FS_STATUS_OK || response->result_token == 0) return 0;
    const u64 file_token = response->result_token;
    if (!vfs_request(FS_OP_OPEN_EXEC, file_token, 0, 0, 0)) return 0;
    response = (volatile struct fs_response_header *)VFS_RESPONSE_VA;
    if (response->status != FS_STATUS_OK || response->result_token == 0 || response->file_bytes == 0) return 0;
    const u64 open_token = response->result_token;
    const u64 file_bytes = response->file_bytes;
    if (!alloc_pages_at(image_va, file_bytes, MAX_SCHED_IMAGE_PAGES)) return 0;

    u64 offset = 0;
    while (offset < file_bytes) {
        u32 request_len = FS_RESPONSE_PAYLOAD_BYTES;
        if ((u64)request_len > file_bytes - offset) request_len = (u32)(file_bytes - offset);
        if (!vfs_request(FS_OP_READ, open_token, offset, request_len, 0)) return 0;
        response = (volatile struct fs_response_header *)VFS_RESPONSE_VA;
        if (response->status != FS_STATUS_OK || response->inline_bytes == 0) return 0;
        volatile u8 *dst = (volatile u8 *)(image_va + offset);
        volatile u8 *src = (volatile u8 *)(VFS_RESPONSE_VA + FS_RESPONSE_HEADER_BYTES);
        for (u16 i = 0; i < response->inline_bytes; i++) dst[i] = src[i];
        offset += response->inline_bytes;
    }

    const u64 vm_token = syscall3(SYSCALL_INSTALL_VM_OBJECT, image_va, file_bytes, 1);
    if ((vm_token & VM_OBJECT_TOKEN_TAG) != VM_OBJECT_TOKEN_TAG) return 0;
    const u64 exec_token = syscall2(SYSCALL_INSTALL_EXEC_IMAGE, vm_token, 1);
    if ((exec_token & EXEC_IMAGE_TOKEN_TAG) != EXEC_IMAGE_TOKEN_TAG) return 0;
    *exec_token_out = exec_token;
    return 1;
}

static volatile struct device_catalog_entry *find_device_catalog_entry(u64 kind) {
    if (g_device_catalog_va == 0) return 0;
    volatile struct device_catalog_page *page = (volatile struct device_catalog_page *)g_device_catalog_va;
    if (page->magic != DEVICE_CATALOG_MAGIC || page->version != DEVICE_CATALOG_VERSION) return 0;
    for (u64 i = 0; i < page->entry_count && i < DEVICE_CATALOG_MAX_ENTRIES; i++) {
        volatile struct device_catalog_entry *entry = &page->entries[i];
        if (entry->present != 0 && entry->kind == kind) return entry;
    }
    return 0;
}

static int grant_driver_mmio(volatile struct device_catalog_entry *entry, u64 child_slot) {
    if (syscall3(SYSCALL_GRANT_CAP, entry->common_page_paddr, child_slot, PAGE_RIGHT_CPU_READ | PAGE_RIGHT_CPU_WRITE) != SYSCALL_OK) return 0;
    if (syscall3(SYSCALL_GRANT_CAP, entry->notify_page_paddr, child_slot, PAGE_RIGHT_CPU_READ | PAGE_RIGHT_CPU_WRITE) != SYSCALL_OK) return 0;
    if (entry->isr_page_paddr != 0 &&
        syscall3(SYSCALL_GRANT_CAP, entry->isr_page_paddr, child_slot, PAGE_RIGHT_CPU_READ) != SYSCALL_OK) return 0;
    if (entry->device_page_paddr != 0 &&
        syscall3(SYSCALL_GRANT_CAP, entry->device_page_paddr, child_slot, PAGE_RIGHT_CPU_READ | PAGE_RIGHT_CPU_WRITE) != SYSCALL_OK) return 0;
    return 1;
}

static int grant_driver_queue_caps(volatile struct device_catalog_entry *entry, u64 child_slot, u64 *iommu, u64 *q0_submit, u64 *q0_notify, u64 *q1_submit, u64 *q1_notify, u64 *command) {
    *iommu = decode_queue_cap(syscall2(SYSCALL_GRANT_QUEUE_CAP, encode_queue_cap(QUEUE_CAP_KIND_IOMMU, entry->iommu_token), child_slot), QUEUE_CAP_KIND_IOMMU);
    *q0_submit = decode_queue_cap(syscall2(SYSCALL_GRANT_QUEUE_CAP, encode_queue_cap(QUEUE_CAP_KIND_VIRTQUEUE, entry->queue0_submit_token), child_slot), QUEUE_CAP_KIND_VIRTQUEUE);
    *q0_notify = decode_queue_cap(syscall2(SYSCALL_GRANT_QUEUE_CAP, encode_queue_cap(QUEUE_CAP_KIND_VIRTQUEUE, entry->queue0_notify_token), child_slot), QUEUE_CAP_KIND_VIRTQUEUE);
    *q1_submit = decode_queue_cap(syscall2(SYSCALL_GRANT_QUEUE_CAP, encode_queue_cap(QUEUE_CAP_KIND_VIRTQUEUE, entry->queue1_submit_token), child_slot), QUEUE_CAP_KIND_VIRTQUEUE);
    *q1_notify = decode_queue_cap(syscall2(SYSCALL_GRANT_QUEUE_CAP, encode_queue_cap(QUEUE_CAP_KIND_VIRTQUEUE, entry->queue1_notify_token), child_slot), QUEUE_CAP_KIND_VIRTQUEUE);
    *command = decode_queue_cap(syscall2(SYSCALL_GRANT_QUEUE_CAP, encode_queue_cap(QUEUE_CAP_KIND_COMMAND, entry->command_token), child_slot), QUEUE_CAP_KIND_COMMAND);
    return *iommu != 0 && *q0_submit != 0 && *q0_notify != 0 && *q1_submit != 0 && *q1_notify != 0 && *command != 0;
}

static int load_root_driver_exec(const char *path, u64 *exec_token_out) {
    const u64 image_va = g_next_sched_image_va;
    g_next_sched_image_va += MAX_SCHED_IMAGE_PAGES * 4096;
    return load_exec_from_fat(path, image_va, exec_token_out);
}

static int spawn_root_driver_with_exec(u64 exec_token, u64 catalog_kind, u64 endpoint_id, u64 service_kind, const char *ready_label) {
    volatile struct device_catalog_entry *entry = find_device_catalog_entry(catalog_kind);
    if (!entry) return 0;
    if ((exec_token & EXEC_IMAGE_TOKEN_TAG) != EXEC_IMAGE_TOKEN_TAG) return 0;

    u64 config_paddr = 0;
    const u64 cfg_va = g_next_driver_config_va;
    g_next_driver_config_va += 0x1000;
    if (syscall4(SYSCALL_ALLOC_MAP_PAGES, cfg_va, 1, 1, (u64)&config_paddr) != SYSCALL_OK || config_paddr < 0x1000) return 0;
    clear_page(cfg_va);
    volatile u64 *cfg = (volatile u64 *)cfg_va;

    static struct bootstrap_descriptor_table table;
    clear_bytes(&table, sizeof(table));
    table.page_count = 1;
    table.pages[0].source_va = cfg_va;
    table.pages[0].target_va = PROCESS_STANDARD_CONFIG_TARGET_VA;
    table.pages[0].flags = SPAWN_FLAG_BOOTSTRAP_PAGE_WRITABLE;

    const u64 spawned = syscall4(
        SYSCALL_SPAWN_EXEC,
        exec_token,
        (u64)&table,
        0,
        SPAWN_FLAG_BOOTSTRAP_EXTENDED_DESCRIPTOR_TABLE
    );
    const u64 child_slot = decode_spawn_process_slot(spawned);
    if (child_slot == 0) return 0;
    if (syscall3(SYSCALL_INSTALL_ENDPOINT, 0, endpoint_id, child_slot) != SYSCALL_OK ||
        syscall2(SYSCALL_PUBLISH_SERVICE_ENDPOINT, endpoint_id, child_slot) != SYSCALL_OK ||
        !grant_driver_mmio(entry, child_slot))
    {
        return 0;
    }

    u64 iommu = 0, q0_submit = 0, q0_notify = 0, q1_submit = 0, q1_notify = 0, command = 0;
    if (!grant_driver_queue_caps(entry, child_slot, &iommu, &q0_submit, &q0_notify, &q1_submit, &q1_notify, &command)) return 0;

    if (catalog_kind == DEVICE_CATALOG_KIND_CONSOLE) {
        cfg[0] = CONSOLE_CONFIG_MAGIC;
        cfg[1] = CONSOLE_CONFIG_VERSION;
        cfg[CONSOLE_ENDPOINT_ID_INDEX] = endpoint_id;
        cfg[CONSOLE_COMMON_PAGE_PADDR_INDEX] = entry->common_page_paddr;
        cfg[CONSOLE_NOTIFY_PAGE_PADDR_INDEX] = entry->notify_page_paddr;
        cfg[CONSOLE_ISR_PAGE_PADDR_INDEX] = entry->isr_page_paddr;
        cfg[CONSOLE_DEVICE_PAGE_PADDR_INDEX] = entry->device_page_paddr;
        cfg[CONSOLE_COMMON_PAGE_OFFSET_INDEX] = entry->common_page_offset;
        cfg[CONSOLE_NOTIFY_PAGE_OFFSET_INDEX] = entry->notify_page_offset;
        cfg[CONSOLE_ISR_PAGE_OFFSET_INDEX] = entry->isr_page_offset;
        cfg[CONSOLE_DEVICE_PAGE_OFFSET_INDEX] = entry->device_page_offset;
        cfg[CONSOLE_NOTIFY_OFF_MULTIPLIER_INDEX] = entry->notify_off_multiplier;
        cfg[CONSOLE_IOMMU_TOKEN_INDEX] = iommu;
        cfg[CONSOLE_RX_QUEUE_SUBMIT_TOKEN_INDEX] = q0_submit;
        cfg[CONSOLE_RX_QUEUE_NOTIFY_TOKEN_INDEX] = q0_notify;
        cfg[CONSOLE_TX_QUEUE_SUBMIT_TOKEN_INDEX] = q1_submit;
        cfg[CONSOLE_TX_QUEUE_NOTIFY_TOKEN_INDEX] = q1_notify;
        cfg[CONSOLE_COMMAND_TOKEN_INDEX] = command;
        cfg[CONSOLE_RESOURCE_ID_INDEX] = entry->resource_id;
        (void)syscall2(SYSCALL_SIGNAL_ENDPOINT, endpoint_id, 0);
        if (cfg[CONSOLE_DRIVER_STATUS_INDEX] != CONSOLE_STATUS_READY) {
            user_log("[seed2_root] console status wait deferred\n");
        }
        g_console_endpoint_id = endpoint_id;
        g_console_process_slot = child_slot;
    } else {
        cfg[0] = NET_CONFIG_MAGIC;
        cfg[1] = NET_CONFIG_VERSION;
        cfg[NET_ENDPOINT_ID_INDEX] = endpoint_id;
        cfg[NET_COMMON_PAGE_PADDR_INDEX] = entry->common_page_paddr;
        cfg[NET_NOTIFY_PAGE_PADDR_INDEX] = entry->notify_page_paddr;
        cfg[NET_ISR_PAGE_PADDR_INDEX] = entry->isr_page_paddr;
        cfg[NET_DEVICE_PAGE_PADDR_INDEX] = entry->device_page_paddr;
        cfg[NET_COMMON_PAGE_OFFSET_INDEX] = entry->common_page_offset;
        cfg[NET_NOTIFY_PAGE_OFFSET_INDEX] = entry->notify_page_offset;
        cfg[NET_ISR_PAGE_OFFSET_INDEX] = entry->isr_page_offset;
        cfg[NET_DEVICE_PAGE_OFFSET_INDEX] = entry->device_page_offset;
        cfg[NET_NOTIFY_OFF_MULTIPLIER_INDEX] = entry->notify_off_multiplier;
        cfg[NET_IOMMU_TOKEN_INDEX] = iommu;
        cfg[NET_RX_QUEUE_SUBMIT_TOKEN_INDEX] = q0_submit;
        cfg[NET_RX_QUEUE_NOTIFY_TOKEN_INDEX] = q0_notify;
        cfg[NET_TX_QUEUE_SUBMIT_TOKEN_INDEX] = q1_submit;
        cfg[NET_TX_QUEUE_NOTIFY_TOKEN_INDEX] = q1_notify;
        cfg[NET_COMMAND_TOKEN_INDEX] = command;
        cfg[NET_RESOURCE_ID_INDEX] = entry->resource_id;
        (void)syscall2(SYSCALL_SIGNAL_ENDPOINT, endpoint_id, 0);
        if (cfg[NET_DRIVER_STATUS_INDEX] == NET_STATUS_FAILED) {
            user_log("[seed2_root] net status failed deferred\n");
        } else if (cfg[NET_DRIVER_STATUS_INDEX] != NET_STATUS_READY) {
            user_log("[seed2_root] net status wait deferred\n");
        }
        g_net_endpoint_id = endpoint_id;
        g_net_process_slot = child_slot;
    }

    service_registry_set(service_kind, child_slot, endpoint_id);
    user_log(ready_label);
    return 1;
}

static int spawn_root_driver(const char *path, u64 catalog_kind, u64 endpoint_id, u64 service_kind, const char *ready_label) {
    u64 exec_token = 0;
    if (!load_root_driver_exec(path, &exec_token)) return 0;
    return spawn_root_driver_with_exec(exec_token, catalog_kind, endpoint_id, service_kind, ready_label);
}

static void wait_device_catalog_ready(volatile u64 *config) {
    g_device_catalog_va = config[9];
    if (g_device_catalog_va == 0) return;
    for (u64 i = 0; i < 100000; i++) {
        if (config[10] == DEVICE_CATALOG_READY) return;
        (void)wait_event_poll();
    }
    user_log("[seed2_root] device catalog ready timeout\n");
    g_device_catalog_va = 0;
}

static void launch_root_console_driver(void) {
    if (g_device_catalog_va == 0) {
        user_log("[seed2_root] device catalog missing\n");
        return;
    }
    if (!spawn_root_driver("/srv/virtio_console.elf", DEVICE_CATALOG_KIND_CONSOLE, ROOT_CONSOLE_ENDPOINT_ID, SERVICE_KIND_CONSOLE, "[seed2_root] console_server ready\n")) {
        user_log("[seed2_root] console_server launch failed\n");
    }
}

static void preload_root_net_driver(void) {
    if (g_net_exec_token != 0) return;
    if (!load_root_driver_exec("/srv/virtio_net.elf", &g_net_exec_token)) {
        user_log("[seed2_root] net_server preload failed\n");
    }
}

static void launch_root_net_driver(void) {
    if (g_device_catalog_va == 0) {
        user_log("[seed2_root] device catalog missing\n");
        return;
    }
    if (g_net_exec_token == 0) preload_root_net_driver();
    if (!spawn_root_driver_with_exec(g_net_exec_token, DEVICE_CATALOG_KIND_NET, ROOT_NET_ENDPOINT_ID, SERVICE_KIND_NET, "[seed2_root] net_server ready\n")) {
        user_log("[seed2_root] net_server launch failed\n");
    }
}

static u64 decode_spawn_process_slot(u64 value) {
    if ((value & SPAWN_RESULT_TAG) == 0) return 0;
    return value & SPAWN_RESULT_PROCESS_MASK;
}

static void launch_rootfs_vfs(void) {
    u64 exec_token = 0;
    if (!load_exec_from_fat("/srv/rootfs_vfs.elf", ROOTFS_VFS_IMAGE_VA, &exec_token)) {
        user_log("[seed2_root] rootfs_vfs load failed\n");
        return;
    }
    user_log("[seed2_root] rootfs_vfs exec ready\n");

    u64 config_paddr = 0;
    if (syscall4(SYSCALL_ALLOC_MAP_PAGES, ROOTFS_VFS_CONFIG_VA, 1, 1, (u64)&config_paddr) != SYSCALL_OK) {
        user_log("[seed2_root] rootfs_vfs config alloc failed\n");
        return;
    }
    clear_page(ROOTFS_VFS_CONFIG_VA);
    volatile u64 *config = (volatile u64 *)ROOTFS_VFS_CONFIG_VA;
    config[0] = ROOTFS_VFS_ENDPOINT_ID;
    config[1] = 0x31534656;
    config[2] = 0;
    config[3] = g_fat.endpoint_id;
    config[4] = g_fat.process_slot;
    config[5] = g_net_endpoint_id;
    config[6] = g_net_process_slot;

    static struct bootstrap_descriptor_table table;
    clear_bytes(&table, sizeof(table));
    table.page_count = 2;
    table.pages[0].source_va = ROOTFS_VFS_CONFIG_VA;
    table.pages[0].target_va = PROCESS_STANDARD_CONFIG_TARGET_VA;
    table.pages[0].flags = SPAWN_FLAG_BOOTSTRAP_PAGE_WRITABLE;
    table.pages[1].source_va = SERVICE_REGISTRY_SOURCE_VA;
    table.pages[1].target_va = PROCESS_SERVICE_REGISTRY_SHADOW_VA;
    table.pages[1].flags = 0;

    const u64 spawned = syscall4(
        SYSCALL_SPAWN_EXEC,
        exec_token,
        (u64)&table,
        0,
        SPAWN_FLAG_BOOTSTRAP_EXTENDED_DESCRIPTOR_TABLE
    );
    const u64 child_slot = decode_spawn_process_slot(spawned);
    if (child_slot == 0) {
        user_log("[seed2_root] rootfs_vfs spawn failed\n");
        return;
    }
    g_rootfs_vfs_process_slot = child_slot;
    if (syscall3(SYSCALL_INSTALL_ENDPOINT, 0, ROOTFS_VFS_ENDPOINT_ID, child_slot) != SYSCALL_OK ||
        syscall2(SYSCALL_PUBLISH_SERVICE_ENDPOINT, ROOTFS_VFS_ENDPOINT_ID, child_slot) != SYSCALL_OK)
    {
        user_log("[seed2_root] rootfs_vfs endpoint publish deferred\n");
    } else {
        user_log("[seed2_root] rootfs_vfs endpoint published\n");
        service_registry_set(SERVICE_KIND_VFS, child_slot, ROOTFS_VFS_ENDPOINT_ID);
        (void)syscall2(SYSCALL_SIGNAL_ENDPOINT, ROOTFS_VFS_ENDPOINT_ID, 0);
    }
    user_log("[seed2_root] rootfs_vfs ready wait deferred\n");
}

static void load_startup_manifest(void) {
    if (load_text_from_fat("/sys/startup_manifest.txt", g_startup_manifest, sizeof(g_startup_manifest), &g_startup_manifest_len)) {
        user_log("[seed2_root] startup manifest ready via fat\n");
    } else if (g_rootfs_vfs_process_slot != 0 &&
        connect_vfs(ROOTFS_VFS_ENDPOINT_ID, g_rootfs_vfs_process_slot) &&
        load_text_from_vfs("/sys/startup_manifest.txt", g_startup_manifest, sizeof(g_startup_manifest), &g_startup_manifest_len))
    {
        user_log("[seed2_root] startup manifest ready via vfs\n");
    } else {
        user_log("[seed2_root] startup manifest load failed\n");
    }
}

static int provided_has(const char *service) {
    if (cstr_empty(service)) return 1;
    for (u32 i = 0; i < g_provided_service_count && i < MAX_PROVIDED_SERVICES; i++) {
        if (cstr_eq(g_provided_services[i], service)) return 1;
    }
    return 0;
}

static void provided_add(const char *service) {
    if (cstr_empty(service) || provided_has(service)) return;
    if (g_provided_service_count >= MAX_PROVIDED_SERVICES) return;
    copy_value(g_provided_services[g_provided_service_count], sizeof(g_provided_services[0]), (const u8 *)service, (u16)cstr_len(service));
    g_provided_service_count++;
}

static int node_completed_by_name(const char *name) {
    if (cstr_empty(name)) return 1;
    for (u32 i = 0; i < g_startup_node_count && i < MAX_STARTUP_NODES; i++) {
        if (cstr_eq(g_startup_nodes[i].name, name)) return g_startup_nodes[i].completed != 0;
    }
    return 0;
}

static void seed_existing_services(void) {
    provided_add("block_service");
    provided_add("fat_server_service");
    provided_add("vfs_service");
}

static void parse_manifest_token(struct startup_node *node, const u8 *key, u16 key_len, const u8 *value, u16 value_len) {
    if (key_equals(key, key_len, "action")) copy_value(node->action, sizeof(node->action), value, value_len);
    else if (key_equals(key, key_len, "name")) copy_value(node->name, sizeof(node->name), value, value_len);
    else if (key_equals(key, key_len, "path")) copy_value(node->path, sizeof(node->path), value, value_len);
    else if (key_equals(key, key_len, "label")) copy_value(node->label, sizeof(node->label), value, value_len);
    else if (key_equals(key, key_len, "load")) copy_value(node->load, sizeof(node->load), value, value_len);
    else if (key_equals(key, key_len, "after")) copy_value(node->after, sizeof(node->after), value, value_len);
    else if (key_equals(key, key_len, "requires")) copy_value(node->requires, sizeof(node->requires), value, value_len);
    else if (key_equals(key, key_len, "provides")) copy_value(node->provides, sizeof(node->provides), value, value_len);
}

static void parse_manifest_line(const u8 *line, u16 len) {
    if (g_startup_node_count >= MAX_STARTUP_NODES) return;
    u16 pos = 0;
    while (pos < len && (line[pos] == ' ' || line[pos] == '\t')) pos++;
    if (pos >= len || line[pos] == '#') return;

    struct startup_node *node = &g_startup_nodes[g_startup_node_count];
    for (u16 i = 0; i < sizeof(*node); i++) ((u8 *)node)[i] = 0;

    while (pos < len) {
        while (pos < len && (line[pos] == ' ' || line[pos] == '\t')) pos++;
        const u16 key_start = pos;
        while (pos < len && line[pos] != '=' && line[pos] != ' ' && line[pos] != '\t') pos++;
        if (pos >= len || line[pos] != '=') {
            while (pos < len && line[pos] != ' ' && line[pos] != '\t') pos++;
            continue;
        }
        const u16 key_len = pos - key_start;
        pos++;
        const u16 value_start = pos;
        while (pos < len && line[pos] != ' ' && line[pos] != '\t' && line[pos] != '\r') pos++;
        parse_manifest_token(node, line + key_start, key_len, line + value_start, pos - value_start);
    }

    if (!cstr_empty(node->name) || !cstr_empty(node->path) || !cstr_empty(node->provides)) {
        g_startup_node_count++;
    }
}

static void parse_startup_manifest(void) {
    g_startup_node_count = 0;
    u32 pos = 0;
    while (pos < g_startup_manifest_len) {
        const u32 line_start = pos;
        while (pos < g_startup_manifest_len && g_startup_manifest[pos] != '\n') pos++;
        u32 line_len = pos - line_start;
        if (line_len > 0 && g_startup_manifest[line_start + line_len - 1] == '\r') line_len--;
        if (line_len < 512) parse_manifest_line(g_startup_manifest + line_start, (u16)line_len);
        if (pos < g_startup_manifest_len && g_startup_manifest[pos] == '\n') pos++;
    }
}

static int node_dependencies_ready(struct startup_node *node) {
    if (!cstr_empty(node->after) && !node_completed_by_name(node->after)) return 0;
    if (!cstr_empty(node->requires) && !provided_has(node->requires)) return 0;
    return 1;
}

static void mark_node_completed(struct startup_node *node) {
    node->completed = 1;
    if (!cstr_empty(node->provides)) provided_add(node->provides);
}

static int startup_node_ready_after_spawn(struct startup_node *node) {
    if (cstr_eq(node->provides, "exec_service")) {
        return syscall2(SYSCALL_SIGNAL_ENDPOINT, EXEC_SERVICE_ENDPOINT_ID, 0) == SYSCALL_OK;
    }
    return 1;
}

static int startup_has_pending_nodes(void) {
    for (u32 i = 0; i < g_startup_node_count; i++) {
        if (!g_startup_nodes[i].completed) return 1;
    }
    return 0;
}

static int startup_has_spawned_pending_nodes(void) {
    for (u32 i = 0; i < g_startup_node_count; i++) {
        if (!g_startup_nodes[i].completed && g_startup_nodes[i].spawned) return 1;
    }
    return 0;
}

static void log_startup_node(const char *prefix, struct startup_node *node) {
    user_log(prefix);
    user_log(cstr_empty(node->name) ? node->path : node->name);
    user_log("\n");
}

static int spawn_manifest_node(struct startup_node *node) {
    if (cstr_empty(node->path)) return 0;
    if (!cstr_empty(node->load) && !cstr_eq(node->load, "rootfs") && !cstr_eq(node->load, "bootfs")) return 0;

    u64 exec_token = 0;
    const u64 image_va = g_next_sched_image_va;
    g_next_sched_image_va += MAX_SCHED_IMAGE_PAGES * 4096;
    if (!connect_vfs(ROOTFS_VFS_ENDPOINT_ID, g_rootfs_vfs_process_slot)) return 0;
    if (!load_exec_from_vfs(node->path, image_va, &exec_token)) return 0;

    static struct bootstrap_descriptor_table table;
    clear_bytes(&table, sizeof(table));
    table.page_count = 1;
    table.pages[0].source_va = SERVICE_REGISTRY_SOURCE_VA;
    table.pages[0].target_va = PROCESS_SERVICE_REGISTRY_SHADOW_VA;
    table.pages[0].flags = 0;

    u64 spawn_flags = SPAWN_FLAG_BOOTSTRAP_EXTENDED_DESCRIPTOR_TABLE;
    if (cstr_eq(node->action, "process_builder")) spawn_flags |= SPAWN_FLAG_CHILD_BOOTSTRAP_OWNER;
    const u64 spawned = syscall4(SYSCALL_SPAWN_EXEC, exec_token, (u64)&table, 0, spawn_flags);
    const u64 child_slot = decode_spawn_process_slot(spawned);
    if (child_slot == 0) return 0;
    node->spawned = 1;
    node->child_slot = child_slot;
    return 1;
}

static void run_startup_scheduler(void) {
    seed_existing_services();
    parse_startup_manifest();
    user_log("[seed2_root] manifest scheduler begin\n");

    for (;;) {
        int progressed = 0;
        for (u32 i = 0; i < g_startup_node_count; i++) {
            struct startup_node *node = &g_startup_nodes[i];
            if (node->completed) continue;
            if (node->spawned) {
                if (startup_node_ready_after_spawn(node)) {
                    mark_node_completed(node);
                    log_startup_node("[seed2_root] manifest ready ", node);
                    progressed = 1;
                }
                continue;
            }
            if (!node_dependencies_ready(node)) continue;
            if (!cstr_empty(node->provides) && provided_has(node->provides)) {
                mark_node_completed(node);
                progressed = 1;
                continue;
            }
            if (spawn_manifest_node(node)) {
                log_startup_node("[seed2_root] manifest spawned ", node);
                if (startup_node_ready_after_spawn(node)) {
                    mark_node_completed(node);
                    log_startup_node("[seed2_root] manifest ready ", node);
                }
                progressed = 1;
            } else {
                log_startup_node("[seed2_root] manifest node deferred ", node);
            }
        }
        if (!startup_has_pending_nodes()) break;
        if (!progressed) {
            if (startup_has_spawned_pending_nodes()) {
                (void)wait_event();
                continue;
            }
            break;
        }
    }
    user_log("[seed2_root] manifest scheduler done\n");
}

void seed2_root_main(void) {
    user_log("[seed2_root] started\n");
    service_registry_init();
    volatile u64 *config = (volatile u64 *)ROOT_CONFIG_VA;
    const u64 fat_endpoint_id = config[3];
    const u64 fat_process_slot = config[4];
    const u64 console_endpoint_id = config[5];
    const u64 console_process_slot = config[6];
    const u64 net_endpoint_id = config[7];
    const u64 net_process_slot = config[8];
    wait_device_catalog_ready(config);
    g_console_endpoint_id = console_endpoint_id;
    g_console_process_slot = console_process_slot;
    g_net_endpoint_id = net_endpoint_id;
    g_net_process_slot = net_process_slot;
    if (connect_fat(fat_endpoint_id, fat_process_slot)) {
        user_log("[seed2_root] fat connect ok\n");
        service_registry_set(SERVICE_KIND_FAT_FS, fat_process_slot, fat_endpoint_id);
        if (g_console_endpoint_id != 0 && g_console_process_slot != 0) service_registry_set(SERVICE_KIND_CONSOLE, g_console_process_slot, g_console_endpoint_id);
        if (g_net_endpoint_id != 0 && g_net_process_slot != 0) service_registry_set(SERVICE_KIND_NET, g_net_process_slot, g_net_endpoint_id);
        launch_root_console_driver();
        load_startup_manifest();
        preload_root_net_driver();
        launch_root_net_driver();
        launch_rootfs_vfs();
        run_startup_scheduler();
    } else {
        user_log("[seed2_root] fat connect failed\n");
    }
    for (;;) (void)wait_event();
}
