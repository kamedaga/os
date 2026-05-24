#include "fs_protocol.h"
#include "exec_elf.h"

#define OFFSETOF(type, member) __builtin_offsetof(type, member)

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
    SYSCALL_GRANT_VM_OBJECT = 0x1F,
    SYSCALL_GRANT_CAP_ON_ENDPOINT = 0x24,
    SYSCALL_INSTALL_ENDPOINT = 0x26,
    SYSCALL_MAP_VM_OBJECT = 0x28,
    SYSCALL_SHARE_CAP = 0x2B,
    SYSCALL_SIGNAL_ENDPOINT = 0x2C,
    SYSCALL_GET_PROCESS_SLOT = 0x2E,
    SYSCALL_PUBLISH_SERVICE_ENDPOINT = 0x33,
    SYSCALL_MAP_PAGE_ANYWHERE = 0x5C,
    SYSCALL_CREATE_SUSPENDED_PROCESS = 0x41,
    SYSCALL_MAP_VM_OBJECT_TO_PROCESS = 0x42,
    SYSCALL_ALLOC_MAP_PAGES_TO_PROCESS = 0x43,
    SYSCALL_SET_PROCESS_INITIAL_CONTEXT = 0x44,
    SYSCALL_START_PROCESS = 0x45,
    SYSCALL_ABORT_PROCESS = 0x46,
    SYSCALL_COPY_TO_PROCESS = 0x47,
    SYSCALL_CREATE_VM_OBJECT_FROM_CURRENT_PAGES = 0x3F,
    SYSCALL_SET_PROCESS_BOOTSTRAP_OWNER = 0x6B,
    SYSCALL_CREATE_IPC_BUFFER_FROM_PAGE = 0x5E,
    SYSCALL_GRANT_IPC_BUFFER_ON_ENDPOINT = 0x5F,
    SYSCALL_SHARE_IPC_BUFFER_ON_ENDPOINT = 0x60,
    SYSCALL_GRANT_QUEUE_CAP = 0x23,
    SYSCALL_OK = 0,
    SYSCALL_ERR_ENDPOINT = 9,
    PAGE_RIGHT_CPU_READ = 0x1,
    PAGE_RIGHT_CPU_WRITE = 0x2,
    PAGE_RIGHT_GRANT = 0x8,
    IPC_BUFFER_TOKEN_TAG = 0xA000000000000000ULL,
    IPC_BUFFER_TOKEN_MASK = 0x0FFFFFFFFFFFFFFFULL,
    IPC_BUFFER_RIGHT_READ = 0x1,
    IPC_BUFFER_RIGHT_WRITE = 0x2,
    IPC_BUFFER_RIGHT_MAP = 0x4,
    IPC_BUFFER_RIGHT_GRANT = 0x8,
    IPC_BUFFER_ROLE_REQUEST = 1,
    IPC_BUFFER_ROLE_RESPONSE = 2,
    ROOT_CONFIG_VA = 0x3C002000,
    DEVICE_CATALOG_VA = 0x3C030000,
    ROOTFS_VFS_IMAGE_VA = 0x29100000,
    SCHED_IMAGE_BASE_VA = 0x2B000000,
    ROOTFS_VFS_CONFIG_VA = 0x2A200000,
    SERVICE_REGISTRY_SOURCE_VA = 0x2A300000,
    DRIVER_CONFIG_BASE_VA = 0x2A400000,
    PROCESS_STANDARD_CONFIG_TARGET_VA = 0x3C002000,
    PROCESS_SERVICE_REGISTRY_SHADOW_VA = 0x3C2C0000,
    USER_ELF_BASE_VA = EXEC_USER_LAYOUT_LOW_VA,
    USER_LAYOUT_TOP_VA = EXEC_USER_LAYOUT_TOP_VA,
    USER_DYNAMIC_MAP_BASE_VA = EXEC_USER_DYNAMIC_MAP_BASE_VA,
    USER_DYNAMIC_MAP_END_VA = EXEC_USER_DYNAMIC_MAP_END_VA,
    USER_ET_DYN_BASE_VA = EXEC_USER_ET_DYN_BASE_VA,
    USER_STACK_TOP = EXEC_USER_STACK_TOP_VA,
    USER_STACK_PAGES = EXEC_USER_STACK_PAGE_COUNT,
    USER_STACK_BOTTOM_VA = USER_STACK_TOP - USER_STACK_PAGES * 4096,
    USER_ENTRY_RSP = USER_STACK_TOP - 8,
    REPLY_ENDPOINT_ID = 0xEB,
    ROOTFS_VFS_ENDPOINT_ID = 0x90,
    LINUX_ABI_ENDPOINT_ID = 0x90,
    LINUX_ABI_READY_ENDPOINT_ID = 0x94,
    ROOT_CONSOLE_ENDPOINT_ID = 0x88,
    ROOT_NET_ENDPOINT_ID = 0x89,
    TTY_SERVICE_ENDPOINT_ID = 0x8A,
    SERVICE_REGISTRY_MAGIC = 0x53525643,
    SERVICE_REGISTRY_VERSION = 1,
    SERVICE_REGISTRY_MAX_ENTRIES = 12,
    SERVICE_KIND_VFS = 2,
    SERVICE_KIND_FAT_FS = 9,
    SERVICE_KIND_CONSOLE = 10,
    SERVICE_KIND_NET = 11,
    SERVICE_KIND_TTY = 12,
    SERVICE_KIND_EXEC = 13,
    SERVICE_FLAG_PROCESS_SLOT_COMPAT = 1,
    LINUX_ABI_BOOTSTRAP_MAGIC = 0x4C41424943464731ULL,
    LINUX_ABI_BOOTSTRAP_VERSION = 3,
    LINUX_ABI_BOOTSTRAP_READY = 0x4C414249524459ULL,
    LINUX_ABI_CONFIG_TARGET_VA = 0x3C002000,
    LINUX_ABI_REQUEST_PAGES_VA = 0x26500000,
    LINUX_ABI_BOOT_REQUEST_PAGE_VA = 0x26540000,
    LINUX_MMAP_BASE_VA = EXEC_LINUX_MMAP_BASE_VA,
    LINUX_BRK_INITIAL_VA = EXEC_LINUX_BRK_INITIAL_VA,
    LINUX_ABI_EXEC_PATH_BYTES = 128,
    VM_OBJECT_TOKEN_TAG = 1ULL << 62,
    VM_OBJECT_RIGHT_READ = 0x1,
    VM_OBJECT_RIGHT_WRITE = 0x2,
    VM_OBJECT_RIGHT_MAP = 0x4,
    VM_OBJECT_RIGHT_GRANT = 0x8,
    VM_RIGHT_READ_MAP = 0x5,
    VM_RIGHT_READ_MAP_GRANT = 0xD,
    SPAWN_RESULT_TAG = 1ULL << 63,
    SPAWN_RESULT_PROCESS_MASK = 0xFFFFFFFFULL,
    PROCESS_BUILDER_TOKEN_TAG = 1ULL << 60,
    PROCESS_BUILDER_PROCESS_MASK = 0xFFFFFFFFULL,
    BOOTSTRAP_PAGE_WRITABLE = 1ULL << 0,
    BOOTSTRAP_CAP_KIND_VM_OBJECT = 2,
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

struct loaded_file {
    u64 image_va;
    u64 file_bytes;
    u64 vm_token;
};

struct linux_abi_bootstrap_config {
    u64 magic;
    u64 version;
    u64 exec_vm_token;
    u64 standard_interpreter_vm_token;
    u64 standard_interpreter_file_bytes;
    u64 abi_trap_request_page_va;
    u64 status;
    u16 exec_path_bytes;
    u8 reserved0[6];
    char exec_path[LINUX_ABI_EXEC_PATH_BYTES];
    u64 ready_endpoint_id;
    u64 ready_process_slot;
    u64 user_low_va;
    u64 user_top_va;
    u64 dynamic_map_base_va;
    u64 dynamic_map_end_va;
    u64 et_dyn_base_va;
    u64 stack_top_va;
    u64 stack_page_count;
    u64 mmap_base_va;
    u64 brk_initial_va;
};

_Static_assert(OFFSETOF(struct linux_abi_bootstrap_config, exec_path) == 64, "linux abi cfg exec path offset");
_Static_assert(OFFSETOF(struct linux_abi_bootstrap_config, user_low_va) == 208, "linux abi cfg layout offset");
_Static_assert(sizeof(struct linux_abi_bootstrap_config) == 280, "linux abi cfg size");

static void populate_exec_layout_config(struct exec_bootstrap_config *cfg) {
    cfg->user_low_va = USER_ELF_BASE_VA;
    cfg->user_top_va = USER_LAYOUT_TOP_VA;
    cfg->dynamic_map_base_va = USER_DYNAMIC_MAP_BASE_VA;
    cfg->dynamic_map_end_va = USER_DYNAMIC_MAP_END_VA;
    cfg->et_dyn_base_va = USER_ET_DYN_BASE_VA;
    cfg->stack_top_va = USER_STACK_TOP;
    cfg->stack_page_count = USER_STACK_PAGES;
    cfg->mmap_base_va = LINUX_MMAP_BASE_VA;
    cfg->brk_initial_va = LINUX_BRK_INITIAL_VA;
}

static void populate_linux_abi_layout_config(struct linux_abi_bootstrap_config *cfg) {
    cfg->user_low_va = USER_ELF_BASE_VA;
    cfg->user_top_va = USER_LAYOUT_TOP_VA;
    cfg->dynamic_map_base_va = USER_DYNAMIC_MAP_BASE_VA;
    cfg->dynamic_map_end_va = USER_DYNAMIC_MAP_END_VA;
    cfg->et_dyn_base_va = USER_ET_DYN_BASE_VA;
    cfg->stack_top_va = USER_STACK_TOP;
    cfg->stack_page_count = USER_STACK_PAGES;
    cfg->mmap_base_va = LINUX_MMAP_BASE_VA;
    cfg->brk_initial_va = LINUX_BRK_INITIAL_VA;
}

struct backend_session {
    u8 active;
    u8 reserved0[7];
    u64 endpoint_id;
    u64 process_slot;
    u64 request_paddr;
    u64 response_paddr;
    u64 request_token;
    u64 response_token;
    u64 request_va;
    u64 response_va;
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
static struct loaded_file g_net_image;
static struct loaded_file g_exec_interpreter_image;
static struct loaded_file g_exec_service_image;
static u64 g_exec_service_process_slot;
static u64 g_linux_abi_process_slot;
static u64 g_linux_abi_config_va;
static unsigned char g_loader_page[4096];
static u32 g_startup_node_count;
static u32 g_provided_service_count;
static u64 g_next_sched_image_va = SCHED_IMAGE_BASE_VA;
static u64 g_next_driver_config_va = DRIVER_CONFIG_BASE_VA;
static u64 g_device_catalog_va;

static u64 decode_started_process_slot(u64 value);

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

static void user_log_hex(const char *prefix, u64 value) {
    static const char digits[] = "0123456789abcdef";
    char buf[17];
    for (int i = 15; i >= 0; i--) {
        buf[i] = digits[value & 0xFULL];
        value >>= 4;
    }
    user_log(prefix);
    user_log_len(buf, 16);
    user_log("\n");
}

static u64 syscall0(u64 nr) {
    u64 ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(nr) : "rcx", "rdx", "r8", "r9", "r10", "r11", "memory");
    return ret;
}

static u64 syscall1(u64 nr, u64 a0) {
    u64 ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(nr), "D"(a0) : "rcx", "rdx", "r8", "r9", "r10", "r11", "memory");
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

static u64 syscall5(u64 nr, u64 a0, u64 a1, u64 a2, u64 a3, u64 a4) {
    register u64 r8 __asm__("r8") = a4;
    u64 ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(nr), "D"(a0), "S"(a1), "d"(a2), "c"(a3), "r"(r8) : "r9", "r10", "r11", "memory");
    return ret;
}

static u64 wait_event_poll(void) { return syscall2(SYSCALL_WAIT_EVENT, 0, 1); }
static u64 wait_event(void) { return syscall2(SYSCALL_WAIT_EVENT, 1, 1); }
static u64 map_page_anywhere(u64 paddr, u64 writable) { return syscall2(SYSCALL_MAP_PAGE_ANYWHERE, paddr, writable); }
static int is_ipc_buffer_token(u64 token) { return (token & ~IPC_BUFFER_TOKEN_MASK) == IPC_BUFFER_TOKEN_TAG && (token & IPC_BUFFER_TOKEN_MASK) != 0; }
static u64 create_ipc_buffer_from_page(u64 paddr, u64 rights_bits, u64 role) { return syscall3(SYSCALL_CREATE_IPC_BUFFER_FROM_PAGE, paddr, rights_bits, role); }
static u64 grant_ipc_buffer_on_endpoint(u64 token, u64 endpoint_id, u64 rights_bits) { return syscall3(SYSCALL_GRANT_IPC_BUFFER_ON_ENDPOINT, token, endpoint_id, rights_bits); }
static u64 share_ipc_buffer_on_endpoint(u64 token, u64 endpoint_id, u64 rights_bits) { return syscall3(SYSCALL_SHARE_IPC_BUFFER_ON_ENDPOINT, token, endpoint_id, rights_bits); }
static void clear_page(u64 va) { volatile u64 *p = (volatile u64 *)va; for (u64 i = 0; i < 512; i++) p[i] = 0; }
static void clear_bytes(void *ptr, u64 bytes) { u8 *p = (u8 *)ptr; for (u64 i = 0; i < bytes; i++) p[i] = 0; }

static int is_vm_object_token(u64 token) {
    return (token & VM_OBJECT_TOKEN_TAG) != 0 && (token & ~VM_OBJECT_TOKEN_TAG) != 0;
}

static void exec_launch_full_fence(void) {
    __sync_synchronize();
}

static void exec_launch_publish_request_op(volatile struct exec_launch_request *request, u64 op) {
    exec_launch_full_fence();
    request->op = op;
    exec_launch_full_fence();
}

static int is_process_builder_token(u64 token) {
    return (token & PROCESS_BUILDER_TOKEN_TAG) != 0 && (token & PROCESS_BUILDER_PROCESS_MASK) != 0;
}

static u64 process_slot_from_builder_token(u64 token) {
    return token & PROCESS_BUILDER_PROCESS_MASK;
}

static int add_u64(u64 a, u64 b, u64 *out) {
    *out = a + b;
    return *out >= a;
}

static u64 read_u64_le_unchecked(const unsigned char *bytes) {
    u64 value = 0;
    for (u64 i = 0; i < 8; i++) value |= (u64)bytes[i] << (i * 8);
    return value;
}

static long long read_i64_le_unchecked(const unsigned char *bytes) {
    return (long long)read_u64_le_unchecked(bytes);
}

static void write_u64_le(unsigned char *bytes, u64 value) {
    for (u64 i = 0; i < 8; i++) bytes[i] = (unsigned char)((value >> (i * 8)) & 0xff);
}

static u64 create_suspended_process(void) {
    const u64 token = syscall1(SYSCALL_CREATE_SUSPENDED_PROCESS, 0);
    return is_process_builder_token(token) ? token : 0;
}

static int alloc_map_pages_to_process(u64 process_token, u64 target_va, u64 page_count, u64 prot_bits) {
    return syscall5(SYSCALL_ALLOC_MAP_PAGES_TO_PROCESS, process_token, target_va, page_count, prot_bits, 0) == SYSCALL_OK;
}

static int copy_to_process(u64 process_token, u64 dest_va, u64 src_va, u64 byte_len) {
    return syscall4(SYSCALL_COPY_TO_PROCESS, process_token, dest_va, src_va, byte_len) == SYSCALL_OK;
}

static u64 install_shared_current_page(u64 source_va) {
    const u64 rights = VM_OBJECT_RIGHT_READ | VM_OBJECT_RIGHT_WRITE | VM_OBJECT_RIGHT_MAP;
    const u64 token = syscall3(SYSCALL_CREATE_VM_OBJECT_FROM_CURRENT_PAGES, source_va, 4096, rights);
    if (!is_vm_object_token(token)) {
        user_log_hex("[seed2_root] shared page token=", token);
        return 0;
    }
    const u64 remap = syscall2(SYSCALL_MAP_VM_OBJECT, token, source_va);
    if (remap != SYSCALL_OK) {
        user_log_hex("[seed2_root] shared page remap=", remap);
        return 0;
    }
    return token;
}

static u64 create_vm_object_from_current_pages(u64 source_va, u64 size_bytes, u64 rights) {
    const u64 token = syscall3(SYSCALL_CREATE_VM_OBJECT_FROM_CURRENT_PAGES, source_va, size_bytes, rights);
    if (!is_vm_object_token(token)) return 0;
    if (syscall2(SYSCALL_MAP_VM_OBJECT, token, source_va) != SYSCALL_OK) return 0;
    return token;
}

static int map_vm_object_to_process(u64 process_token, u64 vm_token, u64 target_va, u64 prot_bits) {
    return syscall4(SYSCALL_MAP_VM_OBJECT_TO_PROCESS, process_token, vm_token, target_va, prot_bits) == SYSCALL_OK;
}

static int set_process_initial_context(u64 process_token, u64 rip, u64 rsp) {
    return syscall4(SYSCALL_SET_PROCESS_INITIAL_CONTEXT, process_token, rip, rsp, 0) == SYSCALL_OK;
}

static int set_process_bootstrap_owner(u64 process_token, int enabled) {
    return syscall2(SYSCALL_SET_PROCESS_BOOTSTRAP_OWNER, process_token, enabled ? 1 : 0) == SYSCALL_OK;
}

static u64 start_process(u64 process_token) {
    return decode_started_process_slot(syscall1(SYSCALL_START_PROCESS, process_token));
}

static void abort_process(u64 process_token) {
    if (is_process_builder_token(process_token)) (void)syscall1(SYSCALL_ABORT_PROCESS, process_token);
}

static u64 prot_bits_from_phdr(const struct exec_elf_program_header *phdr) {
    u64 bits = 0;
    if ((phdr->flags & EXEC_ELF_PF_R) != 0) bits |= 1ULL << 0;
    if ((phdr->flags & EXEC_ELF_PF_W) != 0) bits |= 1ULL << 1;
    if ((phdr->flags & EXEC_ELF_PF_X) != 0) bits |= 1ULL << 2;
    return bits;
}

static int file_offset_for_vaddr(u64 source_va, const struct exec_elf_header *ehdr, u64 vaddr, u64 size, u64 file_bytes, u64 *file_off_out) {
    for (exec_u16 i = 0; i < ehdr->phnum; i++) {
        struct exec_elf_program_header phdr;
        if (exec_elf_parse_program_header((const void *)source_va, file_bytes, ehdr, i, &phdr) != EXEC_ELF_OK) return 0;
        if (phdr.p_type != EXEC_ELF_PT_LOAD) continue;
        if (vaddr < phdr.vaddr) continue;
        const u64 delta = vaddr - phdr.vaddr;
        if (delta > phdr.filesz || size > phdr.filesz - delta) continue;
        return add_u64(phdr.offset, delta, file_off_out);
    }
    return 0;
}

static int copy_page_from_elf(u64 source_va, const struct exec_elf_header *ehdr, u64 file_bytes, u64 page_vaddr, unsigned char *page) {
    for (u64 i = 0; i < 4096; i++) page[i] = 0;
    for (exec_u16 i = 0; i < ehdr->phnum; i++) {
        struct exec_elf_program_header phdr;
        if (exec_elf_parse_program_header((const void *)source_va, file_bytes, ehdr, i, &phdr) != EXEC_ELF_OK) return 0;
        if (phdr.p_type != EXEC_ELF_PT_LOAD || phdr.filesz == 0) continue;
        u64 file_end = 0;
        if (!add_u64(phdr.offset, phdr.filesz, &file_end) || file_end > file_bytes) return 0;
        const u64 seg_file_start = phdr.vaddr;
        u64 seg_file_end = 0;
        if (!add_u64(phdr.vaddr, phdr.filesz, &seg_file_end)) return 0;
        const u64 page_end = page_vaddr + 4096;
        const u64 copy_start = page_vaddr > seg_file_start ? page_vaddr : seg_file_start;
        const u64 copy_end = page_end < seg_file_end ? page_end : seg_file_end;
        if (copy_end <= copy_start) continue;
        u64 file_off = 0;
        if (!file_offset_for_vaddr(source_va, ehdr, copy_start, copy_end - copy_start, file_bytes, &file_off)) return 0;
        const unsigned char *src = (const unsigned char *)(source_va + file_off);
        for (u64 j = 0; j < copy_end - copy_start; j++) page[(copy_start - page_vaddr) + j] = src[j];
    }
    return 1;
}

static int apply_relative_relocations(u64 process_token, u64 source_va, const struct exec_elf_header *ehdr, u64 file_bytes, u64 load_bias) {
    struct exec_elf_program_header dynamic;
    int have_dynamic = 0;
    for (exec_u16 i = 0; i < ehdr->phnum; i++) {
        if (exec_elf_parse_program_header((const void *)source_va, file_bytes, ehdr, i, &dynamic) != EXEC_ELF_OK) return 0;
        if (dynamic.p_type == EXEC_ELF_PT_DYNAMIC) { have_dynamic = 1; break; }
    }
    if (!have_dynamic || dynamic.filesz == 0) return 1;
    u64 dyn_end = 0;
    if (!add_u64(dynamic.offset, dynamic.filesz, &dyn_end) || dyn_end > file_bytes) return 0;
    u64 rela_va = 0, rela_size = 0, rela_ent = 24;
    for (u64 off = dynamic.offset; off + 16 <= dyn_end; off += 16) {
        const unsigned char *dyn = (const unsigned char *)(source_va + off);
        const long long tag = read_i64_le_unchecked(dyn);
        const u64 value = read_u64_le_unchecked(dyn + 8);
        if (tag == 0) break;
        if (tag == 7) rela_va = value;
        if (tag == 8) rela_size = value;
        if (tag == 9) rela_ent = value;
    }
    if (rela_va == 0 || rela_size == 0) return 1;
    if (rela_ent != 24 || (rela_size % 24) != 0) return 0;
    u64 rela_file_off = 0;
    if (!file_offset_for_vaddr(source_va, ehdr, rela_va, rela_size, file_bytes, &rela_file_off)) return 0;
    for (u64 off = rela_file_off; off < rela_file_off + rela_size; off += 24) {
        const unsigned char *rela = (const unsigned char *)(source_va + off);
        const u64 r_offset = read_u64_le_unchecked(rela);
        const u64 r_info = read_u64_le_unchecked(rela + 8);
        const long long r_addend = read_i64_le_unchecked(rela + 16);
        if ((r_info & 0xffffffffULL) != 8 || (r_info >> 32) != 0) continue;
        u64 dest_va = 0;
        if (!add_u64(load_bias, r_offset, &dest_va)) return 0;
        u64 relocated = load_bias;
        if (r_addend >= 0) {
            if (!add_u64(load_bias, (u64)r_addend, &relocated)) return 0;
        } else {
            const u64 magnitude = (u64)(-r_addend);
            if (load_bias < magnitude) return 0;
            relocated = load_bias - magnitude;
        }
        unsigned char bytes[8];
        write_u64_le(bytes, relocated);
        if (!copy_to_process(process_token, dest_va, (u64)bytes, 8)) return 0;
    }
    return 1;
}

static int load_elf_private(u64 process_token, const struct loaded_file *image, u64 *entry_out) {
    struct exec_elf_summary summary;
    const enum exec_elf_error image_status = exec_elf_validate_image((const void *)image->image_va, image->file_bytes, &summary);
    if (image_status != EXEC_ELF_OK) {
        user_log_hex("[seed2_root] pb elf validate=", (u64)image_status);
        return 0;
    }
    const u64 load_bias = summary.is_pie ? USER_ELF_BASE_VA : 0;
    for (exec_u16 i = 0; i < summary.header.phnum; i++) {
        struct exec_elf_program_header phdr;
        if (exec_elf_parse_program_header((const void *)image->image_va, image->file_bytes, &summary.header, i, &phdr) != EXEC_ELF_OK) return 0;
        if (phdr.p_type != EXEC_ELF_PT_LOAD) continue;
        u64 segment_start = 0, segment_end = 0;
        if (exec_elf_validate_load_segment(&phdr, image->file_bytes, &segment_start, &segment_end) != EXEC_ELF_OK) return 0;
        for (u64 page_vaddr = segment_start; page_vaddr < segment_end; page_vaddr += 4096) {
            u64 target_va = 0;
            if (!add_u64(load_bias, page_vaddr, &target_va)) return 0;
            if (!copy_page_from_elf(image->image_va, &summary.header, image->file_bytes, page_vaddr, g_loader_page)) return 0;
            if (!alloc_map_pages_to_process(process_token, target_va, 1, prot_bits_from_phdr(&phdr))) return 0;
            if (!copy_to_process(process_token, target_va, (u64)g_loader_page, 4096)) return 0;
        }
    }
    if (!apply_relative_relocations(process_token, image->image_va, &summary.header, image->file_bytes, load_bias)) return 0;
    return add_u64(load_bias, summary.header.entry, entry_out);
}

static int install_bootstrap_table(u64 process_token, const struct bootstrap_descriptor_table *table) {
    const u64 child_slot = process_slot_from_builder_token(process_token);
    for (u16 i = 0; i < table->page_count; i++) {
        const u64 prot = (table->pages[i].flags & BOOTSTRAP_PAGE_WRITABLE) != 0 ? 3 : 1;
        if ((table->pages[i].flags & BOOTSTRAP_PAGE_WRITABLE) != 0) {
            const u64 shared = install_shared_current_page(table->pages[i].source_va);
            if (shared == 0 || !map_vm_object_to_process(process_token, shared, table->pages[i].target_va, prot)) {
                user_log_hex("[seed2_root] pb share boot va=", table->pages[i].target_va);
                return 0;
            }
            continue;
        }
        if (!alloc_map_pages_to_process(process_token, table->pages[i].target_va, 1, prot)) {
            user_log_hex("[seed2_root] pb map boot va=", table->pages[i].target_va);
            return 0;
        }
        if (!copy_to_process(process_token, table->pages[i].target_va, table->pages[i].source_va, 4096)) {
            user_log_hex("[seed2_root] pb copy boot va=", table->pages[i].target_va);
            return 0;
        }
    }
    for (u16 i = 0; i < table->cap_count; i++) {
        if (table->caps[i].kind != 2) {
            user_log_hex("[seed2_root] pb cap kind=", table->caps[i].kind);
            return 0;
        }
        const u64 granted = syscall3(SYSCALL_GRANT_VM_OBJECT, table->caps[i].source_token, child_slot, table->caps[i].rights_bits);
        if (!is_vm_object_token(granted)) {
            user_log_hex("[seed2_root] pb grant vm=", granted);
            return 0;
        }
        if (!copy_to_process(process_token, table->caps[i].target_token_va, (u64)&granted, sizeof(granted))) {
            user_log_hex("[seed2_root] pb cap copy va=", table->caps[i].target_token_va);
            return 0;
        }
    }
    return 1;
}

static u64 launch_process_builder_image(const struct loaded_file *image, const struct bootstrap_descriptor_table *table, int bootstrap_owner) {
    const u64 process_token = create_suspended_process();
    if (process_token == 0) {
        user_log("[seed2_root] pb create failed\n");
        return 0;
    }
    u64 entry = 0;
    if (!load_elf_private(process_token, image, &entry)) {
        user_log("[seed2_root] pb load failed\n");
        abort_process(process_token);
        return 0;
    }
    if (!alloc_map_pages_to_process(process_token, USER_STACK_BOTTOM_VA, USER_STACK_PAGES, 3)) {
        user_log("[seed2_root] pb stack failed\n");
        abort_process(process_token);
        return 0;
    }
    if (!install_bootstrap_table(process_token, table)) {
        user_log("[seed2_root] pb bootstrap failed\n");
        abort_process(process_token);
        return 0;
    }
    if (bootstrap_owner && !set_process_bootstrap_owner(process_token, 1)) {
        user_log("[seed2_root] pb owner failed\n");
        abort_process(process_token);
        return 0;
    }
    if (!set_process_initial_context(process_token, entry, USER_ENTRY_RSP)) {
        user_log_hex("[seed2_root] pb ctx entry=", entry);
        abort_process(process_token);
        return 0;
    }
    const u64 slot = start_process(process_token);
    if (slot == 0) {
        user_log("[seed2_root] pb start failed\n");
        abort_process(process_token);
    }
    return slot;
}

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

static u64 grant_response_buffer(void) {
    u64 ret = grant_ipc_buffer_on_endpoint(g_fat.response_token, g_fat.endpoint_id, IPC_BUFFER_RIGHT_READ | IPC_BUFFER_RIGHT_WRITE | IPC_BUFFER_RIGHT_MAP);
    if (is_ipc_buffer_token(ret)) return ret;
    if (ret == SYSCALL_ERR_ENDPOINT && install_fat_endpoint()) ret = grant_ipc_buffer_on_endpoint(g_fat.response_token, g_fat.endpoint_id, IPC_BUFFER_RIGHT_READ | IPC_BUFFER_RIGHT_WRITE | IPC_BUFFER_RIGHT_MAP);
    return is_ipc_buffer_token(ret) ? ret : 0;
}

static int share_request_buffer(void) {
    u64 ret = share_ipc_buffer_on_endpoint(g_fat.request_token, g_fat.endpoint_id, IPC_BUFFER_RIGHT_READ | IPC_BUFFER_RIGHT_MAP);
    if (ret == SYSCALL_OK) return 1;
    if (ret == SYSCALL_ERR_ENDPOINT && install_fat_endpoint()) ret = share_ipc_buffer_on_endpoint(g_fat.request_token, g_fat.endpoint_id, IPC_BUFFER_RIGHT_READ | IPC_BUFFER_RIGHT_MAP);
    return ret == SYSCALL_OK;
}

static int signal_fat(void) {
    u64 ret = syscall2(SYSCALL_SIGNAL_ENDPOINT, g_fat.endpoint_id, 0);
    if (ret == SYSCALL_OK) return 1;
    if (ret == SYSCALL_ERR_ENDPOINT && install_fat_endpoint()) ret = syscall2(SYSCALL_SIGNAL_ENDPOINT, g_fat.endpoint_id, 0);
    return ret == SYSCALL_OK;
}

static int wait_response(u64 expected_seq, u16 expected_op) {
    volatile struct fs_response_header *response = (volatile struct fs_response_header *)g_fat.response_va;
    for (u64 i = 0; i < 4096; i++) {
        if (response->response_seq == expected_seq) return response->magic == FS_RESPONSE_MAGIC && response->version == FS_PROTOCOL_VERSION && response->op == expected_op;
        (void)wait_event_poll();
    }
    return 0;
}

static u64 make_nonce(u64 request_token, u64 response_token, u64 endpoint_id, u64 process_slot) {
    u64 nonce = request_token ^ ((response_token << 17) | (response_token >> 47)) ^ ((endpoint_id << 7) | (endpoint_id >> 57)) ^ process_slot ^ 0x5eed2002f5007ULL;
    return nonce == 0 ? 1 : nonce;
}

static int connect_fat(u64 endpoint_id, u64 process_slot) {
    g_fat.endpoint_id = endpoint_id;
    g_fat.process_slot = process_slot;
    if (endpoint_id == 0 || process_slot == 0) return 0;
    g_fat.request_paddr = syscall0(SYSCALL_ALLOC_PAGE);
    g_fat.response_paddr = syscall0(SYSCALL_ALLOC_PAGE);
    if (g_fat.request_paddr < 0x1000 || g_fat.response_paddr < 0x1000) return 0;
    g_fat.request_va = map_page_anywhere(g_fat.request_paddr, 1);
    g_fat.response_va = map_page_anywhere(g_fat.response_paddr, 1);
    if (g_fat.request_va < 0x1000 || g_fat.response_va < 0x1000) return 0;
    const u64 owner_rights = IPC_BUFFER_RIGHT_READ | IPC_BUFFER_RIGHT_WRITE | IPC_BUFFER_RIGHT_MAP | IPC_BUFFER_RIGHT_GRANT;
    g_fat.request_token = create_ipc_buffer_from_page(g_fat.request_paddr, owner_rights, IPC_BUFFER_ROLE_REQUEST);
    g_fat.response_token = create_ipc_buffer_from_page(g_fat.response_paddr, owner_rights, IPC_BUFFER_ROLE_RESPONSE);
    if (!is_ipc_buffer_token(g_fat.request_token) || !is_ipc_buffer_token(g_fat.response_token)) return 0;
    const u64 remote_response_token = grant_response_buffer();
    if (!is_ipc_buffer_token(remote_response_token)) return 0;

    clear_page(g_fat.request_va);
    clear_page(g_fat.response_va);
    const u64 self_slot = syscall0(SYSCALL_GET_PROCESS_SLOT);
    g_fat.session_nonce = make_nonce(g_fat.request_token, g_fat.response_token, endpoint_id, self_slot);
    volatile struct fs_request_header *request = (volatile struct fs_request_header *)g_fat.request_va;
    request->magic = FS_REQUEST_MAGIC;
    request->version = FS_PROTOCOL_VERSION;
    request->op = FS_OP_CONNECT;
    request->arg0 = remote_response_token;
    request->arg1 = self_slot;
    request->session_nonce = g_fat.session_nonce;
    __asm__ volatile("" ::: "memory");
    request->request_seq = 1;

    if (!share_request_buffer()) return 0;
    if (!wait_response(1, FS_OP_CONNECT)) return 0;
    volatile struct fs_response_header *response = (volatile struct fs_response_header *)g_fat.response_va;
    if (response->status != FS_STATUS_OK || response->result_token == 0) return 0;
    g_fat.root_token = response->result_token;
    g_fat.next_seq = 2;
    g_fat.active = 1;
    return 1;
}

static int fs_request(u16 op, u64 token, u64 offset, u32 length, const char *path) {
    const u16 path_len = path ? (u16)cstr_len(path) : 0;
    const u64 seq = g_fat.next_seq++;
    clear_page(g_fat.request_va);
    clear_page(g_fat.response_va);
    volatile struct fs_request_header *request = (volatile struct fs_request_header *)g_fat.request_va;
    request->magic = FS_REQUEST_MAGIC;
    request->version = FS_PROTOCOL_VERSION;
    request->op = op;
    request->object_token = token;
    request->offset = offset;
    request->length = length;
    request->path_bytes = path_len;
    request->session_nonce = g_fat.session_nonce;
    volatile u8 *payload = (volatile u8 *)(g_fat.request_va + FS_REQUEST_HEADER_BYTES);
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

static int load_image_from_fat(const char *path, u64 image_va, struct loaded_file *out) {
    if (!fs_request(FS_OP_LOOKUP, g_fat.root_token, 0, 0, path)) return 0;
    volatile struct fs_response_header *response = (volatile struct fs_response_header *)g_fat.response_va;
    if (response->status != FS_STATUS_OK || response->result_token == 0) return 0;
    const u64 file_token = response->result_token;
    if (!fs_request(FS_OP_OPEN_EXEC, file_token, 0, 0, 0)) return 0;
    response = (volatile struct fs_response_header *)g_fat.response_va;
    if (response->status != FS_STATUS_OK || response->result_token == 0 || response->file_bytes == 0) return 0;
    const u64 open_token = response->result_token;
    const u64 file_bytes = response->file_bytes;
    if (!alloc_pages_at(image_va, file_bytes, MAX_SCHED_IMAGE_PAGES)) return 0;

    u64 offset = 0;
    while (offset < file_bytes) {
        u32 request_len = FS_RESPONSE_PAYLOAD_BYTES;
        if ((u64)request_len > file_bytes - offset) request_len = (u32)(file_bytes - offset);
        if (!fs_request(FS_OP_READ, open_token, offset, request_len, 0)) return 0;
        response = (volatile struct fs_response_header *)g_fat.response_va;
        if (response->status != FS_STATUS_OK || response->inline_bytes == 0) return 0;
        volatile u8 *dst = (volatile u8 *)(image_va + offset);
        volatile u8 *src = (volatile u8 *)(g_fat.response_va + FS_RESPONSE_HEADER_BYTES);
        for (u16 i = 0; i < response->inline_bytes; i++) dst[i] = src[i];
        offset += response->inline_bytes;
    }

    const u64 vm_token = create_vm_object_from_current_pages(image_va, file_bytes, 0xF);
    if ((vm_token & VM_OBJECT_TOKEN_TAG) != VM_OBJECT_TOKEN_TAG) return 0;
    out->image_va = image_va;
    out->file_bytes = file_bytes;
    out->vm_token = vm_token;
    return 1;
}

static int load_text_from_fat(const char *path, u8 *buffer, u32 capacity, u32 *len_out) {
    if (!fs_request(FS_OP_LOOKUP, g_fat.root_token, 0, 0, path)) return 0;
    volatile struct fs_response_header *response = (volatile struct fs_response_header *)g_fat.response_va;
    if (response->status != FS_STATUS_OK || response->result_token == 0) return 0;
    const u64 file_token = response->result_token;
    if (!fs_request(FS_OP_OPEN, file_token, 0, 0, 0)) return 0;
    response = (volatile struct fs_response_header *)g_fat.response_va;
    if (response->status != FS_STATUS_OK || response->result_token == 0) return 0;
    if (response->file_bytes >= capacity) return 0;
    const u64 open_token = response->result_token;
    const u64 file_bytes = response->file_bytes;
    u64 offset = 0;
    while (offset < file_bytes) {
        u32 request_len = FS_RESPONSE_PAYLOAD_BYTES;
        if ((u64)request_len > file_bytes - offset) request_len = (u32)(file_bytes - offset);
        if (!fs_request(FS_OP_READ, open_token, offset, request_len, 0)) return 0;
        response = (volatile struct fs_response_header *)g_fat.response_va;
        if (response->status != FS_STATUS_OK) return 0;
        if (response->inline_bytes == 0 && offset < file_bytes) return 0;
        volatile u8 *src = (volatile u8 *)(g_fat.response_va + FS_RESPONSE_HEADER_BYTES);
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

static u64 grant_vfs_response_buffer(void) {
    u64 ret = grant_ipc_buffer_on_endpoint(g_vfs.response_token, g_vfs.endpoint_id, IPC_BUFFER_RIGHT_READ | IPC_BUFFER_RIGHT_WRITE | IPC_BUFFER_RIGHT_MAP);
    if (is_ipc_buffer_token(ret)) return ret;
    if (ret == SYSCALL_ERR_ENDPOINT && install_vfs_endpoint()) ret = grant_ipc_buffer_on_endpoint(g_vfs.response_token, g_vfs.endpoint_id, IPC_BUFFER_RIGHT_READ | IPC_BUFFER_RIGHT_WRITE | IPC_BUFFER_RIGHT_MAP);
    return is_ipc_buffer_token(ret) ? ret : 0;
}

static int share_vfs_request_buffer(void) {
    u64 ret = share_ipc_buffer_on_endpoint(g_vfs.request_token, g_vfs.endpoint_id, IPC_BUFFER_RIGHT_READ | IPC_BUFFER_RIGHT_MAP);
    if (ret == SYSCALL_OK) return 1;
    if (ret == SYSCALL_ERR_ENDPOINT && install_vfs_endpoint()) ret = share_ipc_buffer_on_endpoint(g_vfs.request_token, g_vfs.endpoint_id, IPC_BUFFER_RIGHT_READ | IPC_BUFFER_RIGHT_MAP);
    return ret == SYSCALL_OK;
}

static int signal_vfs(void) {
    u64 ret = syscall2(SYSCALL_SIGNAL_ENDPOINT, g_vfs.endpoint_id, 0);
    if (ret == SYSCALL_OK) return 1;
    if (ret == SYSCALL_ERR_ENDPOINT && install_vfs_endpoint()) ret = syscall2(SYSCALL_SIGNAL_ENDPOINT, g_vfs.endpoint_id, 0);
    return ret == SYSCALL_OK;
}

static int wait_vfs_response(u64 expected_seq, u16 expected_op) {
    volatile struct fs_response_header *response = (volatile struct fs_response_header *)g_vfs.response_va;
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
    g_vfs.request_va = map_page_anywhere(g_vfs.request_paddr, 1);
    g_vfs.response_va = map_page_anywhere(g_vfs.response_paddr, 1);
    if (g_vfs.request_va < 0x1000 || g_vfs.response_va < 0x1000) return 0;
    const u64 owner_rights = IPC_BUFFER_RIGHT_READ | IPC_BUFFER_RIGHT_WRITE | IPC_BUFFER_RIGHT_MAP | IPC_BUFFER_RIGHT_GRANT;
    g_vfs.request_token = create_ipc_buffer_from_page(g_vfs.request_paddr, owner_rights, IPC_BUFFER_ROLE_REQUEST);
    g_vfs.response_token = create_ipc_buffer_from_page(g_vfs.response_paddr, owner_rights, IPC_BUFFER_ROLE_RESPONSE);
    if (!is_ipc_buffer_token(g_vfs.request_token) || !is_ipc_buffer_token(g_vfs.response_token)) return 0;
    const u64 remote_response_token = grant_vfs_response_buffer();
    if (!is_ipc_buffer_token(remote_response_token)) return 0;

    clear_page(g_vfs.request_va);
    clear_page(g_vfs.response_va);
    const u64 self_slot = syscall0(SYSCALL_GET_PROCESS_SLOT);
    g_vfs.session_nonce = make_nonce(g_vfs.request_token, g_vfs.response_token, endpoint_id, self_slot);
    volatile struct fs_request_header *request = (volatile struct fs_request_header *)g_vfs.request_va;
    request->magic = FS_REQUEST_MAGIC;
    request->version = FS_PROTOCOL_VERSION;
    request->op = FS_OP_CONNECT;
    request->arg0 = remote_response_token;
    request->arg1 = self_slot;
    request->session_nonce = g_vfs.session_nonce;
    __asm__ volatile("" ::: "memory");
    request->request_seq = 1;

    if (!share_vfs_request_buffer()) return 0;
    if (!wait_vfs_response(1, FS_OP_CONNECT)) return 0;
    volatile struct fs_response_header *response = (volatile struct fs_response_header *)g_vfs.response_va;
    if (response->status != FS_STATUS_OK || response->result_token == 0) return 0;
    g_vfs.root_token = response->result_token;
    g_vfs.next_seq = 2;
    g_vfs.active = 1;
    return 1;
}

static int vfs_request(u16 op, u64 token, u64 offset, u32 length, const char *path) {
    const u16 path_len = path ? (u16)cstr_len(path) : 0;
    const u64 seq = g_vfs.next_seq++;
    clear_page(g_vfs.request_va);
    clear_page(g_vfs.response_va);
    volatile struct fs_request_header *request = (volatile struct fs_request_header *)g_vfs.request_va;
    request->magic = FS_REQUEST_MAGIC;
    request->version = FS_PROTOCOL_VERSION;
    request->op = op;
    request->object_token = token;
    request->offset = offset;
    request->length = length;
    request->path_bytes = path_len;
    request->session_nonce = g_vfs.session_nonce;
    volatile u8 *payload = (volatile u8 *)(g_vfs.request_va + FS_REQUEST_HEADER_BYTES);
    for (u16 i = 0; i < path_len; i++) payload[i] = (u8)path[i];
    __asm__ volatile("" ::: "memory");
    request->request_seq = seq;
    if (!signal_vfs()) return 0;
    return wait_vfs_response(seq, op);
}

static int load_text_from_vfs(const char *path, u8 *buffer, u32 capacity, u32 *len_out) {
    if (!vfs_request(FS_OP_LOOKUP, g_vfs.root_token, 0, 0, path)) return 0;
    volatile struct fs_response_header *response = (volatile struct fs_response_header *)g_vfs.response_va;
    if (response->status != FS_STATUS_OK || response->result_token == 0) return 0;
    const u64 file_token = response->result_token;
    if (!vfs_request(FS_OP_OPEN, file_token, 0, 0, 0)) return 0;
    response = (volatile struct fs_response_header *)g_vfs.response_va;
    if (response->status != FS_STATUS_OK || response->result_token == 0) return 0;
    if (response->file_bytes >= capacity) return 0;
    const u64 open_token = response->result_token;
    const u64 file_bytes = response->file_bytes;
    u64 offset = 0;
    while (offset < file_bytes) {
        u32 request_len = FS_RESPONSE_PAYLOAD_BYTES;
        if ((u64)request_len > file_bytes - offset) request_len = (u32)(file_bytes - offset);
        if (!vfs_request(FS_OP_READ, open_token, offset, request_len, 0)) return 0;
        response = (volatile struct fs_response_header *)g_vfs.response_va;
        if (response->status != FS_STATUS_OK) return 0;
        if (response->inline_bytes == 0 && offset < file_bytes) return 0;
        volatile u8 *src = (volatile u8 *)(g_vfs.response_va + FS_RESPONSE_HEADER_BYTES);
        for (u16 i = 0; i < response->inline_bytes; i++) buffer[offset + i] = src[i];
        offset += response->inline_bytes;
    }
    buffer[file_bytes] = 0;
    *len_out = (u32)file_bytes;
    return 1;
}

static int load_image_from_vfs(const char *path, u64 image_va, struct loaded_file *out) {
    if (!vfs_request(FS_OP_LOOKUP, g_vfs.root_token, 0, 0, path)) return 0;
    volatile struct fs_response_header *response = (volatile struct fs_response_header *)g_vfs.response_va;
    if (response->status != FS_STATUS_OK || response->result_token == 0) return 0;
    const u64 file_token = response->result_token;
    if (!vfs_request(FS_OP_OPEN_EXEC, file_token, 0, 0, 0)) return 0;
    response = (volatile struct fs_response_header *)g_vfs.response_va;
    if (response->status != FS_STATUS_OK || response->result_token == 0 || response->file_bytes == 0) return 0;
    const u64 open_token = response->result_token;
    const u64 file_bytes = response->file_bytes;
    if (!alloc_pages_at(image_va, file_bytes, MAX_SCHED_IMAGE_PAGES)) return 0;

    u64 offset = 0;
    while (offset < file_bytes) {
        u32 request_len = FS_RESPONSE_PAYLOAD_BYTES;
        if ((u64)request_len > file_bytes - offset) request_len = (u32)(file_bytes - offset);
        if (!vfs_request(FS_OP_READ, open_token, offset, request_len, 0)) return 0;
        response = (volatile struct fs_response_header *)g_vfs.response_va;
        if (response->status != FS_STATUS_OK || response->inline_bytes == 0) return 0;
        volatile u8 *dst = (volatile u8 *)(image_va + offset);
        volatile u8 *src = (volatile u8 *)(g_vfs.response_va + FS_RESPONSE_HEADER_BYTES);
        for (u16 i = 0; i < response->inline_bytes; i++) dst[i] = src[i];
        offset += response->inline_bytes;
    }

    const u64 vm_token = create_vm_object_from_current_pages(image_va, file_bytes, 0xF);
    if ((vm_token & VM_OBJECT_TOKEN_TAG) != VM_OBJECT_TOKEN_TAG) return 0;
    out->image_va = image_va;
    out->file_bytes = file_bytes;
    out->vm_token = vm_token;
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

static int load_root_driver_image(const char *path, struct loaded_file *out) {
    const u64 image_va = g_next_sched_image_va;
    g_next_sched_image_va += MAX_SCHED_IMAGE_PAGES * 4096;
    return load_image_from_fat(path, image_va, out);
}

static int spawn_root_driver_with_image(const struct loaded_file *image, u64 catalog_kind, u64 endpoint_id, u64 service_kind, const char *ready_label) {
    volatile struct device_catalog_entry *entry = find_device_catalog_entry(catalog_kind);
    if (!entry) return 0;
    if (!is_vm_object_token(image->vm_token)) return 0;

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
    table.pages[0].flags = BOOTSTRAP_PAGE_WRITABLE;

    const u64 child_slot = launch_process_builder_image(image, &table, 0);
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
    struct loaded_file image;
    if (!load_root_driver_image(path, &image)) return 0;
    return spawn_root_driver_with_image(&image, catalog_kind, endpoint_id, service_kind, ready_label);
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
    if (g_net_image.vm_token != 0) return;
    if (!load_root_driver_image("/srv/virtio_net.elf", &g_net_image)) {
        user_log("[seed2_root] net_server preload failed\n");
    }
}

static void launch_root_net_driver(void) {
    if (g_device_catalog_va == 0) {
        user_log("[seed2_root] device catalog missing\n");
        return;
    }
    if (g_net_image.vm_token == 0) preload_root_net_driver();
    if (!spawn_root_driver_with_image(&g_net_image, DEVICE_CATALOG_KIND_NET, ROOT_NET_ENDPOINT_ID, SERVICE_KIND_NET, "[seed2_root] net_server ready\n")) {
        user_log("[seed2_root] net_server launch failed\n");
    }
}

static u64 decode_started_process_slot(u64 value) {
    if ((value & SPAWN_RESULT_TAG) == 0) return 0;
    return value & SPAWN_RESULT_PROCESS_MASK;
}

static void launch_rootfs_vfs(void) {
    struct loaded_file image;
    if (!load_image_from_fat("/srv/rootfs_vfs.elf", ROOTFS_VFS_IMAGE_VA, &image)) {
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
    table.pages[0].flags = BOOTSTRAP_PAGE_WRITABLE;
    table.pages[1].source_va = SERVICE_REGISTRY_SOURCE_VA;
    table.pages[1].target_va = PROCESS_SERVICE_REGISTRY_SHADOW_VA;
    table.pages[1].flags = 0;

    const u64 child_slot = launch_process_builder_image(&image, &table, 0);
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
        service_registry_set(SERVICE_KIND_EXEC, node->child_slot, EXEC_LAUNCH_ENDPOINT_ID);
        g_exec_service_process_slot = node->child_slot;
        return syscall2(SYSCALL_SIGNAL_ENDPOINT, EXEC_LAUNCH_ENDPOINT_ID, 0) == SYSCALL_OK;
    }
    if (cstr_eq(node->provides, "linux_abi_server")) {
        volatile struct linux_abi_bootstrap_config *cfg = (volatile struct linux_abi_bootstrap_config *)g_linux_abi_config_va;
        return cfg != 0 && cfg->status == LINUX_ABI_BOOTSTRAP_READY;
    }
    if (cstr_eq(node->provides, "tty_service")) {
        if (syscall2(SYSCALL_SIGNAL_ENDPOINT, TTY_SERVICE_ENDPOINT_ID, 0) != SYSCALL_OK) return 0;
        service_registry_set(SERVICE_KIND_TTY, node->child_slot, TTY_SERVICE_ENDPOINT_ID);
        return 1;
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

static int launch_exec_server_node(struct startup_node *node, const struct loaded_file *image);
static int launch_linux_abi_server_node(struct startup_node *node, const struct loaded_file *image);
static int launch_linux_exec_node(struct startup_node *node, const struct loaded_file *image);

static int spawn_manifest_node(struct startup_node *node) {
    if (cstr_empty(node->path)) {
        log_startup_node("[seed2_root] manifest missing path ", node);
        return 0;
    }
    if (!cstr_empty(node->load) && !cstr_eq(node->load, "rootfs") && !cstr_eq(node->load, "bootfs")) {
        log_startup_node("[seed2_root] manifest unsupported load ", node);
        return 0;
    }

    struct loaded_file image;
    const u64 image_va = g_next_sched_image_va;
    g_next_sched_image_va += MAX_SCHED_IMAGE_PAGES * 4096;
    if (!connect_vfs(ROOTFS_VFS_ENDPOINT_ID, g_rootfs_vfs_process_slot)) return 0;
    if (!load_image_from_vfs(node->path, image_va, &image)) {
        log_startup_node("[seed2_root] manifest image load failed ", node);
        return 0;
    }
    if (cstr_eq(node->provides, "exec_service")) return launch_exec_server_node(node, &image);
    if (cstr_eq(node->action, "linux_abi_server")) return launch_linux_abi_server_node(node, &image);
    if (cstr_eq(node->action, "linux_exec")) return launch_linux_exec_node(node, &image);

    static struct bootstrap_descriptor_table table;
    clear_bytes(&table, sizeof(table));
    table.page_count = 1;
    table.pages[0].source_va = SERVICE_REGISTRY_SOURCE_VA;
    table.pages[0].target_va = PROCESS_SERVICE_REGISTRY_SHADOW_VA;
    table.pages[0].flags = 0;

    const u64 child_slot = launch_process_builder_image(&image, &table, cstr_eq(node->action, "process_builder"));
    if (child_slot == 0) return 0;
    node->spawned = 1;
    node->child_slot = child_slot;
    return 1;
}

static int ensure_exec_interpreter_image(void) {
    if (is_vm_object_token(g_exec_interpreter_image.vm_token)) return 1;
    const u64 image_va = g_next_sched_image_va;
    g_next_sched_image_va += MAX_SCHED_IMAGE_PAGES * 4096;
    return load_image_from_vfs("/lib/ld-musl-x86_64.so.1", image_va, &g_exec_interpreter_image);
}

static int launch_exec_server_node(struct startup_node *node, const struct loaded_file *image) {
    if (!ensure_exec_interpreter_image()) {
        user_log("[seed2_root] exec interpreter load failed\n");
        return 0;
    }
    u64 config_paddr = 0;
    const u64 cfg_va = g_next_driver_config_va;
    g_next_driver_config_va += 0x1000;
    if (syscall4(SYSCALL_ALLOC_MAP_PAGES, cfg_va, 1, 1, (u64)&config_paddr) != SYSCALL_OK || config_paddr < 0x1000) {
        user_log("[seed2_root] exec config alloc failed\n");
        return 0;
    }
    clear_page(cfg_va);

    struct exec_bootstrap_config *cfg = (struct exec_bootstrap_config *)cfg_va;
    cfg->magic = EXEC_BOOTSTRAP_MAGIC;
    cfg->version = EXEC_BOOTSTRAP_VERSION;
    cfg->flags = EXEC_BOOTSTRAP_FLAG_SERVICE_MODE;
    cfg->interpreter_file_bytes = g_exec_interpreter_image.file_bytes;
    populate_exec_layout_config(cfg);

    static struct bootstrap_descriptor_table table;
    clear_bytes(&table, sizeof(table));
    table.page_count = 2;
    table.cap_count = 1;
    table.pages[0].source_va = cfg_va;
    table.pages[0].target_va = EXEC_BOOTSTRAP_TARGET_VA;
    table.pages[0].flags = BOOTSTRAP_PAGE_WRITABLE;
    table.pages[1].source_va = SERVICE_REGISTRY_SOURCE_VA;
    table.pages[1].target_va = PROCESS_SERVICE_REGISTRY_SHADOW_VA;
    table.pages[1].flags = 0;
    table.caps[0].source_token = g_exec_interpreter_image.vm_token;
    table.caps[0].target_token_va = EXEC_BOOTSTRAP_TARGET_VA + OFFSETOF(struct exec_bootstrap_config, interpreter_vm_token);
    table.caps[0].rights_bits = VM_RIGHT_READ_MAP;
    table.caps[0].kind = BOOTSTRAP_CAP_KIND_VM_OBJECT;

    const u64 child_slot = launch_process_builder_image(image, &table, 1);
    if (child_slot == 0) {
        user_log("[seed2_root] exec process launch failed\n");
        return 0;
    }
    g_exec_service_image = *image;
    node->spawned = 1;
    node->child_slot = child_slot;
    return 1;
}

static void copy_cstr_limited(char *dst, u16 capacity, const char *src, u16 *len_out) {
    u16 n = 0;
    if (capacity == 0) {
        *len_out = 0;
        return;
    }
    while (src[n] != 0 && n + 1 < capacity) {
        dst[n] = src[n];
        n++;
    }
    dst[n] = 0;
    *len_out = n;
}

static int ensure_exec_service_image(void) {
    if (is_vm_object_token(g_exec_service_image.vm_token)) return 1;
    const u64 image_va = g_next_sched_image_va;
    g_next_sched_image_va += MAX_SCHED_IMAGE_PAGES * 4096;
    return load_image_from_vfs("/srv/exec_service.elf", image_va, &g_exec_service_image);
}

static int launch_linux_abi_server_node(struct startup_node *node, const struct loaded_file *image) {
    if (!ensure_exec_interpreter_image() || !ensure_exec_service_image()) return 0;

    u64 config_paddr = 0;
    const u64 cfg_va = g_next_driver_config_va;
    g_next_driver_config_va += 0x1000;
    if (syscall4(SYSCALL_ALLOC_MAP_PAGES, cfg_va, 1, 1, (u64)&config_paddr) != SYSCALL_OK || config_paddr < 0x1000) return 0;
    clear_page(cfg_va);

    struct linux_abi_bootstrap_config *cfg = (struct linux_abi_bootstrap_config *)cfg_va;
    cfg->magic = LINUX_ABI_BOOTSTRAP_MAGIC;
    cfg->version = LINUX_ABI_BOOTSTRAP_VERSION;
    cfg->standard_interpreter_file_bytes = g_exec_interpreter_image.file_bytes;
    cfg->abi_trap_request_page_va = LINUX_ABI_BOOT_REQUEST_PAGE_VA;
    cfg->ready_endpoint_id = LINUX_ABI_READY_ENDPOINT_ID;
    cfg->ready_process_slot = syscall0(SYSCALL_GET_PROCESS_SLOT);
    populate_linux_abi_layout_config(cfg);
    copy_cstr_limited(cfg->exec_path, LINUX_ABI_EXEC_PATH_BYTES, "/cmd/dash_interactive.elf", &cfg->exec_path_bytes);

    static struct bootstrap_descriptor_table table;
    clear_bytes(&table, sizeof(table));
    table.page_count = 2;
    table.cap_count = 2;
    table.pages[0].source_va = cfg_va;
    table.pages[0].target_va = LINUX_ABI_CONFIG_TARGET_VA;
    table.pages[0].flags = BOOTSTRAP_PAGE_WRITABLE;
    table.pages[1].source_va = SERVICE_REGISTRY_SOURCE_VA;
    table.pages[1].target_va = PROCESS_SERVICE_REGISTRY_SHADOW_VA;
    table.pages[1].flags = 0;
    table.caps[0].source_token = g_exec_service_image.vm_token;
    table.caps[0].target_token_va = LINUX_ABI_CONFIG_TARGET_VA + OFFSETOF(struct linux_abi_bootstrap_config, exec_vm_token);
    table.caps[0].rights_bits = VM_RIGHT_READ_MAP_GRANT;
    table.caps[0].kind = BOOTSTRAP_CAP_KIND_VM_OBJECT;
    table.caps[1].source_token = g_exec_interpreter_image.vm_token;
    table.caps[1].target_token_va = LINUX_ABI_CONFIG_TARGET_VA + OFFSETOF(struct linux_abi_bootstrap_config, standard_interpreter_vm_token);
    table.caps[1].rights_bits = VM_RIGHT_READ_MAP_GRANT;
    table.caps[1].kind = BOOTSTRAP_CAP_KIND_VM_OBJECT;

    const u64 child_slot = launch_process_builder_image(image, &table, 1);
    if (child_slot == 0) return 0;
    g_linux_abi_process_slot = child_slot;
    g_linux_abi_config_va = cfg_va;
    node->spawned = 1;
    node->child_slot = child_slot;
    return 1;
}

static int append_exec_arg(struct exec_bootstrap_config *cfg, u16 *cursor, const char *value, u16 *off_out, u16 *len_out) {
    const u64 len64 = cstr_len(value);
    if (len64 > 0xffff) return 0;
    const u16 len = (u16)len64;
    if ((u64)*cursor + len > sizeof(cfg->arg_data)) return 0;
    *off_out = *cursor;
    *len_out = len;
    for (u16 i = 0; i < len; i++) cfg->arg_data[*cursor + i] = (u8)value[i];
    *cursor = (u16)(*cursor + len);
    return 1;
}

static int configure_dash_exec_args(struct exec_bootstrap_config *cfg, const char *path) {
    u16 cursor = 0;
    if (!append_exec_arg(cfg, &cursor, path, &cfg->execfn_offset, &cfg->execfn_bytes)) return 0;
    if (!append_exec_arg(cfg, &cursor, path, &cfg->argv_offsets[0], &cfg->argv_bytes[0])) return 0;
    if (!append_exec_arg(cfg, &cursor, "-i", &cfg->argv_offsets[1], &cfg->argv_bytes[1])) return 0;
    cfg->argv_count = 2;
    static const char *envp[] = {
        "PATH=/bin:/usr/bin:/usr/lib/uutils:/cmd",
        "HOME=/",
        "SHELL=/bin/dash",
        "TERM=ansi",
        "VIMINIT=set directory=/tmp//",
        "PS1=# ",
        "CAPABILITYOS=1",
    };
    for (u16 i = 0; i < 7; i++) {
        if (!append_exec_arg(cfg, &cursor, envp[i], &cfg->envp_offsets[i], &cfg->envp_bytes[i])) return 0;
    }
    cfg->envp_count = 7;
    cfg->arg_data_bytes = cursor;
    return 1;
}

static int launch_linux_exec_node(struct startup_node *node, const struct loaded_file *image) {
    if (g_exec_service_process_slot == 0 || g_linux_abi_process_slot == 0) {
        user_log("[seed2_root] dash exec deferred: service slot missing\n");
        return 0;
    }
    if (!ensure_exec_interpreter_image()) {
        user_log("[seed2_root] dash exec deferred: interpreter unavailable\n");
        return 0;
    }

    const u64 request_paddr = syscall0(SYSCALL_ALLOC_PAGE);
    const u64 response_paddr = syscall0(SYSCALL_ALLOC_PAGE);
    if (request_paddr < 0x1000 || response_paddr < 0x1000) {
        user_log("[seed2_root] dash exec deferred: ipc page alloc failed\n");
        return 0;
    }
    const u64 request_va = map_page_anywhere(request_paddr, 1);
    const u64 response_va = map_page_anywhere(response_paddr, 1);
    if (request_va < 0x1000 || response_va < 0x1000) {
        user_log("[seed2_root] dash exec deferred: ipc page map failed\n");
        return 0;
    }
    clear_page(request_va);
    clear_page(response_va);

    const u64 owner_rights = IPC_BUFFER_RIGHT_READ | IPC_BUFFER_RIGHT_WRITE | IPC_BUFFER_RIGHT_MAP | IPC_BUFFER_RIGHT_GRANT;
    const u64 request_token = create_ipc_buffer_from_page(request_paddr, owner_rights, IPC_BUFFER_ROLE_REQUEST);
    const u64 response_token = create_ipc_buffer_from_page(response_paddr, owner_rights, IPC_BUFFER_ROLE_RESPONSE);
    if (!is_ipc_buffer_token(request_token) || !is_ipc_buffer_token(response_token)) {
        user_log("[seed2_root] dash exec deferred: ipc buffer create failed\n");
        return 0;
    }

    if (syscall3(SYSCALL_INSTALL_ENDPOINT, 0, EXEC_LAUNCH_ENDPOINT_ID, g_exec_service_process_slot) != SYSCALL_OK) {
        user_log("[seed2_root] dash exec deferred: exec endpoint install failed\n");
        return 0;
    }
    const u64 remote_response = grant_ipc_buffer_on_endpoint(
        response_token,
        EXEC_LAUNCH_ENDPOINT_ID,
        IPC_BUFFER_RIGHT_READ | IPC_BUFFER_RIGHT_WRITE | IPC_BUFFER_RIGHT_MAP
    );
    if (!is_ipc_buffer_token(remote_response)) {
        user_log("[seed2_root] dash exec deferred: response grant failed\n");
        return 0;
    }

    const u64 executable_token = syscall3(SYSCALL_GRANT_VM_OBJECT, image->vm_token, g_exec_service_process_slot, VM_RIGHT_READ_MAP);
    const u64 interpreter_token = syscall3(SYSCALL_GRANT_VM_OBJECT, g_exec_interpreter_image.vm_token, g_exec_service_process_slot, VM_RIGHT_READ_MAP);
    if (!is_vm_object_token(executable_token) || !is_vm_object_token(interpreter_token)) {
        user_log("[seed2_root] dash exec deferred: vm object grant failed\n");
        return 0;
    }

    volatile struct exec_launch_request *request = (volatile struct exec_launch_request *)request_va;
    request->magic = EXEC_LAUNCH_REQUEST_MAGIC;
    request->version = EXEC_LAUNCH_VERSION;
    request->response_token = remote_response;
    struct exec_bootstrap_config cfg;
    clear_bytes(&cfg, sizeof(cfg));
    cfg.magic = EXEC_BOOTSTRAP_MAGIC;
    cfg.version = EXEC_BOOTSTRAP_VERSION;
    cfg.executable_vm_token = executable_token;
    cfg.executable_file_bytes = image->file_bytes;
    cfg.interpreter_vm_token = interpreter_token;
    cfg.interpreter_file_bytes = g_exec_interpreter_image.file_bytes;
    cfg.fs_endpoint_id = ROOTFS_VFS_ENDPOINT_ID;
    cfg.fs_compat_process_slot = g_rootfs_vfs_process_slot;
    cfg.abi_trap_endpoint_id = LINUX_ABI_ENDPOINT_ID;
    cfg.abi_trap_endpoint_process_slot = g_linux_abi_process_slot;
    cfg.abi_trap_flavor = 1;
    cfg.abi_trap_request_page_va = LINUX_ABI_REQUEST_PAGES_VA;
    populate_exec_layout_config(&cfg);
    if (!configure_dash_exec_args(&cfg, cstr_empty(node->path) ? "/cmd/dash_interactive.elf" : node->path)) {
        user_log("[seed2_root] dash exec deferred: argv config failed\n");
        return 0;
    }
    {
        const u8 *src = (const u8 *)&cfg;
        volatile u8 *dst = (volatile u8 *)&request->config;
        for (u64 i = 0; i < sizeof(cfg); i++) dst[i] = src[i];
    }
    request->seq = 1;
    exec_launch_publish_request_op(request, EXEC_LAUNCH_OP_START);

    if (share_ipc_buffer_on_endpoint(request_token, EXEC_LAUNCH_ENDPOINT_ID, IPC_BUFFER_RIGHT_READ | IPC_BUFFER_RIGHT_MAP) != SYSCALL_OK) {
        user_log("[seed2_root] dash exec deferred: request share failed\n");
        return 0;
    }
    (void)syscall2(SYSCALL_SIGNAL_ENDPOINT, EXEC_LAUNCH_ENDPOINT_ID, 0);

    volatile struct exec_launch_response *response = (volatile struct exec_launch_response *)response_va;
    for (u64 i = 0; i < 2000000; i++) {
        if (response->magic == EXEC_LAUNCH_RESPONSE_MAGIC &&
            response->version == EXEC_LAUNCH_VERSION &&
            response->op == EXEC_LAUNCH_OP_START &&
            response->seq == 1)
        {
            exec_launch_full_fence();
            if (response->status != EXEC_LAUNCH_STATUS_OK || response->child_process_slot == 0) {
                user_log_hex("[seed2_root] dash exec start status=", response->status);
                user_log_hex("[seed2_root] dash exec start child=", response->child_process_slot);
                return 0;
            }
            exec_launch_publish_request_op(request, EXEC_LAUNCH_OP_START_READY);
            (void)syscall2(SYSCALL_SIGNAL_ENDPOINT, EXEC_LAUNCH_ENDPOINT_ID, 0);
            for (u64 j = 0; j < 2000000; j++) {
                if (response->magic == EXEC_LAUNCH_RESPONSE_MAGIC &&
                    response->version == EXEC_LAUNCH_VERSION &&
                    response->op == EXEC_LAUNCH_OP_STARTED &&
                    response->seq == 1)
                {
                    exec_launch_full_fence();
                    if (response->status != EXEC_LAUNCH_STATUS_OK ||
                        response->child_process_slot == 0) {
                        user_log_hex("[seed2_root] dash exec started status=", response->status);
                        user_log_hex("[seed2_root] dash exec started child=", response->child_process_slot);
                        return 0;
                    }
                    break;
                }
                if ((j & 0x3ffULL) == 0) (void)wait_event_poll();
                __asm__ volatile("pause" ::: "memory");
            }
            if (response->op != EXEC_LAUNCH_OP_STARTED) {
                user_log_hex("[seed2_root] dash exec started op=", response->op);
                return 0;
            }
            node->spawned = 1;
            node->child_slot = response->child_process_slot;
            user_log("DashShim: dash spawned\n");
            return 1;
        }
        if ((i & 0x3ffULL) == 0) (void)wait_event_poll();
        __asm__ volatile("pause" ::: "memory");
    }
    user_log("[seed2_root] dash exec timeout\n");
    return 0;
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
