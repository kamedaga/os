#include "exec_service_abi.h"

#define EXEC_SERVICE_PROFILE 0

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef int i32;
typedef long long i64;

#define OFFSETOF(type, member) __builtin_offsetof(type, member)

enum {
    SYSCALL_ALLOC_PAGE = 0x1,
    SYSCALL_MAP_PAGE = 0x2,
    SYSCALL_LOG = 0x9,
    SYSCALL_ALLOC_MAP_PAGES = 0xC,
    SYSCALL_WAIT_EVENT = 0x17,
    SYSCALL_SPAWN_EXEC = 0x1D,
    SYSCALL_INSTALL_VM_OBJECT = 0x1E,
    SYSCALL_INSTALL_EXEC_IMAGE = 0x20,
    SYSCALL_INSTALL_ENDPOINT = 0x26,
    SYSCALL_ACCEPT_CAP_TRANSFER = 0x2A,
    SYSCALL_SHARE_CAP = 0x2B,
    SYSCALL_SIGNAL_ENDPOINT = 0x2C,
    SYSCALL_GET_PROCESS_SLOT = 0x2E,
    SYSCALL_GET_PROCESS_STATUS = 0x30,
    SYSCALL_PUBLISH_SERVICE_ENDPOINT = 0x33,
    SYSCALL_PROCESS_EXIT = 0x34,
    SYSCALL_IPC_CALL_REPLY_RECV = 0x40,
    SYSCALL_OK = 0,
    SYSCALL_ERR_ENDPOINT = 9,
    IPC_CALL_FLAG_SIGNAL_ONLY = 0x2,

    PAGE_BYTES = 4096,
    SERVICE_REGISTRY_SHADOW_VA = 0x3C2C0000,
    SERVICE_REGISTRY_MAGIC = 0x53525643,
    SERVICE_REGISTRY_VERSION = 1,
    SERVICE_REGISTRY_MAX_ENTRIES = 12,
    SERVICE_KIND_VFS = 2,

    VFS_REQUEST_VA = 0x2B000000,
    VFS_RESPONSE_VA = 0x2B001000,
    FS_REQUEST_MAGIC = 0x51534653,
    FS_RESPONSE_MAGIC = 0x52534653,
    FS_PROTOCOL_VERSION = 1,
    FS_MAX_PATH_BYTES = 128,
    FS_REQUEST_HEADER_BYTES = 72,
    FS_RESPONSE_HEADER_BYTES = 72,
    FS_RESPONSE_PAYLOAD_BYTES = PAGE_BYTES - FS_RESPONSE_HEADER_BYTES,
    FS_OP_CONNECT = 1,
    FS_OP_LOOKUP = 16,
    FS_OP_OPEN = 17,
    FS_OP_READ = 18,
    FS_OP_CLOSE = 21,
    FS_STATUS_OK = 0,
    FS_OBJECT_NONE = 0,
    FS_OBJECT_FILE = 3,

    SPAWN_RESULT_TAG = 1ULL << 63,
    SPAWN_RESULT_PROCESS_MASK = 0xffffffffULL,
    SPAWN_FLAG_BOOTSTRAP_PAGE_WRITABLE = 1 << 0,
    SPAWN_FLAG_BOOTSTRAP_EXTENDED_DESCRIPTOR_TABLE = 1 << 2,
    SPAWN_FLAG_CHILD_BOOTSTRAP_OWNER = 1 << 3,
    BOOTSTRAP_CAP_KIND_VM_OBJECT = 2,
    VM_OBJECT_TOKEN_TAG = 1ULL << 62,
    EXEC_IMAGE_TOKEN_TAG = (1ULL << 62) | (1ULL << 61),
    VM_RIGHT_READ_MAP = 0x5,
    VM_RIGHT_READ_MAP_GRANT = 0xD,
    EXEC_RIGHT_EXEC_GRANT = 0x3,

    PROCESS_STANDARD_CONFIG_TARGET_VA = 0x3C002000,
    PROCESS_SERVICE_REGISTRY_SHADOW_VA = 0x3C2C0000,
    EXEC_LOADER_BOOTSTRAP_MAGIC = 0x5845434C44523031ULL,
    EXEC_LOADER_BOOTSTRAP_VERSION = 2,
    EXEC_LOADER_CONFIG_TARGET_VA = 0x3C002000,
    LINUX_ABI_BOOTSTRAP_MAGIC = 0x4C41424943464731ULL,
    LINUX_ABI_BOOTSTRAP_VERSION = 2,
    LINUX_ABI_BOOTSTRAP_READY = 0x4C414249524459ULL,
    LINUX_ABI_ENDPOINT_ID = 0x90,
    LINUX_ABI_TRAP_REQUEST_PAGE_VA = 0x2A000000,

    EXECVE_MAX_ARGV = 8,
    EXECVE_MAX_ENVP = 16,
    EXECVE_MAX_ARG_DATA_BYTES = 2048,

    SCRATCH_APP_IMAGE_VA = 0x26000000,
    SCRATCH_LD_IMAGE_VA = 0x26200000,
    SCRATCH_EXEC_LOADER_IMAGE_VA = 0x26400000,
    SCRATCH_LINUX_ABI_IMAGE_VA = 0x26600000,
    SCRATCH_EXEC_CONFIG_VA = 0x26800000,
    SCRATCH_EXEC_TABLE_VA = 0x26801000,
    SCRATCH_LINUX_ABI_CONFIG_VA = 0x26802000,
    SCRATCH_LINUX_ABI_TABLE_VA = 0x26803000,
    SCRATCH_REGISTRY_COPY_VA = 0x26804000,
    SCRATCH_REQUEST_MAP_BASE_VA = 0x26900000,
    SCRATCH_RESPONSE_MAP_BASE_VA = 0x26910000,
    MAX_APP_IMAGE_BYTES = 2 * 1024 * 1024,
    MAX_LD_IMAGE_BYTES = 768 * 1024,
    MAX_SERVICE_IMAGE_BYTES = 256 * 1024,
};

struct service_entry { u64 kind; u64 process_slot; u64 endpoint_id; u64 flags; };
struct service_registry_page { u64 magic; u64 version; u64 entry_count; u64 reserved0; struct service_entry entries[SERVICE_REGISTRY_MAX_ENTRIES]; };
struct fs_request_header {
    u32 magic; u16 version; u16 op; u64 request_seq; u64 object_token; u64 offset; u32 length; u32 flags;
    u16 path_bytes; u16 inline_bytes; u32 reserved0; u64 arg0; u64 arg1; u64 session_nonce;
};
struct fs_response_header {
    u32 magic; u16 version; u16 op; u64 response_seq; i32 status; u32 result_flags; u64 result_token;
    u64 file_bytes; u64 cursor_next; u16 inline_bytes; u8 object_kind; u8 reserved0; u32 reserved1; u64 arg0; u64 arg1;
};
struct vfs_client { int active; u64 endpoint_id; u64 process_slot; u64 request_paddr; u64 response_paddr; u64 root_token; u64 next_seq; u64 session_nonce; };
struct exec_loader_config {
    u64 magic; u64 version; u64 executable_vm_token; u64 executable_file_bytes; u64 flags;
    u64 interpreter_vm_token; u64 interpreter_file_bytes; u64 bootfs_vm_token; u64 bootfs_file_bytes;
    u64 fs_endpoint_id; u64 fs_compat_process_slot; u64 abi_trap_endpoint_id; u64 abi_trap_endpoint_process_slot;
    u64 abi_trap_flavor; u64 abi_trap_request_page_va;
    u16 execfn_offset; u16 execfn_bytes; u16 argv_count; u16 envp_count; u16 arg_data_bytes; u16 reserved_arg0;
    u16 argv_offsets[EXECVE_MAX_ARGV]; u16 argv_bytes[EXECVE_MAX_ARGV];
    u16 envp_offsets[EXECVE_MAX_ENVP]; u16 envp_bytes[EXECVE_MAX_ENVP];
    u8 arg_data[EXECVE_MAX_ARG_DATA_BYTES];
};
struct bootstrap_page_descriptor { u64 source_va; u64 target_va; u64 flags; };
struct bootstrap_cap_descriptor { u64 source_token; u64 target_token_va; u64 rights_bits; u8 kind; u8 reserved[7]; };
struct bootstrap_descriptor_table {
    u16 page_count; u16 cap_count; u32 reserved0;
    struct bootstrap_page_descriptor page_descriptors[136];
    struct bootstrap_cap_descriptor cap_descriptors[8];
};
struct linux_abi_bootstrap_config {
    u64 magic;
    u64 version;
    u64 exec_loader_vm_token;
    u64 standard_interpreter_vm_token;
    u64 standard_interpreter_file_bytes;
    u64 abi_trap_request_page_va;
    u64 status;
    u16 exec_path_bytes;
    u8 reserved0[6];
    char exec_path[128];
};

static struct vfs_client g_vfs;
static int scratch_ready = 0;
static u64 g_loader_exec_token = 0;
static u64 g_loader_vm_token = 0;
static u64 g_linux_abi_exec_token = 0;
static u64 g_ld_vm_token = 0;
static u64 g_ld_bytes = 0;
static u64 request_map_next = SCRATCH_REQUEST_MAP_BASE_VA;
static u64 response_map_next = SCRATCH_RESPONSE_MAP_BASE_VA;
static const char *g_vfs_read_profile_label = 0;

static u64 cstr_len(const char *s) { u64 n = 0; while (s[n] != 0) n++; return n; }
static u64 min_u64(u64 a, u64 b) { return a < b ? a : b; }
static void user_log_len(const char *message, u64 len) { u64 ret; __asm__ volatile("int $0x80" : "=a"(ret) : "a"((u64)SYSCALL_LOG), "D"((u64)message), "S"(len) : "rcx", "rdx", "r8", "r9", "r10", "r11", "memory"); (void)ret; }
static void user_log(const char *message) { user_log_len(message, cstr_len(message)); }
static void profile_vfs_read_step(const char *step) {
    if (!EXEC_SERVICE_PROFILE || g_vfs_read_profile_label == 0) return;
    user_log("ExecService.prof: ");
    user_log(g_vfs_read_profile_label);
    user_log(" ");
    user_log(step);
    user_log("\n");
}
static void clear_page(u64 va) { volatile u64 *p = (volatile u64 *)va; for (u64 i = 0; i < 512; i++) p[i] = 0; }
static u64 syscall0(u64 nr) { u64 ret; __asm__ volatile("int $0x80" : "=a"(ret) : "a"(nr) : "rcx", "rdx", "r8", "r9", "r10", "r11", "memory"); return ret; }
static u64 syscall1(u64 nr, u64 a0) { u64 ret; __asm__ volatile("int $0x80" : "=a"(ret) : "a"(nr), "D"(a0) : "rcx", "rdx", "r8", "r9", "r10", "r11", "memory"); return ret; }
static u64 syscall2(u64 nr, u64 a0, u64 a1) { u64 ret; __asm__ volatile("int $0x80" : "=a"(ret) : "a"(nr), "D"(a0), "S"(a1) : "rcx", "rdx", "r8", "r9", "r10", "r11", "memory"); return ret; }
static u64 syscall3(u64 nr, u64 a0, u64 a1, u64 a2) { u64 ret; __asm__ volatile("int $0x80" : "=a"(ret) : "a"(nr), "D"(a0), "S"(a1), "d"(a2) : "rcx", "r8", "r9", "r10", "r11", "memory"); return ret; }
static u64 syscall4(u64 nr, u64 a0, u64 a1, u64 a2, u64 a3) { u64 ret; __asm__ volatile("int $0x80" : "=a"(ret) : "a"(nr), "D"(a0), "S"(a1), "d"(a2), "c"(a3) : "r8", "r9", "r10", "r11", "memory"); return ret; }
static u64 ipc_call_reply_recv_signal_only(u64 endpoint_id) {
    register u64 rax __asm__("rax") = SYSCALL_IPC_CALL_REPLY_RECV;
    register u64 rdi __asm__("rdi") = 0;
    register u64 rsi __asm__("rsi") = endpoint_id;
    register u64 rdx __asm__("rdx") = IPC_CALL_FLAG_SIGNAL_ONLY;
    __asm__ volatile("syscall" : "+a"(rax) : "D"(rdi), "S"(rsi), "d"(rdx) : "rcx", "r8", "r9", "r10", "r11", "memory");
    return rax;
}
static u64 alloc_map_pages(u64 target_va, u64 page_count, u64 writable) { return syscall4(SYSCALL_ALLOC_MAP_PAGES, target_va, page_count, writable, 0); }
static int map_page(u64 va, u64 paddr, u64 writable) { return syscall3(SYSCALL_MAP_PAGE, va, paddr, writable) == SYSCALL_OK; }
static int is_vm_object_token(u64 token) { return (token & EXEC_IMAGE_TOKEN_TAG) != EXEC_IMAGE_TOKEN_TAG && (token & VM_OBJECT_TOKEN_TAG) != 0 && (token & ~VM_OBJECT_TOKEN_TAG) != 0; }
static int is_exec_image_token(u64 token) { return (token & EXEC_IMAGE_TOKEN_TAG) == EXEC_IMAGE_TOKEN_TAG && (token & ~EXEC_IMAGE_TOKEN_TAG) != 0; }
static u64 decode_spawned_process_slot(u64 value) { if ((value & SPAWN_RESULT_TAG) == 0) return 0; return value & SPAWN_RESULT_PROCESS_MASK; }

static int find_service(u64 kind, struct service_entry *out) {
    volatile struct service_registry_page *page = (volatile struct service_registry_page *)SERVICE_REGISTRY_SHADOW_VA;
    if (page->magic != SERVICE_REGISTRY_MAGIC || page->version != SERVICE_REGISTRY_VERSION) return 0;
    for (u64 i = 0; i < page->entry_count && i < SERVICE_REGISTRY_MAX_ENTRIES; i++) {
        if (page->entries[i].kind != kind) continue;
        out->kind = page->entries[i].kind; out->process_slot = page->entries[i].process_slot; out->endpoint_id = page->entries[i].endpoint_id; out->flags = page->entries[i].flags;
        return 1;
    }
    return 0;
}

static u64 make_nonce(u64 request_paddr, u64 response_paddr, u64 endpoint_id, u64 process_slot) {
    const u64 nonce = request_paddr ^ ((response_paddr << 17) | (response_paddr >> 47)) ^ ((endpoint_id << 7) | (endpoint_id >> 57)) ^ process_slot ^ 0x9e3779b97f4a7c15ULL;
    return nonce == 0 ? 1 : nonce;
}
static int install_vfs_endpoint(void) { return syscall3(SYSCALL_INSTALL_ENDPOINT, 0, g_vfs.endpoint_id, g_vfs.process_slot) == SYSCALL_OK; }
static int grant_vfs_response_page(void) { u64 ret = syscall3(0x24, g_vfs.response_paddr, g_vfs.endpoint_id, 3); if (ret == SYSCALL_OK) return 1; if (ret == SYSCALL_ERR_ENDPOINT && install_vfs_endpoint()) ret = syscall3(0x24, g_vfs.response_paddr, g_vfs.endpoint_id, 3); return ret == SYSCALL_OK; }
static int share_vfs_request_page(void) { u64 ret = syscall2(SYSCALL_SHARE_CAP, g_vfs.request_paddr, g_vfs.endpoint_id); if (ret == SYSCALL_OK) return 1; if (ret == SYSCALL_ERR_ENDPOINT && install_vfs_endpoint()) ret = syscall2(SYSCALL_SHARE_CAP, g_vfs.request_paddr, g_vfs.endpoint_id); return ret == SYSCALL_OK; }
static int signal_vfs(void) {
    u64 ret = ipc_call_reply_recv_signal_only(g_vfs.endpoint_id);
    if (ret == SYSCALL_OK) return 1;
    if (ret == SYSCALL_ERR_ENDPOINT && install_vfs_endpoint()) {
        ret = ipc_call_reply_recv_signal_only(g_vfs.endpoint_id);
        if (ret == SYSCALL_OK) return 1;
    }
    ret = syscall2(SYSCALL_SIGNAL_ENDPOINT, g_vfs.endpoint_id, 0);
    if (ret == SYSCALL_OK) return 1;
    if (ret == SYSCALL_ERR_ENDPOINT && install_vfs_endpoint()) ret = syscall2(SYSCALL_SIGNAL_ENDPOINT, g_vfs.endpoint_id, 0);
    return ret == SYSCALL_OK;
}
static int wait_vfs_response(u64 expected_seq, u16 expected_op) { volatile struct fs_response_header *response = (volatile struct fs_response_header *)VFS_RESPONSE_VA; for (u64 i = 0; i < 8192; i++) { if (response->response_seq == expected_seq) return response->magic == FS_RESPONSE_MAGIC && response->version == FS_PROTOCOL_VERSION && response->op == expected_op; (void)syscall2(SYSCALL_WAIT_EVENT, 0, 1); } return 0; }

static int connect_vfs_from_registry(void) {
    struct service_entry entry;
    if (!find_service(SERVICE_KIND_VFS, &entry)) return 0;
    g_vfs.endpoint_id = entry.endpoint_id; g_vfs.process_slot = entry.process_slot;
    g_vfs.request_paddr = syscall0(SYSCALL_ALLOC_PAGE);
    g_vfs.response_paddr = syscall0(SYSCALL_ALLOC_PAGE);
    if (g_vfs.request_paddr < 0x1000 || g_vfs.response_paddr < 0x1000) return 0;
    if (!map_page(VFS_REQUEST_VA, g_vfs.request_paddr, 1)) return 0;
    if (!map_page(VFS_RESPONSE_VA, g_vfs.response_paddr, 1)) return 0;
    if (!grant_vfs_response_page()) return 0;
    clear_page(VFS_REQUEST_VA); clear_page(VFS_RESPONSE_VA);
    const u64 self_slot = syscall0(SYSCALL_GET_PROCESS_SLOT);
    g_vfs.session_nonce = make_nonce(g_vfs.request_paddr, g_vfs.response_paddr, g_vfs.endpoint_id, self_slot);
    volatile struct fs_request_header *request = (volatile struct fs_request_header *)VFS_REQUEST_VA;
    request->magic = FS_REQUEST_MAGIC; request->version = FS_PROTOCOL_VERSION; request->op = FS_OP_CONNECT; request->arg0 = g_vfs.response_paddr; request->arg1 = self_slot; request->session_nonce = g_vfs.session_nonce;
    __asm__ volatile("" ::: "memory");
    request->request_seq = 1;
    if (!share_vfs_request_page()) return 0;
    if (!wait_vfs_response(1, FS_OP_CONNECT)) return 0;
    volatile struct fs_response_header *response = (volatile struct fs_response_header *)VFS_RESPONSE_VA;
    if (response->status != FS_STATUS_OK || response->result_token == 0) return 0;
    g_vfs.root_token = response->result_token; g_vfs.next_seq = 2; g_vfs.active = 1;
    user_log("ExecService: vfs connect ok\n");
    return 1;
}

static int vfs_request(u16 op, u64 token, u64 offset, u32 length, const char *path, u16 path_len) {
    if (!g_vfs.active) return 0;
    clear_page(VFS_REQUEST_VA); clear_page(VFS_RESPONSE_VA);
    volatile struct fs_request_header *request = (volatile struct fs_request_header *)VFS_REQUEST_VA;
    const u64 seq = g_vfs.next_seq++;
    request->magic = FS_REQUEST_MAGIC; request->version = FS_PROTOCOL_VERSION; request->op = op; request->object_token = token; request->offset = offset; request->length = length; request->session_nonce = g_vfs.session_nonce;
    if (path != 0 && path_len != 0) {
        if (path_len > FS_MAX_PATH_BYTES) return 0;
        request->path_bytes = path_len;
        volatile u8 *payload = (volatile u8 *)(VFS_REQUEST_VA + FS_REQUEST_HEADER_BYTES);
        for (u64 i = 0; i < path_len; i++) payload[i] = (u8)path[i];
    }
    __asm__ volatile("" ::: "memory");
    request->request_seq = seq;
    if (!signal_vfs()) return 0;
    return wait_vfs_response(seq, op);
}

static int vfs_lookup_file_token(const char *path, u16 path_len, u64 *token_out, u64 *file_bytes_out) {
    if (!vfs_request(FS_OP_LOOKUP, g_vfs.root_token, 0, 0, path, path_len)) { user_log("ExecService: vfs lookup request failed\n"); return 0; }
    volatile struct fs_response_header *response = (volatile struct fs_response_header *)VFS_RESPONSE_VA;
    if (response->status != FS_STATUS_OK || response->result_token == 0 || response->object_kind != FS_OBJECT_FILE) { user_log("ExecService: vfs lookup status failed\n"); return 0; }
    *token_out = response->result_token;
    *file_bytes_out = response->file_bytes;
    return response->file_bytes != 0;
}

static int vfs_read_file_to_buffer(const char *path, u16 path_len, u64 buffer_va, u64 buffer_cap, u64 *file_bytes_out) {
    u64 file_token = 0; u64 file_bytes = 0;
    profile_vfs_read_step("lookup begin");
    if (!vfs_lookup_file_token(path, path_len, &file_token, &file_bytes)) { user_log("ExecService: vfs read lookup failed\n"); return 0; }
    profile_vfs_read_step("lookup done");
    if (file_bytes == 0 || file_bytes > buffer_cap) { user_log("ExecService: vfs read size invalid\n"); return 0; }
    if (!vfs_request(FS_OP_OPEN, file_token, 0, 0, 0, 0)) { user_log("ExecService: vfs open request failed\n"); return 0; }
    volatile struct fs_response_header *response = (volatile struct fs_response_header *)VFS_RESPONSE_VA;
    if (response->status != FS_STATUS_OK || response->result_token == 0) { user_log("ExecService: vfs open status failed\n"); return 0; }
    profile_vfs_read_step("open done");
    const u64 open_token = response->result_token;
    u64 copied = 0;
    int ok = 1;
    while (copied < file_bytes) {
        const u64 chunk = min_u64(file_bytes - copied, FS_RESPONSE_PAYLOAD_BYTES);
        if (!vfs_request(FS_OP_READ, open_token, copied, (u32)chunk, 0, 0)) { user_log("ExecService: vfs read request failed\n"); ok = 0; break; }
        response = (volatile struct fs_response_header *)VFS_RESPONSE_VA;
        if (response->status != FS_STATUS_OK || response->inline_bytes == 0) { user_log("ExecService: vfs read chunk failed\n"); ok = 0; break; }
        volatile u8 *src = (volatile u8 *)(VFS_RESPONSE_VA + FS_RESPONSE_HEADER_BYTES);
        u8 *dst = (u8 *)(buffer_va + copied);
        for (u64 i = 0; i < response->inline_bytes; i++) dst[i] = src[i];
        copied += response->inline_bytes;
    }
    profile_vfs_read_step("read done");
    (void)vfs_request(FS_OP_CLOSE, open_token, 0, 0, 0, 0);
    profile_vfs_read_step("close done");
    if (!ok) return 0;
    *file_bytes_out = file_bytes;
    return 1;
}

static int alloc_map_range_self(u64 base_va, u64 page_count) {
    u64 done = 0;
    while (done < page_count) {
        const u64 chunk = min_u64(page_count - done, 64);
        if (alloc_map_pages(base_va + done * PAGE_BYTES, chunk, 1) != SYSCALL_OK) return 0;
        done += chunk;
    }
    return 1;
}
static int ensure_scratch(void) {
    if (scratch_ready) return 1;
    if (!alloc_map_range_self(SCRATCH_APP_IMAGE_VA, MAX_APP_IMAGE_BYTES / PAGE_BYTES)) return 0;
    if (!alloc_map_range_self(SCRATCH_LD_IMAGE_VA, MAX_LD_IMAGE_BYTES / PAGE_BYTES)) return 0;
    if (!alloc_map_range_self(SCRATCH_EXEC_LOADER_IMAGE_VA, MAX_SERVICE_IMAGE_BYTES / PAGE_BYTES)) return 0;
    if (!alloc_map_range_self(SCRATCH_LINUX_ABI_IMAGE_VA, MAX_SERVICE_IMAGE_BYTES / PAGE_BYTES)) return 0;
    if (!alloc_map_range_self(SCRATCH_EXEC_CONFIG_VA, 1)) return 0;
    if (!alloc_map_range_self(SCRATCH_EXEC_TABLE_VA, 1)) return 0;
    if (!alloc_map_range_self(SCRATCH_LINUX_ABI_CONFIG_VA, 1)) return 0;
    if (!alloc_map_range_self(SCRATCH_LINUX_ABI_TABLE_VA, 1)) return 0;
    if (!alloc_map_range_self(SCRATCH_REGISTRY_COPY_VA, 1)) return 0;
    scratch_ready = 1;
    return 1;
}

static u64 install_vm_object_from_buffer(u64 buffer_va, u64 file_bytes) {
    const u64 token = syscall3(SYSCALL_INSTALL_VM_OBJECT, buffer_va, file_bytes, VM_RIGHT_READ_MAP_GRANT);
    return is_vm_object_token(token) ? token : 0;
}
static u64 install_exec_image_from_vm(u64 vm_token) {
    const u64 token = syscall2(SYSCALL_INSTALL_EXEC_IMAGE, vm_token, EXEC_RIGHT_EXEC_GRANT);
    return is_exec_image_token(token) ? token : 0;
}
static int init_assets(void);
static u64 load_exec_image(const char *path, u16 path_len, u64 buffer_va, u64 buffer_cap, u64 *vm_token_out, u64 *bytes_out) {
    u64 bytes = 0;
    if (!vfs_read_file_to_buffer(path, path_len, buffer_va, buffer_cap, &bytes)) return 0;
    const u64 vm_token = install_vm_object_from_buffer(buffer_va, bytes);
    if (vm_token == 0) return 0;
    const u64 exec_token = install_exec_image_from_vm(vm_token);
    if (exec_token == 0) return 0;
    if (vm_token_out != 0) *vm_token_out = vm_token;
    if (bytes_out != 0) *bytes_out = bytes;
    return exec_token;
}

static int append_arg(struct exec_loader_config *cfg, u16 *cursor, const char *data, u16 len, u16 *offset_out, u16 *bytes_out) {
    if (len == 0 || (u64)*cursor + len > EXECVE_MAX_ARG_DATA_BYTES) return 0;
    const u16 off = *cursor;
    for (u16 i = 0; i < len; i++) cfg->arg_data[off + i] = (u8)data[i];
    *cursor = (u16)(off + len);
    cfg->arg_data_bytes = *cursor;
    *offset_out = off;
    *bytes_out = len;
    return 1;
}

static int configure_exec_args(struct exec_loader_config *cfg, const struct exec_service_request *request) {
    const char *arg_data = (const char *)request->arg_data;
    u16 cursor = 0;
    if (!append_arg(cfg, &cursor, arg_data, request->path_bytes, &cfg->execfn_offset, &cfg->execfn_bytes)) return 0;
    cfg->argv_count = request->argv_count;
    cfg->envp_count = request->envp_count;
    for (u16 i = 0; i < request->argv_count; i++) {
        const u16 off = request->argv_offsets[i], len = request->argv_bytes[i];
        if ((u64)off + len > request->arg_data_bytes) return 0;
        if (!append_arg(cfg, &cursor, arg_data + off, len, &cfg->argv_offsets[i], &cfg->argv_bytes[i])) return 0;
    }
    for (u16 i = 0; i < request->envp_count; i++) {
        const u16 off = request->envp_offsets[i], len = request->envp_bytes[i];
        if ((u64)off + len > request->arg_data_bytes) return 0;
        if (!append_arg(cfg, &cursor, arg_data + off, len, &cfg->envp_offsets[i], &cfg->envp_bytes[i])) return 0;
    }
    return 1;
}

static u64 spawn_linux_abi_server(const struct exec_service_request *request) {
    volatile u8 *registry_src = (volatile u8 *)PROCESS_SERVICE_REGISTRY_SHADOW_VA;
    volatile u8 *registry_dst = (volatile u8 *)SCRATCH_REGISTRY_COPY_VA;
    for (u64 i = 0; i < PAGE_BYTES; i++) registry_dst[i] = registry_src[i];

    struct linux_abi_bootstrap_config *cfg = (struct linux_abi_bootstrap_config *)SCRATCH_LINUX_ABI_CONFIG_VA;
    clear_page(SCRATCH_LINUX_ABI_CONFIG_VA);
    cfg->magic = LINUX_ABI_BOOTSTRAP_MAGIC;
    cfg->version = LINUX_ABI_BOOTSTRAP_VERSION;
    cfg->standard_interpreter_file_bytes = g_ld_bytes;
    cfg->abi_trap_request_page_va = LINUX_ABI_TRAP_REQUEST_PAGE_VA;
    cfg->exec_path_bytes = request->path_bytes;
    for (u16 i = 0; i < request->path_bytes && i < sizeof(cfg->exec_path) - 1; i++) {
        cfg->exec_path[i] = (char)request->arg_data[i];
    }
    cfg->exec_path[request->path_bytes < sizeof(cfg->exec_path) ? request->path_bytes : sizeof(cfg->exec_path) - 1] = 0;

    struct bootstrap_descriptor_table *table = (struct bootstrap_descriptor_table *)SCRATCH_LINUX_ABI_TABLE_VA;
    clear_page(SCRATCH_LINUX_ABI_TABLE_VA);
    table->page_count = 2;
    table->cap_count = 2;
    table->page_descriptors[0].source_va = SCRATCH_LINUX_ABI_CONFIG_VA;
    table->page_descriptors[0].target_va = PROCESS_STANDARD_CONFIG_TARGET_VA;
    table->page_descriptors[0].flags = SPAWN_FLAG_BOOTSTRAP_PAGE_WRITABLE;
    table->page_descriptors[1].source_va = SCRATCH_REGISTRY_COPY_VA;
    table->page_descriptors[1].target_va = PROCESS_SERVICE_REGISTRY_SHADOW_VA;
    table->cap_descriptors[0].source_token = g_loader_vm_token;
    table->cap_descriptors[0].target_token_va = PROCESS_STANDARD_CONFIG_TARGET_VA + OFFSETOF(struct linux_abi_bootstrap_config, exec_loader_vm_token);
    table->cap_descriptors[0].rights_bits = VM_RIGHT_READ_MAP;
    table->cap_descriptors[0].kind = BOOTSTRAP_CAP_KIND_VM_OBJECT;
    table->cap_descriptors[1].source_token = g_ld_vm_token;
    table->cap_descriptors[1].target_token_va = PROCESS_STANDARD_CONFIG_TARGET_VA + OFFSETOF(struct linux_abi_bootstrap_config, standard_interpreter_vm_token);
    table->cap_descriptors[1].rights_bits = VM_RIGHT_READ_MAP_GRANT;
    table->cap_descriptors[1].kind = BOOTSTRAP_CAP_KIND_VM_OBJECT;

    const u64 spawned = syscall4(SYSCALL_SPAWN_EXEC, g_linux_abi_exec_token, SCRATCH_LINUX_ABI_TABLE_VA, 0, SPAWN_FLAG_BOOTSTRAP_EXTENDED_DESCRIPTOR_TABLE | SPAWN_FLAG_CHILD_BOOTSTRAP_OWNER);
    const u64 slot = decode_spawned_process_slot(spawned);
    if (slot == 0) return 0;
    for (u64 i = 0; i < 100000000; i++) {
        if (cfg->status == LINUX_ABI_BOOTSTRAP_READY) return slot;
        (void)syscall2(SYSCALL_WAIT_EVENT, 0, 1);
    }
    return 0;
}

static u64 spawn_exec_loader(const struct exec_service_request *request, u64 app_vm_token, u64 app_bytes, u64 abi_server_slot) {
    struct exec_loader_config *cfg = (struct exec_loader_config *)SCRATCH_EXEC_CONFIG_VA;
    clear_page(SCRATCH_EXEC_CONFIG_VA);
    cfg->magic = EXEC_LOADER_BOOTSTRAP_MAGIC;
    cfg->version = EXEC_LOADER_BOOTSTRAP_VERSION;
    cfg->executable_file_bytes = app_bytes;
    cfg->interpreter_file_bytes = g_ld_bytes;
    cfg->fs_endpoint_id = g_vfs.endpoint_id;
    cfg->fs_compat_process_slot = g_vfs.process_slot;
    cfg->abi_trap_endpoint_id = LINUX_ABI_ENDPOINT_ID;
    cfg->abi_trap_endpoint_process_slot = abi_server_slot;
    cfg->abi_trap_flavor = 1;
    cfg->abi_trap_request_page_va = LINUX_ABI_TRAP_REQUEST_PAGE_VA;
    if (!configure_exec_args(cfg, request)) return 0;

    struct bootstrap_descriptor_table *table = (struct bootstrap_descriptor_table *)SCRATCH_EXEC_TABLE_VA;
    clear_page(SCRATCH_EXEC_TABLE_VA);
    table->page_count = 1;
    table->cap_count = 2;
    table->page_descriptors[0].source_va = SCRATCH_EXEC_CONFIG_VA;
    table->page_descriptors[0].target_va = EXEC_LOADER_CONFIG_TARGET_VA;
    table->cap_descriptors[0].source_token = app_vm_token;
    table->cap_descriptors[0].target_token_va = EXEC_LOADER_CONFIG_TARGET_VA + OFFSETOF(struct exec_loader_config, executable_vm_token);
    table->cap_descriptors[0].rights_bits = VM_RIGHT_READ_MAP;
    table->cap_descriptors[0].kind = BOOTSTRAP_CAP_KIND_VM_OBJECT;
    table->cap_descriptors[1].source_token = g_ld_vm_token;
    table->cap_descriptors[1].target_token_va = EXEC_LOADER_CONFIG_TARGET_VA + OFFSETOF(struct exec_loader_config, interpreter_vm_token);
    table->cap_descriptors[1].rights_bits = VM_RIGHT_READ_MAP;
    table->cap_descriptors[1].kind = BOOTSTRAP_CAP_KIND_VM_OBJECT;
    return decode_spawned_process_slot(syscall4(SYSCALL_SPAWN_EXEC, g_loader_exec_token, SCRATCH_EXEC_TABLE_VA, 0, SPAWN_FLAG_BOOTSTRAP_EXTENDED_DESCRIPTOR_TABLE | SPAWN_FLAG_CHILD_BOOTSTRAP_OWNER));
}

static void write_response(u64 response_paddr, u32 status, u64 abi_slot, u64 loader_slot) {
    const u64 response_va = response_map_next;
    response_map_next += PAGE_BYTES;
    if (!map_page(response_va, response_paddr, 1)) return;
    struct exec_service_response *response = (struct exec_service_response *)response_va;
    response->magic = EXEC_SERVICE_ABI_MAGIC;
    response->version = EXEC_SERVICE_ABI_VERSION;
    response->op = EXEC_SERVICE_OP_SPAWN_LINUX;
    response->status = status;
    response->linux_abi_process_slot = abi_slot;
    response->exec_loader_process_slot = loader_slot;
}

static void handle_request_paddr(u64 request_paddr) {
    const u64 request_va = request_map_next;
    request_map_next += PAGE_BYTES;
    if (!map_page(request_va, request_paddr, 0)) {
        user_log("ExecService: request map failed\n");
        return;
    }
    const struct exec_service_request *request = (const struct exec_service_request *)request_va;
    if (request->magic != EXEC_SERVICE_ABI_MAGIC || request->version != EXEC_SERVICE_ABI_VERSION ||
        request->op != EXEC_SERVICE_OP_SPAWN_LINUX || request->path_bytes == 0 ||
        request->path_bytes > EXEC_SERVICE_MAX_PATH_BYTES || request->argv_count == 0 ||
        request->argv_count > EXEC_SERVICE_MAX_ARGV || request->envp_count > EXEC_SERVICE_MAX_ENVP ||
        request->arg_data_bytes > EXEC_SERVICE_MAX_ARG_DATA_BYTES || request->response_paddr < 0x1000) {
        user_log("ExecService: request invalid\n");
        if (request->response_paddr >= 0x1000) write_response(request->response_paddr, EXEC_SERVICE_STATUS_INVALID, 0, 0);
        return;
    }

    user_log("ExecService: spawn request\n");
    if (g_loader_exec_token == 0 && !init_assets()) {
        write_response(request->response_paddr, EXEC_SERVICE_STATUS_IO, 0, 0);
        return;
    }
    if (!connect_vfs_from_registry()) {
        write_response(request->response_paddr, EXEC_SERVICE_STATUS_IO, 0, 0);
        return;
    }

    u64 app_bytes = 0;
    if (!vfs_read_file_to_buffer((const char *)request->arg_data, request->path_bytes, SCRATCH_APP_IMAGE_VA, MAX_APP_IMAGE_BYTES, &app_bytes)) {
        write_response(request->response_paddr, EXEC_SERVICE_STATUS_NOT_FOUND, 0, 0);
        return;
    }
    const u64 app_vm_token = install_vm_object_from_buffer(SCRATCH_APP_IMAGE_VA, app_bytes);
    if (app_vm_token == 0) {
        write_response(request->response_paddr, EXEC_SERVICE_STATUS_IO, 0, 0);
        return;
    }
    const u64 abi_slot = spawn_linux_abi_server(request);
    if (abi_slot == 0) {
        write_response(request->response_paddr, EXEC_SERVICE_STATUS_SPAWN_FAILED, 0, 0);
        return;
    }
    const u64 loader_slot = spawn_exec_loader(request, app_vm_token, app_bytes, abi_slot);
    if (loader_slot == 0) {
        write_response(request->response_paddr, EXEC_SERVICE_STATUS_SPAWN_FAILED, abi_slot, 0);
        return;
    }
    user_log("ExecService: spawn ok\n");
    write_response(request->response_paddr, EXEC_SERVICE_STATUS_OK, abi_slot, loader_slot);
}

static int init_assets(void) {
    if (!ensure_scratch()) return 0;
    if (!connect_vfs_from_registry()) return 0;
    user_log("ExecService: load exec_loader begin\n");
    u64 bytes = 0;
    g_loader_exec_token = load_exec_image("/srv/exec_loader.elf", 20, SCRATCH_EXEC_LOADER_IMAGE_VA, MAX_SERVICE_IMAGE_BYTES, &g_loader_vm_token, &bytes);
    if (g_loader_exec_token == 0) return 0;
    user_log("ExecService: load exec_loader ok\n");
    user_log("ExecService: load linux_abi_server begin\n");
    g_linux_abi_exec_token = load_exec_image("/srv/linux_abi_server.elf", 25, SCRATCH_LINUX_ABI_IMAGE_VA, MAX_SERVICE_IMAGE_BYTES, 0, &bytes);
    if (g_linux_abi_exec_token == 0) return 0;
    user_log("ExecService: load linux_abi_server ok\n");
    user_log("ExecService: load ld begin\n");
    g_vfs_read_profile_label = "ld";
    if (!vfs_read_file_to_buffer("/lib/ld-musl-x86_64.so.1", 24, SCRATCH_LD_IMAGE_VA, MAX_LD_IMAGE_BYTES, &g_ld_bytes)) {
        g_vfs_read_profile_label = 0;
        return 0;
    }
    g_vfs_read_profile_label = 0;
    if (EXEC_SERVICE_PROFILE) user_log("ExecService.prof: ld vm install begin\n");
    g_ld_vm_token = install_vm_object_from_buffer(SCRATCH_LD_IMAGE_VA, g_ld_bytes);
    if (EXEC_SERVICE_PROFILE) user_log("ExecService.prof: ld vm install done\n");
    if (g_ld_vm_token != 0) user_log("ExecService: load ld ok\n");
    return g_ld_vm_token != 0;
}

void exec_service_main(void) {
    user_log("ExecService: started\n");
    if (!init_assets()) {
        user_log("ExecService: init failed\n");
        for (;;) __asm__ volatile("pause");
    }
    const u64 self_slot = syscall0(SYSCALL_GET_PROCESS_SLOT);
    if (syscall3(SYSCALL_INSTALL_ENDPOINT, 0, EXEC_SERVICE_ENDPOINT_ID, self_slot) != SYSCALL_OK ||
        syscall2(SYSCALL_PUBLISH_SERVICE_ENDPOINT, EXEC_SERVICE_ENDPOINT_ID, self_slot) != SYSCALL_OK) {
        user_log("ExecService: publish failed\n");
        for (;;) __asm__ volatile("pause");
    }
    user_log("ExecService: endpoint ready\n");
    for (;;) {
        const u64 received = syscall2(SYSCALL_WAIT_EVENT, 1, 1);
        if (received < 0x1000) continue;
        const u64 request_paddr = syscall1(SYSCALL_ACCEPT_CAP_TRANSFER, received);
        if (request_paddr < 0x1000) continue;
        handle_request_paddr(request_paddr);
    }
}
