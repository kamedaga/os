#include "exec_abi.h"
#include "exec_elf.h"

typedef unsigned long long u64;

enum {
    EXEC_OPT_SOURCE_PATH_CACHE = 1,
};

enum {
    SYSCALL_MAP_PAGE = 0x2,
    SYSCALL_LOG = 0x9,
    SYSCALL_ALLOC_MAP_PAGES = 0xC,
    SYSCALL_CREATE_VM_OBJECT_FROM_CURRENT_PAGES = 0x3F,
    SYSCALL_WAIT_EVENT = 0x17,
    SYSCALL_MAP_VM_OBJECT = 0x28,
    SYSCALL_INSTALL_ENDPOINT = 0x26,
    SYSCALL_ACCEPT_CAP_TRANSFER = 0x2A,
    SYSCALL_GRANT_VM_OBJECT = 0x1F,
    SYSCALL_GET_PROCESS_SLOT = 0x2E,
    SYSCALL_GET_PROCESS_STATUS = 0x30,
    SYSCALL_GET_TICK_COUNT = 0x2D,
    SYSCALL_GET_RTC_UNIX_TIME = 0x3E,
    SYSCALL_GET_MEMORY_STATS = 0x3C,
    SYSCALL_PUBLISH_SERVICE_ENDPOINT = 0x33,
    SYSCALL_PROCESS_EXIT = 0x34,
    SYSCALL_RELEASE_VM_OBJECT = 0x29,
    SYSCALL_DROP_VM_OBJECT = 0x31,
    SYSCALL_CREATE_SUSPENDED_PROCESS = 0x41,
    SYSCALL_MAP_VM_OBJECT_TO_PROCESS = 0x42,
    SYSCALL_ALLOC_MAP_PAGES_TO_PROCESS = 0x43,
    SYSCALL_SET_PROCESS_INITIAL_CONTEXT = 0x44,
    SYSCALL_START_PROCESS = 0x45,
    SYSCALL_ABORT_PROCESS = 0x46,
    SYSCALL_COPY_TO_PROCESS = 0x47,
    SYSCALL_SET_PROCESS_ABI_TRAP_DELEGATE = 0x4B,
    SYSCALL_MAP_VM_OBJECT_RANGE_TO_PROCESS = 0x6A,
    SYSCALL_MAP_PAGE_ANYWHERE = 0x5C,
    SYSCALL_ACCEPT_IPC_BUFFER_TRANSFER = 0x61,
    SYSCALL_MAP_IPC_BUFFER_ANYWHERE = 0x62,
    SYSCALL_CAPSULE_GRANT = 0x76,
    SYSCALL_OK = 0,
    PAGE_BYTES = 4096,
    USER_LAYOUT_DEFAULT_LOW_VA = EXEC_USER_LAYOUT_LOW_VA,
    USER_LAYOUT_DEFAULT_TOP_VA = EXEC_USER_LAYOUT_TOP_VA,
    USER_STACK_DEFAULT_TOP = EXEC_USER_STACK_TOP_VA,
    LINUX_STACK_DEFAULT_PAGES = EXEC_USER_STACK_PAGE_COUNT,
    SOURCE_IMAGE_SMALL_BASE_VA = 0x40000000,
    SOURCE_IMAGE_SMALL_SLOT_SPAN = 0x01000000,
    SOURCE_IMAGE_SMALL_SLOT_COUNT = 24,
    SOURCE_IMAGE_LARGE_BASE_VA = 0x58000000,
    SOURCE_IMAGE_LARGE_SLOT_SPAN = 0x08000000,
    SOURCE_IMAGE_LARGE_SLOT_COUNT = 2,
    SOURCE_IMAGE_SLOT_COUNT = SOURCE_IMAGE_SMALL_SLOT_COUNT + SOURCE_IMAGE_LARGE_SLOT_COUNT,
    SOURCE_IMAGE_PATH_BYTES = 160,
    LINUX_ABI_REQUEST_PAGES_BASE_VA = 0x26500000,
    LINUX_ABI_REQUEST_PAGE_COUNT = 64,
    EXEC_DEVICE_CATALOG_VA = 0x26700000,
    EXEC_CHILD_DEVICE_CATALOG_BASE_VA = 0x26800000,
    EXEC_ARG_DATA_VA = 0x26900000,
    EXEC_LINUX_VDSO_VA = EXEC_LINUX_MMAP_BASE_VA - 0x2000ULL,
    DEVICE_CATALOG_MAGIC = 0x44455643,
    DEVICE_CATALOG_VERSION = 1,
    DEVICE_CATALOG_MAX_ENTRIES = 23,
    VM_OBJECT_TOKEN_TAG = 1ULL << 62,
    VM_RIGHT_READ_MAP_GRANT = 0xD,
    PROCESS_BUILDER_TOKEN_TAG = 1ULL << 60,
    PROCESS_BUILDER_PROCESS_MASK = 0xffffffffULL,
    IPC_BUFFER_TOKEN_TAG = 0xA000000000000000ULL,
    IPC_BUFFER_TOKEN_MASK = 0x0FFFFFFFFFFFFFFFULL,
    CAPSULE_TOKEN_MAGIC_MASK = 0xFF00000000000000ULL,
    CAPSULE_TOKEN_MAGIC_TAG = 0xCA00000000000000ULL,
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
    DEVICE_FD_RIGHT_CHILD =
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
        CAPSULE_RIGHT_HOTPLUG_OBSERVE,
    SPAWN_RESULT_TAG = 1ULL << 63,
    SPAWN_RESULT_PROCESS_MASK = 0xffffffffULL,
    VM_RIGHT_READ_MAP = 0x5,
    VM_RIGHT_READ_WRITE_MAP = 0x7,
    ET_DYN_DEFAULT_ALLOC_START_VA = EXEC_USER_ET_DYN_BASE_VA,
    MAX_CHILD_MAPPED_PAGES = 32768,
    AT_NULL = 0,
    AT_PHDR = 3,
    AT_PHENT = 4,
    AT_PHNUM = 5,
    AT_PAGESZ = 6,
    AT_BASE = 7,
    AT_FLAGS = 8,
    AT_ENTRY = 9,
    AT_UID = 11,
    AT_EUID = 12,
    AT_GID = 13,
    AT_EGID = 14,
    AT_PLATFORM = 15,
    AT_HWCAP = 16,
    AT_CLKTCK = 17,
    AT_SECURE = 23,
    AT_RANDOM = 25,
    AT_EXECFN = 31,
    AT_SYSINFO_EHDR = 33,
    DT_NULL = 0,
    DT_RELA = 7,
    DT_RELASZ = 8,
    DT_RELAENT = 9,
    DT_RELRSZ = 35,
    DT_RELR = 36,
    DT_RELRENT = 37,
    ELF_DYN_BYTES = 16,
    ELF_RELA_BYTES = 24,
    ELF_RELR_BYTES = 8,
    R_X86_64_RELATIVE = 8,
    PROCESS_STATUS_INACTIVE = 0,
    PROCESS_STATUS_ACTIVE = 1,
    PROCESS_STATUS_FAULTED = 2,
};

struct byte_slice {
    const unsigned char *ptr;
    u64 len;
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
    u64 device_fd;
};

struct device_catalog_page {
    u64 magic;
    u64 version;
    u64 entry_count;
    u64 reserved0;
    struct device_catalog_entry entries[DEVICE_CATALOG_MAX_ENTRIES];
};

struct child_map_tracker {
    u64 pages[MAX_CHILD_MAPPED_PAGES];
    u64 page_count;
};

struct loaded_image {
    struct exec_elf_summary summary;
    u64 load_bias;
    u64 entry_va;
    u64 phdr_va;
    u64 mapped_pages;
    u64 initial_rip;
    u64 initial_rsp;
};

struct initial_user_context {
    u64 flags;
    u64 rip;
    u64 rsp;
    u64 rflags;
    u64 rax;
    u64 rbx;
    u64 rcx;
    u64 rdx;
    u64 rsi;
    u64 rdi;
    u64 rbp;
    u64 r8;
    u64 r9;
    u64 r10;
    u64 r11;
    u64 r12;
    u64 r13;
    u64 r14;
    u64 r15;
    u64 fs_base;
    u64 reserved0;
    u64 reserved1;
};

struct load_layout {
    u64 next_dyn_base;
};

struct linux_stack_config {
    struct byte_slice execfn;
    struct byte_slice argv[EXEC_MAX_ARGV];
    u64 argv_count;
    struct byte_slice envp[EXEC_MAX_ENVP];
    u64 envp_count;
    struct byte_slice platform;
};

struct aux_entry {
    u64 tag;
    u64 value;
};

static u64 service_request_paddr;
static u64 service_response_paddr;
static u64 service_request_token;
static u64 service_response_token;
static u64 service_request_va;
static u64 service_response_va;
static u64 service_last_request_seq;
static u64 service_interpreter_vm_token;
static u64 service_interpreter_file_bytes;
static u64 service_arg_data_vm_token;
static u64 service_arg_data_vm_bytes;
static u64 service_vdso_cycles_per_tick;
static int service_direct_map_enabled;
static u64 service_direct_attempts;
static u64 service_direct_mapped_pages;
static u64 service_direct_fallbacks;
static u64 service_direct_map_failures;
static int service_profile_enabled;
static u64 service_profile_start_tick;
static u64 service_profile_last_tick;
static u64 service_profile_count;
static const char *service_profile_labels[64];
static u64 service_profile_dt[64];
static u64 service_profile_total[64];
static u64 service_source_sweep_log_count;
static u64 service_source_map_log_count;
static struct exec_bootstrap_config service_request_config;
static struct exec_bootstrap_config one_shot_config;
static u64 source_image_tokens[SOURCE_IMAGE_SLOT_COUNT];
static u64 source_image_bytes[SOURCE_IMAGE_SLOT_COUNT];
static u64 source_image_path_bytes[SOURCE_IMAGE_SLOT_COUNT];
static unsigned char source_image_paths[SOURCE_IMAGE_SLOT_COUNT][SOURCE_IMAGE_PATH_BYTES];
static u64 source_image_evict_cursor;
static struct child_map_tracker child_map_tracker_storage;
static unsigned char private_page_buffer[PAGE_BYTES];
static const unsigned char linux_platform[] = "x86_64";
static const unsigned char default_execfn[] = "/cmd/musl_smoke.elf";
static const unsigned char default_arg1[] = "argv-smoke";
static const unsigned char default_env_path[] = "PATH=/bin:/usr/bin:/usr/lib/uutils:/cmd";
static const unsigned char default_env_os[] = "CAPABILITYOS=1";

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

static void user_log_dec_value(u64 value) {
    char buf[32];
    u64 pos = sizeof(buf);
    if (value == 0) {
        user_log("0");
        return;
    }
    while (value != 0 && pos != 0) {
        buf[--pos] = (char)('0' + (value % 10));
        value /= 10;
    }
    user_log_len(buf + pos, sizeof(buf) - pos);
}

static void user_log_hex_value(u64 value) {
    static const char hex[] = "0123456789ABCDEF";
    char buf[18];
    buf[0] = '0';
    buf[1] = 'x';
    for (u64 i = 0; i < 16; i++) {
        const u64 shift = (15 - i) * 4;
        buf[2 + i] = hex[(value >> shift) & 0xF];
    }
    user_log_len(buf, sizeof(buf));
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

static u64 syscall1(u64 nr, u64 a0) {
    u64 ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(nr), "D"(a0)
        : "rcx", "rdx", "r8", "r9", "r10", "r11", "memory");
    return ret;
}

static u64 syscall2(u64 nr, u64 a0, u64 a1) {
    u64 ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(nr), "D"(a0), "S"(a1)
        : "rcx", "rdx", "r8", "r9", "r10", "r11", "memory");
    return ret;
}

static u64 syscall3(u64 nr, u64 a0, u64 a1, u64 a2) {
    u64 ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(nr), "D"(a0), "S"(a1), "d"(a2)
        : "rcx", "r8", "r9", "r10", "r11", "memory");
    return ret;
}

static u64 syscall4(u64 nr, u64 a0, u64 a1, u64 a2, u64 a3) {
    u64 ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(nr), "D"(a0), "S"(a1), "d"(a2), "c"(a3)
        : "r8", "r9", "r10", "r11", "memory");
    return ret;
}

static u64 syscall5(u64 nr, u64 a0, u64 a1, u64 a2, u64 a3, u64 a4) {
    register u64 r8 __asm__("r8") = a4;
    u64 ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(nr), "D"(a0), "S"(a1), "d"(a2), "c"(a3), "r"(r8)
        : "r9", "r10", "r11", "memory");
    return ret;
}

static u64 syscall6(u64 nr, u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
    register u64 r8 __asm__("r8") = a4;
    register u64 r9 __asm__("r9") = a5;
    u64 ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(nr), "D"(a0), "S"(a1), "d"(a2), "c"(a3), "r"(r8), "r"(r9)
        : "r10", "r11", "memory");
    return ret;
}

static void log_memory_stats(const char *where) {
    u64 stats[4] = {0, 0, 0, 0};
    const u64 status = syscall1(SYSCALL_GET_MEMORY_STATS, (u64)stats);
    user_log("Exec: mem where=");
    user_log(where);
    user_log(" status=");
    user_log_hex_value(status);
    user_log(" total=");
    user_log_dec_value(stats[0]);
    user_log(" used=");
    user_log_dec_value(stats[1]);
    user_log(" free=");
    user_log_dec_value(stats[2]);
    user_log(" page=");
    user_log_dec_value(stats[3]);
    user_log("\n");
}

static void process_exit(u64 code) {
    (void)syscall1(SYSCALL_PROCESS_EXIT, code);
    for (;;) __asm__ volatile("pause");
}

static u64 map_page_anywhere(u64 paddr, u64 writable) {
    return syscall2(SYSCALL_MAP_PAGE_ANYWHERE, paddr, writable);
}

static int is_ipc_buffer_token(u64 token) {
    return (token & ~IPC_BUFFER_TOKEN_MASK) == IPC_BUFFER_TOKEN_TAG && (token & IPC_BUFFER_TOKEN_MASK) != 0;
}

static u64 accept_ipc_buffer_transfer(u64 transfer_id) {
    return syscall1(SYSCALL_ACCEPT_IPC_BUFFER_TRANSFER, transfer_id);
}

static u64 map_ipc_buffer_anywhere(u64 token, u64 writable) {
    return syscall2(SYSCALL_MAP_IPC_BUFFER_ANYWHERE, token, writable);
}

static int is_vm_object_token(u64 token) {
    return (token & VM_OBJECT_TOKEN_TAG) != 0 &&
        (token & ~VM_OBJECT_TOKEN_TAG) != 0;
}

static int is_process_builder_token(u64 token) {
    return (token & PROCESS_BUILDER_TOKEN_TAG) != 0 &&
        (token & PROCESS_BUILDER_PROCESS_MASK) != 0;
}

static int is_fd(u64 token) {
    return token >= 16 && token < 256;
}

static int map_vm_object(u64 token, u64 target_va) {
    return syscall3(SYSCALL_MAP_VM_OBJECT, token, target_va, 0) == SYSCALL_OK;
}

static u64 release_vm_object_mapping_status(u64 token, u64 target_va, u64 size_bytes) {
    return syscall3(SYSCALL_RELEASE_VM_OBJECT, token, target_va, size_bytes);
}

static int release_vm_object_mapping(u64 token, u64 target_va, u64 size_bytes) {
    return release_vm_object_mapping_status(token, target_va, size_bytes) == SYSCALL_OK;
}

static void drop_vm_object_token(u64 token) {
    if (is_vm_object_token(token)) (void)syscall3(SYSCALL_DROP_VM_OBJECT, token, 0, 0);
}

static u64 arg_data_capacity_for_config(const struct exec_bootstrap_config *cfg) {
    if (is_vm_object_token(cfg->arg_data_vm_token) && cfg->arg_data_vm_bytes != 0) {
        const struct exec_arg_block *block = (const struct exec_arg_block *)EXEC_ARG_DATA_VA;
        if (cfg->arg_data_vm_token == service_arg_data_vm_token &&
            service_arg_data_vm_bytes >= EXEC_OFFSETOF(struct exec_arg_block, arg_data) &&
            block->magic == EXEC_ARG_BLOCK_MAGIC &&
            block->version == EXEC_ARG_BLOCK_VERSION) {
            return block->arg_data_bytes;
        }
        return 0;
    }
    return EXEC_INLINE_ARG_DATA_BYTES;
}

static int config_uses_external_arg_block(const struct exec_bootstrap_config *cfg) {
    if (!is_vm_object_token(cfg->arg_data_vm_token)) return 0;
    if (cfg->arg_data_vm_token != service_arg_data_vm_token) return 0;
    if (service_arg_data_vm_bytes < EXEC_OFFSETOF(struct exec_arg_block, arg_data)) return 0;
    const struct exec_arg_block *block = (const struct exec_arg_block *)EXEC_ARG_DATA_VA;
    return block->magic == EXEC_ARG_BLOCK_MAGIC && block->version == EXEC_ARG_BLOCK_VERSION;
}

static const struct exec_arg_block *arg_block_for_config(const struct exec_bootstrap_config *cfg) {
    return config_uses_external_arg_block(cfg) ? (const struct exec_arg_block *)EXEC_ARG_DATA_VA : (const struct exec_arg_block *)0;
}

static struct exec_arg_block *mutable_arg_block_for_config(struct exec_bootstrap_config *cfg) {
    return config_uses_external_arg_block(cfg) ? (struct exec_arg_block *)EXEC_ARG_DATA_VA : (struct exec_arg_block *)0;
}

static unsigned char *mutable_arg_data_for_config(struct exec_bootstrap_config *cfg) {
    struct exec_arg_block *block = mutable_arg_block_for_config(cfg);
    if (block != 0) return block->arg_data;
    return cfg->arg_data;
}

static const unsigned char *arg_data_for_config(const struct exec_bootstrap_config *cfg) {
    const struct exec_arg_block *block = arg_block_for_config(cfg);
    if (block != 0) return block->arg_data;
    return cfg->arg_data;
}

static u64 config_argv_count(const struct exec_bootstrap_config *cfg) {
    const struct exec_arg_block *block = arg_block_for_config(cfg);
    return block != 0 ? block->argv_count : cfg->argv_count;
}

static u64 config_envp_count(const struct exec_bootstrap_config *cfg) {
    const struct exec_arg_block *block = arg_block_for_config(cfg);
    return block != 0 ? block->envp_count : cfg->envp_count;
}

static int config_argv_desc(const struct exec_bootstrap_config *cfg, u64 index, u64 *offset_out, u64 *len_out) {
    const struct exec_arg_block *block = arg_block_for_config(cfg);
    if (block != 0) {
        if (index >= block->argv_count || index >= EXEC_MAX_ARGV) return 0;
        *offset_out = block->argv_offsets[index];
        *len_out = block->argv_bytes[index];
        return 1;
    }
    if (index >= cfg->argv_count || index >= EXEC_INLINE_MAX_ARGV) return 0;
    *offset_out = cfg->argv_offsets[index];
    *len_out = cfg->argv_bytes[index];
    return 1;
}

static int config_envp_desc(const struct exec_bootstrap_config *cfg, u64 index, u64 *offset_out, u64 *len_out) {
    const struct exec_arg_block *block = arg_block_for_config(cfg);
    if (block != 0) {
        if (index >= block->envp_count || index >= EXEC_MAX_ENVP) return 0;
        *offset_out = block->envp_offsets[index];
        *len_out = block->envp_bytes[index];
        return 1;
    }
    if (index >= cfg->envp_count || index >= EXEC_MAX_ENVP) return 0;
    *offset_out = cfg->envp_offsets[index];
    *len_out = cfg->envp_bytes[index];
    return 1;
}

static u64 page_aligned_size(u64 bytes) {
    return (bytes + PAGE_BYTES - 1) & ~(u64)(PAGE_BYTES - 1);
}

static int map_external_arg_data_for_config(struct exec_bootstrap_config *cfg) {
    if (!is_vm_object_token(cfg->arg_data_vm_token)) {
        if (cfg->arg_data_bytes > EXEC_INLINE_ARG_DATA_BYTES) return 0;
        return 1;
    }
    if (cfg->arg_data_vm_bytes < EXEC_OFFSETOF(struct exec_arg_block, arg_data) ||
        cfg->arg_data_vm_bytes > sizeof(struct exec_arg_block)) return 0;
    if (service_arg_data_vm_token == cfg->arg_data_vm_token && service_arg_data_vm_bytes == cfg->arg_data_vm_bytes) return 1;
    if (service_arg_data_vm_token != 0) {
        const u64 old_token = service_arg_data_vm_token;
        (void)release_vm_object_mapping(service_arg_data_vm_token, EXEC_ARG_DATA_VA, page_aligned_size(service_arg_data_vm_bytes));
        service_arg_data_vm_token = 0;
        service_arg_data_vm_bytes = 0;
        drop_vm_object_token(old_token);
    }
    if (!map_vm_object(cfg->arg_data_vm_token, EXEC_ARG_DATA_VA)) {
        user_log("Exec: arg data map failed\n");
        return 0;
    }
    service_arg_data_vm_token = cfg->arg_data_vm_token;
    service_arg_data_vm_bytes = cfg->arg_data_vm_bytes;
    const struct exec_arg_block *block = (const struct exec_arg_block *)EXEC_ARG_DATA_VA;
    if (block->magic != EXEC_ARG_BLOCK_MAGIC || block->version != EXEC_ARG_BLOCK_VERSION) return 0;
    if (block->argv_count != cfg->argv_count || block->envp_count != cfg->envp_count) return 0;
    if (block->execfn_offset != cfg->execfn_offset || block->execfn_bytes != cfg->execfn_bytes) return 0;
    if (block->arg_data_bytes != cfg->arg_data_bytes || block->arg_data_bytes > EXEC_MAX_ARG_DATA_BYTES) return 0;
    if (cfg->arg_data_vm_bytes < EXEC_OFFSETOF(struct exec_arg_block, arg_data) + block->arg_data_bytes) return 0;
    return 1;
}

static u64 create_suspended_process(void) {
    const u64 token = syscall1(SYSCALL_CREATE_SUSPENDED_PROCESS, 0);
    return is_process_builder_token(token) ? token : 0;
}

static u64 decode_spawned_process_slot(u64 value) {
    if ((value & SPAWN_RESULT_TAG) == 0) return 0;
    return value & SPAWN_RESULT_PROCESS_MASK;
}

static u64 start_process(u64 process_token) {
    return decode_spawned_process_slot(syscall1(SYSCALL_START_PROCESS, process_token));
}

static void abort_process(u64 process_token) {
    if (is_process_builder_token(process_token)) (void)syscall1(SYSCALL_ABORT_PROCESS, process_token);
}

static u64 alloc_map_pages_to_process_status(u64 process_token, u64 target_va, u64 page_count, u64 prot_bits) {
    return syscall5(SYSCALL_ALLOC_MAP_PAGES_TO_PROCESS, process_token, target_va, page_count, prot_bits, 0);
}

static int alloc_map_pages_to_process(u64 process_token, u64 target_va, u64 page_count, u64 prot_bits) {
    return alloc_map_pages_to_process_status(process_token, target_va, page_count, prot_bits) == SYSCALL_OK;
}

static u64 map_vm_object_range_to_process_raw(u64 process_token, u64 vm_token, u64 object_page_offset, u64 target_va, u64 page_count, u64 prot_bits) {
    return syscall6(SYSCALL_MAP_VM_OBJECT_RANGE_TO_PROCESS, process_token, vm_token, object_page_offset, target_va, page_count, prot_bits);
}

static int alloc_map_pages_to_process_chunked(u64 process_token, u64 target_va, u64 page_count, u64 prot_bits) {
    const u64 max_batch_pages = 64;
    u64 done = 0;
    while (done < page_count) {
        const u64 remaining = page_count - done;
        const u64 batch = remaining < max_batch_pages ? remaining : max_batch_pages;
        if (!alloc_map_pages_to_process(process_token, target_va + done * PAGE_BYTES, batch, prot_bits)) return 0;
        done += batch;
    }
    return 1;
}

static int copy_to_process(u64 process_token, u64 dest_va, u64 src_va, u64 byte_len) {
    return syscall4(SYSCALL_COPY_TO_PROCESS, process_token, dest_va, src_va, byte_len) == SYSCALL_OK;
}

static int zero_process_range(u64 process_token, u64 dest_va, u64 byte_len) {
    static const unsigned char zero_page[PAGE_BYTES] = {0};
    u64 done = 0;
    while (done < byte_len) {
        const u64 remaining = byte_len - done;
        const u64 chunk = remaining < PAGE_BYTES ? remaining : PAGE_BYTES;
        if (!copy_to_process(process_token, dest_va + done, (u64)zero_page, chunk)) return 0;
        done += chunk;
    }
    return 1;
}

static int set_process_initial_context(u64 process_token, u64 rip, u64 rsp) {
    struct initial_user_context ctx;
    unsigned char *bytes = (unsigned char *)&ctx;
    for (u64 i = 0; i < sizeof(ctx); i++) bytes[i] = 0;
    ctx.rip = rip;
    ctx.rsp = rsp;
    ctx.rflags = 0x202ULL;
    return syscall4(SYSCALL_SET_PROCESS_INITIAL_CONTEXT, process_token, rip, rsp, (u64)&ctx) == SYSCALL_OK;
}

static int set_process_abi_trap_delegate(u64 process_token, u64 endpoint_id, u64 target_process_slot, u64 flavor, u64 request_page_va) {
    return syscall5(SYSCALL_SET_PROCESS_ABI_TRAP_DELEGATE, process_token, endpoint_id, target_process_slot, flavor, request_page_va) == SYSCALL_OK;
}

static int install_endpoint(u64 endpoint_id, u64 process_slot) {
    return syscall3(SYSCALL_INSTALL_ENDPOINT, 0, endpoint_id, process_slot) == SYSCALL_OK;
}

static int publish_service_endpoint(u64 endpoint_id, u64 process_slot) {
    return syscall2(SYSCALL_PUBLISH_SERVICE_ENDPOINT, endpoint_id, process_slot) == SYSCALL_OK;
}

static u64 wait_event(int mailbox, u64 ticks) {
    return syscall2(SYSCALL_WAIT_EVENT, mailbox ? 1 : 0, ticks);
}

static u64 accept_cap_transfer(u64 transfer_id) {
    return syscall1(SYSCALL_ACCEPT_CAP_TRANSFER, transfer_id);
}

static u64 cfg_value_or_default(u64 value, u64 fallback) {
    return value != 0 ? value : fallback;
}

static u64 cfg_user_low_va(const struct exec_bootstrap_config *cfg) {
    return cfg_value_or_default(cfg->user_low_va, USER_LAYOUT_DEFAULT_LOW_VA);
}

static u64 cfg_user_top_va(const struct exec_bootstrap_config *cfg) {
    return cfg_value_or_default(cfg->user_top_va, USER_LAYOUT_DEFAULT_TOP_VA);
}

static u64 cfg_et_dyn_base_va(const struct exec_bootstrap_config *cfg) {
    return cfg_value_or_default(cfg->et_dyn_base_va, ET_DYN_DEFAULT_ALLOC_START_VA);
}

static u64 cfg_stack_top_va(const struct exec_bootstrap_config *cfg) {
    return cfg_value_or_default(cfg->stack_top_va, USER_STACK_DEFAULT_TOP);
}

static u64 cfg_stack_page_count(const struct exec_bootstrap_config *cfg) {
    return cfg_value_or_default(cfg->stack_page_count, LINUX_STACK_DEFAULT_PAGES);
}

static u64 cfg_mmap_base_va(const struct exec_bootstrap_config *cfg) {
    return cfg_value_or_default(cfg->mmap_base_va, EXEC_LINUX_MMAP_BASE_VA);
}

static u64 cfg_brk_initial_va(const struct exec_bootstrap_config *cfg) {
    return cfg_value_or_default(cfg->brk_initial_va, EXEC_LINUX_BRK_INITIAL_VA);
}

static int checked_stack_layout(const struct exec_bootstrap_config *cfg, u64 *top_out, u64 *bottom_out, u64 *pages_out, u64 *bytes_out) {
    const u64 top = cfg_stack_top_va(cfg);
    const u64 pages = cfg_stack_page_count(cfg);
    if ((top & (PAGE_BYTES - 1)) != 0 || pages == 0) return 0;
    if (pages > (((u64)1 << 32) / PAGE_BYTES)) return 0;
    const u64 bytes = pages * PAGE_BYTES;
    if (top < bytes) return 0;
    const u64 bottom = top - bytes;
    const u64 user_low = cfg_user_low_va(cfg);
    const u64 user_top = cfg_user_top_va(cfg);
    if (user_low >= user_top) return 0;
    if (bottom < user_low || top > user_top) return 0;
    *top_out = top;
    *bottom_out = bottom;
    *pages_out = pages;
    *bytes_out = bytes;
    return 1;
}

static int valid_user_layout_config(const struct exec_bootstrap_config *cfg) {
    const u64 user_low = cfg_user_low_va(cfg);
    const u64 user_top = cfg_user_top_va(cfg);
    if ((user_low & (PAGE_BYTES - 1)) != 0 || (user_top & (PAGE_BYTES - 1)) != 0) return 0;
    if (user_low >= user_top) return 0;
    const u64 et_dyn = cfg_et_dyn_base_va(cfg);
    if ((et_dyn & (PAGE_BYTES - 1)) != 0 || et_dyn < user_low || et_dyn >= user_top) return 0;
    const u64 mmap_base = cfg_mmap_base_va(cfg);
    if ((mmap_base & (PAGE_BYTES - 1)) != 0 || mmap_base < user_low || mmap_base >= user_top) return 0;
    const u64 brk_initial = cfg_brk_initial_va(cfg);
    if ((brk_initial & (PAGE_BYTES - 1)) != 0 || brk_initial < user_low || brk_initial >= user_top) return 0;
    if (cfg->dynamic_map_base_va != 0 || cfg->dynamic_map_end_va != 0) {
        if ((cfg->dynamic_map_base_va & (PAGE_BYTES - 1)) != 0) return 0;
        if ((cfg->dynamic_map_end_va & (PAGE_BYTES - 1)) != 0) return 0;
        if (cfg->dynamic_map_base_va < user_low || cfg->dynamic_map_end_va > user_top) return 0;
        if (cfg->dynamic_map_base_va >= cfg->dynamic_map_end_va) return 0;
    }
    u64 stack_top = 0;
    u64 stack_bottom = 0;
    u64 stack_pages = 0;
    u64 stack_bytes = 0;
    return checked_stack_layout(cfg, &stack_top, &stack_bottom, &stack_pages, &stack_bytes);
}

static int valid_config(const struct exec_bootstrap_config *cfg) {
    if (cfg->magic != EXEC_BOOTSTRAP_MAGIC || cfg->version != EXEC_BOOTSTRAP_VERSION) return 0;
    if (config_uses_external_arg_block(cfg)) {
        if (config_argv_count(cfg) > EXEC_MAX_ARGV || config_envp_count(cfg) > EXEC_MAX_ENVP) return 0;
    } else {
        if (cfg->argv_count > EXEC_INLINE_MAX_ARGV || cfg->envp_count > EXEC_MAX_ENVP) return 0;
    }
    if (cfg->arg_data_bytes > arg_data_capacity_for_config(cfg)) return 0;
    if (!valid_user_layout_config(cfg)) return 0;
    return 1;
}

static int bootstrap_slice(const struct exec_bootstrap_config *cfg, u64 offset, u64 len, struct byte_slice *out) {
    if (len == 0) return 0;
    if (cfg->arg_data_bytes > arg_data_capacity_for_config(cfg)) return 0;
    if (offset > cfg->arg_data_bytes || len > (u64)cfg->arg_data_bytes - offset) return 0;
    out->ptr = arg_data_for_config(cfg) + offset;
    out->len = len;
    return 1;
}

static struct byte_slice empty_slice(void) {
    struct byte_slice out;
    out.ptr = (const unsigned char *)0;
    out.len = 0;
    return out;
}

static struct byte_slice execfn_cache_key_from_config(const struct exec_bootstrap_config *cfg) {
    struct byte_slice out = empty_slice();
    if (cfg->execfn_bytes == 0 || cfg->execfn_bytes > SOURCE_IMAGE_PATH_BYTES) return out;
    if (!bootstrap_slice(cfg, cfg->execfn_offset, cfg->execfn_bytes, &out)) return empty_slice();
    return out;
}

static void default_stack_config(struct linux_stack_config *out) {
    out->execfn.ptr = default_execfn;
    out->execfn.len = sizeof(default_execfn) - 1;
    out->argv_count = 2;
    out->argv[0] = out->execfn;
    out->argv[1].ptr = default_arg1;
    out->argv[1].len = sizeof(default_arg1) - 1;
    out->envp_count = 2;
    out->envp[0].ptr = default_env_path;
    out->envp[0].len = sizeof(default_env_path) - 1;
    out->envp[1].ptr = default_env_os;
    out->envp[1].len = sizeof(default_env_os) - 1;
    out->platform.ptr = linux_platform;
    out->platform.len = sizeof(linux_platform) - 1;
}

static int stack_config_from_bootstrap(const struct exec_bootstrap_config *cfg, struct linux_stack_config *out) {
    const u64 argv_count = config_argv_count(cfg);
    const u64 envp_count = config_envp_count(cfg);
    if (cfg->arg_data_bytes == 0 || argv_count == 0) {
        default_stack_config(out);
        return 1;
    }
    if (argv_count > EXEC_MAX_ARGV || envp_count > EXEC_MAX_ENVP) return 0;
    if (!bootstrap_slice(cfg, cfg->execfn_offset, cfg->execfn_bytes, &out->execfn)) return 0;
    out->argv_count = argv_count;
    out->envp_count = envp_count;
    for (u64 i = 0; i < out->argv_count; i++) {
        u64 offset = 0;
        u64 len = 0;
        if (!config_argv_desc(cfg, i, &offset, &len)) return 0;
        if (!bootstrap_slice(cfg, offset, len, &out->argv[i])) return 0;
    }
    for (u64 i = 0; i < out->envp_count; i++) {
        u64 offset = 0;
        u64 len = 0;
        if (!config_envp_desc(cfg, i, &offset, &len)) return 0;
        if (!bootstrap_slice(cfg, offset, len, &out->envp[i])) return 0;
    }
    out->platform.ptr = linux_platform;
    out->platform.len = sizeof(linux_platform) - 1;
    return 1;
}

static u64 process_slot_from_builder_token(u64 process_token) {
    return process_token & PROCESS_BUILDER_PROCESS_MASK;
}

static u64 abi_trap_request_page_for_process_token(u64 config_request_page_va, u64 process_token) {
    if (config_request_page_va < LINUX_ABI_REQUEST_PAGES_BASE_VA) return config_request_page_va;
    const u64 offset = config_request_page_va - LINUX_ABI_REQUEST_PAGES_BASE_VA;
    if ((offset & (PAGE_BYTES - 1)) != 0 || offset >= LINUX_ABI_REQUEST_PAGE_COUNT * PAGE_BYTES) return config_request_page_va;
    if (!is_process_builder_token(process_token)) return config_request_page_va;
    const u64 process_slot = process_slot_from_builder_token(process_token);
    if (process_slot >= LINUX_ABI_REQUEST_PAGE_COUNT) return config_request_page_va;
    return LINUX_ABI_REQUEST_PAGES_BASE_VA + process_slot * PAGE_BYTES;
}

static int configure_abi_trap_delegate(u64 process_token, const struct exec_bootstrap_config *cfg) {
    if (cfg->abi_trap_endpoint_id == 0 || cfg->abi_trap_endpoint_process_slot == 0) return 1;
    const u64 flavor = cfg->abi_trap_flavor != 0 ? cfg->abi_trap_flavor : 1;
    const u64 request_page_va = abi_trap_request_page_for_process_token(cfg->abi_trap_request_page_va, process_token);
    if (request_page_va == 0) return 0;
    return set_process_abi_trap_delegate(
        process_token,
        cfg->abi_trap_endpoint_id,
        cfg->abi_trap_endpoint_process_slot,
        flavor,
        request_page_va
    );
}

static int add_u64(u64 a, u64 b, u64 *out) {
    return !exec_elf_add_overflows_u64(a, b, out);
}

static int add_signed_u64(u64 base, long long addend, u64 *out) {
    if (addend >= 0) return add_u64(base, (u64)addend, out);
    if (addend == (-9223372036854775807LL - 1LL)) return 0;
    const u64 magnitude = (u64)(-addend);
    if (base < magnitude) return 0;
    *out = base - magnitude;
    return 1;
}

static int align_up_u64(u64 value, u64 alignment, u64 *out) {
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) return 0;
    const u64 mask = alignment - 1;
    u64 plus;
    if (!add_u64(value, mask, &plus)) return 0;
    *out = plus & ~mask;
    return 1;
}

static int tracker_contains(const struct child_map_tracker *tracker, u64 page_va) {
    for (u64 i = 0; i < tracker->page_count; i++) {
        if (tracker->pages[i] == page_va) return 1;
    }
    return 0;
}

static int tracker_add(struct child_map_tracker *tracker, u64 page_va) {
    if (tracker_contains(tracker, page_va)) return 1;
    if (tracker->page_count >= MAX_CHILD_MAPPED_PAGES) return 0;
    tracker->pages[tracker->page_count++] = page_va;
    return 1;
}

static struct child_map_tracker *tracker_reset(void) {
    struct child_map_tracker *tracker = &child_map_tracker_storage;
    for (u64 i = 0; i < MAX_CHILD_MAPPED_PAGES; i++) tracker->pages[i] = 0;
    tracker->page_count = 0;
    return tracker;
}

static int ranges_overlap(u64 a_start, u64 a_end, u64 b_start, u64 b_end) {
    return a_start < b_end && b_start < a_end;
}

static void zero_bytes(unsigned char *dst, u64 len) {
    for (u64 i = 0; i < len; i++) dst[i] = 0;
}

static void copy_bytes(unsigned char *dst, const unsigned char *src, u64 len) {
    for (u64 i = 0; i < len; i++) dst[i] = src[i];
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

static void write_u32_le(unsigned char *bytes, unsigned int value) {
    for (u64 i = 0; i < 4; i++) bytes[i] = (unsigned char)((value >> (i * 8)) & 0xff);
}

static void write_u16_le(unsigned char *bytes, unsigned int value) {
    bytes[0] = (unsigned char)(value & 0xff);
    bytes[1] = (unsigned char)((value >> 8) & 0xff);
}

static unsigned int read_u32_le_unchecked(const unsigned char *bytes) {
    unsigned int value = 0;
    for (u64 i = 0; i < 4; i++) value |= (unsigned int)bytes[i] << (i * 8);
    return value;
}

static u64 read_tsc(void) {
    unsigned int lo;
    unsigned int hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    return ((u64)hi << 32) | (u64)lo;
}

static unsigned int elf_hash_name(const char *name) {
    unsigned int h = 0;
    while (*name != 0) {
        h = (h << 4) + (unsigned char)*name++;
        const unsigned int g = h & 0xf0000000U;
        if (g != 0) h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

static u64 vdso_cycles_per_tick(void) {
    if (service_vdso_cycles_per_tick != 0) return service_vdso_cycles_per_tick;
    enum { VDSO_CALIBRATION_TICKS = 16 };
    const u64 start_tick = syscall0(SYSCALL_GET_TICK_COUNT);
    const u64 start_tsc = read_tsc();
    u64 end_tick = start_tick;
    u64 end_tsc = start_tsc;
    for (u64 spins = 0; spins < 50000000ULL; spins++) {
        end_tick = syscall0(SYSCALL_GET_TICK_COUNT);
        end_tsc = read_tsc();
        if (end_tick >= start_tick + VDSO_CALIBRATION_TICKS) break;
        __asm__ volatile("pause" ::: "memory");
    }
    const u64 delta_ticks = end_tick > start_tick ? end_tick - start_tick : 1;
    const u64 delta_cycles = end_tsc > start_tsc ? end_tsc - start_tsc : 1000000ULL;
    u64 cycles_per_tick = delta_cycles / delta_ticks;
    if (cycles_per_tick < 1000ULL) cycles_per_tick = 1000000ULL;
    service_vdso_cycles_per_tick = cycles_per_tick;
    return cycles_per_tick;
}

static void emit8(unsigned char *page, u64 *off, unsigned int value) {
    page[(*off)++] = (unsigned char)value;
}

static void emit32(unsigned char *page, u64 *off, unsigned int value) {
    write_u32_le(page + *off, value);
    *off += 4;
}

static void emit64(unsigned char *page, u64 *off, u64 value) {
    write_u64_le(page + *off, value);
    *off += 8;
}

static u64 emit_jcc32(unsigned char *page, u64 *off, unsigned int cc) {
    emit8(page, off, 0x0f);
    emit8(page, off, cc);
    const u64 patch = *off;
    emit32(page, off, 0);
    return patch;
}

static void patch_rel32(unsigned char *page, u64 patch_off, u64 target_off) {
    const long long rel = (long long)target_off - (long long)(patch_off + 4);
    write_u32_le(page + patch_off, (unsigned int)rel);
}

static void emit_load_r8_from_abs(unsigned char *page, u64 *off, u64 address) {
    emit8(page, off, 0x49);
    emit8(page, off, 0xb8);
    emit64(page, off, address);
    emit8(page, off, 0x4d);
    emit8(page, off, 0x8b);
    emit8(page, off, 0x00);
}

static void emit_add_rax_mem_r8_abs(unsigned char *page, u64 *off, u64 address) {
    emit8(page, off, 0x49);
    emit8(page, off, 0xb8);
    emit64(page, off, address);
    emit8(page, off, 0x49);
    emit8(page, off, 0x03);
    emit8(page, off, 0x00);
}

static void emit_vdso_elapsed_ns(unsigned char *page, u64 *off, u64 vdso_va, u64 data_off) {
    emit8(page, off, 0x0f); emit8(page, off, 0x31);             /* rdtsc */
    emit8(page, off, 0x48); emit8(page, off, 0xc1); emit8(page, off, 0xe2); emit8(page, off, 0x20);
    emit8(page, off, 0x48); emit8(page, off, 0x09); emit8(page, off, 0xd0);
    emit_load_r8_from_abs(page, off, vdso_va + data_off + 0);
    emit8(page, off, 0x4c); emit8(page, off, 0x29); emit8(page, off, 0xc0);
    emit8(page, off, 0xb9); emit32(page, off, 1000000U);
    emit8(page, off, 0x48); emit8(page, off, 0xf7); emit8(page, off, 0xe1);
    emit8(page, off, 0x48); emit8(page, off, 0xbb); emit64(page, off, vdso_va + data_off + 16);
    emit8(page, off, 0x48); emit8(page, off, 0x8b); emit8(page, off, 0x1b);
    emit8(page, off, 0x48); emit8(page, off, 0xf7); emit8(page, off, 0xf3);
}

static void emit_vdso_div_1e9(unsigned char *page, u64 *off) {
    emit8(page, off, 0x31); emit8(page, off, 0xd2);
    emit8(page, off, 0xbb); emit32(page, off, 1000000000U);
    emit8(page, off, 0x48); emit8(page, off, 0xf7); emit8(page, off, 0xf3);
}

static void emit_vdso_return0(unsigned char *page, u64 *off) {
    emit8(page, off, 0x31); emit8(page, off, 0xc0);
    emit8(page, off, 0x5b);
    emit8(page, off, 0xc3);
}

static u64 emit_vdso_clock_gettime(unsigned char *page, u64 off, u64 vdso_va, u64 data_off) {
    emit8(page, &off, 0x53);
    emit8(page, &off, 0x83); emit8(page, &off, 0xff); emit8(page, &off, 0x00);
    const u64 mono_patch = emit_jcc32(page, &off, 0x85);

    emit_vdso_elapsed_ns(page, &off, vdso_va, data_off);
    emit_add_rax_mem_r8_abs(page, &off, vdso_va + data_off + 32);
    emit_vdso_div_1e9(page, &off);
    emit_add_rax_mem_r8_abs(page, &off, vdso_va + data_off + 24);
    emit8(page, &off, 0x48); emit8(page, &off, 0x89); emit8(page, &off, 0x06);
    emit8(page, &off, 0x48); emit8(page, &off, 0x89); emit8(page, &off, 0x56); emit8(page, &off, 0x08);
    emit_vdso_return0(page, &off);

    const u64 mono_off = off;
    patch_rel32(page, mono_patch, mono_off);
    emit_vdso_elapsed_ns(page, &off, vdso_va, data_off);
    emit_add_rax_mem_r8_abs(page, &off, vdso_va + data_off + 8);
    emit_vdso_div_1e9(page, &off);
    emit8(page, &off, 0x48); emit8(page, &off, 0x89); emit8(page, &off, 0x06);
    emit8(page, &off, 0x48); emit8(page, &off, 0x89); emit8(page, &off, 0x56); emit8(page, &off, 0x08);
    emit_vdso_return0(page, &off);
    return off;
}

static u64 emit_vdso_gettimeofday(unsigned char *page, u64 off, u64 vdso_va, u64 data_off) {
    emit8(page, &off, 0x53);
    emit8(page, &off, 0x48); emit8(page, &off, 0x85); emit8(page, &off, 0xff);
    const u64 skip_tv_patch = emit_jcc32(page, &off, 0x84);
    emit_vdso_elapsed_ns(page, &off, vdso_va, data_off);
    emit_add_rax_mem_r8_abs(page, &off, vdso_va + data_off + 32);
    emit_vdso_div_1e9(page, &off);
    emit_add_rax_mem_r8_abs(page, &off, vdso_va + data_off + 24);
    emit8(page, &off, 0x49); emit8(page, &off, 0x89); emit8(page, &off, 0xc0);
    emit8(page, &off, 0x48); emit8(page, &off, 0x89); emit8(page, &off, 0xd0);
    emit8(page, &off, 0x31); emit8(page, &off, 0xd2);
    emit8(page, &off, 0xbb); emit32(page, &off, 1000U);
    emit8(page, &off, 0x48); emit8(page, &off, 0xf7); emit8(page, &off, 0xf3);
    emit8(page, &off, 0x4c); emit8(page, &off, 0x89); emit8(page, &off, 0x07);
    emit8(page, &off, 0x48); emit8(page, &off, 0x89); emit8(page, &off, 0x47); emit8(page, &off, 0x08);
    const u64 skip_tv_off = off;
    patch_rel32(page, skip_tv_patch, skip_tv_off);
    emit8(page, &off, 0x48); emit8(page, &off, 0x85); emit8(page, &off, 0xf6);
    const u64 skip_tz_patch = emit_jcc32(page, &off, 0x84);
    emit8(page, &off, 0x48); emit8(page, &off, 0xc7); emit8(page, &off, 0x06); emit32(page, &off, 0);
    patch_rel32(page, skip_tz_patch, off);
    emit_vdso_return0(page, &off);
    return off;
}

static u64 emit_vdso_time(unsigned char *page, u64 off, u64 vdso_va, u64 data_off) {
    emit8(page, &off, 0x53);
    emit_vdso_elapsed_ns(page, &off, vdso_va, data_off);
    emit_add_rax_mem_r8_abs(page, &off, vdso_va + data_off + 32);
    emit_vdso_div_1e9(page, &off);
    emit_add_rax_mem_r8_abs(page, &off, vdso_va + data_off + 24);
    emit8(page, &off, 0x48); emit8(page, &off, 0x85); emit8(page, &off, 0xff);
    const u64 skip_store_patch = emit_jcc32(page, &off, 0x84);
    emit8(page, &off, 0x48); emit8(page, &off, 0x89); emit8(page, &off, 0x07);
    patch_rel32(page, skip_store_patch, off);
    emit8(page, &off, 0x5b);
    emit8(page, &off, 0xc3);
    return off;
}

static u64 append_vdso_string(unsigned char *page, u64 *off, const char *s) {
    const u64 start = *off;
    while (*s != 0) page[(*off)++] = (unsigned char)*s++;
    page[(*off)++] = 0;
    return start;
}

static void write_elf64_sym(unsigned char *page, u64 off, u64 name, unsigned int info, unsigned int shndx, u64 value, u64 size) {
    write_u32_le(page + off + 0, (unsigned int)name);
    page[off + 4] = (unsigned char)info;
    page[off + 5] = 0;
    write_u16_le(page + off + 6, shndx);
    write_u64_le(page + off + 8, value);
    write_u64_le(page + off + 16, size);
}

static void write_elf64_dyn(unsigned char *page, u64 off, u64 tag, u64 value) {
    write_u64_le(page + off + 0, tag);
    write_u64_le(page + off + 8, value);
}

static void write_elf64_phdr(unsigned char *page, u64 off, unsigned int type, unsigned int flags, u64 file_off, u64 vaddr, u64 filesz, u64 memsz, u64 align) {
    write_u32_le(page + off + 0, type);
    write_u32_le(page + off + 4, flags);
    write_u64_le(page + off + 8, file_off);
    write_u64_le(page + off + 16, vaddr);
    write_u64_le(page + off + 24, 0);
    write_u64_le(page + off + 32, filesz);
    write_u64_le(page + off + 40, memsz);
    write_u64_le(page + off + 48, align);
}

static void vdso_hash_insert(unsigned char *page, u64 hash_off, unsigned int nbucket, unsigned int sym_index, const char *name) {
    const unsigned int bucket_index = elf_hash_name(name) % nbucket;
    const u64 buckets_off = hash_off + 8;
    const u64 chains_off = buckets_off + (u64)nbucket * 4;
    const unsigned int old_head = read_u32_le_unchecked(page + buckets_off + (u64)bucket_index * 4);
    write_u32_le(page + chains_off + (u64)sym_index * 4, old_head);
    write_u32_le(page + buckets_off + (u64)bucket_index * 4, sym_index);
}

static void build_linux_vdso_page(unsigned char *page, u64 vdso_va) {
    enum {
        VDSO_DYNAMIC_OFF = 0x100,
        VDSO_HASH_OFF = 0x1a0,
        VDSO_SYMTAB_OFF = 0x200,
        VDSO_STRTAB_OFF = 0x280,
        VDSO_VERSYM_OFF = 0x310,
        VDSO_VERDEF_OFF = 0x320,
        VDSO_DATA_OFF = 0x380,
        VDSO_CODE_OFF = 0x400,
        VDSO_BUCKET_COUNT = 8,
        VDSO_SYMBOL_COUNT = 4,
    };
    zero_bytes(page, PAGE_BYTES);

    page[0] = 0x7f; page[1] = 'E'; page[2] = 'L'; page[3] = 'F';
    page[4] = 2; page[5] = 1; page[6] = 1;
    write_u16_le(page + 16, 3);
    write_u16_le(page + 18, 62);
    write_u32_le(page + 20, 1);
    write_u64_le(page + 32, 64);
    write_u16_le(page + 52, 64);
    write_u16_le(page + 54, 56);
    write_u16_le(page + 56, 2);
    write_elf64_phdr(page, 64, 1, 5, 0, 0, PAGE_BYTES, PAGE_BYTES, PAGE_BYTES);
    write_elf64_phdr(page, 120, 2, 4, VDSO_DYNAMIC_OFF, VDSO_DYNAMIC_OFF, 9 * 16, 9 * 16, 8);

    u64 code_off = VDSO_CODE_OFF;
    const u64 clock_off = code_off;
    code_off = emit_vdso_clock_gettime(page, code_off, vdso_va, VDSO_DATA_OFF);
    const u64 gettimeofday_off = code_off;
    code_off = emit_vdso_gettimeofday(page, code_off, vdso_va, VDSO_DATA_OFF);
    const u64 time_off = code_off;
    code_off = emit_vdso_time(page, code_off, vdso_va, VDSO_DATA_OFF);
    (void)code_off;

    u64 str_off = VDSO_STRTAB_OFF;
    page[str_off++] = 0;
    const u64 clock_name = append_vdso_string(page, &str_off, "__vdso_clock_gettime");
    const u64 gettimeofday_name = append_vdso_string(page, &str_off, "__vdso_gettimeofday");
    const u64 time_name = append_vdso_string(page, &str_off, "__vdso_time");
    const u64 version_name = append_vdso_string(page, &str_off, "LINUX_2.6");

    write_elf64_sym(page, VDSO_SYMTAB_OFF + 24, clock_name - VDSO_STRTAB_OFF, 0x12, 1, clock_off, gettimeofday_off - clock_off);
    write_elf64_sym(page, VDSO_SYMTAB_OFF + 48, gettimeofday_name - VDSO_STRTAB_OFF, 0x12, 1, gettimeofday_off, time_off - gettimeofday_off);
    write_elf64_sym(page, VDSO_SYMTAB_OFF + 72, time_name - VDSO_STRTAB_OFF, 0x12, 1, time_off, 96);

    write_u32_le(page + VDSO_HASH_OFF + 0, VDSO_BUCKET_COUNT);
    write_u32_le(page + VDSO_HASH_OFF + 4, VDSO_SYMBOL_COUNT);
    vdso_hash_insert(page, VDSO_HASH_OFF, VDSO_BUCKET_COUNT, 1, "__vdso_clock_gettime");
    vdso_hash_insert(page, VDSO_HASH_OFF, VDSO_BUCKET_COUNT, 2, "__vdso_gettimeofday");
    vdso_hash_insert(page, VDSO_HASH_OFF, VDSO_BUCKET_COUNT, 3, "__vdso_time");

    write_u16_le(page + VDSO_VERSYM_OFF + 0, 0);
    write_u16_le(page + VDSO_VERSYM_OFF + 2, 2);
    write_u16_le(page + VDSO_VERSYM_OFF + 4, 2);
    write_u16_le(page + VDSO_VERSYM_OFF + 6, 2);
    write_u16_le(page + VDSO_VERDEF_OFF + 0, 1);
    write_u16_le(page + VDSO_VERDEF_OFF + 2, 0);
    write_u16_le(page + VDSO_VERDEF_OFF + 4, 2);
    write_u16_le(page + VDSO_VERDEF_OFF + 6, 1);
    write_u32_le(page + VDSO_VERDEF_OFF + 8, 0x03ae75f6U);
    write_u32_le(page + VDSO_VERDEF_OFF + 12, 20);
    write_u32_le(page + VDSO_VERDEF_OFF + 16, 0);
    write_u32_le(page + VDSO_VERDEF_OFF + 20, (unsigned int)(version_name - VDSO_STRTAB_OFF));
    write_u32_le(page + VDSO_VERDEF_OFF + 24, 0);

    write_elf64_dyn(page, VDSO_DYNAMIC_OFF + 0, 4, VDSO_HASH_OFF);
    write_elf64_dyn(page, VDSO_DYNAMIC_OFF + 16, 5, VDSO_STRTAB_OFF);
    write_elf64_dyn(page, VDSO_DYNAMIC_OFF + 32, 6, VDSO_SYMTAB_OFF);
    write_elf64_dyn(page, VDSO_DYNAMIC_OFF + 48, 10, str_off - VDSO_STRTAB_OFF);
    write_elf64_dyn(page, VDSO_DYNAMIC_OFF + 64, 11, 24);
    write_elf64_dyn(page, VDSO_DYNAMIC_OFF + 80, 0x6ffffff0ULL, VDSO_VERSYM_OFF);
    write_elf64_dyn(page, VDSO_DYNAMIC_OFF + 96, 0x6ffffffcULL, VDSO_VERDEF_OFF);
    write_elf64_dyn(page, VDSO_DYNAMIC_OFF + 112, 0x6ffffffdULL, 1);
    write_elf64_dyn(page, VDSO_DYNAMIC_OFF + 128, 0, 0);

    const u64 now_tick = syscall0(SYSCALL_GET_TICK_COUNT);
    const u64 now_tsc = read_tsc();
    const u64 cycles_per_tick = vdso_cycles_per_tick();
    const u64 realtime_sec = syscall0(SYSCALL_GET_RTC_UNIX_TIME);
    write_u64_le(page + VDSO_DATA_OFF + 0, now_tsc);
    write_u64_le(page + VDSO_DATA_OFF + 8, now_tick * 1000000ULL);
    write_u64_le(page + VDSO_DATA_OFF + 16, cycles_per_tick);
    write_u64_le(page + VDSO_DATA_OFF + 24, realtime_sec == 0 ? 1700000000ULL : realtime_sec);
    write_u64_le(page + VDSO_DATA_OFF + 32, now_tick * 1000000ULL);
}

static int install_linux_vdso(u64 process_token, const struct exec_bootstrap_config *bootstrap, u64 *vdso_va_out) {
    const u64 mmap_base = cfg_mmap_base_va(bootstrap);
    if (mmap_base < PAGE_BYTES * 4) return 0;
    const u64 vdso_va = mmap_base - PAGE_BYTES * 2;
    if (vdso_va < cfg_user_low_va(bootstrap) || vdso_va + PAGE_BYTES > cfg_user_top_va(bootstrap)) return 0;
    build_linux_vdso_page(private_page_buffer, vdso_va);
    if (!alloc_map_pages_to_process(process_token, vdso_va, 1, 5)) return 0;
    if (!copy_to_process(process_token, vdso_va, (u64)private_page_buffer, PAGE_BYTES)) return 0;
    *vdso_va_out = vdso_va;
    return 1;
}

static int push_stack_bytes(u64 process_token, u64 *sp, u64 stack_bottom_va, const unsigned char *bytes, u64 len, u64 *va_out) {
    if (len == 0 || *sp < stack_bottom_va || len > *sp - stack_bottom_va) return 0;
    *sp -= len;
    if (!copy_to_process(process_token, *sp, (u64)bytes, len)) return 0;
    if (va_out != 0) *va_out = *sp;
    return 1;
}

static int push_stack_string(u64 process_token, u64 *sp, u64 stack_bottom_va, struct byte_slice value, u64 *va_out) {
    static const unsigned char nul[1] = {0};
    if (!push_stack_bytes(process_token, sp, stack_bottom_va, nul, 1, 0)) return 0;
    if (value.len == 0) {
        if (va_out != 0) *va_out = *sp;
        return 1;
    }
    return push_stack_bytes(process_token, sp, stack_bottom_va, value.ptr, value.len, va_out);
}

static int append_aux(struct aux_entry *entries, u64 max_entries, u64 *count, u64 tag, u64 value) {
    if (*count >= max_entries) return 0;
    entries[*count].tag = tag;
    entries[*count].value = value;
    (*count)++;
    return 1;
}

enum {
    STACK_AUX_ENTRY_MAX = 24,
    STACK_WORD_MAX = 1 + EXEC_MAX_ARGV + 1 + EXEC_MAX_ENVP + 1 + STACK_AUX_ENTRY_MAX * 2,
};

static int push_stack_words(u64 process_token, u64 *sp, u64 stack_bottom_va, const u64 *words, u64 count) {
    if (count > STACK_WORD_MAX) return 0;
    const u64 byte_len = count * 8;
    if (*sp < stack_bottom_va || byte_len > *sp - stack_bottom_va) return 0;
    *sp -= byte_len;
    return copy_to_process(process_token, *sp, (u64)words, byte_len);
}

static int install_linux_initial_stack(
    u64 process_token,
    const struct loaded_image *main_image,
    u64 interp_base,
    const struct exec_bootstrap_config *bootstrap,
    const struct linux_stack_config *config,
    u64 vdso_va,
    u64 *initial_rsp_out
) {
    u64 stack_top = 0;
    u64 stack_bottom = 0;
    u64 stack_pages = 0;
    u64 stack_bytes = 0;
    if (!checked_stack_layout(bootstrap, &stack_top, &stack_bottom, &stack_pages, &stack_bytes)) return 0;
    if (!alloc_map_pages_to_process_chunked(process_token, stack_bottom, stack_pages, 3)) return 0;
    if (!zero_process_range(process_token, stack_bottom, stack_bytes)) return 0;

    u64 sp = stack_top;
    const unsigned char random_bytes[16] = {
        0x43, 0x61, 0x70, 0x4f, 0x53, 0x2d, 0x6c, 0x69,
        0x6e, 0x75, 0x78, 0x2d, 0x61, 0x62, 0x69, 0x00
    };
    u64 random_va = 0;
    u64 platform_va = 0;
    u64 execfn_va = 0;
    if (!push_stack_bytes(process_token, &sp, stack_bottom, random_bytes, sizeof(random_bytes), &random_va)) return 0;
    if (!push_stack_string(process_token, &sp, stack_bottom, config->platform, &platform_va)) return 0;
    if (!push_stack_string(process_token, &sp, stack_bottom, config->execfn, &execfn_va)) return 0;

    u64 argv_ptrs[EXEC_MAX_ARGV];
    u64 envp_ptrs[EXEC_MAX_ENVP];
    for (u64 i = 0; i < EXEC_MAX_ARGV; i++) argv_ptrs[i] = 0;
    for (u64 i = 0; i < EXEC_MAX_ENVP; i++) envp_ptrs[i] = 0;
    if (config->argv_count == 0 || config->argv_count > EXEC_MAX_ARGV || config->envp_count > EXEC_MAX_ENVP) return 0;

    for (u64 i = config->argv_count; i > 0; i--) {
        const u64 index = i - 1;
        if (!push_stack_string(process_token, &sp, stack_bottom, config->argv[index], &argv_ptrs[index])) return 0;
    }
    for (u64 i = config->envp_count; i > 0; i--) {
        const u64 index = i - 1;
        if (!push_stack_string(process_token, &sp, stack_bottom, config->envp[index], &envp_ptrs[index])) return 0;
    }

    sp &= ~15ULL;

    struct aux_entry aux[STACK_AUX_ENTRY_MAX];
    u64 aux_count = 0;
    if (!append_aux(aux, STACK_AUX_ENTRY_MAX, &aux_count, AT_PHDR, main_image->phdr_va)) return 0;
    if (!append_aux(aux, STACK_AUX_ENTRY_MAX, &aux_count, AT_PHENT, main_image->summary.header.phentsize)) return 0;
    if (!append_aux(aux, STACK_AUX_ENTRY_MAX, &aux_count, AT_PHNUM, main_image->summary.header.phnum)) return 0;
    if (!append_aux(aux, STACK_AUX_ENTRY_MAX, &aux_count, AT_PAGESZ, PAGE_BYTES)) return 0;
    if (!append_aux(aux, STACK_AUX_ENTRY_MAX, &aux_count, AT_BASE, interp_base)) return 0;
    if (!append_aux(aux, STACK_AUX_ENTRY_MAX, &aux_count, AT_FLAGS, 0)) return 0;
    if (!append_aux(aux, STACK_AUX_ENTRY_MAX, &aux_count, AT_ENTRY, main_image->entry_va)) return 0;
    if (!append_aux(aux, STACK_AUX_ENTRY_MAX, &aux_count, AT_UID, 0)) return 0;
    if (!append_aux(aux, STACK_AUX_ENTRY_MAX, &aux_count, AT_EUID, 0)) return 0;
    if (!append_aux(aux, STACK_AUX_ENTRY_MAX, &aux_count, AT_GID, 0)) return 0;
    if (!append_aux(aux, STACK_AUX_ENTRY_MAX, &aux_count, AT_EGID, 0)) return 0;
    if (!append_aux(aux, STACK_AUX_ENTRY_MAX, &aux_count, AT_HWCAP, 0)) return 0;
    if (!append_aux(aux, STACK_AUX_ENTRY_MAX, &aux_count, AT_CLKTCK, 100)) return 0;
    if (!append_aux(aux, STACK_AUX_ENTRY_MAX, &aux_count, AT_SECURE, 0)) return 0;
    if (!append_aux(aux, STACK_AUX_ENTRY_MAX, &aux_count, AT_RANDOM, random_va)) return 0;
    if (!append_aux(aux, STACK_AUX_ENTRY_MAX, &aux_count, AT_EXECFN, execfn_va)) return 0;
    if (!append_aux(aux, STACK_AUX_ENTRY_MAX, &aux_count, AT_PLATFORM, platform_va)) return 0;
    if (!append_aux(aux, STACK_AUX_ENTRY_MAX, &aux_count, AT_SYSINFO_EHDR, vdso_va)) return 0;
    if (!append_aux(aux, STACK_AUX_ENTRY_MAX, &aux_count, AT_NULL, 0)) return 0;

    u64 words[STACK_WORD_MAX];
    u64 count = 0;
    words[count++] = config->argv_count;
    for (u64 i = 0; i < config->argv_count; i++) words[count++] = argv_ptrs[i];
    words[count++] = 0;
    for (u64 i = 0; i < config->envp_count; i++) words[count++] = envp_ptrs[i];
    words[count++] = 0;
    for (u64 i = 0; i < aux_count; i++) {
        words[count++] = aux[i].tag;
        words[count++] = aux[i].value;
    }
    if (!push_stack_words(process_token, &sp, stack_bottom, words, count)) return 0;
    *initial_rsp_out = sp;
    return 1;
}

static int phdr_mem_range(const struct exec_elf_program_header *phdr, u64 *start_out, u64 *end_out) {
    if (phdr->p_type != EXEC_ELF_PT_LOAD || phdr->memsz == 0) return 0;
    const u64 start = phdr->vaddr;
    u64 end;
    if (!add_u64(phdr->vaddr, phdr->memsz, &end)) return 0;
    *start_out = start;
    *end_out = end;
    return end > start;
}

static int phdr_file_range(const struct exec_elf_program_header *phdr, u64 *start_out, u64 *end_out) {
    u64 end;
    if (!add_u64(phdr->offset, phdr->filesz, &end)) return 0;
    *start_out = phdr->offset;
    *end_out = end;
    return end >= phdr->offset;
}

static int file_offset_for_vaddr(
    u64 source_va,
    const struct exec_elf_header *ehdr,
    u64 vaddr,
    u64 size,
    u64 file_bytes,
    u64 *file_off_out
) {
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

static int find_dynamic_phdr(
    u64 source_va,
    const struct exec_elf_header *ehdr,
    u64 file_bytes,
    struct exec_elf_program_header *dynamic_out
) {
    for (exec_u16 i = 0; i < ehdr->phnum; i++) {
        struct exec_elf_program_header phdr;
        if (exec_elf_parse_program_header((const void *)source_va, file_bytes, ehdr, i, &phdr) != EXEC_ELF_OK) return 0;
        if (phdr.p_type != EXEC_ELF_PT_DYNAMIC) continue;
        *dynamic_out = phdr;
        return 1;
    }
    return 0;
}

static int find_relative_relocation_table(
    u64 source_va,
    const struct exec_elf_header *ehdr,
    const struct exec_elf_program_header *dynamic,
    u64 file_bytes,
    u64 *rela_file_off_out,
    u64 *rela_size_out
) {
    if (dynamic->filesz == 0) {
        *rela_file_off_out = 0;
        *rela_size_out = 0;
        return 1;
    }
    u64 dyn_file_end;
    if (!add_u64(dynamic->offset, dynamic->filesz, &dyn_file_end) || dyn_file_end > file_bytes) return 0;
    u64 rela_va = 0;
    u64 rela_size = 0;
    u64 rela_ent = ELF_RELA_BYTES;

    for (u64 off = dynamic->offset; off + ELF_DYN_BYTES <= dyn_file_end; off += ELF_DYN_BYTES) {
        const unsigned char *dyn = (const unsigned char *)(source_va + off);
        const long long tag = read_i64_le_unchecked(dyn);
        const u64 value = read_u64_le_unchecked(dyn + 8);
        if (tag == DT_NULL) break;
        if (tag == DT_RELA) rela_va = value;
        if (tag == DT_RELASZ) rela_size = value;
        if (tag == DT_RELAENT) rela_ent = value;
    }

    if (rela_va == 0 || rela_size == 0) {
        *rela_file_off_out = 0;
        *rela_size_out = 0;
        return 1;
    }
    if (rela_ent != ELF_RELA_BYTES || (rela_size % ELF_RELA_BYTES) != 0) return 0;
    if (!file_offset_for_vaddr(source_va, ehdr, rela_va, rela_size, file_bytes, rela_file_off_out)) return 0;
    *rela_size_out = rela_size;
    return 1;
}

static int find_packed_relative_relocation_table(
    u64 source_va,
    const struct exec_elf_header *ehdr,
    const struct exec_elf_program_header *dynamic,
    u64 file_bytes,
    u64 *relr_file_off_out,
    u64 *relr_size_out
) {
    if (dynamic->filesz == 0) {
        *relr_file_off_out = 0;
        *relr_size_out = 0;
        return 1;
    }
    u64 dyn_file_end;
    if (!add_u64(dynamic->offset, dynamic->filesz, &dyn_file_end) || dyn_file_end > file_bytes) return 0;
    u64 relr_va = 0;
    u64 relr_size = 0;
    u64 relr_ent = ELF_RELR_BYTES;

    for (u64 off = dynamic->offset; off + ELF_DYN_BYTES <= dyn_file_end; off += ELF_DYN_BYTES) {
        const unsigned char *dyn = (const unsigned char *)(source_va + off);
        const long long tag = read_i64_le_unchecked(dyn);
        const u64 value = read_u64_le_unchecked(dyn + 8);
        if (tag == DT_NULL) break;
        if (tag == DT_RELR) relr_va = value;
        if (tag == DT_RELRSZ) relr_size = value;
        if (tag == DT_RELRENT) relr_ent = value;
    }

    if (relr_va == 0 || relr_size == 0) {
        *relr_file_off_out = 0;
        *relr_size_out = 0;
        return 1;
    }
    if (relr_ent != ELF_RELR_BYTES || (relr_size % ELF_RELR_BYTES) != 0) return 0;
    if (!file_offset_for_vaddr(source_va, ehdr, relr_va, relr_size, file_bytes, relr_file_off_out)) return 0;
    *relr_size_out = relr_size;
    return 1;
}

static int page_requires_private_mapping(
    u64 source_va,
    const struct exec_elf_header *ehdr,
    u64 file_bytes,
    u64 page_vaddr,
    int *requires_private_out
) {
    *requires_private_out = 0;
    u64 page_end;
    if (!add_u64(page_vaddr, PAGE_BYTES, &page_end)) return 0;

    for (exec_u16 i = 0; i < ehdr->phnum; i++) {
        struct exec_elf_program_header phdr;
        if (exec_elf_parse_program_header((const void *)source_va, file_bytes, ehdr, i, &phdr) != EXEC_ELF_OK) return 0;
        if (phdr.p_type != EXEC_ELF_PT_LOAD) continue;
        u64 segment_start;
        u64 segment_end;
        if (exec_elf_validate_load_segment(&phdr, file_bytes, &segment_start, &segment_end) != EXEC_ELF_OK) return 0;
        if (!ranges_overlap(page_vaddr, page_end, segment_start, segment_end)) continue;
        if ((phdr.flags & EXEC_ELF_PF_W) != 0 || phdr.memsz != phdr.filesz) {
            *requires_private_out = 1;
            return 1;
        }
        u64 file_mem_end = 0;
        if (!add_u64(phdr.vaddr, phdr.filesz, &file_mem_end)) return 0;
        if (page_vaddr < phdr.vaddr || page_end > file_mem_end) {
            *requires_private_out = 1;
            return 1;
        }
        const u64 file_off = phdr.offset + (page_vaddr - phdr.vaddr);
        if ((file_off & (PAGE_BYTES - 1)) != 0) {
            *requires_private_out = 1;
            return 1;
        }
    }
    return 1;
}

static int page_prot_bits_from_image(
    u64 source_va,
    const struct exec_elf_header *ehdr,
    u64 file_bytes,
    u64 page_vaddr,
    u64 *prot_bits_out
) {
    int read = 0;
    int write = 0;
    int exec = 0;
    u64 page_end;
    if (!add_u64(page_vaddr, PAGE_BYTES, &page_end)) return 0;

    for (exec_u16 i = 0; i < ehdr->phnum; i++) {
        struct exec_elf_program_header phdr;
        if (exec_elf_parse_program_header((const void *)source_va, file_bytes, ehdr, i, &phdr) != EXEC_ELF_OK) return 0;
        if (phdr.p_type != EXEC_ELF_PT_LOAD) continue;
        u64 segment_start;
        u64 segment_end;
        if (exec_elf_validate_load_segment(&phdr, file_bytes, &segment_start, &segment_end) != EXEC_ELF_OK) return 0;
        if (!ranges_overlap(page_vaddr, page_end, segment_start, segment_end)) continue;
        read = read || ((phdr.flags & EXEC_ELF_PF_R) != 0);
        write = write || ((phdr.flags & EXEC_ELF_PF_W) != 0);
        exec = exec || ((phdr.flags & EXEC_ELF_PF_X) != 0);
    }

    if (!read && !write && !exec) return 0;
    if (exec && write) return 0;
    *prot_bits_out = (read ? (1ULL << 0) : 0) | (write ? (1ULL << 1) : 0) | (exec ? (1ULL << 2) : 0);
    return 1;
}

static int fill_private_page_from_image(
    u64 source_va,
    const struct exec_elf_header *ehdr,
    u64 file_bytes,
    u64 load_bias,
    u64 target_page_va
) {
    if (target_page_va < load_bias) return 0;
    const u64 page_vaddr = target_page_va - load_bias;
    u64 page_end;
    if (!add_u64(page_vaddr, PAGE_BYTES, &page_end)) return 0;
    zero_bytes(private_page_buffer, PAGE_BYTES);

    for (exec_u16 i = 0; i < ehdr->phnum; i++) {
        struct exec_elf_program_header phdr;
        if (exec_elf_parse_program_header((const void *)source_va, file_bytes, ehdr, i, &phdr) != EXEC_ELF_OK) return 0;
        if (phdr.p_type != EXEC_ELF_PT_LOAD) continue;
        u64 mem_start;
        u64 mem_end;
        if (!phdr_mem_range(&phdr, &mem_start, &mem_end)) return 0;
        if (!ranges_overlap(page_vaddr, page_end, mem_start, mem_end)) continue;

        const u64 copy_start = page_vaddr > mem_start ? page_vaddr : mem_start;
        const u64 copy_end_mem = page_end < mem_end ? page_end : mem_end;
        const u64 segment_offset = copy_start - phdr.vaddr;
        if (segment_offset >= phdr.filesz) continue;

        u64 file_avail = phdr.filesz - segment_offset;
        u64 copy_len = copy_end_mem - copy_start;
        if (copy_len > file_avail) copy_len = file_avail;
        if (copy_len == 0) continue;

        u64 file_off;
        if (!add_u64(phdr.offset, segment_offset, &file_off)) return 0;
        u64 file_copy_end;
        if (!add_u64(file_off, copy_len, &file_copy_end) || file_copy_end > file_bytes) return 0;
        copy_bytes(private_page_buffer + (copy_start - page_vaddr), (const unsigned char *)(source_va + file_off), copy_len);
    }
    return 1;
}

static int choose_load_bias(struct load_layout *layout, const struct exec_elf_summary *summary, u64 *load_bias_out) {
    if (!summary->is_pie) {
        *load_bias_out = 0;
        return 1;
    }
    u64 load_bias;
    if (!align_up_u64(layout->next_dyn_base, summary->max_align, &load_bias)) return 0;
    u64 mapped_end;
    u64 next_base;
    if (!add_u64(load_bias, summary->max_load_vaddr, &mapped_end)) return 0;
    if (!align_up_u64(mapped_end, PAGE_BYTES, &next_base)) return 0;
    if (next_base <= layout->next_dyn_base) return 0;
    layout->next_dyn_base = next_base;
    *load_bias_out = load_bias;
    return 1;
}

static int program_headers_va(u64 source_va, const struct exec_elf_summary *summary, u64 file_bytes, u64 load_bias, u64 *phdr_va_out) {
    u64 phdr_table_bytes;
    u64 phdr_table_end;
    if (summary->header.phnum != 0 && summary->header.phentsize > (~0ULL / (u64)summary->header.phnum)) return 0;
    phdr_table_bytes = (u64)summary->header.phnum * (u64)summary->header.phentsize;
    if (!add_u64(summary->header.phoff, phdr_table_bytes, &phdr_table_end)) return 0;
    if (phdr_table_end > file_bytes) return 0;

    for (exec_u16 i = 0; i < summary->header.phnum; i++) {
        struct exec_elf_program_header phdr;
        if (exec_elf_parse_program_header((const void *)source_va, file_bytes, &summary->header, i, &phdr) != EXEC_ELF_OK) return 0;
        if (phdr.p_type != 6U) continue;
        return add_u64(load_bias, phdr.vaddr, phdr_va_out);
    }

    for (exec_u16 i = 0; i < summary->header.phnum; i++) {
        struct exec_elf_program_header phdr;
        if (exec_elf_parse_program_header((const void *)source_va, file_bytes, &summary->header, i, &phdr) != EXEC_ELF_OK) return 0;
        if (phdr.p_type != EXEC_ELF_PT_LOAD) continue;
        u64 file_start;
        u64 file_end;
        if (!phdr_file_range(&phdr, &file_start, &file_end)) return 0;
        if (summary->header.phoff < file_start || phdr_table_end > file_end) continue;
        const u64 delta = summary->header.phoff - file_start;
        u64 segment_va;
        if (!add_u64(load_bias, phdr.vaddr, &segment_va)) return 0;
        return add_u64(segment_va, delta, phdr_va_out);
    }

    return 0;
}

static int init_loaded_image(
    const struct exec_elf_summary *summary,
    u64 source_va,
    u64 file_bytes,
    u64 load_bias,
    u64 mapped_pages,
    struct loaded_image *out
) {
    u64 entry_va;
    u64 phdr_va;
    if (!add_u64(load_bias, summary->header.entry, &entry_va)) return 0;
    if (!program_headers_va(source_va, summary, file_bytes, load_bias, &phdr_va)) return 0;
    out->summary = *summary;
    out->load_bias = load_bias;
    out->entry_va = entry_va;
    out->phdr_va = phdr_va;
    out->mapped_pages = mapped_pages;
    out->initial_rip = entry_va;
    out->initial_rsp = 0;
    return 1;
}

static int source_path_matches(u64 slot, struct byte_slice path, u64 file_bytes) {
    if (path.len == 0 || source_image_path_bytes[slot] != path.len || source_image_bytes[slot] != file_bytes) return 0;
    for (u64 i = 0; i < path.len; i++) {
        if (source_image_paths[slot][i] != path.ptr[i]) return 0;
    }
    return 1;
}

static u64 source_image_slot_va(u64 slot) {
    if (slot < SOURCE_IMAGE_SMALL_SLOT_COUNT) {
        return SOURCE_IMAGE_SMALL_BASE_VA + slot * SOURCE_IMAGE_SMALL_SLOT_SPAN;
    }
    const u64 large_slot = slot - SOURCE_IMAGE_SMALL_SLOT_COUNT;
    return SOURCE_IMAGE_LARGE_BASE_VA + large_slot * SOURCE_IMAGE_LARGE_SLOT_SPAN;
}

static u64 source_image_slot_capacity(u64 slot) {
    return slot < SOURCE_IMAGE_SMALL_SLOT_COUNT ? SOURCE_IMAGE_SMALL_SLOT_SPAN : SOURCE_IMAGE_LARGE_SLOT_SPAN;
}

static void source_path_store(u64 slot, struct byte_slice path) {
    if (path.len == 0 || path.len > SOURCE_IMAGE_PATH_BYTES) {
        source_image_path_bytes[slot] = 0;
        return;
    }
    for (u64 i = 0; i < path.len; i++) source_image_paths[slot][i] = path.ptr[i];
    source_image_path_bytes[slot] = path.len;
}

static void source_path_clear(u64 slot) {
    source_image_path_bytes[slot] = 0;
    if (slot < SOURCE_IMAGE_SLOT_COUNT) source_image_paths[slot][0] = 0;
}

static int release_source_image_slot(u64 slot) {
    if (slot >= SOURCE_IMAGE_SLOT_COUNT || source_image_tokens[slot] == 0) return 1;
    const u64 source_va = source_image_slot_va(slot);
    const u64 mapped_bytes = (source_image_bytes[slot] + PAGE_BYTES - 1) & ~(u64)(PAGE_BYTES - 1);
    if (mapped_bytes == 0) return 0;
    const u64 status = release_vm_object_mapping_status(source_image_tokens[slot], source_va, mapped_bytes);
    if (status != SYSCALL_OK) {
        user_log("Exec: source release failed slot=");
        user_log_dec_value(slot);
        user_log(" status=");
        user_log_hex_value(status);
        user_log(" va=");
        user_log_hex_value(source_va);
        user_log(" bytes=");
        user_log_dec_value(mapped_bytes);
        user_log("\n");
        return 0;
    }
    source_image_tokens[slot] = 0;
    source_image_bytes[slot] = 0;
    source_path_clear(slot);
    return 1;
}

static u64 source_image_slots_in_use(void) {
    u64 count = 0;
    for (u64 i = 0; i < SOURCE_IMAGE_SLOT_COUNT; i++) {
        if (source_image_tokens[i] != 0) count++;
    }
    return count;
}

static u64 source_image_releasable_slots_in_use(void) {
    u64 count = 0;
    for (u64 i = 0; i < SOURCE_IMAGE_SLOT_COUNT; i++) {
        if (source_image_tokens[i] == 0) continue;
        if (source_image_tokens[i] == service_interpreter_vm_token) continue;
        count++;
    }
    return count;
}

static int find_reusable_source_image_slot(u64 file_bytes, u64 *slot_out) {
    for (u64 i = 0; i < SOURCE_IMAGE_SLOT_COUNT; i++) {
        if (source_image_tokens[i] != 0) continue;
        if (file_bytes > source_image_slot_capacity(i)) continue;
        *slot_out = i;
        return 1;
    }
    for (u64 attempts = 0; attempts < SOURCE_IMAGE_SLOT_COUNT; attempts++) {
        const u64 slot = (source_image_evict_cursor + attempts) % SOURCE_IMAGE_SLOT_COUNT;
        if (file_bytes > source_image_slot_capacity(slot)) continue;
        if (source_image_tokens[slot] == service_interpreter_vm_token) continue;
        if (!release_source_image_slot(slot)) continue;
        source_image_evict_cursor = (slot + 1) % SOURCE_IMAGE_SLOT_COUNT;
        *slot_out = slot;
        return 1;
    }
    return 0;
}

static u64 release_all_source_image_slots(void) {
    u64 released = 0;
    const u64 before = source_image_slots_in_use();
    for (u64 i = 0; i < SOURCE_IMAGE_SLOT_COUNT; i++) {
        if (source_image_tokens[i] == 0) continue;
        if (source_image_tokens[i] == service_interpreter_vm_token) continue;
        if (release_source_image_slot(i)) released++;
    }
    const u64 remaining = source_image_slots_in_use();
    const u64 remaining_releasable = source_image_releasable_slots_in_use();
    if (remaining_releasable != 0) {
        user_log("Exec: source release incomplete released=");
        user_log_dec_value(released);
        user_log(" remaining=");
        user_log_dec_value(remaining_releasable);
        user_log("\n");
    }
    if (service_source_sweep_log_count < 8) {
        service_source_sweep_log_count++;
        user_log("Exec: source sweep before=");
        user_log_dec_value(before);
        user_log(" released=");
        user_log_dec_value(released);
        user_log(" remaining=");
        user_log_dec_value(remaining);
        user_log("\n");
    }
    return released;
}

static void release_source_image_slots_except(u64 keep_source_va) {
    for (u64 i = 0; i < SOURCE_IMAGE_SLOT_COUNT; i++) {
        if (source_image_tokens[i] == 0) continue;
        if (source_image_slot_va(i) == keep_source_va) continue;
        if (source_image_tokens[i] == service_interpreter_vm_token) continue;
        (void)release_source_image_slot(i);
    }
}

static u64 map_source_image(u64 vm_token, u64 file_bytes, struct byte_slice cache_path, u64 *effective_vm_token_out) {
    if (file_bytes == 0) return 0;

    if (EXEC_OPT_SOURCE_PATH_CACHE) {
        for (u64 i = 0; i < SOURCE_IMAGE_SLOT_COUNT; i++) {
            if (source_path_matches(i, cache_path, file_bytes)) {
                if (service_source_map_log_count < 8) {
                    service_source_map_log_count++;
                    user_log("Exec: source reuse path slot=");
                    user_log_dec_value(i);
                    user_log("\n");
                }
                if (effective_vm_token_out != 0) *effective_vm_token_out = source_image_tokens[i];
                return source_image_slot_va(i);
            }
        }
    }

    for (u64 i = 0; i < SOURCE_IMAGE_SLOT_COUNT; i++) {
        if (source_image_tokens[i] == vm_token && source_image_bytes[i] == file_bytes) {
            if (service_source_map_log_count < 8) {
                service_source_map_log_count++;
                user_log("Exec: source reuse token slot=");
                user_log_dec_value(i);
                user_log("\n");
            }
            if (effective_vm_token_out != 0) *effective_vm_token_out = source_image_tokens[i];
            return source_image_slot_va(i);
        }
    }

    if (!is_vm_object_token(vm_token)) {
        user_log("Exec: source token invalid\n");
        return 0;
    }

    u64 slot = 0;
    if (find_reusable_source_image_slot(file_bytes, &slot)) {
        const u64 source_va = source_image_slot_va(slot);
        if (!map_vm_object(vm_token, source_va)) {
            user_log("Exec: source vm map failed\n");
            return 0;
        }
        source_image_tokens[slot] = vm_token;
        source_image_bytes[slot] = file_bytes;
        if (EXEC_OPT_SOURCE_PATH_CACHE) source_path_store(slot, cache_path);
        if (service_source_map_log_count < 8) {
            service_source_map_log_count++;
            user_log("Exec: source mapped slot=");
            user_log_dec_value(slot);
            user_log(" bytes=");
            user_log_dec_value(file_bytes);
            user_log("\n");
        }
        if (effective_vm_token_out != 0) *effective_vm_token_out = vm_token;
        return source_va;
    }

    user_log("Exec: source map cache full\n");
    return 0;
}

static int env_prefix_match(const struct exec_bootstrap_config *cfg, u64 index, const char *prefix, u64 *value_offset_out, u64 *value_len_out) {
    u64 offset = 0;
    u64 len = 0;
    if (!config_envp_desc(cfg, index, &offset, &len)) return 0;
    const u64 prefix_len = cstr_len(prefix);
    if (len < prefix_len || offset > cfg->arg_data_bytes || len > (u64)cfg->arg_data_bytes - offset) return 0;
    const unsigned char *arg_data = arg_data_for_config(cfg);
    for (u64 i = 0; i < prefix_len; i++) {
        if ((char)arg_data[offset + i] != prefix[i]) return 0;
    }
    *value_offset_out = offset + prefix_len;
    *value_len_out = len - prefix_len;
    return 1;
}

static int find_env_value(const struct exec_bootstrap_config *cfg, const char *prefix, u64 *index_out, u64 *value_offset_out, u64 *value_len_out) {
    const u64 envp_count = config_envp_count(cfg);
    for (u64 i = 0; i < envp_count && i < EXEC_MAX_ENVP; i++) {
        if (env_prefix_match(cfg, i, prefix, value_offset_out, value_len_out)) {
            *index_out = i;
            return 1;
        }
    }
    return 0;
}

static int config_has_env_prefix(const struct exec_bootstrap_config *cfg, const char *prefix) {
    u64 index = 0;
    u64 value_offset = 0;
    u64 value_len = 0;
    return find_env_value(cfg, prefix, &index, &value_offset, &value_len);
}

static void service_profile_log_execfn(const struct exec_bootstrap_config *cfg) {
    const u64 len = cfg->execfn_bytes;
    if (cfg->execfn_offset > cfg->arg_data_bytes || len > (u64)cfg->arg_data_bytes - cfg->execfn_offset) {
        user_log("?");
        return;
    }
    const unsigned char *arg_data = arg_data_for_config(cfg);
    u64 printable_len = len;
    if (printable_len != 0 && arg_data[cfg->execfn_offset + printable_len - 1] == 0) printable_len--;
    user_log_len((const char *)arg_data + cfg->execfn_offset, printable_len);
}

static void service_profile_begin(const struct exec_bootstrap_config *cfg) {
    service_profile_enabled = config_has_env_prefix(cfg, "CAPABILITYOS_EXEC_SERVICE_PROFILE=");
    if (!service_profile_enabled) return;
    service_profile_start_tick = syscall0(SYSCALL_GET_TICK_COUNT);
    service_profile_last_tick = service_profile_start_tick;
    service_profile_count = 0;
    user_log("ExecService.profile.begin path=");
    service_profile_log_execfn(cfg);
    user_log(" tick=");
    user_log_dec_value(service_profile_start_tick);
    user_log("\n");
}

static void service_profile_step(const char *label) {
    if (!service_profile_enabled) return;
    const u64 now = syscall0(SYSCALL_GET_TICK_COUNT);
    if (service_profile_count < sizeof(service_profile_dt) / sizeof(service_profile_dt[0])) {
        const u64 slot = service_profile_count++;
        service_profile_labels[slot] = label;
        service_profile_dt[slot] = now - service_profile_last_tick;
        service_profile_total[slot] = now - service_profile_start_tick;
    }
    service_profile_last_tick = now;
}

static void service_profile_flush(const struct exec_bootstrap_config *cfg) {
    if (!service_profile_enabled) return;
    u64 max_index = 0;
    for (u64 i = 0; i < service_profile_count; i++) {
        if (service_profile_dt[i] > service_profile_dt[max_index]) max_index = i;
    }
    const u64 total = service_profile_count == 0 ? 0 : service_profile_total[service_profile_count - 1];
    user_log("ExecService.profile.summary path=");
    service_profile_log_execfn(cfg);
    user_log(" count=");
    user_log_dec_value(service_profile_count);
    user_log(" total=");
    user_log_dec_value(total);
    user_log(" max_step=");
    user_log(service_profile_count != 0 ? service_profile_labels[max_index] : "none");
    user_log(" max_dt=");
    user_log_dec_value(service_profile_count != 0 ? service_profile_dt[max_index] : 0);
    if (service_direct_attempts != 0 || service_direct_fallbacks != 0 || service_direct_map_failures != 0) {
        user_log(" direct_attempts=");
        user_log_dec_value(service_direct_attempts);
        user_log(" direct_pages=");
        user_log_dec_value(service_direct_mapped_pages);
        user_log(" direct_fallbacks=");
        user_log_dec_value(service_direct_fallbacks);
        user_log(" direct_map_fail=");
        user_log_dec_value(service_direct_map_failures);
    }
    user_log("\n");
}

static int parse_hex_env_value(const struct exec_bootstrap_config *cfg, u64 value_offset, u64 value_len, u64 *out) {
    if (value_len < 3 || value_offset > cfg->arg_data_bytes || value_len > (u64)cfg->arg_data_bytes - value_offset) return 0;
    u64 i = 0;
    const unsigned char *arg_data = arg_data_for_config(cfg);
    if (value_len >= 2 && arg_data[value_offset] == '0' && (arg_data[value_offset + 1] == 'x' || arg_data[value_offset + 1] == 'X')) {
        i = 2;
    }
    u64 value = 0;
    for (; i < value_len; i++) {
        const unsigned char ch = arg_data[value_offset + i];
        u64 digit = 0;
        if (ch >= '0' && ch <= '9') digit = (u64)(ch - '0');
        else if (ch >= 'a' && ch <= 'f') digit = (u64)(10 + ch - 'a');
        else if (ch >= 'A' && ch <= 'F') digit = (u64)(10 + ch - 'A');
        else return 0;
        value = (value << 4) | digit;
    }
    *out = value;
    return 1;
}

static int write_hex_env_value(struct exec_bootstrap_config *cfg, u64 value_offset, u64 value_len, u64 value) {
    if (value_len < 18 || value_offset > cfg->arg_data_bytes || value_len > (u64)cfg->arg_data_bytes - value_offset) return 0;
    unsigned char *arg_data = mutable_arg_data_for_config(cfg);
    arg_data[value_offset] = '0';
    arg_data[value_offset + 1] = 'x';
    for (int shift = 60; shift >= 0; shift -= 4) {
        const unsigned int digit = (unsigned int)((value >> (u64)shift) & 0xFULL);
        arg_data[value_offset + 2 + (u64)((60 - shift) / 4)] =
            (unsigned char)(digit < 10 ? ('0' + digit) : ('a' + digit - 10));
    }
    return 1;
}

static int config_execfn_contains(const struct exec_bootstrap_config *cfg, const char *needle) {
    const u64 needle_len = cstr_len(needle);
    if (needle_len == 0 || cfg->execfn_offset > cfg->arg_data_bytes ||
        cfg->execfn_bytes > (u64)cfg->arg_data_bytes - cfg->execfn_offset) {
        return 0;
    }
    const unsigned char *arg_data = arg_data_for_config(cfg);
    for (u64 i = 0; i + needle_len <= cfg->execfn_bytes; i++) {
        int match = 1;
        for (u64 j = 0; j < needle_len; j++) {
            if ((char)arg_data[cfg->execfn_offset + i + j] != needle[j]) {
                match = 0;
                break;
            }
        }
        if (match) return 1;
    }
    return 0;
}

static u64 create_child_device_catalog(
    const struct device_catalog_page *source,
    u64 child_process_slot,
    struct device_catalog_page **out_page
) {
    static u64 next_catalog_va = EXEC_CHILD_DEVICE_CATALOG_BASE_VA;
    if (source == 0 || out_page == 0 ||
        source->magic != DEVICE_CATALOG_MAGIC || source->version != DEVICE_CATALOG_VERSION) {
        return 0;
    }

    const u64 catalog_va = next_catalog_va;
    next_catalog_va += 4096;
    u64 catalog_paddr = 0;
    if (syscall4(SYSCALL_ALLOC_MAP_PAGES, catalog_va, 1, 1, (u64)&catalog_paddr) != SYSCALL_OK || catalog_paddr < 4096) {
        return 0;
    }

    struct device_catalog_page *dest = (struct device_catalog_page *)catalog_va;
    for (u64 i = 0; i < 4096 / sizeof(u64); i++) {
        ((u64 *)dest)[i] = 0;
    }
    dest->magic = DEVICE_CATALOG_MAGIC;
    dest->version = DEVICE_CATALOG_VERSION;

    u64 count = source->entry_count;
    if (count > DEVICE_CATALOG_MAX_ENTRIES) count = DEVICE_CATALOG_MAX_ENTRIES;
    for (u64 i = 0; i < count; i++) {
        const struct device_catalog_entry *entry = &source->entries[i];
        if (entry->present == 0 || !is_fd(entry->device_fd)) continue;
        if (dest->entry_count >= DEVICE_CATALOG_MAX_ENTRIES) break;

        const u64 child_fd = syscall3(SYSCALL_CAPSULE_GRANT, entry->device_fd, child_process_slot, DEVICE_FD_RIGHT_CHILD);
        if (!is_fd(child_fd)) continue;

        struct device_catalog_entry *child_entry = &dest->entries[dest->entry_count++];
        const u64 *src_words = (const u64 *)entry;
        u64 *dst_words = (u64 *)child_entry;
        for (u64 word = 0; word < sizeof(struct device_catalog_entry) / sizeof(u64); word++) {
            dst_words[word] = src_words[word];
        }
        child_entry->device_fd = child_fd;
    }
    if (dest->entry_count == 0) return 0;

    const u64 local_vm = syscall3(SYSCALL_CREATE_VM_OBJECT_FROM_CURRENT_PAGES, catalog_va, 4096, VM_RIGHT_READ_MAP_GRANT);
    if (!is_vm_object_token(local_vm)) return 0;
    if (!map_vm_object(local_vm, catalog_va)) return 0;
    const u64 child_vm = syscall3(SYSCALL_GRANT_VM_OBJECT, local_vm, child_process_slot, VM_RIGHT_READ_MAP);
    if (!is_vm_object_token(child_vm)) return 0;

    *out_page = dest;
    return child_vm;
}

static struct device_catalog_entry *select_device_catalog_entry(
    const struct exec_bootstrap_config *cfg,
    struct device_catalog_page *page
) {
    if (page->magic != DEVICE_CATALOG_MAGIC || page->version != DEVICE_CATALOG_VERSION) return 0;

    u64 resource_index = 0;
    u64 resource_offset = 0;
    u64 resource_len = 0;
    if (find_env_value(cfg, "KOBOX_PACHAOS_DEVICE_RESOURCE=", &resource_index, &resource_offset, &resource_len)) {
        u64 resource_id = 0;
        if (!parse_hex_env_value(cfg, resource_offset, resource_len, &resource_id)) return 0;
        for (u64 i = 0; i < page->entry_count && i < DEVICE_CATALOG_MAX_ENTRIES; i++) {
            struct device_catalog_entry *entry = &page->entries[i];
            if (entry->present != 0 && entry->resource_id == resource_id && is_fd(entry->device_fd)) return entry;
        }
        return 0;
    }

    for (u64 i = 0; i < page->entry_count && i < DEVICE_CATALOG_MAX_ENTRIES; i++) {
        struct device_catalog_entry *entry = &page->entries[i];
        if (entry->present != 0 && is_fd(entry->device_fd)) return entry;
    }
    return 0;
}

static int materialize_capsule_from_catalog(struct exec_bootstrap_config *cfg, u64 child_process_slot) {
    u64 catalog_index = 0;
    u64 catalog_offset = 0;
    u64 catalog_len = 0;
    if (!find_env_value(cfg, "PACHA_EXEC_DEVICE_CATALOG=", &catalog_index, &catalog_offset, &catalog_len)) return 1;
    if (!config_execfn_contains(cfg, "kobox")) return 1;

    u64 catalog_token = 0;
    if (!parse_hex_env_value(cfg, catalog_offset, catalog_len, &catalog_token) || !is_vm_object_token(catalog_token)) {
        user_log("Exec: device catalog env invalid\n");
        return 0;
    }
    static u64 mapped_catalog_token;
    static u64 service_catalog_token;
    u64 source_catalog_token = catalog_token;
    if (service_catalog_token != 0 && service_catalog_token != source_catalog_token) {
        source_catalog_token = service_catalog_token;
    }
    if (mapped_catalog_token != source_catalog_token) {
        if (mapped_catalog_token != 0) {
            (void)release_vm_object_mapping(mapped_catalog_token, EXEC_DEVICE_CATALOG_VA, 4096);
            mapped_catalog_token = 0;
        }
        if (!map_vm_object(source_catalog_token, EXEC_DEVICE_CATALOG_VA)) {
            if (service_catalog_token == 0 || service_catalog_token == source_catalog_token) {
                user_log("Exec: device catalog map failed\n");
                return 0;
            }
            source_catalog_token = service_catalog_token;
            if (!map_vm_object(source_catalog_token, EXEC_DEVICE_CATALOG_VA)) {
                user_log("Exec: device catalog source map failed\n");
                return 0;
            }
        }
        mapped_catalog_token = source_catalog_token;
    }
    service_catalog_token = source_catalog_token;

    u64 kobox_index = 0;
    u64 kobox_offset = 0;
    u64 kobox_len = 0;
    if (!find_env_value(cfg, "KOBOX_PACHAOS_DEVICE_FD=", &kobox_index, &kobox_offset, &kobox_len)) {
        user_log("Exec: capsule target env missing\n");
        return 0;
    }

    struct device_catalog_page *page = (struct device_catalog_page *)EXEC_DEVICE_CATALOG_VA;
    struct device_catalog_page *child_page = 0;
    const u64 child_catalog = create_child_device_catalog(page, child_process_slot, &child_page);
    if (!is_vm_object_token(child_catalog) || child_page == 0) {
        user_log("Exec: child device catalog create failed\n");
        return 0;
    }

    struct device_catalog_entry *entry = select_device_catalog_entry(cfg, child_page);
    if (entry == 0) {
        user_log("Exec: device catalog entry missing\n");
        return 0;
    }

    if (!write_hex_env_value(cfg, kobox_offset, kobox_len, entry->device_fd)) {
        user_log("Exec: device fd env write failed\n");
        return 0;
    }
    if (!write_hex_env_value(cfg, catalog_offset, catalog_len, child_catalog)) {
        user_log("Exec: device catalog env write failed\n");
        return 0;
    }
    (void)catalog_index;
    (void)kobox_index;
    return 1;
}

static int materialize_capsule_env(struct exec_bootstrap_config *cfg, u64 child_process_slot) {
    if (!materialize_capsule_from_catalog(cfg, child_process_slot)) return 0;

    u64 source_index = 0;
    u64 source_offset = 0;
    u64 source_len = 0;
    if (!find_env_value(cfg, "PACHA_EXEC_DEVICE_FD_SOURCE=", &source_index, &source_offset, &source_len)) return 1;
    u64 source_fd = 0;
    if (!parse_hex_env_value(cfg, source_offset, source_len, &source_fd) || !is_fd(source_fd)) {
        user_log("Exec: device fd source env invalid\n");
        return 0;
    }

    u64 kobox_index = 0;
    u64 kobox_offset = 0;
    u64 kobox_len = 0;
    if (!find_env_value(cfg, "KOBOX_PACHAOS_DEVICE_FD=", &kobox_index, &kobox_offset, &kobox_len)) {
        user_log("Exec: capsule target env missing\n");
        return 0;
    }

    const u64 granted = syscall3(SYSCALL_CAPSULE_GRANT, source_fd, child_process_slot, DEVICE_FD_RIGHT_CHILD);
    if (!is_fd(granted)) {
        user_log("Exec: device fd transfer failed\n");
        return 0;
    }
    if (!write_hex_env_value(cfg, kobox_offset, kobox_len, granted)) {
        user_log("Exec: device fd env write failed\n");
        return 0;
    }
    (void)source_index;
    (void)kobox_index;
    return 1;
}

static int map_and_validate_vm_image(u64 vm_token, u64 file_bytes, struct byte_slice cache_path, u64 *source_va_out, u64 *effective_vm_token_out, struct exec_elf_summary *summary_out) {
    u64 effective_vm_token = 0;
    const u64 source_va = map_source_image(vm_token, file_bytes, cache_path, &effective_vm_token);
    if (source_va == 0) return 0;

    const enum exec_elf_error err = exec_elf_validate_image((const void *)source_va, file_bytes, summary_out);
    if (err != EXEC_ELF_OK) {
        user_log("Exec: ELF validation failed ");
        user_log(exec_elf_error_name(err));
        user_log("\n");
        return 0;
    }
    *source_va_out = source_va;
    if (effective_vm_token_out != 0) *effective_vm_token_out = effective_vm_token;
    return 1;
}

static int direct_map_segment(
    u64 process_token,
    struct child_map_tracker *tracker,
    u64 source_vm_token,
    const struct exec_elf_program_header *phdr,
    u64 file_bytes,
    u64 load_bias,
    u64 source_va,
    const struct exec_elf_header *ehdr,
    int *mapped_out
) {
    *mapped_out = 0;
    if (!service_direct_map_enabled) {
        *mapped_out = -1;
        return 1;
    }
    service_direct_attempts++;
    if ((phdr->flags & EXEC_ELF_PF_W) != 0) return 1;
    if ((phdr->flags & EXEC_ELF_PF_X) == 0) {
        *mapped_out = -1;
        return 1;
    }
    if (phdr->filesz == 0 || phdr->memsz != phdr->filesz) return 1;

    u64 file_end = 0;
    if (!add_u64(phdr->offset, phdr->filesz, &file_end) || file_end > file_bytes) return 0;
    u64 file_page_start = 0;
    if (!align_up_u64(phdr->offset, PAGE_BYTES, &file_page_start)) return 0;
    const u64 file_page_end = file_end & ~(u64)(PAGE_BYTES - 1);
    if (file_page_end <= file_page_start) return 1;

    const u64 segment_delta = file_page_start - phdr->offset;
    u64 page_vaddr = 0;
    if (!add_u64(phdr->vaddr, segment_delta, &page_vaddr)) return 0;
    u64 target_va = 0;
    if (!add_u64(load_bias, page_vaddr, &target_va)) return 0;
    if ((target_va & (PAGE_BYTES - 1)) != 0) return 1;

    u64 prot_bits = 0;
    if (!page_prot_bits_from_image(source_va, ehdr, file_bytes, page_vaddr, &prot_bits)) return 0;

    const u64 page_count = (file_page_end - file_page_start) / PAGE_BYTES;
    u64 map_status = map_vm_object_range_to_process_raw(process_token, source_vm_token, file_page_start / PAGE_BYTES, target_va, page_count, prot_bits);
    if (map_status != SYSCALL_OK) {
        release_source_image_slots_except(source_va);
        map_status = map_vm_object_range_to_process_raw(process_token, source_vm_token, file_page_start / PAGE_BYTES, target_va, page_count, prot_bits);
    }
    if (map_status != SYSCALL_OK) {
        user_log("Exec: direct range map failed status=");
        user_log_hex_value(map_status);
        user_log(" off=");
        user_log_hex_value(file_page_start / PAGE_BYTES);
        user_log(" va=");
        user_log_hex_value(target_va);
        user_log(" pages=");
        user_log_dec_value(page_count);
        user_log(" prot=");
        user_log_hex_value(prot_bits);
        user_log("\n");
        service_direct_map_failures++;
        return 0;
    }
    for (u64 page = 0; page < page_count; page++) {
        if (!tracker_add(tracker, target_va + page * PAGE_BYTES)) return 0;
    }
    service_direct_mapped_pages += page_count;
    *mapped_out = (int)page_count;
    return 1;
}

static int private_map_page(
    u64 process_token,
    struct child_map_tracker *tracker,
    u64 source_va,
    const struct exec_elf_header *ehdr,
    u64 file_bytes,
    u64 load_bias,
    u64 target_page_va,
    int *mapped_out
) {
    *mapped_out = 0;
    if (tracker_contains(tracker, target_page_va)) return 1;
    const u64 page_vaddr = target_page_va - load_bias;
    u64 prot_bits = 0;
    if (!page_prot_bits_from_image(source_va, ehdr, file_bytes, page_vaddr, &prot_bits)) return 0;
    if (!fill_private_page_from_image(source_va, ehdr, file_bytes, load_bias, target_page_va)) return 0;
    u64 alloc_status = alloc_map_pages_to_process_status(process_token, target_page_va, 1, prot_bits);
    if (alloc_status != SYSCALL_OK) {
        release_source_image_slots_except(source_va);
        alloc_status = alloc_map_pages_to_process_status(process_token, target_page_va, 1, prot_bits);
        if (alloc_status != SYSCALL_OK) {
            user_log("Exec: private alloc failed status=");
            user_log_hex_value(alloc_status);
            user_log(" va=");
            user_log_hex_value(target_page_va);
            user_log(" prot=");
            user_log_hex_value(prot_bits);
            user_log("\n");
            log_memory_stats("private_alloc");
            return 0;
        }
    }
    if (!copy_to_process(process_token, target_page_va, (u64)private_page_buffer, PAGE_BYTES)) {
        user_log("Exec: private copy failed\n");
        return 0;
    }
    if (!tracker_add(tracker, target_page_va)) {
        user_log("Exec: private tracker failed\n");
        return 0;
    }
    *mapped_out = 1;
    return 1;
}

static int private_map_required_pages(
    u64 process_token,
    struct child_map_tracker *tracker,
    u64 source_va,
    const struct exec_elf_header *ehdr,
    u64 file_bytes,
    u64 load_bias,
    int map_all_load_pages,
    u64 *mapped_count_out
) {
    *mapped_count_out = 0;
    for (exec_u16 i = 0; i < ehdr->phnum; i++) {
        struct exec_elf_program_header phdr;
        if (exec_elf_parse_program_header((const void *)source_va, file_bytes, ehdr, i, &phdr) != EXEC_ELF_OK) return 0;
        if (phdr.p_type != EXEC_ELF_PT_LOAD) continue;
        u64 segment_start;
        u64 segment_end;
        if (exec_elf_validate_load_segment(&phdr, file_bytes, &segment_start, &segment_end) != EXEC_ELF_OK) return 0;
        for (u64 page_vaddr = segment_start; page_vaddr < segment_end; page_vaddr += PAGE_BYTES) {
            int requires_private = 0;
            if (!page_requires_private_mapping(source_va, ehdr, file_bytes, page_vaddr, &requires_private)) return 0;
            if (!map_all_load_pages && !requires_private) continue;
            u64 target_page_va;
            if (!add_u64(load_bias, page_vaddr, &target_page_va)) return 0;
            int mapped = 0;
            if (!private_map_page(process_token, tracker, source_va, ehdr, file_bytes, load_bias, target_page_va, &mapped)) return 0;
            if (mapped) (*mapped_count_out)++;
        }
    }
    return 1;
}

static int apply_relative_relocations(
    u64 process_token,
    u64 source_va,
    const struct exec_elf_header *ehdr,
    u64 file_bytes,
    u64 load_bias
) {
    struct exec_elf_program_header dynamic;
    if (!find_dynamic_phdr(source_va, ehdr, file_bytes, &dynamic)) return 1;

    u64 rela_file_off = 0;
    u64 rela_size = 0;
    if (!find_relative_relocation_table(source_va, ehdr, &dynamic, file_bytes, &rela_file_off, &rela_size)) return 0;
    if (rela_size == 0) return 1;

    u64 rela_end;
    if (!add_u64(rela_file_off, rela_size, &rela_end) || rela_end > file_bytes) return 0;
    for (u64 off = rela_file_off; off < rela_end; off += ELF_RELA_BYTES) {
        const unsigned char *rela = (const unsigned char *)(source_va + off);
        const u64 r_offset = read_u64_le_unchecked(rela);
        const u64 r_info = read_u64_le_unchecked(rela + 8);
        const long long r_addend = read_i64_le_unchecked(rela + 16);
        const u64 r_type = r_info & 0xffffffffULL;
        const u64 r_sym = r_info >> 32;
        if (r_type != R_X86_64_RELATIVE || r_sym != 0) continue;

        u64 dest_va;
        u64 relocated;
        unsigned char bytes[8];
        if (!add_u64(load_bias, r_offset, &dest_va)) return 0;
        if ((dest_va & (PAGE_BYTES - 1)) > PAGE_BYTES - 8) return 0;
        if (!add_signed_u64(load_bias, r_addend, &relocated)) return 0;
        write_u64_le(bytes, relocated);
        if (!copy_to_process(process_token, dest_va, (u64)bytes, 8)) return 0;
    }
    return 1;
}

static int read_image_u64_at_vaddr(
    u64 source_va,
    const struct exec_elf_header *ehdr,
    u64 file_bytes,
    u64 vaddr,
    u64 *value_out
) {
    u64 file_off = 0;
    if (!file_offset_for_vaddr(source_va, ehdr, vaddr, 8, file_bytes, &file_off)) {
        *value_out = 0;
        return 1;
    }
    if (file_off > file_bytes || file_bytes - file_off < 8) return 0;
    *value_out = read_u64_le_unchecked((const unsigned char *)(source_va + file_off));
    return 1;
}

static int apply_packed_relative_reloc_at(
    u64 process_token,
    u64 source_va,
    const struct exec_elf_header *ehdr,
    u64 file_bytes,
    u64 load_bias,
    u64 r_offset
) {
    u64 addend = 0;
    u64 dest_va = 0;
    u64 relocated = 0;
    unsigned char bytes[8];
    if (!read_image_u64_at_vaddr(source_va, ehdr, file_bytes, r_offset, &addend)) return 0;
    if (!add_u64(load_bias, r_offset, &dest_va)) return 0;
    if ((dest_va & (PAGE_BYTES - 1)) > PAGE_BYTES - 8) return 0;
    if (!add_u64(load_bias, addend, &relocated)) return 0;
    write_u64_le(bytes, relocated);
    return copy_to_process(process_token, dest_va, (u64)bytes, 8);
}

static int apply_packed_relative_relocations(
    u64 process_token,
    u64 source_va,
    const struct exec_elf_header *ehdr,
    u64 file_bytes,
    u64 load_bias
) {
    struct exec_elf_program_header dynamic;
    if (!find_dynamic_phdr(source_va, ehdr, file_bytes, &dynamic)) return 1;

    u64 relr_file_off = 0;
    u64 relr_size = 0;
    if (!find_packed_relative_relocation_table(source_va, ehdr, &dynamic, file_bytes, &relr_file_off, &relr_size)) return 0;
    if (relr_size == 0) return 1;

    u64 relr_end;
    if (!add_u64(relr_file_off, relr_size, &relr_end) || relr_end > file_bytes) return 0;
    u64 next_r_offset = 0;
    for (u64 off = relr_file_off; off < relr_end; off += ELF_RELR_BYTES) {
        const u64 entry = read_u64_le_unchecked((const unsigned char *)(source_va + off));
        if ((entry & 1ULL) == 0) {
            if (!apply_packed_relative_reloc_at(process_token, source_va, ehdr, file_bytes, load_bias, entry)) return 0;
            next_r_offset = entry + ELF_RELR_BYTES;
            continue;
        }
        const u64 bitmap = entry >> 1;
        for (u64 bit = 0; bit < 63; bit++) {
            if ((bitmap & (1ULL << bit)) == 0) continue;
            const u64 r_offset = next_r_offset + bit * ELF_RELR_BYTES;
            if (!apply_packed_relative_reloc_at(process_token, source_va, ehdr, file_bytes, load_bias, r_offset)) return 0;
        }
        next_r_offset += 63 * ELF_RELR_BYTES;
    }
    return 1;
}

static int load_elf_image_into_process(
    u64 process_token,
    struct child_map_tracker *tracker,
    struct load_layout *layout,
    u64 vm_token,
    u64 file_bytes,
    u64 source_va,
    const struct exec_elf_summary *summary,
    int apply_initial_relocations,
    struct loaded_image *image_out
) {
    u64 load_bias = 0;
    if (!choose_load_bias(layout, summary, &load_bias)) {
        user_log("Exec: load bias failed\n");
        return 0;
    }

    int ok = 1;
    u64 mapped_count = 0;
    int map_all_load_pages = 0;
    for (exec_u16 i = 0; i < summary->header.phnum; i++) {
        struct exec_elf_program_header phdr;
        const enum exec_elf_error err = exec_elf_parse_program_header((const void *)source_va, file_bytes, &summary->header, i, &phdr);
        if (err != EXEC_ELF_OK) {
            user_log("Exec: phdr parse failed\n");
            ok = 0;
            break;
        }
        if (phdr.p_type != EXEC_ELF_PT_LOAD) continue;
        int mapped = 0;
        if (!direct_map_segment(process_token, tracker, vm_token, &phdr, file_bytes, load_bias, source_va, &summary->header, &mapped)) {
            user_log("Exec: direct segment failed\n");
            ok = 0;
            break;
        }
        if (mapped > 0) mapped_count += (u64)mapped;
        if (mapped < 0) map_all_load_pages = 1;
    }
    if (ok) {
        u64 private_count = 0;
        if (!private_map_required_pages(process_token, tracker, source_va, &summary->header, file_bytes, load_bias, map_all_load_pages, &private_count)) {
            user_log("Exec: private pages failed\n");
            ok = 0;
        } else {
            mapped_count += private_count;
        }
    }

    if (!ok || mapped_count == 0) {
        user_log("Exec: mapped count failed\n");
        return 0;
    }
    if (apply_initial_relocations) {
        if (!apply_relative_relocations(process_token, source_va, &summary->header, file_bytes, load_bias)) {
            user_log("Exec: relative reloc failed\n");
            return 0;
        }
        if (!apply_packed_relative_relocations(process_token, source_va, &summary->header, file_bytes, load_bias)) {
            user_log("Exec: packed relative reloc failed\n");
            return 0;
        }
    }
    return init_loaded_image(summary, source_va, file_bytes, load_bias, mapped_count, image_out);
}

static int prepare_process(struct exec_bootstrap_config *cfg, struct loaded_image *image_out, u64 *process_token_out, u64 *child_process_slot_out) {
    (void)config_execfn_contains;
    const int direct_map_main = 0;
    service_direct_map_enabled = direct_map_main;
    service_direct_attempts = 0;
    service_direct_mapped_pages = 0;
    service_direct_fallbacks = 0;
    service_direct_map_failures = 0;
    u64 main_source_va = 0;
    u64 main_vm_token = 0;
    struct exec_elf_summary main_summary;
    const struct byte_slice main_cache_path = execfn_cache_key_from_config(cfg);
    if (!map_and_validate_vm_image(cfg->executable_vm_token, cfg->executable_file_bytes, main_cache_path, &main_source_va, &main_vm_token, &main_summary)) return 0;
    service_profile_step("main image validated");

    const u64 process_token = create_suspended_process();
    if (process_token == 0) {
        user_log("Exec: create suspended failed\n");
        return 0;
    }
    const u64 child_process_slot = process_slot_from_builder_token(process_token);
    service_profile_step("process created");
    if (!materialize_capsule_env(cfg, child_process_slot)) {
        abort_process(process_token);
        return 0;
    }
    service_profile_step("capsules materialized");

    struct child_map_tracker *tracker = tracker_reset();

    struct load_layout layout;
    layout.next_dyn_base = cfg_et_dyn_base_va(cfg);

    const int main_apply_initial_relocations = !main_summary.has_interp;
    if (!load_elf_image_into_process(process_token, tracker, &layout, main_vm_token, cfg->executable_file_bytes, main_source_va, &main_summary, main_apply_initial_relocations, image_out)) {
        abort_process(process_token);
        user_log("Exec: main image map failed\n");
        return 0;
    }
    service_profile_step("main image loaded");
    service_direct_map_enabled = 0;

    u64 interp_base = 0;
    if (image_out->summary.has_interp) {
        if (!is_vm_object_token(cfg->interpreter_vm_token) || cfg->interpreter_file_bytes == 0) {
            abort_process(process_token);
            user_log("Exec: interpreter token absent\n");
            return 0;
        }
        u64 interp_source_va = 0;
        u64 interp_vm_token = 0;
        struct exec_elf_summary interp_summary;
        struct loaded_image interp_image;
        if (!map_and_validate_vm_image(cfg->interpreter_vm_token, cfg->interpreter_file_bytes, empty_slice(), &interp_source_va, &interp_vm_token, &interp_summary)) {
            abort_process(process_token);
            user_log("Exec: interpreter validation failed\n");
            return 0;
        }
        service_profile_step("interpreter validated");
        if (!load_elf_image_into_process(process_token, tracker, &layout, interp_vm_token, cfg->interpreter_file_bytes, interp_source_va, &interp_summary, 0, &interp_image)) {
            abort_process(process_token);
            user_log("Exec: interpreter map failed\n");
            return 0;
        }
        interp_base = interp_image.load_bias;
        image_out->initial_rip = interp_image.entry_va;
        service_profile_step("interpreter loaded");
    }

    struct linux_stack_config stack_config;
    if (!stack_config_from_bootstrap(cfg, &stack_config)) {
        abort_process(process_token);
        user_log("Exec: stack config failed\n");
        return 0;
    }
    service_profile_step("stack config built");
    u64 vdso_va = 0;
    if (!install_linux_vdso(process_token, cfg, &vdso_va)) {
        abort_process(process_token);
        user_log("Exec: vdso install failed\n");
        return 0;
    }
    service_profile_step("vdso installed");
    if (!install_linux_initial_stack(process_token, image_out, interp_base, cfg, &stack_config, vdso_va, &image_out->initial_rsp)) {
        abort_process(process_token);
        user_log("Exec: stack install failed\n");
        return 0;
    }
    service_profile_step("stack installed");
    if (!set_process_initial_context(process_token, image_out->initial_rip, image_out->initial_rsp)) {
        abort_process(process_token);
        user_log("Exec: set initial context failed\n");
        return 0;
    }
    service_profile_step("context set");
    if (!configure_abi_trap_delegate(process_token, cfg)) {
        abort_process(process_token);
        user_log("Exec: abi trap delegate failed\n");
        return 0;
    }
    service_profile_step("abi delegate configured");
    *process_token_out = process_token;
    *child_process_slot_out = child_process_slot;
    return 1;
}

static int start_prepared_process(u64 process_token, u64 expected_child_process_slot) {
    const u64 child_process_slot = start_process(process_token);
    if (child_process_slot == 0) {
        abort_process(process_token);
        user_log("Exec: start process failed\n");
        return 0;
    }
    if (child_process_slot != expected_child_process_slot) {
        user_log("Exec: child slot mismatch\n");
        return 0;
    }
    return 1;
}

static int prepare_and_start_process(struct exec_bootstrap_config *cfg, struct loaded_image *image_out, u64 *child_process_slot_out) {
    u64 process_token = 0;
    if (!prepare_process(cfg, image_out, &process_token, child_process_slot_out)) return 0;
    return start_prepared_process(process_token, *child_process_slot_out);
}

static int map_service_request_page(u64 request_paddr) {
    if (service_request_paddr == request_paddr) return 1;
    if (service_request_paddr != 0) return 0;
    service_request_va = map_page_anywhere(request_paddr, 0);
    if (service_request_va < PAGE_BYTES) return 0;
    service_request_paddr = request_paddr;
    return 1;
}

static int map_service_response_page(u64 response_paddr) {
    if (service_response_paddr == response_paddr) return 1;
    if (service_response_paddr != 0) return 0;
    service_response_va = map_page_anywhere(response_paddr, 1);
    if (service_response_va < PAGE_BYTES) return 0;
    service_response_paddr = response_paddr;
    return 1;
}

static int map_service_request_buffer(u64 request_token) {
    if (service_request_token == request_token) return 1;
    const u64 request_va = map_ipc_buffer_anywhere(request_token, 0);
    if (request_va < PAGE_BYTES) return 0;
    service_request_va = request_va;
    service_request_token = request_token;
    return 1;
}

static int map_service_response_buffer(u64 response_token) {
    if (service_response_token == response_token) return 1;
    const u64 response_va = map_ipc_buffer_anywhere(response_token, 1);
    if (response_va < PAGE_BYTES) return 0;
    service_response_va = response_va;
    service_response_token = response_token;
    return 1;
}

static void exec_launch_full_fence(void) {
    __sync_synchronize();
}

static void write_service_response(u64 op, u64 seq, u64 status, u64 child_process_slot) {
    volatile struct exec_launch_response *response = (volatile struct exec_launch_response *)service_response_va;
    response->magic = EXEC_LAUNCH_RESPONSE_MAGIC;
    response->version = EXEC_LAUNCH_VERSION;
    response->seq = seq;
    response->status = status;
    response->child_process_slot = child_process_slot;
    exec_launch_full_fence();
    response->op = op;
    exec_launch_full_fence();
}

static int wait_for_start_ready(u64 seq) {
    volatile struct exec_launch_request *request = (volatile struct exec_launch_request *)service_request_va;
    for (u64 attempts = 0; attempts < 2000000; attempts++) {
        if (request->magic == EXEC_LAUNCH_REQUEST_MAGIC &&
            request->version == EXEC_LAUNCH_VERSION &&
            request->seq == seq &&
            request->op == EXEC_LAUNCH_OP_START_READY) {
            return 1;
        }
        if ((attempts & 0x3ffu) == 0) (void)wait_event(1, 1);
        __asm__ volatile("pause" ::: "memory");
    }
    return 0;
}

static u64 pending_mapped_service_request_seq(void) {
    if (service_request_paddr == 0 && service_request_token == 0) return 0;
    const volatile struct exec_launch_request *request = (const volatile struct exec_launch_request *)service_request_va;
    if (request->magic != EXEC_LAUNCH_REQUEST_MAGIC ||
        request->version != EXEC_LAUNCH_VERSION ||
        (request->op != EXEC_LAUNCH_OP_START && request->op != EXEC_LAUNCH_OP_TRIM_CACHES)) {
        return 0;
    }
    exec_launch_full_fence();
    return request->seq;
}

static void handle_mapped_service_request(void) {
    const volatile struct exec_launch_request *request = (const volatile struct exec_launch_request *)service_request_va;
    if (request->magic != EXEC_LAUNCH_REQUEST_MAGIC ||
        request->version != EXEC_LAUNCH_VERSION ||
        !is_ipc_buffer_token(request->response_token)) {
        user_log("Exec: service request invalid\n");
        return;
    }
    exec_launch_full_fence();

    const u64 seq = request->seq;
    const u64 response_token = request->response_token;
    service_last_request_seq = seq;
    if (!map_service_response_buffer(response_token)) {
        user_log("Exec: service response map failed\n");
        return;
    }
    if (request->op == EXEC_LAUNCH_OP_TRIM_CACHES) {
        (void)release_all_source_image_slots();
        write_service_response(EXEC_LAUNCH_OP_TRIM_CACHES, seq, EXEC_LAUNCH_STATUS_OK, 0);
        return;
    }
    if (request->op != EXEC_LAUNCH_OP_START) {
        write_service_response(request->op, seq, EXEC_LAUNCH_STATUS_INVALID, 0);
        return;
    }

    struct exec_bootstrap_config *config = &service_request_config;
    {
        const volatile unsigned char *src = (const volatile unsigned char *)&request->config;
        unsigned char *dst = (unsigned char *)config;
        for (u64 i = 0; i < sizeof(*config); i++) dst[i] = src[i];
    }
    if (!is_vm_object_token(config->interpreter_vm_token) && is_vm_object_token(service_interpreter_vm_token)) {
        config->interpreter_vm_token = service_interpreter_vm_token;
        config->interpreter_file_bytes = service_interpreter_file_bytes;
    }

    if (!map_external_arg_data_for_config(config) || !valid_config(config)) {
        write_service_response(EXEC_LAUNCH_OP_START, seq, EXEC_LAUNCH_STATUS_INVALID, 0);
        return;
    }
    service_profile_begin(config);
    service_profile_step("config ready");

    struct loaded_image image;
    u64 process_token = 0;
    u64 child_process_slot = 0;
    if (!prepare_process(config, &image, &process_token, &child_process_slot)) {
        (void)release_all_source_image_slots();
        write_service_response(EXEC_LAUNCH_OP_START, seq, EXEC_LAUNCH_STATUS_LAUNCH_FAILED, 0);
        return;
    }
    (void)image;
    (void)release_all_source_image_slots();

    write_service_response(EXEC_LAUNCH_OP_START, seq, EXEC_LAUNCH_STATUS_OK, child_process_slot);
    service_profile_step("start response written");
    if (!wait_for_start_ready(seq)) {
        abort_process(process_token);
        user_log("Exec: start ack timeout\n");
        return;
    }
    service_profile_step("start ack received");
    if (!start_prepared_process(process_token, child_process_slot)) {
        write_service_response(EXEC_LAUNCH_OP_STARTED, seq, EXEC_LAUNCH_STATUS_START_FAILED, child_process_slot);
        user_log("Exec: start after response failed\n");
        return;
    }
    service_profile_step("process started");
    write_service_response(EXEC_LAUNCH_OP_STARTED, seq, EXEC_LAUNCH_STATUS_OK, child_process_slot);
    service_profile_step("started response written");
    service_profile_flush(config);
    user_log("Exec: launch ok\n");
}

static void handle_service_request_token(u64 request_token) {
    if (!map_service_request_buffer(request_token)) {
        user_log("Exec: service request token map failed\n");
        return;
    }
    handle_mapped_service_request();
}

static void handle_service_request(u64 request_paddr) {
    if (!map_service_request_page(request_paddr)) {
        user_log("Exec: service request map failed\n");
        return;
    }

    const volatile struct exec_launch_request *request = (const volatile struct exec_launch_request *)service_request_va;
    if (request->response_token < PAGE_BYTES) {
        user_log("Exec: service legacy response invalid\n");
        return;
    }
    exec_launch_full_fence();
    const u64 seq = request->seq;
    const u64 response_paddr = request->response_token;
    service_last_request_seq = seq;
    if (!map_service_response_page(response_paddr)) {
        user_log("Exec: service legacy response map failed\n");
        return;
    }
    if (request->op == EXEC_LAUNCH_OP_TRIM_CACHES) {
        (void)release_all_source_image_slots();
        write_service_response(EXEC_LAUNCH_OP_TRIM_CACHES, seq, EXEC_LAUNCH_STATUS_OK, 0);
        return;
    }
    if (request->op != EXEC_LAUNCH_OP_START) {
        write_service_response(request->op, seq, EXEC_LAUNCH_STATUS_INVALID, 0);
        return;
    }

    struct exec_bootstrap_config *config = &service_request_config;
    {
        const volatile unsigned char *src = (const volatile unsigned char *)&request->config;
        unsigned char *dst = (unsigned char *)config;
        for (u64 i = 0; i < sizeof(*config); i++) dst[i] = src[i];
    }
    if (!is_vm_object_token(config->interpreter_vm_token) && is_vm_object_token(service_interpreter_vm_token)) {
        config->interpreter_vm_token = service_interpreter_vm_token;
        config->interpreter_file_bytes = service_interpreter_file_bytes;
    }

    if (!map_external_arg_data_for_config(config) || !valid_config(config)) {
        write_service_response(EXEC_LAUNCH_OP_START, seq, EXEC_LAUNCH_STATUS_INVALID, 0);
        return;
    }
    service_profile_begin(config);
    service_profile_step("config ready");

    struct loaded_image image;
    u64 process_token = 0;
    u64 child_process_slot = 0;
    if (!prepare_process(config, &image, &process_token, &child_process_slot)) {
        (void)release_all_source_image_slots();
        write_service_response(EXEC_LAUNCH_OP_START, seq, EXEC_LAUNCH_STATUS_LAUNCH_FAILED, 0);
        return;
    }
    (void)image;
    (void)release_all_source_image_slots();

    write_service_response(EXEC_LAUNCH_OP_START, seq, EXEC_LAUNCH_STATUS_OK, child_process_slot);
    service_profile_step("start response written");
    if (!wait_for_start_ready(seq)) {
        abort_process(process_token);
        user_log("Exec: start ack timeout\n");
        return;
    }
    service_profile_step("start ack received");
    if (!start_prepared_process(process_token, child_process_slot)) {
        write_service_response(EXEC_LAUNCH_OP_STARTED, seq, EXEC_LAUNCH_STATUS_START_FAILED, child_process_slot);
        user_log("Exec: start after response failed\n");
        return;
    }
    service_profile_step("process started");
    write_service_response(EXEC_LAUNCH_OP_STARTED, seq, EXEC_LAUNCH_STATUS_OK, child_process_slot);
    service_profile_step("started response written");
    service_profile_flush(config);
    user_log("Exec: launch ok\n");
}

static void run_exec_service(const struct exec_bootstrap_config *cfg) {
    service_interpreter_vm_token = cfg->interpreter_vm_token;
    service_interpreter_file_bytes = cfg->interpreter_file_bytes;
    const u64 self_slot = syscall1(SYSCALL_GET_PROCESS_SLOT, 0);
    if (self_slot == 0 ||
        !install_endpoint(EXEC_LAUNCH_ENDPOINT_ID, self_slot) ||
        !publish_service_endpoint(EXEC_LAUNCH_ENDPOINT_ID, self_slot))
    {
        user_log("Exec: service endpoint failed\n");
        process_exit(1);
    }

    user_log("ExecService: endpoint ready\n");
    for (;;) {
        const u64 received = wait_event(1, 1);
        if (received >= PAGE_BYTES) {
            const u64 request_token = accept_ipc_buffer_transfer(received);
            if (is_ipc_buffer_token(request_token)) {
                handle_service_request_token(request_token);
            } else {
                const u64 request_paddr = accept_cap_transfer(received);
                if (request_paddr >= PAGE_BYTES) handle_service_request(request_paddr);
            }
            continue;
        }

        const u64 seq = pending_mapped_service_request_seq();
        if (seq != 0 && seq != service_last_request_seq) {
            if (service_request_token != 0) handle_mapped_service_request();
            else handle_service_request(service_request_paddr);
        }
    }
}

static void run_exec_once(const struct exec_bootstrap_config *cfg) {
    struct loaded_image image;
    u64 child_process_slot = 0;
    {
        const volatile unsigned char *src = (const volatile unsigned char *)cfg;
        volatile unsigned char *dst = (volatile unsigned char *)&one_shot_config;
        for (u64 i = 0; i < sizeof(one_shot_config); i++) dst[i] = src[i];
    }
    if (!prepare_and_start_process(&one_shot_config, &image, &child_process_slot)) {
        user_log("Exec: one-shot launch failed\n");
        process_exit(1);
    }
    (void)image;

    for (;;) {
        const u64 status = syscall1(SYSCALL_GET_PROCESS_STATUS, child_process_slot);
        const u64 kind = status & 0xff;
        if (kind == PROCESS_STATUS_INACTIVE) {
            process_exit(0);
        }
        if (kind == PROCESS_STATUS_FAULTED) {
            user_log("Exec: child faulted\n");
            process_exit(1);
        }
        if (kind != PROCESS_STATUS_ACTIVE) {
            user_log("Exec: child status invalid\n");
            process_exit(1);
        }
        (void)wait_event(0, 1);
    }
}

void exec_main(void) {
    const struct exec_bootstrap_config *cfg = (const struct exec_bootstrap_config *)EXEC_BOOTSTRAP_TARGET_VA;
    if (!valid_config(cfg)) {
        user_log("Exec: missing bootstrap config\n");
        process_exit(1);
    }

    if ((cfg->flags & EXEC_BOOTSTRAP_FLAG_SERVICE_MODE) != 0) {
        run_exec_service(cfg);
    }

    run_exec_once(cfg);
}
