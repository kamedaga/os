#include "exec_elf.h"

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef int i32;

enum {
    SYSCALL_ALLOC_PAGE = 0x1,
    SYSCALL_LOG = 0x9,
    SYSCALL_MAP_PAGE = 0x2,
    SYSCALL_ALLOC_MAP_PAGES = 0xC,
    SYSCALL_GRANT_CAP = 0x8,
    SYSCALL_GRANT_CAPS_BATCH = 0x14,
    SYSCALL_WAIT_EVENT = 0x17,
    SYSCALL_GRANT_VM_OBJECT = 0x1F,
    SYSCALL_GRANT_CAP_ON_ENDPOINT = 0x24,
    SYSCALL_INSTALL_ENDPOINT = 0x26,
    SYSCALL_MAP_VM_OBJECT = 0x28,
    SYSCALL_SHARE_CAP = 0x2B,
    SYSCALL_SIGNAL_ENDPOINT = 0x2C,
    SYSCALL_GET_PROCESS_SLOT = 0x2E,
    SYSCALL_INSTALL_MMIO_CAP = 0x2F,
    SYSCALL_INSTALL_CAPS_BATCH = 0x32,
    SYSCALL_PUBLISH_SERVICE_ENDPOINT = 0x33,
    SYSCALL_GRANT_QUEUE_CAP = 0x23,
    SYSCALL_MAP_PAGE_ANYWHERE = 0x5C,
    SYSCALL_CREATE_IPC_BUFFER_FROM_PAGE = 0x5E,
    SYSCALL_GRANT_IPC_BUFFER_ON_ENDPOINT = 0x5F,
    SYSCALL_SHARE_IPC_BUFFER_ON_ENDPOINT = 0x60,
    SYSCALL_CREATE_SUSPENDED_PROCESS = 0x41,
    SYSCALL_MAP_VM_OBJECT_TO_PROCESS = 0x42,
    SYSCALL_ALLOC_MAP_PAGES_TO_PROCESS = 0x43,
    SYSCALL_SET_PROCESS_INITIAL_CONTEXT = 0x44,
    SYSCALL_START_PROCESS = 0x45,
    SYSCALL_ABORT_PROCESS = 0x46,
    SYSCALL_COPY_TO_PROCESS = 0x47,
    SYSCALL_CREATE_VM_OBJECT_FROM_CURRENT_PAGES = 0x3F,
    SYSCALL_SET_PROCESS_BOOTSTRAP_OWNER = 0x6B,
    SYSCALL_CAPSULE_GRANT = 0x76,
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
    VM_OBJECT_RIGHT_READ = 0x1,
    VM_OBJECT_RIGHT_WRITE = 0x2,
    VM_OBJECT_RIGHT_MAP = 0x4,
    VM_RIGHT_READ_MAP = 0x5,

    PROCESS_AUX_BASE_VA = 0x3C000000,
    PROCESS_STANDARD_CONFIG_TARGET_VA = 0x3C002000,
    PROCESS_SERVICE_REGISTRY_SHADOW_VA = 0x3C2C0000,
    MANAGER_INIT_CONFIG_TARGET_VA = 0x3C021000,
    DYNAMIC_BOOTSTRAP_SOURCE_BASE_VA = 0x3C106000,
    INSPECT_MMIO_BASE_VA = 0x3F000000,

    INIT_BOOTSTRAP_MAGIC = 0x49425453,
    INIT_BOOTSTRAP_VERSION = 18,
    INIT_CONFIG_MAGIC = 0x49425443,
    INIT_CONFIG_VERSION = 1,
    INIT_BOOT_ARCHIVE_FLAG_PRESENT = 1,
    INIT_DEVICE_FLAG_PRESENT = 1,
    INIT_DISPLAY_FLAG_PRESENT = 1,
    INIT_MAX_SPAWN_PAGE_DESCRIPTORS = 8,
    INIT_MAX_DEVICE_DESCRIPTORS = 8,
    INIT_MAX_DEVICE_QUEUE_GRANTS = 4,
    INIT_MAX_BOOT_ARCHIVE_PAGES = 128,
    INIT_DEVICE_TRANSPORT_VIRTIO_PCI_MODERN = 1,
    INIT_DEVICE_TRANSPORT_PCI_FUNCTION = 2,

    MANAGER_INIT_MAGIC = 0x4D494248,
    MANAGER_INIT_VERSION = 6,
    MANAGER_INIT_MAX_DEVICE_GRANTS = 8,
    MANAGER_INIT_MAX_DEVICE_QUEUE_GRANTS = 4,

    BOOTFS_MAGIC = 0x53465442,
    BOOTFS_VERSION = 1,
    BOOTFS_KIND_REGULAR = 1,

    VM_OBJECT_TOKEN_TAG = 1ULL << 62,
    SPAWN_RESULT_TAG = 1ULL << 63,
    SPAWN_RESULT_PROCESS_MASK = 0xFFFFFFFFULL,
    PROCESS_BUILDER_TOKEN_TAG = 1ULL << 60,
    PROCESS_BUILDER_PROCESS_MASK = 0xFFFFFFFFULL,
    BOOTSTRAP_PAGE_WRITABLE = 1ULL << 0,
    BOOTSTRAP_CAP_KIND_VM_OBJECT = 2,
    USER_ELF_BASE_VA = 0x20000000,
    USER_STACK_TOP = 0x3C000000,
    USER_STACK_PAGES = 128,
    USER_STACK_BOTTOM_VA = USER_STACK_TOP - USER_STACK_PAGES * 4096,
    USER_ENTRY_RSP = USER_STACK_TOP - 8,

    BLOCK_CONFIG_MAGIC = 0x424C4B43,
    BLOCK_CONFIG_VERSION = 1,
    BLOCK_STATUS_READY = 0x44524459,
    BLOCK_ENDPOINT_ID_INDEX = 2,
    BLOCK_COMMON_PAGE_PADDR_INDEX = 3,
    BLOCK_NOTIFY_PAGE_PADDR_INDEX = 4,
    BLOCK_ISR_PAGE_PADDR_INDEX = 5,
    BLOCK_DEVICE_PAGE_PADDR_INDEX = 6,
    BLOCK_COMMON_PAGE_OFFSET_INDEX = 7,
    BLOCK_NOTIFY_PAGE_OFFSET_INDEX = 8,
    BLOCK_ISR_PAGE_OFFSET_INDEX = 9,
    BLOCK_DEVICE_PAGE_OFFSET_INDEX = 10,
    BLOCK_NOTIFY_OFF_MULTIPLIER_INDEX = 11,
    BLOCK_IOMMU_TOKEN_INDEX = 12,
    BLOCK_QUEUE_SUBMIT_TOKEN_INDEX = 13,
    BLOCK_QUEUE_NOTIFY_TOKEN_INDEX = 14,
    BLOCK_COMMAND_TOKEN_INDEX = 15,
    BLOCK_CAPACITY_SECTORS_INDEX = 17,
    BLOCK_LOGICAL_BLOCK_SIZE_INDEX = 18,
    BLOCK_DRIVER_STATUS_INDEX = 19,
    BLOCK_RESOURCE_ID_INDEX = 20,

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
    CONSOLE_RX_QUEUE_INDEX = 0,
    CONSOLE_TX_QUEUE_INDEX = 1,

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
    NET_RX_QUEUE_INDEX = 0,
    NET_TX_QUEUE_INDEX = 1,

    SERVICE_REGISTRY_MAGIC = 0x53525643,
    SERVICE_REGISTRY_VERSION = 1,
    SERVICE_REGISTRY_MAX_ENTRIES = 12,
    SERVICE_KIND_BLOCK = 4,
    SERVICE_KIND_CONSOLE = 10,
    SERVICE_KIND_FAT_FS = 9,
    SERVICE_KIND_NET = 11,
    SERVICE_FLAG_PROCESS_SLOT_COMPAT = 1,

    VIRTIO_VENDOR_ID = 0x1AF4,
    VIRTIO_BLK_DEVICE_MODERN = 0x1042,
    VIRTIO_BLK_DEVICE_LEGACY = 0x1001,
    VIRTIO_BLK_SUBSYSTEM_ID = 0x0002,
    VIRTIO_CONSOLE_DEVICE_MODERN = 0x1043,
    VIRTIO_CONSOLE_DEVICE_LEGACY = 0x1003,
    VIRTIO_CONSOLE_SUBSYSTEM_ID = 0x0003,
    VIRTIO_NET_DEVICE_MODERN = 0x1041,
    VIRTIO_NET_DEVICE_LEGACY = 0x1000,
    VIRTIO_NET_SUBSYSTEM_ID = 0x0001,
    VIRTIO_BLK_CAPACITY_OFFSET = 0x00,
    VIRTIO_BLK_BLOCK_SIZE_OFFSET = 0x14,

    QUEUE_CAP_TAG_BASE = (1ULL << 62) | (1ULL << 60),
    QUEUE_CAP_KIND_SHIFT = 56,
    QUEUE_CAP_KIND_MASK = 0x0FULL << 56,
    QUEUE_CAP_KIND_IOMMU = 1,
    QUEUE_CAP_KIND_VIRTQUEUE = 2,
    QUEUE_CAP_KIND_COMMAND = 3,

    CAPSULE_RIGHT_QUERY = 1ULL << 0,
    CAPSULE_RIGHT_CONFIG_READ = 1ULL << 1,
    CAPSULE_RIGHT_CONFIG_WRITE = 1ULL << 2,
    CAPSULE_RIGHT_BAR_INFO = 1ULL << 3,
    CAPSULE_RIGHT_BAR_MAP = 1ULL << 4,
    CAPSULE_RIGHT_DMA_ALLOC = 1ULL << 5,
    CAPSULE_RIGHT_DMA_MAP_USER = 1ULL << 6,
    CAPSULE_RIGHT_IRQ_BIND = 1ULL << 7,
    CAPSULE_RIGHT_BUS_MASTER = 1ULL << 8,
    CAPSULE_RIGHT_RESET = 1ULL << 9,
    CAPSULE_RIGHT_POWER = 1ULL << 10,
    CAPSULE_RIGHT_HOTPLUG_OBSERVE = 1ULL << 11,
    CAPSULE_RIGHT_GRANT = 1ULL << 12,
    CAPSULE_RIGHT_DEVICE_TO_ROOT =
        CAPSULE_RIGHT_QUERY |
        CAPSULE_RIGHT_CONFIG_READ |
        CAPSULE_RIGHT_CONFIG_WRITE |
        CAPSULE_RIGHT_BAR_INFO |
        CAPSULE_RIGHT_BAR_MAP |
        CAPSULE_RIGHT_DMA_ALLOC |
        CAPSULE_RIGHT_DMA_MAP_USER |
        CAPSULE_RIGHT_IRQ_BIND |
        CAPSULE_RIGHT_BUS_MASTER |
        CAPSULE_RIGHT_RESET |
        CAPSULE_RIGHT_POWER |
        CAPSULE_RIGHT_HOTPLUG_OBSERVE |
        CAPSULE_RIGHT_GRANT,
    CAPSULE_TOKEN_MAGIC_MASK = 0xFF00000000000000ULL,
    CAPSULE_TOKEN_MAGIC_TAG = 0xCA00000000000000ULL,

    DEVICE_CATALOG_MAGIC = 0x44455643,
    DEVICE_CATALOG_VERSION = 1,
    DEVICE_CATALOG_TARGET_VA = 0x3C030000,
    DEVICE_CATALOG_READY = 0x44564352,
    DEVICE_CATALOG_MAX_ENTRIES = 23,
    DEVICE_CATALOG_KIND_CONSOLE = 1,
    DEVICE_CATALOG_KIND_NET = 2,
    DEVICE_CATALOG_KIND_PCI_FUNCTION = 3,

    ROOT_SEED2_IMAGE_VA = 0x28100000,
    BOOTFS_OBJECT_BASE_VA = 0x28200000,
    BOOTFS_OBJECT_SLOT_BYTES = 0x00100000,
    ROOT_SEED2_CONFIG_VA = 0x2A000000,
    BOOT_TEXT_FB_VA = 0x3E000000,
    ROOT_SEED2_CONFIG_MAGIC = 0x32545253,
    ROOT_SEED2_CONFIG_VERSION = 1,
    MAX_ROOT_SEED2_PAGES = 256,

    FS_PAGE_BYTES = 4096,
    FS_MAX_PATH_BYTES = 512,
    FS_REQUEST_MAGIC = 0x51534653u,
    FS_RESPONSE_MAGIC = 0x52534653u,
    FS_PROTOCOL_VERSION = 1,
    FS_REQUEST_HEADER_BYTES = 72,
    FS_RESPONSE_HEADER_BYTES = 72,
    FS_RESPONSE_PAYLOAD_BYTES = FS_PAGE_BYTES - FS_RESPONSE_HEADER_BYTES,
    FS_OP_CONNECT = 1,
    FS_OP_LOOKUP = 16,
    FS_OP_READ = 18,
    FS_OP_OPEN_EXEC = 32,
    FS_STATUS_OK = 0,

    NEXT_ENDPOINT_BASE = 0x81,
    ROOTFS_START_BLOCK = 395264,
};

struct bootfs_header {
    u32 magic;
    u16 version;
    u16 header_bytes;
    u64 image_bytes;
    u32 entry_count;
    u32 entry_bytes;
    u64 entry_table_offset;
    u64 string_table_offset;
    u64 string_table_bytes;
    u64 data_offset;
    u64 data_bytes;
    u32 flags;
    u32 reserved0;
    u64 reserved1[4];
};

struct bootfs_entry {
    u32 path_offset;
    u16 path_bytes;
    u8 kind;
    u8 flags;
    u64 data_offset;
    u64 data_bytes;
    u32 mode_bits;
    u32 reserved0;
    u64 reserved1[2];
};

struct device_queue_grant {
    u64 queue_index;
    u64 submit_token;
    u64 notify_token;
};

struct device_descriptor {
    u64 transport;
    u64 flags;
    u64 bootstrap_source_va;
    u64 vendor_id;
    u64 device_id;
    u64 subsystem_id;
    u64 pci_bus;
    u64 pci_device;
    u64 pci_function;
    u64 resource_id;
    u64 queue_count;
    u64 common_page_paddr;
    u64 notify_page_paddr;
    u64 isr_page_paddr;
    u64 device_page_paddr;
    u64 common_page_offset;
    u64 notify_page_offset;
    u64 isr_page_offset;
    u64 device_page_offset;
    u64 notify_off_multiplier;
    u64 init_iommu_token;
    u64 init_queue_grant_count;
    struct device_queue_grant init_queue_grants[INIT_MAX_DEVICE_QUEUE_GRANTS];
    u64 init_command_token;
    u64 init_device_capsule_token;
};

struct boot_archive_descriptor {
    u64 flags;
    u64 image_va;
    u64 size_bytes;
    u64 page_count;
};

struct spawn_page_descriptor {
    u64 kind;
    u64 subject;
    u64 flags;
    u64 source_va;
    u64 target_va;
    u64 spawn_flags;
};

struct display_descriptor {
    u64 flags;
    u64 width;
    u64 height;
    u64 pitch;
    u64 framebuffer_paddr;
    u64 framebuffer_size_bytes;
};

struct init_config_page {
    u64 magic;
    u64 version;
    u64 descriptor_page_va;
    u64 reserved0;
};

struct init_descriptor_page {
    u64 magic;
    u64 version;
    u64 spawn_page_count;
    u64 device_count;
    u64 boot_image_count;
    u64 reserved_counts[3];
    struct boot_archive_descriptor bootfs_archive;
    struct display_descriptor primary_display;
    struct spawn_page_descriptor spawn_pages[INIT_MAX_SPAWN_PAGE_DESCRIPTORS];
    struct device_descriptor devices[INIT_MAX_DEVICE_DESCRIPTORS];
    u8 boot_images[4096];
    u64 bootfs_page_paddrs[INIT_MAX_BOOT_ARCHIVE_PAGES];
};

struct manager_device_grant {
    u64 device_page_paddr;
    u64 iommu_token;
    u64 queue_grant_count;
    struct device_queue_grant queue_grants[MANAGER_INIT_MAX_DEVICE_QUEUE_GRANTS];
    u64 command_token;
    u64 input_kind_hint;
    u64 device_capsule_token;
};

struct manager_config_page {
    u64 magic;
    u64 version;
    u64 ready;
    u64 device_count;
    u64 framebuffer_vm_token;
    u64 bootfs_vm_token;
    struct manager_device_grant device_grants[MANAGER_INIT_MAX_DEVICE_GRANTS];
};

struct bootstrap_page_descriptor {
    u64 source_va;
    u64 target_va;
    u64 flags;
};

struct bootstrap_cap_descriptor {
    u64 source_token;
    u64 target_token_va;
    u64 rights_bits;
    u8 kind;
    u8 reserved[7];
};

struct bootstrap_descriptor_table {
    u16 page_count;
    u16 cap_count;
    u32 reserved0;
    struct bootstrap_page_descriptor pages[136];
    struct bootstrap_cap_descriptor caps[8];
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

struct loaded_file {
    u64 image_va;
    u64 file_bytes;
    u64 vm_token;
};

static int alloc_root_seed2_image_pages(u64 file_bytes);
static int alloc_pages_at(u64 base_va, u64 file_bytes);

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

struct fs_request_header {
    u32 magic;
    u16 version;
    u16 op;
    u64 request_seq;
    u64 object_token;
    u64 offset;
    u32 length;
    u32 flags;
    u16 path_bytes;
    u16 inline_bytes;
    u32 reserved0;
    u64 arg0;
    u64 arg1;
    u64 session_nonce;
};

struct fs_response_header {
    u32 magic;
    u16 version;
    u16 op;
    u64 response_seq;
    i32 status;
    u32 result_flags;
    u64 result_token;
    u64 file_bytes;
    u64 cursor_next;
    u16 inline_bytes;
    u8 object_kind;
    u8 reserved0;
    u32 reserved1;
    u64 arg0;
    u64 arg1;
};

struct queue_grant {
    u64 iommu_token;
    u64 submit_token;
    u64 notify_token;
    u64 command_token;
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
    u64 device_capsule_token;
};

struct device_catalog_page {
    u64 magic;
    u64 version;
    u64 entry_count;
    u64 reserved0;
    struct device_catalog_entry entries[DEVICE_CATALOG_MAX_ENTRIES];
};

static struct manager_config_page g_handoff;
static struct device_descriptor g_block_device;
static struct device_descriptor g_console_device;
static struct device_descriptor g_net_device;
static u64 g_next_endpoint_id = NEXT_ENDPOINT_BASE;
static u64 g_next_bootstrap_source_va = DYNAMIC_BOOTSTRAP_SOURCE_BASE_VA;
static u64 g_next_inspect_mmio_va = INSPECT_MMIO_BASE_VA;
static u64 g_service_registry_source_va;
static u64 g_block_process_slot;
static u64 g_block_endpoint_id;
static u64 g_fat_process_slot;
static u64 g_fat_endpoint_id;
static u64 g_device_catalog_source_va;
static u64 g_device_catalog_paddr;
static struct backend_session g_fat_session;
static u64 g_root_seed2_page_paddrs[MAX_ROOT_SEED2_PAGES];
static u64 g_next_bootfs_object_va = BOOTFS_OBJECT_BASE_VA;
static unsigned char g_loader_page[4096];
static volatile u32 *g_boot_fb;
static u64 g_boot_fb_width;
static u64 g_boot_fb_height;
static u64 g_boot_fb_pitch;
static u64 g_boot_fb_size_bytes;
static u64 g_boot_text_line;

void *memcpy(void *dst, const void *src, u64 n) {
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    for (u64 i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

void *memset(void *dst, int value, u64 n) {
    u8 *d = (u8 *)dst;
    for (u64 i = 0; i < n; i++) d[i] = (u8)value;
    return dst;
}

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

static void user_log_hex(const char *prefix, u64 value) {
    char buf[96];
    u64 n = 0;
    while (prefix[n] != 0 && n + 19 < sizeof(buf)) {
        buf[n] = prefix[n];
        n++;
    }
    buf[n++] = '0';
    buf[n++] = 'x';
    for (int shift = 60; shift >= 0; shift -= 4) {
        const u8 digit = (u8)((value >> (u64)shift) & 0xF);
        buf[n++] = (char)(digit < 10 ? ('0' + digit) : ('a' + digit - 10));
    }
    buf[n++] = '\n';
    user_log_len(buf, n);
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
    register u64 r8_reg __asm__("r8") = a4;
    u64 ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(nr), "D"(a0), "S"(a1), "d"(a2), "c"(a3), "r"(r8_reg) : "r9", "r10", "r11", "memory");
    return ret;
}

static u64 wait_event_poll(void) {
    return syscall2(SYSCALL_WAIT_EVENT, 0, 1);
}

static u64 map_page_anywhere(u64 paddr, u64 writable) {
    return syscall2(SYSCALL_MAP_PAGE_ANYWHERE, paddr, writable);
}

static int is_ipc_buffer_token(u64 token) {
    return (token & ~IPC_BUFFER_TOKEN_MASK) == IPC_BUFFER_TOKEN_TAG && (token & IPC_BUFFER_TOKEN_MASK) != 0;
}

static u64 create_ipc_buffer_from_page(u64 paddr, u64 rights_bits, u64 role) {
    return syscall3(SYSCALL_CREATE_IPC_BUFFER_FROM_PAGE, paddr, rights_bits, role);
}

static u64 grant_ipc_buffer_on_endpoint(u64 token, u64 endpoint_id, u64 rights_bits) {
    return syscall3(SYSCALL_GRANT_IPC_BUFFER_ON_ENDPOINT, token, endpoint_id, rights_bits);
}

static u64 share_ipc_buffer_on_endpoint(u64 token, u64 endpoint_id, u64 rights_bits) {
    return syscall3(SYSCALL_SHARE_IPC_BUFFER_ON_ENDPOINT, token, endpoint_id, rights_bits);
}

static u64 alloc_map_page(u64 source_va) {
    u64 paddr = 0;
    if (syscall4(SYSCALL_ALLOC_MAP_PAGES, source_va, 1, 1, (u64)&paddr) != 0 || paddr < 0x1000) return 0;
    return source_va;
}

static u64 alloc_bootstrap_page(void) {
    const u64 source_va = g_next_bootstrap_source_va;
    g_next_bootstrap_source_va += 0x1000;
    return alloc_map_page(source_va);
}

static void clear_page(u64 va) {
    volatile u64 *p = (volatile u64 *)va;
    for (u64 i = 0; i < 512; i++) p[i] = 0;
}

static int bytes_eq(const char *a, const u8 *b, u64 len) {
    for (u64 i = 0; i < len; i++) {
        if ((u8)a[i] != b[i]) return 0;
    }
    return a[len] == 0;
}

static u64 encode_queue_cap(u64 kind, u64 token) {
    return QUEUE_CAP_TAG_BASE | (kind << QUEUE_CAP_KIND_SHIFT) | token;
}

static u64 decode_queue_cap(u64 value, u64 kind) {
    if ((value & QUEUE_CAP_TAG_BASE) != QUEUE_CAP_TAG_BASE) return 0;
    if (((value & QUEUE_CAP_KIND_MASK) >> QUEUE_CAP_KIND_SHIFT) != kind) return 0;
    return value & ~(QUEUE_CAP_TAG_BASE | QUEUE_CAP_KIND_MASK);
}

static int is_vm_object_token(u64 token) {
    return (token & VM_OBJECT_TOKEN_TAG) == VM_OBJECT_TOKEN_TAG;
}

static int is_process_builder_token(u64 token) {
    return (token & PROCESS_BUILDER_TOKEN_TAG) == PROCESS_BUILDER_TOKEN_TAG && (token & PROCESS_BUILDER_PROCESS_MASK) != 0;
}

static u64 process_slot_from_builder_token(u64 token) {
    return is_process_builder_token(token) ? (token & PROCESS_BUILDER_PROCESS_MASK) : 0;
}

static u64 decode_started_process_slot(u64 value) {
    if ((value & SPAWN_RESULT_TAG) == 0) return 0;
    return value & SPAWN_RESULT_PROCESS_MASK;
}

static u64 create_suspended_process(void) {
    const u64 token = syscall0(SYSCALL_CREATE_SUSPENDED_PROCESS);
    return is_process_builder_token(token) ? token : 0;
}

static u64 alloc_map_pages_to_process_raw(u64 process_token, u64 va, u64 pages, u64 prot) {
    return syscall5(SYSCALL_ALLOC_MAP_PAGES_TO_PROCESS, process_token, va, pages, prot, 0);
}

static u64 copy_to_process_raw(u64 process_token, u64 dst_va, u64 src_va, u64 bytes) {
    return syscall4(SYSCALL_COPY_TO_PROCESS, process_token, dst_va, src_va, bytes);
}

static int copy_to_process(u64 process_token, u64 dst_va, u64 src_va, u64 bytes) {
    return copy_to_process_raw(process_token, dst_va, src_va, bytes) == SYSCALL_OK;
}

static u64 install_shared_current_page(u64 source_va) {
    const u64 rights = VM_OBJECT_RIGHT_READ | VM_OBJECT_RIGHT_WRITE | VM_OBJECT_RIGHT_MAP;
    const u64 token = syscall3(SYSCALL_CREATE_VM_OBJECT_FROM_CURRENT_PAGES, source_va, 4096, rights);
    if (!is_vm_object_token(token)) return 0;
    if (syscall2(SYSCALL_MAP_VM_OBJECT, token, source_va) != SYSCALL_OK) return 0;
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
    (void)syscall1(SYSCALL_ABORT_PROCESS, process_token);
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
        u64 file_offset = 0;
        if (!file_offset_for_vaddr(source_va, ehdr, copy_start, copy_end - copy_start, file_bytes, &file_offset)) return 0;
        const unsigned char *src = (const unsigned char *)(source_va + file_offset);
        for (u64 j = 0; j < copy_end - copy_start; j++) page[(copy_start - page_vaddr) + j] = src[j];
    }
    return 1;
}

static void write_u64_le(unsigned char *dst, u64 value) {
    for (u64 i = 0; i < 8; i++) dst[i] = (unsigned char)((value >> (i * 8)) & 0xff);
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
        user_log_hex("[seed2_boot] pb elf validate=", (u64)image_status);
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
            if (!add_u64(load_bias, page_vaddr, &target_va)) {
                user_log("[seed2_boot] pb target va overflow\n");
                return 0;
            }
            if (!copy_page_from_elf(image->image_va, &summary.header, image->file_bytes, page_vaddr, g_loader_page)) {
                user_log_hex("[seed2_boot] pb copy page fail va=", page_vaddr);
                return 0;
            }
            const u64 map_status = alloc_map_pages_to_process_raw(process_token, target_va, 1, prot_bits_from_phdr(&phdr));
            if (map_status != SYSCALL_OK) {
                user_log_hex("[seed2_boot] pb map seg status=", map_status);
                user_log_hex("[seed2_boot] pb map seg va=", target_va);
                return 0;
            }
            const u64 copy_status = copy_to_process_raw(process_token, target_va, (u64)g_loader_page, 4096);
            if (copy_status != SYSCALL_OK) {
                user_log_hex("[seed2_boot] pb copy seg status=", copy_status);
                user_log_hex("[seed2_boot] pb copy seg va=", target_va);
                return 0;
            }
        }
    }
    if (!apply_relative_relocations(process_token, image->image_va, &summary.header, image->file_bytes, load_bias)) {
        user_log("[seed2_boot] pb rela failed\n");
        return 0;
    }
    return add_u64(load_bias, summary.header.entry, entry_out);
}

static int install_bootstrap_table(u64 process_token, const struct bootstrap_descriptor_table *table) {
    const u64 child_slot = process_slot_from_builder_token(process_token);
    for (u16 i = 0; i < table->page_count; i++) {
        const u64 prot = (table->pages[i].flags & BOOTSTRAP_PAGE_WRITABLE) != 0 ? 3 : 1;
        if ((table->pages[i].flags & BOOTSTRAP_PAGE_WRITABLE) != 0) {
            const u64 shared = install_shared_current_page(table->pages[i].source_va);
            if (shared == 0 || !map_vm_object_to_process(process_token, shared, table->pages[i].target_va, prot)) {
                user_log_hex("[seed2_boot] pb share boot va=", table->pages[i].target_va);
                return 0;
            }
            continue;
        }
        const u64 map_status = alloc_map_pages_to_process_raw(process_token, table->pages[i].target_va, 1, prot);
        if (map_status != SYSCALL_OK) {
            user_log_hex("[seed2_boot] pb map boot status=", map_status);
            user_log_hex("[seed2_boot] pb map boot va=", table->pages[i].target_va);
            return 0;
        }
        const u64 copy_status = copy_to_process_raw(process_token, table->pages[i].target_va, table->pages[i].source_va, 4096);
        if (copy_status != SYSCALL_OK) {
            user_log_hex("[seed2_boot] pb copy boot status=", copy_status);
            user_log_hex("[seed2_boot] pb copy boot va=", table->pages[i].target_va);
            return 0;
        }
    }
    for (u16 i = 0; i < table->cap_count; i++) {
        if (table->caps[i].kind != BOOTSTRAP_CAP_KIND_VM_OBJECT) return 0;
        const u64 granted = syscall3(SYSCALL_GRANT_VM_OBJECT, table->caps[i].source_token, child_slot, table->caps[i].rights_bits);
        if (!is_vm_object_token(granted)) return 0;
        if (!copy_to_process(process_token, table->caps[i].target_token_va, (u64)&granted, sizeof(granted))) return 0;
    }
    return 1;
}

static u64 launch_process_builder_image(const struct loaded_file *image, const struct bootstrap_descriptor_table *table, int bootstrap_owner) {
    const u64 process_token = create_suspended_process();
    if (process_token == 0) {
        user_log("[seed2_boot] pb create failed\n");
        return 0;
    }
    u64 entry = 0;
    if (!load_elf_private(process_token, image, &entry)) {
        abort_process(process_token);
        return 0;
    }
    const u64 stack_status = alloc_map_pages_to_process_raw(process_token, USER_STACK_BOTTOM_VA, USER_STACK_PAGES, 3);
    if (stack_status != SYSCALL_OK) {
        user_log_hex("[seed2_boot] pb stack status=", stack_status);
        abort_process(process_token);
        return 0;
    }
    if (!install_bootstrap_table(process_token, table)) {
        abort_process(process_token);
        return 0;
    }
    if (bootstrap_owner && !set_process_bootstrap_owner(process_token, 1)) {
        user_log("[seed2_boot] pb owner failed\n");
        abort_process(process_token);
        return 0;
    }
    if (!set_process_initial_context(process_token, entry, USER_ENTRY_RSP)) {
        user_log_hex("[seed2_boot] pb ctx entry=", entry);
        abort_process(process_token);
        return 0;
    }
    const u64 slot = start_process(process_token);
    if (slot == 0) {
        user_log("[seed2_boot] pb start failed\n");
        abort_process(process_token);
    }
    return slot;
}

static struct init_descriptor_page *descriptor_page(void) {
    volatile struct init_config_page *cfg = (volatile struct init_config_page *)PROCESS_STANDARD_CONFIG_TARGET_VA;
    if (cfg->magic != INIT_CONFIG_MAGIC || cfg->version != INIT_CONFIG_VERSION || cfg->descriptor_page_va == 0) return 0;
    struct init_descriptor_page *page = (struct init_descriptor_page *)cfg->descriptor_page_va;
    if (page->magic != INIT_BOOTSTRAP_MAGIC || page->version != INIT_BOOTSTRAP_VERSION) return 0;
    return page;
}

static u8 boot_glyph_row(char ch, u64 row) {
    static const u8 upper[26][7] = {
        {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11},
        {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E},
        {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E},
        {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E},
        {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F},
        {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10},
        {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F},
        {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11},
        {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E},
        {0x01, 0x01, 0x01, 0x01, 0x11, 0x11, 0x0E},
        {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11},
        {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F},
        {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11},
        {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11},
        {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},
        {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10},
        {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D},
        {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11},
        {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E},
        {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04},
        {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},
        {0x11, 0x11, 0x11, 0x11, 0x0A, 0x0A, 0x04},
        {0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11},
        {0x11, 0x0A, 0x04, 0x04, 0x04, 0x0A, 0x11},
        {0x11, 0x0A, 0x04, 0x04, 0x04, 0x04, 0x04},
        {0x1F, 0x02, 0x04, 0x08, 0x10, 0x10, 0x1F},
    };
    static const u8 digits[10][7] = {
        {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E},
        {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
        {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},
        {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E},
        {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},
        {0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E},
        {0x0E, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x0E},
        {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
        {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},
        {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x01, 0x0E},
    };
    if (row >= 7) return 0;
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
    if (ch >= 'A' && ch <= 'Z') return upper[ch - 'A'][row];
    if (ch >= '0' && ch <= '9') return digits[ch - '0'][row];
    if (ch == '-') return row == 3 ? 0x1F : 0;
    if (ch == '_') return row == 6 ? 0x1F : 0;
    if (ch == ':') return (row == 2 || row == 4) ? 0x04 : 0;
    if (ch == '.') return row == 6 ? 0x04 : 0;
    if (ch == '/') return (u8)(0x01 << (6 - row));
    if (ch == ' ') return 0;
    return (row == 0 || row == 6) ? 0x0E : (row == 5 ? 0x04 : 0x11);
}

static void boot_fb_put_pixel(u64 x, u64 y, u32 color) {
    if (!g_boot_fb || x >= g_boot_fb_width || y >= g_boot_fb_height) return;
    const u64 index = y * g_boot_fb_pitch + x;
    if (index >= g_boot_fb_size_bytes / 4) return;
    g_boot_fb[index] = color;
}

static void boot_draw_text(u64 x, u64 y, const char *text, u64 scale, u32 color) {
    for (u64 i = 0; text[i] != 0; i++) {
        const u64 glyph_x = x + i * 6 * scale;
        for (u64 gy = 0; gy < 7; gy++) {
            const u8 bits = boot_glyph_row(text[i], gy);
            for (u64 gx = 0; gx < 5; gx++) {
                if ((bits & (1u << (4 - gx))) == 0) continue;
                for (u64 sy = 0; sy < scale; sy++) {
                    for (u64 sx = 0; sx < scale; sx++) {
                        boot_fb_put_pixel(glyph_x + gx * scale + sx, y + gy * scale + sy, color);
                    }
                }
            }
        }
    }
}

static void boot_screen_line(const char *text) {
    if (!g_boot_fb) return;
    const u64 scale = 2;
    const u64 x = 32;
    const u64 y = 96 + g_boot_text_line * 24;
    if (y + 14 >= g_boot_fb_height) return;
    boot_draw_text(x, y, text, scale, 0x00D8F8D8);
    g_boot_text_line++;
}

static void boot_screen_init(void) {
    struct init_descriptor_page *page = descriptor_page();
    if (!page) return;
    struct display_descriptor display = page->primary_display;
    if ((display.flags & INIT_DISPLAY_FLAG_PRESENT) == 0) return;
    if (display.width == 0 || display.height == 0 || display.pitch == 0 || display.framebuffer_size_bytes < 4096) return;

    const u64 base_paddr = display.framebuffer_paddr & ~0xFFFULL;
    const u64 offset = display.framebuffer_paddr - base_paddr;
    const u64 map_bytes = (offset + display.framebuffer_size_bytes + 4095) & ~0xFFFULL;
    const u64 page_count = map_bytes / 4096;
    for (u64 i = 0; i < page_count; i++) {
        const u64 paddr = base_paddr + i * 4096;
        const u64 va = BOOT_TEXT_FB_VA + i * 4096;
        if (syscall2(SYSCALL_INSTALL_MMIO_CAP, paddr, PAGE_RIGHT_CPU_READ | PAGE_RIGHT_CPU_WRITE) != SYSCALL_OK) return;
        if (syscall3(SYSCALL_MAP_PAGE, va, paddr, 1) != SYSCALL_OK) return;
    }

    g_boot_fb = (volatile u32 *)(BOOT_TEXT_FB_VA + offset);
    g_boot_fb_width = display.width;
    g_boot_fb_height = display.height;
    g_boot_fb_pitch = display.pitch;
    g_boot_fb_size_bytes = display.framebuffer_size_bytes;
    g_boot_text_line = 0;

    const u64 max_pixels = g_boot_fb_size_bytes / 4;
    for (u64 y = 0; y < g_boot_fb_height; y++) {
        for (u64 x = 0; x < g_boot_fb_pitch; x++) {
            const u64 index = y * g_boot_fb_pitch + x;
            if (index >= max_pixels) break;
            g_boot_fb[index] = 0x0006080C;
        }
    }
    boot_draw_text(32, 36, "PACHAOS", 4, 0x00F8F8F8);
    boot_screen_line("SEED2_BOOT DIRECT INIT");
}

static struct bootfs_header *bootfs_header(void);

static int map_bootfs_archive(void) {
    struct init_descriptor_page *page = descriptor_page();
    if (!page) {
        user_log("[seed2_boot] bootfs map no descriptor\n");
        return 0;
    }
    if ((page->bootfs_archive.flags & INIT_BOOT_ARCHIVE_FLAG_PRESENT) == 0) {
        user_log_hex("[seed2_boot] bootfs flags=", page->bootfs_archive.flags);
        return 0;
    }
    return bootfs_header() != 0;
}

static struct bootfs_header *bootfs_header(void) {
    struct init_descriptor_page *page = descriptor_page();
    if (!page) return 0;
    struct boot_archive_descriptor archive = page->bootfs_archive;
    if ((archive.flags & INIT_BOOT_ARCHIVE_FLAG_PRESENT) == 0) return 0;
    if (archive.image_va == 0 || archive.size_bytes < sizeof(struct bootfs_header)) return 0;
    struct bootfs_header *header = (struct bootfs_header *)archive.image_va;
    if (header->magic != BOOTFS_MAGIC || header->version != BOOTFS_VERSION) return 0;
    if (header->image_bytes > archive.size_bytes) return 0;
    return header;
}

static int open_image_from_bootfs(const char *path, struct loaded_file *out) {
    struct bootfs_header *header = bootfs_header();
    if (!header) {
        user_log("[seed2_boot] bootfs header invalid\n");
        return 0;
    }
    struct bootfs_entry *entries = (struct bootfs_entry *)((u64)header + header->entry_table_offset);
    for (u32 i = 0; i < header->entry_count; i++) {
        struct bootfs_entry *entry = &entries[i];
        if (entry->kind != BOOTFS_KIND_REGULAR) continue;
        const u8 *entry_path = (const u8 *)((u64)header + header->string_table_offset + entry->path_offset);
        if (!bytes_eq(path, entry_path, entry->path_bytes)) continue;
        if (entry->data_offset < header->data_offset || entry->data_offset + entry->data_bytes > header->image_bytes) return 0;
        const u64 object_va = g_next_bootfs_object_va;
        g_next_bootfs_object_va += BOOTFS_OBJECT_SLOT_BYTES;
        if (!alloc_pages_at(object_va, entry->data_bytes)) return 0;
        const u8 *src = (const u8 *)((u64)header + entry->data_offset);
        u8 *dst = (u8 *)object_va;
        for (u64 j = 0; j < entry->data_bytes; j++) dst[j] = src[j];
        const u64 vm_token = create_vm_object_from_current_pages(object_va, entry->data_bytes, 0xF);
        if ((vm_token & VM_OBJECT_TOKEN_TAG) == 0) {
            user_log("[seed2_boot] bootfs install vm failed\n");
            return 0;
        }
        out->image_va = object_va;
        out->file_bytes = entry->data_bytes;
        out->vm_token = vm_token;
        return 1;
    }
    user_log("[seed2_boot] bootfs path not found\n");
    return 0;
}

static int descriptor_is_seed2_boot_device(const struct device_descriptor *d) {
    if ((d->flags & INIT_DEVICE_FLAG_PRESENT) == 0) return 0;
    if (d->vendor_id != VIRTIO_VENDOR_ID) return 0;
    if ((d->device_id == VIRTIO_BLK_DEVICE_MODERN || d->device_id == VIRTIO_BLK_DEVICE_LEGACY) &&
        d->subsystem_id == VIRTIO_BLK_SUBSYSTEM_ID) return 1;
    if (d->device_id == VIRTIO_CONSOLE_DEVICE_MODERN || d->device_id == VIRTIO_CONSOLE_DEVICE_LEGACY ||
        d->subsystem_id == VIRTIO_CONSOLE_SUBSYSTEM_ID) return 1;
    if (d->device_id == VIRTIO_NET_DEVICE_MODERN || d->device_id == VIRTIO_NET_DEVICE_LEGACY ||
        d->subsystem_id == VIRTIO_NET_SUBSYSTEM_ID) return 1;
    return 0;
}

static void init_handoff_from_descriptor_page(void) {
    struct init_descriptor_page *page = descriptor_page();
    if (!page) {
        user_log("[seed2_boot] descriptor page missing\n");
        return;
    }
    memset(&g_handoff, 0, sizeof(g_handoff));
    g_handoff.magic = MANAGER_INIT_MAGIC;
    g_handoff.version = MANAGER_INIT_VERSION;
    g_handoff.ready = 1;

    u64 out_index = 0;
    for (u64 i = 0; i < page->device_count && i < INIT_MAX_DEVICE_DESCRIPTORS; i++) {
        struct device_descriptor *d = &page->devices[i];
        if (!descriptor_is_seed2_boot_device(d)) continue;
        if (out_index >= MANAGER_INIT_MAX_DEVICE_GRANTS) break;
        struct manager_device_grant *grant = &g_handoff.device_grants[out_index++];
        grant->device_page_paddr = d->device_page_paddr;
        grant->iommu_token = d->init_iommu_token;
        grant->command_token = d->init_command_token;
        grant->device_capsule_token = d->init_device_capsule_token;
        grant->queue_grant_count = d->init_queue_grant_count;
        if (grant->queue_grant_count > MANAGER_INIT_MAX_DEVICE_QUEUE_GRANTS) {
            grant->queue_grant_count = MANAGER_INIT_MAX_DEVICE_QUEUE_GRANTS;
        }
        for (u64 q = 0; q < grant->queue_grant_count; q++) {
            grant->queue_grants[q] = d->init_queue_grants[q];
        }
    }
    g_handoff.device_count = out_index;
}

static int find_block_device(void) {
    struct init_descriptor_page *page = descriptor_page();
    if (!page) return 0;
    for (u64 i = 0; i < page->device_count && i < INIT_MAX_DEVICE_DESCRIPTORS; i++) {
        struct device_descriptor *d = &page->devices[i];
        if ((d->flags & INIT_DEVICE_FLAG_PRESENT) == 0) continue;
        if (d->vendor_id == VIRTIO_VENDOR_ID &&
            (d->device_id == VIRTIO_BLK_DEVICE_MODERN || d->device_id == VIRTIO_BLK_DEVICE_LEGACY) &&
            d->subsystem_id == VIRTIO_BLK_SUBSYSTEM_ID)
        {
            g_block_device = *d;
            return 1;
        }
    }
    return 0;
}

static void device_catalog_add(struct device_descriptor *device, u64 kind);

static int find_console_device(void) {
    struct init_descriptor_page *page = descriptor_page();
    if (!page) return 0;
    for (u64 i = 0; i < page->device_count && i < INIT_MAX_DEVICE_DESCRIPTORS; i++) {
        struct device_descriptor *d = &page->devices[i];
        if ((d->flags & INIT_DEVICE_FLAG_PRESENT) == 0) continue;
        if (d->vendor_id == VIRTIO_VENDOR_ID &&
            (d->device_id == VIRTIO_CONSOLE_DEVICE_MODERN || d->device_id == VIRTIO_CONSOLE_DEVICE_LEGACY ||
                d->subsystem_id == VIRTIO_CONSOLE_SUBSYSTEM_ID))
        {
            g_console_device = *d;
            return 1;
        }
    }
    return 0;
}

static int find_net_device(void) {
    struct init_descriptor_page *page = descriptor_page();
    if (!page) return 0;
    for (u64 i = 0; i < page->device_count && i < INIT_MAX_DEVICE_DESCRIPTORS; i++) {
        struct device_descriptor *d = &page->devices[i];
        if ((d->flags & INIT_DEVICE_FLAG_PRESENT) == 0) continue;
        if (d->vendor_id == VIRTIO_VENDOR_ID &&
            (d->device_id == VIRTIO_NET_DEVICE_MODERN || d->device_id == VIRTIO_NET_DEVICE_LEGACY ||
                d->subsystem_id == VIRTIO_NET_SUBSYSTEM_ID))
        {
            g_net_device = *d;
            return 1;
        }
    }
    return 0;
}

static void device_catalog_add_pci_functions(void) {
    struct init_descriptor_page *page = descriptor_page();
    if (!page) return;
    for (u64 i = 0; i < page->device_count && i < INIT_MAX_DEVICE_DESCRIPTORS; i++) {
        struct device_descriptor *d = &page->devices[i];
        if ((d->flags & INIT_DEVICE_FLAG_PRESENT) == 0) continue;
        if (d->transport != INIT_DEVICE_TRANSPORT_PCI_FUNCTION) continue;
        if (d->init_device_capsule_token == 0) continue;
        device_catalog_add(d, DEVICE_CATALOG_KIND_PCI_FUNCTION);
    }
}

static int find_block_queue_grant(struct queue_grant *out) {
    for (u64 i = 0; i < g_handoff.device_count && i < MANAGER_INIT_MAX_DEVICE_GRANTS; i++) {
        struct manager_device_grant *grant = &g_handoff.device_grants[i];
        if (grant->device_page_paddr != g_block_device.device_page_paddr) continue;
        if (grant->iommu_token == 0 || grant->command_token == 0) return 0;
        for (u64 q = 0; q < grant->queue_grant_count && q < MANAGER_INIT_MAX_DEVICE_QUEUE_GRANTS; q++) {
            if (grant->queue_grants[q].queue_index != 0) continue;
            if (grant->queue_grants[q].submit_token == 0 || grant->queue_grants[q].notify_token == 0) return 0;
            out->iommu_token = grant->iommu_token;
            out->submit_token = grant->queue_grants[q].submit_token;
            out->notify_token = grant->queue_grants[q].notify_token;
            out->command_token = grant->command_token;
            return 1;
        }
    }
    return 0;
}

static int find_device_queue_grant(struct device_descriptor *device, u64 queue_index, struct queue_grant *out) {
    for (u64 i = 0; i < g_handoff.device_count && i < MANAGER_INIT_MAX_DEVICE_GRANTS; i++) {
        struct manager_device_grant *grant = &g_handoff.device_grants[i];
        if (grant->device_page_paddr != device->device_page_paddr) continue;
        if (grant->iommu_token == 0 || grant->command_token == 0) return 0;
        for (u64 q = 0; q < grant->queue_grant_count && q < MANAGER_INIT_MAX_DEVICE_QUEUE_GRANTS; q++) {
            if (grant->queue_grants[q].queue_index != queue_index) continue;
            if (grant->queue_grants[q].submit_token == 0 || grant->queue_grants[q].notify_token == 0) return 0;
            out->iommu_token = grant->iommu_token;
            out->submit_token = grant->queue_grants[q].submit_token;
            out->notify_token = grant->queue_grants[q].notify_token;
            out->command_token = grant->command_token;
            return 1;
        }
    }
    return 0;
}

static int install_device_mmio_caps_for(struct device_descriptor *device) {
    if (syscall2(SYSCALL_INSTALL_MMIO_CAP, device->common_page_paddr, PAGE_RIGHT_CPU_READ | PAGE_RIGHT_CPU_WRITE | PAGE_RIGHT_GRANT) != 0) return 0;
    if (syscall2(SYSCALL_INSTALL_MMIO_CAP, device->notify_page_paddr, PAGE_RIGHT_CPU_READ | PAGE_RIGHT_CPU_WRITE | PAGE_RIGHT_GRANT) != 0) return 0;
    if (device->isr_page_paddr != 0 &&
        syscall2(SYSCALL_INSTALL_MMIO_CAP, device->isr_page_paddr, PAGE_RIGHT_CPU_READ | PAGE_RIGHT_GRANT) != 0) return 0;
    if (device->device_page_paddr != 0 &&
        syscall2(SYSCALL_INSTALL_MMIO_CAP, device->device_page_paddr, PAGE_RIGHT_CPU_READ | PAGE_RIGHT_CPU_WRITE | PAGE_RIGHT_GRANT) != 0) return 0;
    return 1;
}

static int grant_device_mmio(struct device_descriptor *device, u64 child_slot) {
    if (syscall3(SYSCALL_GRANT_CAP, device->common_page_paddr, child_slot, PAGE_RIGHT_CPU_READ | PAGE_RIGHT_CPU_WRITE) != 0) return 0;
    if (syscall3(SYSCALL_GRANT_CAP, device->notify_page_paddr, child_slot, PAGE_RIGHT_CPU_READ | PAGE_RIGHT_CPU_WRITE) != 0) return 0;
    if (device->isr_page_paddr != 0 &&
        syscall3(SYSCALL_GRANT_CAP, device->isr_page_paddr, child_slot, PAGE_RIGHT_CPU_READ) != 0) return 0;
    if (device->device_page_paddr != 0 &&
        syscall3(SYSCALL_GRANT_CAP, device->device_page_paddr, child_slot, PAGE_RIGHT_CPU_READ | PAGE_RIGHT_CPU_WRITE) != 0) return 0;
    return 1;
}

static int grant_device_mmio_with_grant(struct device_catalog_entry *entry, u64 child_slot) {
    u64 status = syscall3(SYSCALL_GRANT_CAP, entry->common_page_paddr, child_slot, PAGE_RIGHT_CPU_READ | PAGE_RIGHT_CPU_WRITE | PAGE_RIGHT_GRANT);
    if (status != 0) {
        user_log_hex("[seed2_boot] catalog common grant status=", status);
        return 0;
    }
    status = syscall3(SYSCALL_GRANT_CAP, entry->notify_page_paddr, child_slot, PAGE_RIGHT_CPU_READ | PAGE_RIGHT_CPU_WRITE | PAGE_RIGHT_GRANT);
    if (status != 0) {
        user_log_hex("[seed2_boot] catalog notify grant status=", status);
        return 0;
    }
    if (entry->isr_page_paddr != 0) {
        status = syscall3(SYSCALL_GRANT_CAP, entry->isr_page_paddr, child_slot, PAGE_RIGHT_CPU_READ | PAGE_RIGHT_GRANT);
        if (status != 0) {
            user_log_hex("[seed2_boot] catalog isr grant status=", status);
            return 0;
        }
    }
    if (entry->device_page_paddr != 0) {
        status = syscall3(SYSCALL_GRANT_CAP, entry->device_page_paddr, child_slot, PAGE_RIGHT_CPU_READ | PAGE_RIGHT_CPU_WRITE | PAGE_RIGHT_GRANT);
        if (status != 0) {
            user_log_hex("[seed2_boot] catalog device grant status=", status);
            return 0;
        }
    }
    return 1;
}

static u64 map_device_cfg_for_read(void) {
    const u64 va = g_next_inspect_mmio_va;
    g_next_inspect_mmio_va += 0x1000;
    if (syscall3(SYSCALL_MAP_PAGE, va, g_block_device.device_page_paddr, 0) != 0) return 0;
    return va + g_block_device.device_page_offset;
}

static u32 mmio_read_u32(u64 addr) {
    volatile u32 *p = (volatile u32 *)addr;
    return *p;
}

static u64 mmio_read_u64(u64 addr) {
    volatile u64 *p = (volatile u64 *)addr;
    return *p;
}

static void service_registry_init(u64 va) {
    clear_page(va);
    volatile struct service_registry_page *page = (volatile struct service_registry_page *)va;
    page->magic = SERVICE_REGISTRY_MAGIC;
    page->version = SERVICE_REGISTRY_VERSION;
}

static void service_registry_set(u64 va, u64 kind, u64 process_slot, u64 endpoint_id) {
    volatile struct service_registry_page *page = (volatile struct service_registry_page *)va;
    if (page->magic != SERVICE_REGISTRY_MAGIC || page->version != SERVICE_REGISTRY_VERSION) service_registry_init(va);
    for (u64 i = 0; i < page->entry_count && i < SERVICE_REGISTRY_MAX_ENTRIES; i++) {
        if (page->entries[i].kind != kind) continue;
        page->entries[i].process_slot = process_slot;
        page->entries[i].endpoint_id = endpoint_id;
        page->entries[i].flags = SERVICE_FLAG_PROCESS_SLOT_COMPAT;
        return;
    }
    if (page->entry_count >= SERVICE_REGISTRY_MAX_ENTRIES) return;
    u64 index = page->entry_count++;
    page->entries[index].kind = kind;
    page->entries[index].process_slot = process_slot;
    page->entries[index].endpoint_id = endpoint_id;
    page->entries[index].flags = SERVICE_FLAG_PROCESS_SLOT_COMPAT;
}

static u64 ensure_service_registry(void) {
    if (g_service_registry_source_va != 0) return g_service_registry_source_va;
    g_service_registry_source_va = alloc_bootstrap_page();
    service_registry_init(g_service_registry_source_va);
    return g_service_registry_source_va;
}

static void device_catalog_add(struct device_descriptor *device, u64 kind) {
    if (g_device_catalog_source_va == 0) return;
    volatile struct device_catalog_page *page = (volatile struct device_catalog_page *)g_device_catalog_source_va;
    if (page->entry_count >= DEVICE_CATALOG_MAX_ENTRIES) return;
    volatile struct device_catalog_entry *entry = &page->entries[page->entry_count++];
    entry->present = 1;
    entry->kind = kind;
    entry->vendor_id = device->vendor_id;
    entry->device_id = device->device_id;
    entry->subsystem_id = device->subsystem_id;
    entry->resource_id = device->resource_id;
    entry->common_page_paddr = device->common_page_paddr;
    entry->notify_page_paddr = device->notify_page_paddr;
    entry->isr_page_paddr = device->isr_page_paddr;
    entry->device_page_paddr = device->device_page_paddr;
    entry->common_page_offset = device->common_page_offset;
    entry->notify_page_offset = device->notify_page_offset;
    entry->isr_page_offset = device->isr_page_offset;
    entry->device_page_offset = device->device_page_offset;
    entry->notify_off_multiplier = device->notify_off_multiplier;
    entry->device_capsule_token = device->init_device_capsule_token;
}

static u64 ensure_device_catalog(void) {
    if (g_device_catalog_source_va != 0) return g_device_catalog_source_va;
    g_device_catalog_source_va = g_next_bootstrap_source_va;
    g_next_bootstrap_source_va += 0x1000;
    if (syscall4(SYSCALL_ALLOC_MAP_PAGES, g_device_catalog_source_va, 1, 1, (u64)&g_device_catalog_paddr) != 0 || g_device_catalog_paddr < 0x1000) {
        user_log("[seed2_boot] device catalog alloc failed\n");
        g_device_catalog_source_va = 0;
        g_device_catalog_paddr = 0;
        return 0;
    }
    clear_page(g_device_catalog_source_va);
    volatile struct device_catalog_page *page = (volatile struct device_catalog_page *)g_device_catalog_source_va;
    page->magic = DEVICE_CATALOG_MAGIC;
    page->version = DEVICE_CATALOG_VERSION;

    if (find_console_device() && install_device_mmio_caps_for(&g_console_device)) {
        device_catalog_add(&g_console_device, DEVICE_CATALOG_KIND_CONSOLE);
    } else {
        user_log("[seed2_boot] console catalog entry skipped\n");
    }
    if (find_net_device() && install_device_mmio_caps_for(&g_net_device)) {
        device_catalog_add(&g_net_device, DEVICE_CATALOG_KIND_NET);
    } else {
        user_log("[seed2_boot] net catalog entry skipped\n");
    }
    device_catalog_add_pci_functions();
    return g_device_catalog_source_va;
}

static int fill_catalog_queue_tokens_for_child(volatile struct device_catalog_entry *entry, u64 child_slot) {
    struct device_descriptor *device = entry->kind == DEVICE_CATALOG_KIND_CONSOLE ? &g_console_device : &g_net_device;
    struct queue_grant q0;
    struct queue_grant q1;
    if (!find_device_queue_grant(device, 0, &q0) || !find_device_queue_grant(device, 1, &q1)) {
        user_log_hex("[seed2_boot] catalog queue find failed kind=", entry->kind);
        return 0;
    }

    const u64 iommu_raw = syscall2(SYSCALL_GRANT_QUEUE_CAP, encode_queue_cap(QUEUE_CAP_KIND_IOMMU, q0.iommu_token), child_slot);
    const u64 q0_submit_raw = syscall2(SYSCALL_GRANT_QUEUE_CAP, encode_queue_cap(QUEUE_CAP_KIND_VIRTQUEUE, q0.submit_token), child_slot);
    const u64 q0_notify_raw = syscall2(SYSCALL_GRANT_QUEUE_CAP, encode_queue_cap(QUEUE_CAP_KIND_VIRTQUEUE, q0.notify_token), child_slot);
    const u64 q1_submit_raw = syscall2(SYSCALL_GRANT_QUEUE_CAP, encode_queue_cap(QUEUE_CAP_KIND_VIRTQUEUE, q1.submit_token), child_slot);
    const u64 q1_notify_raw = syscall2(SYSCALL_GRANT_QUEUE_CAP, encode_queue_cap(QUEUE_CAP_KIND_VIRTQUEUE, q1.notify_token), child_slot);
    const u64 command_raw = syscall2(SYSCALL_GRANT_QUEUE_CAP, encode_queue_cap(QUEUE_CAP_KIND_COMMAND, q0.command_token), child_slot);
    const u64 iommu_child = decode_queue_cap(iommu_raw, QUEUE_CAP_KIND_IOMMU);
    const u64 q0_submit_child = decode_queue_cap(q0_submit_raw, QUEUE_CAP_KIND_VIRTQUEUE);
    const u64 q0_notify_child = decode_queue_cap(q0_notify_raw, QUEUE_CAP_KIND_VIRTQUEUE);
    const u64 q1_submit_child = decode_queue_cap(q1_submit_raw, QUEUE_CAP_KIND_VIRTQUEUE);
    const u64 q1_notify_child = decode_queue_cap(q1_notify_raw, QUEUE_CAP_KIND_VIRTQUEUE);
    const u64 command_child = decode_queue_cap(command_raw, QUEUE_CAP_KIND_COMMAND);
    if (iommu_child == 0 || q0_submit_child == 0 || q0_notify_child == 0 || q1_submit_child == 0 || q1_notify_child == 0 || command_child == 0) {
        user_log_hex("[seed2_boot] catalog queue grant failed kind=", entry->kind);
        user_log_hex("[seed2_boot] catalog iommu raw=", iommu_raw);
        user_log_hex("[seed2_boot] catalog q0 submit raw=", q0_submit_raw);
        user_log_hex("[seed2_boot] catalog q0 notify raw=", q0_notify_raw);
        user_log_hex("[seed2_boot] catalog q1 submit raw=", q1_submit_raw);
        user_log_hex("[seed2_boot] catalog q1 notify raw=", q1_notify_raw);
        user_log_hex("[seed2_boot] catalog command raw=", command_raw);
        return 0;
    }
    entry->iommu_token = iommu_child;
    entry->queue0_submit_token = q0_submit_child;
    entry->queue0_notify_token = q0_notify_child;
    entry->queue1_submit_token = q1_submit_child;
    entry->queue1_notify_token = q1_notify_child;
    entry->command_token = command_child;
    return 1;
}

static int grant_catalog_capsule_to_child(volatile struct device_catalog_entry *entry, u64 child_slot) {
    if (entry->device_capsule_token == 0) return 1;
    const u64 granted = syscall3(
        SYSCALL_CAPSULE_GRANT,
        entry->device_capsule_token,
        child_slot,
        CAPSULE_RIGHT_DEVICE_TO_ROOT
    );
    if ((granted & CAPSULE_TOKEN_MAGIC_MASK) != CAPSULE_TOKEN_MAGIC_TAG) {
        user_log_hex("[seed2_boot] catalog capsule grant failed kind=", entry->kind);
        user_log_hex("[seed2_boot] catalog capsule grant status=", granted);
        return 0;
    }
    entry->device_capsule_token = granted;
    return 1;
}

static int grant_device_catalog_to_child(u64 child_slot) {
    if (g_device_catalog_source_va == 0) return 1;
    volatile struct device_catalog_page *page = (volatile struct device_catalog_page *)g_device_catalog_source_va;
    for (u64 i = 0; i < page->entry_count && i < DEVICE_CATALOG_MAX_ENTRIES; i++) {
        volatile struct device_catalog_entry *entry = &page->entries[i];
        if (entry->present == 0) continue;
        if (!grant_catalog_capsule_to_child(entry, child_slot)) return 0;
        if (entry->kind == DEVICE_CATALOG_KIND_PCI_FUNCTION) continue;
        if (!grant_device_mmio_with_grant((struct device_catalog_entry *)entry, child_slot)) return 0;
        if (!fill_catalog_queue_tokens_for_child(entry, child_slot)) return 0;
    }
    return 1;
}

static void wait_config_word(u64 va, u64 index, u64 expected) {
    volatile u64 *words = (volatile u64 *)va;
    while (words[index] != expected) {
        wait_event_poll();
    }
}

static int install_fat_endpoint_for_boot(void) {
    if (g_fat_session.endpoint_id == 0 || g_fat_session.process_slot == 0) return 0;
    return syscall3(SYSCALL_INSTALL_ENDPOINT, 0, g_fat_session.endpoint_id, g_fat_session.process_slot) == SYSCALL_OK;
}

static u64 grant_fat_response_buffer(void) {
    u64 ret = grant_ipc_buffer_on_endpoint(
        g_fat_session.response_token,
        g_fat_session.endpoint_id,
        IPC_BUFFER_RIGHT_READ | IPC_BUFFER_RIGHT_WRITE | IPC_BUFFER_RIGHT_MAP
    );
    if (is_ipc_buffer_token(ret)) return ret;
    if (ret == SYSCALL_ERR_ENDPOINT && install_fat_endpoint_for_boot()) {
        ret = grant_ipc_buffer_on_endpoint(
            g_fat_session.response_token,
            g_fat_session.endpoint_id,
            IPC_BUFFER_RIGHT_READ | IPC_BUFFER_RIGHT_WRITE | IPC_BUFFER_RIGHT_MAP
        );
    }
    return is_ipc_buffer_token(ret) ? ret : 0;
}

static int share_fat_request_buffer(void) {
    u64 ret = share_ipc_buffer_on_endpoint(
        g_fat_session.request_token,
        g_fat_session.endpoint_id,
        IPC_BUFFER_RIGHT_READ | IPC_BUFFER_RIGHT_MAP
    );
    if (ret == SYSCALL_OK) return 1;
    if (ret == SYSCALL_ERR_ENDPOINT && install_fat_endpoint_for_boot()) {
        ret = share_ipc_buffer_on_endpoint(
            g_fat_session.request_token,
            g_fat_session.endpoint_id,
            IPC_BUFFER_RIGHT_READ | IPC_BUFFER_RIGHT_MAP
        );
    }
    return ret == SYSCALL_OK;
}

static int signal_fat_for_boot(void) {
    u64 ret = syscall2(SYSCALL_SIGNAL_ENDPOINT, g_fat_session.endpoint_id, 0);
    if (ret == SYSCALL_OK) return 1;
    if (ret == SYSCALL_ERR_ENDPOINT && install_fat_endpoint_for_boot()) {
        ret = syscall2(SYSCALL_SIGNAL_ENDPOINT, g_fat_session.endpoint_id, 0);
    }
    return ret == SYSCALL_OK;
}

static int wait_fat_response(u64 expected_seq, u16 expected_op) {
    volatile struct fs_response_header *response = (volatile struct fs_response_header *)g_fat_session.response_va;
    for (u64 i = 0; i < 8192; i++) {
        if (response->response_seq == expected_seq) {
            return response->magic == FS_RESPONSE_MAGIC &&
                response->version == FS_PROTOCOL_VERSION &&
                response->op == expected_op;
        }
        (void)wait_event_poll();
    }
    return 0;
}

static u64 make_fat_nonce(u64 request_token, u64 response_token, u64 endpoint_id, u64 process_slot) {
    u64 nonce =
        request_token ^
        ((response_token << 17) | (response_token >> 47)) ^
        ((endpoint_id << 7) | (endpoint_id >> 57)) ^
        process_slot ^
        0x5eed2002b0075ULL;
    return nonce == 0 ? 1 : nonce;
}

static int connect_fat_for_root_spawn(void) {
    g_fat_session.endpoint_id = g_fat_endpoint_id;
    g_fat_session.process_slot = g_fat_process_slot;
    if (g_fat_session.endpoint_id == 0 || g_fat_session.process_slot == 0) return 0;

    g_fat_session.request_paddr = syscall0(SYSCALL_ALLOC_PAGE);
    g_fat_session.response_paddr = syscall0(SYSCALL_ALLOC_PAGE);
    if (g_fat_session.request_paddr < 0x1000 || g_fat_session.response_paddr < 0x1000) return 0;
    g_fat_session.request_va = map_page_anywhere(g_fat_session.request_paddr, 1);
    g_fat_session.response_va = map_page_anywhere(g_fat_session.response_paddr, 1);
    if (g_fat_session.request_va < 0x1000 || g_fat_session.response_va < 0x1000) return 0;
    const u64 owner_rights = IPC_BUFFER_RIGHT_READ | IPC_BUFFER_RIGHT_WRITE | IPC_BUFFER_RIGHT_MAP | IPC_BUFFER_RIGHT_GRANT;
    g_fat_session.request_token = create_ipc_buffer_from_page(g_fat_session.request_paddr, owner_rights, IPC_BUFFER_ROLE_REQUEST);
    g_fat_session.response_token = create_ipc_buffer_from_page(g_fat_session.response_paddr, owner_rights, IPC_BUFFER_ROLE_RESPONSE);
    if (!is_ipc_buffer_token(g_fat_session.request_token) || !is_ipc_buffer_token(g_fat_session.response_token)) return 0;
    const u64 remote_response_token = grant_fat_response_buffer();
    if (!is_ipc_buffer_token(remote_response_token)) return 0;

    clear_page(g_fat_session.request_va);
    clear_page(g_fat_session.response_va);
    const u64 self_slot = syscall0(SYSCALL_GET_PROCESS_SLOT);
    g_fat_session.session_nonce = make_fat_nonce(
        g_fat_session.request_token,
        g_fat_session.response_token,
        g_fat_session.endpoint_id,
        self_slot
    );

    volatile struct fs_request_header *request = (volatile struct fs_request_header *)g_fat_session.request_va;
    request->magic = FS_REQUEST_MAGIC;
    request->version = FS_PROTOCOL_VERSION;
    request->op = FS_OP_CONNECT;
    request->arg0 = remote_response_token;
    request->arg1 = self_slot;
    request->session_nonce = g_fat_session.session_nonce;
    __asm__ volatile("" ::: "memory");
    request->request_seq = 1;

    if (!share_fat_request_buffer()) return 0;
    if (!wait_fat_response(1, FS_OP_CONNECT)) return 0;
    volatile struct fs_response_header *response = (volatile struct fs_response_header *)g_fat_session.response_va;
    if (response->status != FS_STATUS_OK || response->result_token == 0) return 0;
    g_fat_session.root_token = response->result_token;
    g_fat_session.next_seq = 2;
    g_fat_session.active = 1;
    return 1;
}

static int fat_request(u16 op, u64 token, u64 offset, u32 length, const char *path) {
    const u64 path_len64 = path ? cstr_len(path) : 0;
    if (path_len64 > FS_MAX_PATH_BYTES) return 0;
    const u16 path_len = (u16)path_len64;
    const u64 seq = g_fat_session.next_seq++;
    clear_page(g_fat_session.request_va);
    clear_page(g_fat_session.response_va);
    volatile struct fs_request_header *request = (volatile struct fs_request_header *)g_fat_session.request_va;
    request->magic = FS_REQUEST_MAGIC;
    request->version = FS_PROTOCOL_VERSION;
    request->op = op;
    request->object_token = token;
    request->offset = offset;
    request->length = length;
    request->flags = 0;
    request->path_bytes = path_len;
    request->inline_bytes = 0;
    request->arg0 = 0;
    request->arg1 = 0;
    request->session_nonce = g_fat_session.session_nonce;
    volatile u8 *payload = (volatile u8 *)(g_fat_session.request_va + FS_REQUEST_HEADER_BYTES);
    for (u16 i = 0; i < path_len; i++) payload[i] = (u8)path[i];
    __asm__ volatile("" ::: "memory");
    request->request_seq = seq;
    if (!signal_fat_for_boot()) return 0;
    return wait_fat_response(seq, op);
}

static int alloc_pages_at(u64 base_va, u64 file_bytes) {
    const u64 pages = (file_bytes + 4095) / 4096;
    if (pages == 0 || pages > MAX_ROOT_SEED2_PAGES) return 0;
    for (u64 i = 0; i < pages; i++) {
        const u64 va = base_va + i * 4096;
        g_root_seed2_page_paddrs[i] = 0;
        if (syscall4(SYSCALL_ALLOC_MAP_PAGES, va, 1, 1, (u64)&g_root_seed2_page_paddrs[i]) != SYSCALL_OK) return 0;
        clear_page(va);
    }
    return 1;
}

static int alloc_root_seed2_image_pages(u64 file_bytes) {
    return alloc_pages_at(ROOT_SEED2_IMAGE_VA, file_bytes);
}

static int load_root_seed2_from_fat(struct loaded_file *out) {
    if (!fat_request(FS_OP_LOOKUP, g_fat_session.root_token, 0, 0, "/sbin/seed2.elf")) return 0;
    volatile struct fs_response_header *response = (volatile struct fs_response_header *)g_fat_session.response_va;
    if (response->status != FS_STATUS_OK || response->result_token == 0) return 0;
    const u64 file_token = response->result_token;

    if (!fat_request(FS_OP_OPEN_EXEC, file_token, 0, 0, 0)) return 0;
    response = (volatile struct fs_response_header *)g_fat_session.response_va;
    if (response->status != FS_STATUS_OK || response->result_token == 0 || response->file_bytes == 0) return 0;
    const u64 open_token = response->result_token;
    const u64 file_bytes = response->file_bytes;
    if (!alloc_root_seed2_image_pages(file_bytes)) return 0;

    u64 offset = 0;
    while (offset < file_bytes) {
        u32 request_len = FS_RESPONSE_PAYLOAD_BYTES;
        if ((u64)request_len > file_bytes - offset) request_len = (u32)(file_bytes - offset);
        if (!fat_request(FS_OP_READ, open_token, offset, request_len, 0)) return 0;
        response = (volatile struct fs_response_header *)g_fat_session.response_va;
        if (response->status != FS_STATUS_OK || response->inline_bytes == 0 || response->inline_bytes > request_len) return 0;
        if (offset + response->inline_bytes > file_bytes) return 0;
        volatile u8 *dst = (volatile u8 *)(ROOT_SEED2_IMAGE_VA + offset);
        volatile u8 *src = (volatile u8 *)(g_fat_session.response_va + FS_RESPONSE_HEADER_BYTES);
        for (u16 i = 0; i < response->inline_bytes; i++) dst[i] = src[i];
        offset += response->inline_bytes;
    }

    const u64 vm_token = create_vm_object_from_current_pages(ROOT_SEED2_IMAGE_VA, file_bytes, 0xF);
    if ((vm_token & VM_OBJECT_TOKEN_TAG) != VM_OBJECT_TOKEN_TAG) return 0;
    out->image_va = ROOT_SEED2_IMAGE_VA;
    out->file_bytes = file_bytes;
    out->vm_token = vm_token;
    return 1;
}

static void spawn_root_seed2_direct(void) {
    if (!connect_fat_for_root_spawn()) {
        user_log("[seed2_boot] fat connect failed\n");
        return;
    }
    user_log("[seed2_boot] fat connect ok\n");

    struct loaded_file image;
    if (!load_root_seed2_from_fat(&image)) {
        user_log("[seed2_boot] root seed2 load failed\n");
        return;
    }
    user_log("[seed2_boot] root seed2 exec ready\n");

    u64 config_paddr = 0;
    if (syscall4(SYSCALL_ALLOC_MAP_PAGES, ROOT_SEED2_CONFIG_VA, 1, 1, (u64)&config_paddr) != SYSCALL_OK || config_paddr < 0x1000) {
        user_log("[seed2_boot] root seed2 config alloc failed\n");
        return;
    }
    clear_page(ROOT_SEED2_CONFIG_VA);
    const u64 catalog_va = ensure_device_catalog();
    volatile u64 *config = (volatile u64 *)ROOT_SEED2_CONFIG_VA;
    config[0] = ROOT_SEED2_CONFIG_MAGIC;
    config[1] = ROOT_SEED2_CONFIG_VERSION;
    config[3] = g_fat_endpoint_id;
    config[4] = g_fat_process_slot;
    config[5] = 0;
    config[6] = 0;
    config[7] = 0;
    config[8] = 0;
    config[9] = catalog_va != 0 ? DEVICE_CATALOG_TARGET_VA : 0;
    config[10] = 0;

    struct bootstrap_descriptor_table *table = (struct bootstrap_descriptor_table *)alloc_bootstrap_page();
    if (!table) {
        user_log("[seed2_boot] root seed2 table alloc failed\n");
        return;
    }
    clear_page((u64)table);
    table->page_count = 1;
    table->pages[0].source_va = ROOT_SEED2_CONFIG_VA;
    table->pages[0].target_va = PROCESS_STANDARD_CONFIG_TARGET_VA;
    table->pages[0].flags = BOOTSTRAP_PAGE_WRITABLE;
    if (catalog_va != 0) {
        table->page_count = 2;
        table->pages[1].source_va = catalog_va;
        table->pages[1].target_va = DEVICE_CATALOG_TARGET_VA;
        table->pages[1].flags = BOOTSTRAP_PAGE_WRITABLE;
    }

    const u64 child_slot = launch_process_builder_image(&image, table, 1);
    if (child_slot == 0) {
        user_log("[seed2_boot] root seed2 spawn failed\n");
        return;
    }
    if (!grant_device_catalog_to_child(child_slot)) {
        user_log("[seed2_boot] device catalog grant failed\n");
        return;
    }
    config[10] = DEVICE_CATALOG_READY;
    user_log("[seed2_boot] root seed2 spawned from rootfs\n");
}

static void launch_block_server(void) {
    struct loaded_file image;
    struct queue_grant grant;
    if (!open_image_from_bootfs("/srv/virtio_blk.elf", &image)) {
        user_log("[seed2_boot] open block_server failed\n");
        return;
    }
    if (!find_block_device() || !find_block_queue_grant(&grant) || !install_device_mmio_caps_for(&g_block_device)) {
        user_log("[seed2_boot] block bootstrap resources missing\n");
        return;
    }

    const u64 cfg_va = alloc_bootstrap_page();
    volatile u64 *cfg = (volatile u64 *)cfg_va;
    const u64 dev_cfg_va = map_device_cfg_for_read();
    const u64 capacity_sectors = dev_cfg_va != 0 ? mmio_read_u64(dev_cfg_va + VIRTIO_BLK_CAPACITY_OFFSET) : 0;
    u64 logical_block_size = dev_cfg_va != 0 ? mmio_read_u32(dev_cfg_va + VIRTIO_BLK_BLOCK_SIZE_OFFSET) : 512;
    if (logical_block_size == 0) logical_block_size = 512;

    const u64 endpoint_id = g_next_endpoint_id++;
    clear_page(cfg_va);
    cfg[0] = BLOCK_CONFIG_MAGIC;
    cfg[1] = BLOCK_CONFIG_VERSION;
    cfg[BLOCK_ENDPOINT_ID_INDEX] = endpoint_id;
    cfg[BLOCK_COMMON_PAGE_PADDR_INDEX] = g_block_device.common_page_paddr;
    cfg[BLOCK_NOTIFY_PAGE_PADDR_INDEX] = g_block_device.notify_page_paddr;
    cfg[BLOCK_ISR_PAGE_PADDR_INDEX] = g_block_device.isr_page_paddr;
    cfg[BLOCK_DEVICE_PAGE_PADDR_INDEX] = g_block_device.device_page_paddr;
    cfg[BLOCK_COMMON_PAGE_OFFSET_INDEX] = g_block_device.common_page_offset;
    cfg[BLOCK_NOTIFY_PAGE_OFFSET_INDEX] = g_block_device.notify_page_offset;
    cfg[BLOCK_ISR_PAGE_OFFSET_INDEX] = g_block_device.isr_page_offset;
    cfg[BLOCK_DEVICE_PAGE_OFFSET_INDEX] = g_block_device.device_page_offset;
    cfg[BLOCK_NOTIFY_OFF_MULTIPLIER_INDEX] = g_block_device.notify_off_multiplier;
    cfg[BLOCK_CAPACITY_SECTORS_INDEX] = capacity_sectors;
    cfg[BLOCK_LOGICAL_BLOCK_SIZE_INDEX] = logical_block_size;
    cfg[BLOCK_RESOURCE_ID_INDEX] = g_block_device.resource_id;

    struct bootstrap_descriptor_table *table = (struct bootstrap_descriptor_table *)alloc_bootstrap_page();
    clear_page((u64)table);
    table->page_count = 1;
    table->pages[0].source_va = cfg_va;
    table->pages[0].target_va = PROCESS_STANDARD_CONFIG_TARGET_VA;
    table->pages[0].flags = BOOTSTRAP_PAGE_WRITABLE;

    const u64 child_slot = launch_process_builder_image(&image, table, 0);
    if (child_slot == 0) {
        user_log("[seed2_boot] spawn block_server failed\n");
        return;
    }
    if (syscall3(SYSCALL_INSTALL_ENDPOINT, 0, endpoint_id, child_slot) != 0 ||
        syscall2(SYSCALL_PUBLISH_SERVICE_ENDPOINT, endpoint_id, child_slot) != 0 ||
        !grant_device_mmio(&g_block_device, child_slot))
    {
        user_log("[seed2_boot] block_server publish/grant failed\n");
        return;
    }

    const u64 iommu_child = decode_queue_cap(syscall2(SYSCALL_GRANT_QUEUE_CAP, encode_queue_cap(QUEUE_CAP_KIND_IOMMU, grant.iommu_token), child_slot), QUEUE_CAP_KIND_IOMMU);
    const u64 submit_child = decode_queue_cap(syscall2(SYSCALL_GRANT_QUEUE_CAP, encode_queue_cap(QUEUE_CAP_KIND_VIRTQUEUE, grant.submit_token), child_slot), QUEUE_CAP_KIND_VIRTQUEUE);
    const u64 notify_child = decode_queue_cap(syscall2(SYSCALL_GRANT_QUEUE_CAP, encode_queue_cap(QUEUE_CAP_KIND_VIRTQUEUE, grant.notify_token), child_slot), QUEUE_CAP_KIND_VIRTQUEUE);
    const u64 command_child = decode_queue_cap(syscall2(SYSCALL_GRANT_QUEUE_CAP, encode_queue_cap(QUEUE_CAP_KIND_COMMAND, grant.command_token), child_slot), QUEUE_CAP_KIND_COMMAND);
    if (iommu_child == 0 || submit_child == 0 || notify_child == 0 || command_child == 0) {
        user_log("[seed2_boot] block_server queue grant failed\n");
        return;
    }
    cfg[BLOCK_IOMMU_TOKEN_INDEX] = iommu_child;
    cfg[BLOCK_QUEUE_SUBMIT_TOKEN_INDEX] = submit_child;
    cfg[BLOCK_QUEUE_NOTIFY_TOKEN_INDEX] = notify_child;
    cfg[BLOCK_COMMAND_TOKEN_INDEX] = command_child;
    (void)syscall2(SYSCALL_SIGNAL_ENDPOINT, endpoint_id, 0);
    wait_config_word(cfg_va, BLOCK_DRIVER_STATUS_INDEX, BLOCK_STATUS_READY);

    g_block_process_slot = child_slot;
    g_block_endpoint_id = endpoint_id;
    service_registry_set(ensure_service_registry(), SERVICE_KIND_BLOCK, child_slot, endpoint_id);
    user_log("[seed2_boot] block_server ready\n");
}

static u64 launch_configured_service(const char *path, const char *label, u64 config_magic, u64 backend_endpoint, u64 backend_slot, u64 ready_index, u64 ready_value, u64 service_kind, int bootstrap_owner) {
    struct loaded_file image;
    if (!open_image_from_bootfs(path, &image)) {
        user_log("[seed2_boot] open service failed\n");
        return 0;
    }
    const u64 cfg_va = alloc_bootstrap_page();
    volatile u64 *cfg = (volatile u64 *)cfg_va;
    const u64 endpoint_id = g_next_endpoint_id++;
    clear_page(cfg_va);
    cfg[0] = endpoint_id;
    cfg[1] = config_magic;
    cfg[2] = 0;
    cfg[3] = backend_endpoint;
    cfg[4] = backend_slot;
    cfg[5] = 0;
    cfg[6] = 0;
    if (service_kind == SERVICE_KIND_FAT_FS) cfg[3] = ROOTFS_START_BLOCK;

    struct bootstrap_descriptor_table *table = (struct bootstrap_descriptor_table *)alloc_bootstrap_page();
    clear_page((u64)table);
    table->page_count = 2;
    table->pages[0].source_va = cfg_va;
    table->pages[0].target_va = PROCESS_STANDARD_CONFIG_TARGET_VA;
    table->pages[0].flags = BOOTSTRAP_PAGE_WRITABLE;
    table->pages[1].source_va = ensure_service_registry();
    table->pages[1].target_va = PROCESS_SERVICE_REGISTRY_SHADOW_VA;
    table->pages[1].flags = 0;

    const u64 child_slot = launch_process_builder_image(&image, table, bootstrap_owner);
    if (child_slot == 0) {
        user_log("[seed2_boot] spawn service failed\n");
        return 0;
    }
    if (syscall3(SYSCALL_INSTALL_ENDPOINT, 0, endpoint_id, child_slot) != 0 ||
        syscall2(SYSCALL_PUBLISH_SERVICE_ENDPOINT, endpoint_id, child_slot) != 0)
    {
        user_log("[seed2_boot] service publish failed\n");
        return 0;
    }
    wait_config_word(cfg_va, ready_index, ready_value);
    service_registry_set(ensure_service_registry(), service_kind, child_slot, endpoint_id);
    user_log(label);
    return child_slot;
}

static void launch_fat_server(void) {
    g_fat_endpoint_id = g_next_endpoint_id;
    g_fat_process_slot = launch_configured_service("/srv/fat_server.elf", "[seed2_boot] fat_server ready\n", 0x31544146, 0, 0, 2, 1, SERVICE_KIND_FAT_FS, 0);
}

void seed2_boot_main(void) {
    user_log("[seed2_boot] started\n");
    boot_screen_init();
    init_handoff_from_descriptor_page();
    if (!map_bootfs_archive()) {
        user_log("[seed2_boot] bootfs map failed\n");
        boot_screen_line("BOOTFS FAILED");
        for (;;) wait_event_poll();
    }
    user_log("[seed2_boot] bootfs ready\n");
    boot_screen_line("BOOTFS READY");
    launch_block_server();
    boot_screen_line("BLOCK READY");
    launch_fat_server();
    boot_screen_line("FAT READY");
    spawn_root_seed2_direct();
    boot_screen_line("ROOT SEED2 STARTED");
    user_log("[seed2_boot] bootstrap chain done\n");
    boot_screen_line("HANDOFF DONE");

    for (;;) {
        (void)syscall2(SYSCALL_WAIT_EVENT, 1, 1);
    }
}
