typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;

enum {
    SYSCALL_LOG = 0x9,
    SYSCALL_MAP_PAGE = 0x2,
    SYSCALL_ALLOC_MAP_PAGES = 0xC,
    SYSCALL_GRANT_CAP = 0x8,
    SYSCALL_GRANT_CAPS_BATCH = 0x14,
    SYSCALL_WAIT_EVENT = 0x17,
    SYSCALL_SPAWN_EXEC = 0x1D,
    SYSCALL_INSTALL_VM_OBJECT = 0x1E,
    SYSCALL_INSTALL_EXEC_IMAGE = 0x20,
    SYSCALL_GRANT_CAP_ON_ENDPOINT = 0x24,
    SYSCALL_INSTALL_ENDPOINT = 0x26,
    SYSCALL_MAP_VM_OBJECT = 0x28,
    SYSCALL_SLICE_VM_OBJECT = 0x29,
    SYSCALL_SIGNAL_ENDPOINT = 0x2C,
    SYSCALL_INSTALL_MMIO_CAP = 0x2F,
    SYSCALL_INSTALL_CAPS_BATCH = 0x32,
    SYSCALL_PUBLISH_SERVICE_ENDPOINT = 0x33,
    SYSCALL_GRANT_QUEUE_CAP = 0x23,

    PAGE_RIGHT_CPU_READ = 0x1,
    PAGE_RIGHT_CPU_WRITE = 0x2,
    PAGE_RIGHT_GRANT = 0x8,

    PROCESS_AUX_BASE_VA = 0x3C000000,
    PROCESS_STANDARD_CONFIG_TARGET_VA = 0x3C002000,
    PROCESS_SERVICE_REGISTRY_SHADOW_VA = 0x3C2C0000,
    MANAGER_INIT_CONFIG_TARGET_VA = 0x3C021000,
    DYNAMIC_BOOTSTRAP_SOURCE_BASE_VA = 0x3C106000,
    INSPECT_MMIO_BASE_VA = 0x3F000000,

    INIT_BOOTSTRAP_MAGIC = 0x49425453,
    INIT_BOOTSTRAP_VERSION = 15,
    INIT_CONFIG_MAGIC = 0x49425443,
    INIT_CONFIG_VERSION = 1,
    INIT_BOOT_ARCHIVE_FLAG_PRESENT = 1,
    INIT_DEVICE_FLAG_PRESENT = 1,
    INIT_MAX_SPAWN_PAGE_DESCRIPTORS = 8,
    INIT_MAX_DEVICE_DESCRIPTORS = 6,
    INIT_MAX_DEVICE_QUEUE_GRANTS = 4,
    INIT_MAX_BOOT_ARCHIVE_PAGES = 128,

    MANAGER_INIT_MAGIC = 0x4D494248,
    MANAGER_INIT_VERSION = 4,
    MANAGER_INIT_MAX_DEVICE_GRANTS = 6,
    MANAGER_INIT_MAX_DEVICE_QUEUE_GRANTS = 4,

    BOOTFS_MAGIC = 0x53465442,
    BOOTFS_VERSION = 1,
    BOOTFS_KIND_REGULAR = 1,

    VM_OBJECT_TOKEN_TAG = 1ULL << 62,
    EXEC_IMAGE_TOKEN_TAG = (1ULL << 62) | (1ULL << 61),
    SPAWN_RESULT_TAG = 1ULL << 63,
    SPAWN_RESULT_PROCESS_MASK = 0xFFFFFFFFULL,
    SPAWN_FLAG_BOOTSTRAP_PAGE_WRITABLE = 1ULL << 0,
    SPAWN_FLAG_BOOTSTRAP_EXTENDED_DESCRIPTOR_TABLE = 1ULL << 2,

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

    SERVICE_REGISTRY_MAGIC = 0x53525643,
    SERVICE_REGISTRY_VERSION = 1,
    SERVICE_REGISTRY_MAX_ENTRIES = 12,
    SERVICE_KIND_VFS = 2,
    SERVICE_KIND_BLOCK = 4,
    SERVICE_KIND_FAT_FS = 9,
    SERVICE_FLAG_PROCESS_SLOT_COMPAT = 1,

    VIRTIO_VENDOR_ID = 0x1AF4,
    VIRTIO_BLK_DEVICE_MODERN = 0x1042,
    VIRTIO_BLK_DEVICE_LEGACY = 0x1001,
    VIRTIO_BLK_SUBSYSTEM_ID = 0x0002,
    VIRTIO_BLK_CAPACITY_OFFSET = 0x00,
    VIRTIO_BLK_BLOCK_SIZE_OFFSET = 0x14,

    QUEUE_CAP_TAG_BASE = (1ULL << 62) | (1ULL << 60),
    QUEUE_CAP_KIND_SHIFT = 56,
    QUEUE_CAP_KIND_MASK = 0x0FULL << 56,
    QUEUE_CAP_KIND_IOMMU = 1,
    QUEUE_CAP_KIND_VIRTQUEUE = 2,
    QUEUE_CAP_KIND_COMMAND = 3,

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

struct exec_image {
    u64 token;
    u64 file_bytes;
};

struct queue_grant {
    u64 iommu_token;
    u64 submit_token;
    u64 notify_token;
    u64 command_token;
};

static struct manager_config_page g_handoff;
static struct device_descriptor g_block_device;
static u64 g_next_endpoint_id = NEXT_ENDPOINT_BASE;
static u64 g_next_bootstrap_source_va = DYNAMIC_BOOTSTRAP_SOURCE_BASE_VA;
static u64 g_next_inspect_mmio_va = INSPECT_MMIO_BASE_VA;
static u64 g_service_registry_source_va;
static u64 g_block_process_slot;
static u64 g_block_endpoint_id;
static u64 g_fat_process_slot;
static u64 g_fat_endpoint_id;

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

static u64 wait_event_poll(void) {
    return syscall2(SYSCALL_WAIT_EVENT, 0, 1);
}

static void wait_bootseed_grants_window(void) {
    for (u64 i = 0; i < 4096; i++) {
        (void)wait_event_poll();
    }
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

static u64 decode_spawn_process_slot(u64 value) {
    if ((value & SPAWN_RESULT_TAG) == 0) return 0;
    return value & SPAWN_RESULT_PROCESS_MASK;
}

static struct init_descriptor_page *descriptor_page(void) {
    volatile struct init_config_page *cfg = (volatile struct init_config_page *)PROCESS_STANDARD_CONFIG_TARGET_VA;
    if (cfg->magic != INIT_CONFIG_MAGIC || cfg->version != INIT_CONFIG_VERSION || cfg->descriptor_page_va == 0) return 0;
    struct init_descriptor_page *page = (struct init_descriptor_page *)cfg->descriptor_page_va;
    if (page->magic != INIT_BOOTSTRAP_MAGIC || page->version != INIT_BOOTSTRAP_VERSION) return 0;
    return page;
}

static void wait_for_handoff(void) {
    volatile struct manager_config_page *page = (volatile struct manager_config_page *)MANAGER_INIT_CONFIG_TARGET_VA;
    while (1) {
        if (page->magic == MANAGER_INIT_MAGIC && page->version == MANAGER_INIT_VERSION && page->ready != 0) {
            g_handoff = *(struct manager_config_page *)page;
            return;
        }
        wait_event_poll();
    }
}

static int map_bootfs_archive(void) {
    struct init_descriptor_page *page = descriptor_page();
    if (!page) return 0;
    if ((page->bootfs_archive.flags & INIT_BOOT_ARCHIVE_FLAG_PRESENT) == 0) return 0;
    if ((g_handoff.bootfs_vm_token & VM_OBJECT_TOKEN_TAG) == 0) return 0;
    return syscall2(SYSCALL_MAP_VM_OBJECT, g_handoff.bootfs_vm_token, page->bootfs_archive.image_va) == 0;
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

static int open_exec_from_bootfs(const char *path, struct exec_image *out) {
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
        const u64 vm_token = syscall4(SYSCALL_SLICE_VM_OBJECT, g_handoff.bootfs_vm_token, entry->data_offset, entry->data_bytes, 1);
        if ((vm_token & VM_OBJECT_TOKEN_TAG) == 0) {
            user_log("[seed2_boot] bootfs install vm failed\n");
            return 0;
        }
        const u64 exec_token = syscall2(SYSCALL_INSTALL_EXEC_IMAGE, vm_token, 1);
        if ((exec_token & EXEC_IMAGE_TOKEN_TAG) != EXEC_IMAGE_TOKEN_TAG) {
            user_log("[seed2_boot] bootfs install exec failed\n");
            return 0;
        }
        out->token = exec_token;
        out->file_bytes = entry->data_bytes;
        return 1;
    }
    user_log("[seed2_boot] bootfs path not found\n");
    return 0;
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

static int install_device_mmio_caps(void) {
    if (syscall2(SYSCALL_INSTALL_MMIO_CAP, g_block_device.common_page_paddr, PAGE_RIGHT_CPU_READ | PAGE_RIGHT_CPU_WRITE | PAGE_RIGHT_GRANT) != 0) return 0;
    if (syscall2(SYSCALL_INSTALL_MMIO_CAP, g_block_device.notify_page_paddr, PAGE_RIGHT_CPU_READ | PAGE_RIGHT_CPU_WRITE | PAGE_RIGHT_GRANT) != 0) return 0;
    if (g_block_device.isr_page_paddr != 0 &&
        syscall2(SYSCALL_INSTALL_MMIO_CAP, g_block_device.isr_page_paddr, PAGE_RIGHT_CPU_READ | PAGE_RIGHT_GRANT) != 0) return 0;
    if (g_block_device.device_page_paddr != 0 &&
        syscall2(SYSCALL_INSTALL_MMIO_CAP, g_block_device.device_page_paddr, PAGE_RIGHT_CPU_READ | PAGE_RIGHT_CPU_WRITE | PAGE_RIGHT_GRANT) != 0) return 0;
    return 1;
}

static int grant_block_mmio(u64 child_slot) {
    if (syscall3(SYSCALL_GRANT_CAP, g_block_device.common_page_paddr, child_slot, PAGE_RIGHT_CPU_READ | PAGE_RIGHT_CPU_WRITE) != 0) return 0;
    if (syscall3(SYSCALL_GRANT_CAP, g_block_device.notify_page_paddr, child_slot, PAGE_RIGHT_CPU_READ | PAGE_RIGHT_CPU_WRITE) != 0) return 0;
    if (g_block_device.isr_page_paddr != 0 &&
        syscall3(SYSCALL_GRANT_CAP, g_block_device.isr_page_paddr, child_slot, PAGE_RIGHT_CPU_READ) != 0) return 0;
    if (g_block_device.device_page_paddr != 0 &&
        syscall3(SYSCALL_GRANT_CAP, g_block_device.device_page_paddr, child_slot, PAGE_RIGHT_CPU_READ) != 0) return 0;
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

static u64 spawn_with_table(u64 exec_token, struct bootstrap_descriptor_table *table) {
    return syscall4(SYSCALL_SPAWN_EXEC, exec_token, (u64)table, 0, SPAWN_FLAG_BOOTSTRAP_EXTENDED_DESCRIPTOR_TABLE);
}

static void wait_config_word(u64 va, u64 index, u64 expected) {
    volatile u64 *words = (volatile u64 *)va;
    while (words[index] != expected) {
        wait_event_poll();
    }
}

static void launch_block_server(void) {
    struct exec_image exec;
    struct queue_grant grant;
    if (!open_exec_from_bootfs("/srv/virtio_blk.elf", &exec)) {
        user_log("[seed2_boot] open block_server failed\n");
        return;
    }
    if (!find_block_device() || !find_block_queue_grant(&grant) || !install_device_mmio_caps()) {
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

    static struct bootstrap_descriptor_table table;
    clear_page((u64)&table);
    table.page_count = 1;
    table.pages[0].source_va = cfg_va;
    table.pages[0].target_va = PROCESS_STANDARD_CONFIG_TARGET_VA;
    table.pages[0].flags = SPAWN_FLAG_BOOTSTRAP_PAGE_WRITABLE;

    const u64 spawned = spawn_with_table(exec.token, &table);
    const u64 child_slot = decode_spawn_process_slot(spawned);
    if (child_slot == 0) {
        user_log("[seed2_boot] spawn block_server failed\n");
        return;
    }
    if (syscall3(SYSCALL_INSTALL_ENDPOINT, 0, endpoint_id, child_slot) != 0 ||
        syscall2(SYSCALL_PUBLISH_SERVICE_ENDPOINT, endpoint_id, child_slot) != 0 ||
        !grant_block_mmio(child_slot))
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

static u64 launch_configured_service(const char *path, const char *label, u64 config_magic, u64 backend_endpoint, u64 backend_slot, u64 ready_index, u64 ready_value, u64 service_kind) {
    struct exec_image exec;
    if (!open_exec_from_bootfs(path, &exec)) {
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
    if (service_kind == SERVICE_KIND_FAT_FS) cfg[3] = ROOTFS_START_BLOCK;

    static struct bootstrap_descriptor_table table;
    clear_page((u64)&table);
    table.page_count = 2;
    table.pages[0].source_va = cfg_va;
    table.pages[0].target_va = PROCESS_STANDARD_CONFIG_TARGET_VA;
    table.pages[0].flags = SPAWN_FLAG_BOOTSTRAP_PAGE_WRITABLE;
    table.pages[1].source_va = ensure_service_registry();
    table.pages[1].target_va = PROCESS_SERVICE_REGISTRY_SHADOW_VA;
    table.pages[1].flags = 0;

    const u64 spawned = spawn_with_table(exec.token, &table);
    const u64 child_slot = decode_spawn_process_slot(spawned);
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
    g_fat_process_slot = launch_configured_service("/srv/fat_server.elf", "[seed2_boot] fat_server ready\n", 0x31544146, 0, 0, 2, 1, SERVICE_KIND_FAT_FS);
}

static void launch_bootstrap_vfs(void) {
    (void)launch_configured_service("/srv/bootstrap_vfs.elf", "[seed2_boot] bootstrap_vfs ready\n", 0x31534656, g_fat_endpoint_id, g_fat_process_slot, 2, 1, SERVICE_KIND_VFS);
}

void seed2_boot_main(void) {
    user_log("[seed2_boot] started\n");
    wait_bootseed_grants_window();
    wait_for_handoff();
    if (!map_bootfs_archive()) {
        user_log("[seed2_boot] bootfs map failed\n");
        for (;;) wait_event_poll();
    }
    user_log("[seed2_boot] bootfs ready\n");
    launch_block_server();
    launch_fat_server();
    launch_bootstrap_vfs();
    user_log("[seed2_boot] bootstrap chain done\n");
    user_log("[seed2_boot] root seed2 bootfs copy disabled\n");

    for (;;) {
        (void)syscall2(SYSCALL_WAIT_EVENT, 1, 1);
    }
}
