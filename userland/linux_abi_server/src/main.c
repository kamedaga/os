typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef long long i64;
typedef int i32;

enum {
    SYSCALL_ALLOC_PAGE = 0x1,
    SYSCALL_MAP_PAGE = 0x2,
    SYSCALL_LOG = 0x9,
    SYSCALL_ALLOC_MAP_PAGES = 0xC,
    SYSCALL_WAIT_EVENT = 0x17,
    SYSCALL_GRANT_CAP_ON_ENDPOINT = 0x24,
    SYSCALL_INSTALL_ENDPOINT = 0x26,
    SYSCALL_SHARE_CAP = 0x2B,
    SYSCALL_SIGNAL_ENDPOINT = 0x2C,
    SYSCALL_GET_PROCESS_SLOT = 0x2E,
    SYSCALL_IPC_CALL_REPLY_RECV = 0x40,
    SYSCALL_MAP_ABI_TRAP_REPLY_TARGET_PAGES = 0x4C,
    SYSCALL_COPY_FROM_ABI_TRAP_REPLY_TARGET = 0x4D,
    SYSCALL_COPY_TO_ABI_TRAP_REPLY_TARGET = 0x4E,
    SYSCALL_OK = 0,
    SYSCALL_ERR_ENDPOINT = 9,
    IPC_CALL_FLAG_SIGNAL_ONLY = 0x2,

    PAGE_BYTES = 4096,
    PAGE_RIGHT_CPU_READ = 0x1,
    PAGE_RIGHT_CPU_WRITE = 0x2,
    TRAP_MAGIC = 0x3149424150415254ULL,
    TRAP_VERSION = 1,

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
    FS_STAT_RECORD_BYTES = 56,
    FS_DIRENT_RECORD_BYTES = 24,
    FS_OP_CONNECT = 1,
    FS_OP_LOOKUP = 16,
    FS_OP_OPEN = 17,
    FS_OP_READ = 18,
    FS_OP_READDIR = 19,
    FS_OP_STAT = 20,
    FS_OP_CLOSE = 21,
    FS_STATUS_OK = 0,
    FS_STATUS_END_OF_DIR = 10,
    FS_OBJECT_NONE = 0,
    FS_OBJECT_MOUNT = 1,
    FS_OBJECT_DIRECTORY = 2,
    FS_OBJECT_FILE = 3,
    FS_OBJECT_OPEN_FILE = 4,
    FS_DIR_MODE = 0x4000,
    FS_FILE_MODE = 0x8000,

    LINUX_SYS_READ = 0,
    LINUX_SYS_WRITE = 1,
    LINUX_SYS_OPEN = 2,
    LINUX_SYS_CLOSE = 3,
    LINUX_SYS_STAT = 4,
    LINUX_SYS_FSTAT = 5,
    LINUX_SYS_LSTAT = 6,
    LINUX_SYS_LSEEK = 8,
    LINUX_SYS_MMAP = 9,
    LINUX_SYS_MUNMAP = 11,
    LINUX_SYS_BRK = 12,
    LINUX_SYS_RT_SIGACTION = 13,
    LINUX_SYS_RT_SIGPROCMASK = 14,
    LINUX_SYS_IOCTL = 16,
    LINUX_SYS_ACCESS = 21,
    LINUX_SYS_GETPID = 39,
    LINUX_SYS_EXIT = 60,
    LINUX_SYS_ARCH_PRCTL = 158,
    LINUX_SYS_GETTID = 186,
    LINUX_SYS_FUTEX = 202,
    LINUX_SYS_GETDENTS64 = 217,
    LINUX_SYS_SET_TID_ADDRESS = 218,
    LINUX_SYS_EXIT_GROUP = 231,
    LINUX_SYS_OPENAT = 257,
    LINUX_SYS_NEWFSTATAT = 262,
    LINUX_SYS_SET_ROBUST_LIST = 273,
    LINUX_SYS_PRLIMIT64 = 302,
    LINUX_SYS_GETRANDOM = 318,
    LINUX_SYS_RSEQ = 334,

    AT_FDCWD_U64 = 0xffffffffffffff9cULL,
    AT_EMPTY_PATH = 0x1000,
    O_ACCMODE = 00000003,
    O_RDONLY = 0,
    O_DIRECTORY = 00200000,
    SEEK_SET = 0,
    SEEK_CUR = 1,
    SEEK_END = 2,
    DT_UNKNOWN = 0,
    DT_DIR = 4,
    DT_REG = 8,

    TRAP_RESPONSE_FLAG_EXIT = 1,
};

struct ipc_message { u64 status; u64 request_va; u64 reserved0; u64 reserved1; u64 reserved2; };
struct trap_request {
    u64 magic; unsigned version; unsigned kind; unsigned flavor; unsigned reserved0;
    u64 caller_principal; u64 thread_id; u64 rip; u64 rsp; u64 fault_addr; u64 error_code; u64 nr; u64 args[6];
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
struct fs_stat_record { u8 object_kind; u8 reserved0[7]; u64 size_bytes; u32 mode_bits; u32 reserved1; u64 mtime_unix_sec; u64 reserved2[2]; };
struct fs_dirent_record { u64 next_cursor; u8 object_kind; u8 reserved0[7]; u16 name_bytes; u16 reserved1; u32 reserved2; };
struct linux_stat {
    u64 st_dev; u64 st_ino; u64 st_nlink; u32 st_mode; u32 st_uid; u32 st_gid; u32 __pad0;
    u64 st_rdev; i64 st_size; i64 st_blksize; i64 st_blocks;
    i64 st_atime; u64 st_atime_nsec; i64 st_mtime; u64 st_mtime_nsec; i64 st_ctime; u64 st_ctime_nsec;
    i64 __unused[3];
};

enum fd_kind { FD_UNUSED = 0, FD_STDIO = 1, FD_FILE = 2, FD_DIR = 3 };
struct fd_entry { enum fd_kind kind; u64 token; u64 offset; u64 size; u32 mode_bits; u8 object_kind; };
struct vfs_client { int active; u64 endpoint_id; u64 process_slot; u64 request_paddr; u64 response_paddr; u64 root_token; u64 next_seq; u64 session_nonce; };

static u64 mmap_next_va = 0x31000000ULL;
static u64 brk_next_va = 0x38000000ULL;
static u64 trap_request_page_va = 0;
static struct vfs_client g_vfs;
static struct fd_entry g_fds[32];

static u64 cstr_len(const char *s) { u64 n = 0; while (s[n] != 0) n++; return n; }
static void user_log_len(const char *message, u64 len) { u64 ret; __asm__ volatile("int $0x80" : "=a"(ret) : "a"((u64)SYSCALL_LOG), "D"((u64)message), "S"(len) : "rcx", "rdx", "r8", "r9", "r10", "r11", "memory"); (void)ret; }
static void user_log(const char *message) { user_log_len(message, cstr_len(message)); }
static void user_log_hex_value(u64 value) { static const char hex[] = "0123456789ABCDEF"; char buf[32]; u64 pos = 0; buf[pos++] = '0'; buf[pos++] = 'x'; int started = 0; for (int shift = 60; shift >= 0; shift -= 4) { unsigned nibble = (unsigned)((value >> (u64)shift) & 0xFULL); if (nibble != 0 || started || shift == 0) { buf[pos++] = hex[nibble]; started = 1; } } buf[pos++] = '\n'; user_log_len(buf, pos); }
static void log_syscall_nr(u64 nr) { user_log("LinuxAbiServer: syscall\n"); user_log_hex_value(nr); }
static void clear_page(u64 va) { volatile u64 *p = (volatile u64 *)va; for (u64 i = 0; i < 512; i++) p[i] = 0; }
static u64 min_u64(u64 a, u64 b) { return a < b ? a : b; }
static u64 align_up(u64 value, u64 align) { return (value + align - 1) & ~(align - 1); }

static u64 syscall0(u64 nr) { u64 ret; __asm__ volatile("int $0x80" : "=a"(ret) : "a"(nr) : "rcx", "rdx", "r8", "r9", "r10", "r11", "memory"); return ret; }
static u64 syscall2(u64 nr, u64 a0, u64 a1) { u64 ret; __asm__ volatile("int $0x80" : "=a"(ret) : "a"(nr), "D"(a0), "S"(a1) : "rcx", "rdx", "r8", "r9", "r10", "r11", "memory"); return ret; }
static u64 syscall3(u64 nr, u64 a0, u64 a1, u64 a2) { u64 ret; __asm__ volatile("int $0x80" : "=a"(ret) : "a"(nr), "D"(a0), "S"(a1), "d"(a2) : "rcx", "r8", "r9", "r10", "r11", "memory"); return ret; }
static u64 syscall4(u64 nr, u64 a0, u64 a1, u64 a2, u64 a3) { u64 ret; __asm__ volatile("int $0x80" : "=a"(ret) : "a"(nr), "D"(a0), "S"(a1), "d"(a2), "c"(a3) : "r8", "r9", "r10", "r11", "memory"); return ret; }

static struct ipc_message wait_ipc(void) {
    register u64 rax __asm__("rax") = SYSCALL_WAIT_EVENT; register u64 rdi __asm__("rdi") = 0; register u64 rsi __asm__("rsi") = 0; register u64 rdx __asm__("rdx"); register u64 r8 __asm__("r8");
    __asm__ volatile("int $0x80" : "+r"(rax), "+r"(rdi), "+r"(rsi), "=r"(rdx), "=r"(r8) : : "rcx", "r9", "r10", "r11", "memory");
    struct ipc_message msg = { rax, rdi, rsi, rdx, r8 }; return msg;
}
static struct ipc_message reply(u64 result, u64 flags) {
    register u64 rax __asm__("rax") = SYSCALL_IPC_CALL_REPLY_RECV; register u64 rdi __asm__("rdi") = result; register u64 rsi __asm__("rsi") = 0; register u64 rdx __asm__("rdx") = IPC_CALL_FLAG_SIGNAL_ONLY; register u64 r8 __asm__("r8") = flags; register u64 r9 __asm__("r9") = 0; register u64 r10 __asm__("r10") = 0;
    __asm__ volatile("int $0x80" : "+r"(rax), "+r"(rdi), "+r"(rsi), "+r"(rdx), "+r"(r8), "+r"(r9), "+r"(r10) : : "rcx", "r11", "memory");
    struct ipc_message msg = { rax, rdi, rsi, rdx, r8 }; return msg;
}

static u64 errno_perm(void) { return (u64)(i64)-1; }
static u64 errno_noent(void) { return (u64)(i64)-2; }
static u64 errno_io(void) { return (u64)(i64)-5; }
static u64 errno_badf(void) { return (u64)(i64)-9; }
static u64 errno_again(void) { return (u64)(i64)-11; }
static u64 errno_nomem(void) { return (u64)(i64)-12; }
static u64 errno_acces(void) { return (u64)(i64)-13; }
static u64 errno_fault(void) { return (u64)(i64)-14; }
static u64 errno_busy(void) { return (u64)(i64)-16; }
static u64 errno_notdir(void) { return (u64)(i64)-20; }
static u64 errno_inval(void) { return (u64)(i64)-22; }
static u64 errno_nosys(void) { return (u64)(i64)-38; }
static u64 errno_nametoolong(void) { return (u64)(i64)-36; }

static u64 map_reply_target_pages(u64 target_va, u64 page_count, u64 prot_bits) { return syscall3(SYSCALL_MAP_ABI_TRAP_REPLY_TARGET_PAGES, target_va, page_count, prot_bits); }
static u64 copy_from_target(u64 target_va, void *dst, u64 len) { return syscall3(SYSCALL_COPY_FROM_ABI_TRAP_REPLY_TARGET, (u64)dst, target_va, len); }
static u64 copy_to_target(u64 target_va, const void *src, u64 len) { return syscall3(SYSCALL_COPY_TO_ABI_TRAP_REPLY_TARGET, target_va, (u64)src, len); }
static u64 alloc_map_pages(u64 target_va, u64 page_count, u64 flags) { return syscall4(SYSCALL_ALLOC_MAP_PAGES, target_va, page_count, flags, 0); }

static int copy_cstr_from_target(u64 target_va, char *dst, u64 cap) {
    if (target_va == 0 || cap == 0) return 0;
    for (u64 i = 0; i + 1 < cap; i++) {
        if (copy_from_target(target_va + i, &dst[i], 1) != 1) return 0;
        if (dst[i] == 0) return 1;
    }
    dst[cap - 1] = 0;
    return 0;
}

static int find_service(u64 kind, struct service_entry *out) {
    volatile struct service_registry_page *page = (volatile struct service_registry_page *)SERVICE_REGISTRY_SHADOW_VA;
    if (page->magic != SERVICE_REGISTRY_MAGIC || page->version != SERVICE_REGISTRY_VERSION) return 0;
    for (u64 i = 0; i < page->entry_count && i < SERVICE_REGISTRY_MAX_ENTRIES; i++) {
        if (page->entries[i].kind != kind) continue;
        out->kind = page->entries[i].kind; out->process_slot = page->entries[i].process_slot; out->endpoint_id = page->entries[i].endpoint_id; out->flags = page->entries[i].flags;
        return out->endpoint_id != 0 && out->process_slot != 0;
    }
    return 0;
}

static u64 make_nonce(u64 request_paddr, u64 response_paddr, u64 endpoint_id, u64 process_slot) {
    return request_paddr ^ ((response_paddr << 17) | (response_paddr >> 47)) ^ (endpoint_id << 1) ^ (process_slot << 33) ^ 0x4653434f4e4e4543ULL;
}
static int install_vfs_endpoint(void) { return syscall3(SYSCALL_INSTALL_ENDPOINT, 0, g_vfs.endpoint_id, g_vfs.process_slot) == SYSCALL_OK; }
static int grant_vfs_response_page(void) { u64 ret = syscall3(SYSCALL_GRANT_CAP_ON_ENDPOINT, g_vfs.response_paddr, g_vfs.endpoint_id, PAGE_RIGHT_CPU_READ | PAGE_RIGHT_CPU_WRITE); if (ret == SYSCALL_OK) return 1; if (ret == SYSCALL_ERR_ENDPOINT && install_vfs_endpoint()) ret = syscall3(SYSCALL_GRANT_CAP_ON_ENDPOINT, g_vfs.response_paddr, g_vfs.endpoint_id, PAGE_RIGHT_CPU_READ | PAGE_RIGHT_CPU_WRITE); return ret == SYSCALL_OK; }
static int share_vfs_request_page(void) { u64 ret = syscall2(SYSCALL_SHARE_CAP, g_vfs.request_paddr, g_vfs.endpoint_id); if (ret == SYSCALL_OK) return 1; if (ret == SYSCALL_ERR_ENDPOINT && install_vfs_endpoint()) ret = syscall2(SYSCALL_SHARE_CAP, g_vfs.request_paddr, g_vfs.endpoint_id); return ret == SYSCALL_OK; }
static int signal_vfs(void) { u64 ret = syscall2(SYSCALL_SIGNAL_ENDPOINT, g_vfs.endpoint_id, 0); if (ret == SYSCALL_OK) return 1; if (ret == SYSCALL_ERR_ENDPOINT && install_vfs_endpoint()) ret = syscall2(SYSCALL_SIGNAL_ENDPOINT, g_vfs.endpoint_id, 0); return ret == SYSCALL_OK; }
static int wait_vfs_response(u64 expected_seq, u16 expected_op) { volatile struct fs_response_header *response = (volatile struct fs_response_header *)VFS_RESPONSE_VA; for (u64 i = 0; i < 8192; i++) { if (response->response_seq == expected_seq) return response->magic == FS_RESPONSE_MAGIC && response->version == FS_PROTOCOL_VERSION && response->op == expected_op; (void)syscall2(SYSCALL_WAIT_EVENT, 0, 1); } return 0; }

static int connect_vfs_from_registry(void) {
    struct service_entry entry;
    if (!find_service(SERVICE_KIND_VFS, &entry)) return 0;
    g_vfs.endpoint_id = entry.endpoint_id; g_vfs.process_slot = entry.process_slot;
    g_vfs.request_paddr = syscall0(SYSCALL_ALLOC_PAGE); g_vfs.response_paddr = syscall0(SYSCALL_ALLOC_PAGE);
    if (g_vfs.request_paddr < 0x1000 || g_vfs.response_paddr < 0x1000) return 0;
    if (syscall3(SYSCALL_MAP_PAGE, VFS_REQUEST_VA, g_vfs.request_paddr, 1) != SYSCALL_OK) return 0;
    if (syscall3(SYSCALL_MAP_PAGE, VFS_RESPONSE_VA, g_vfs.response_paddr, 1) != SYSCALL_OK) return 0;
    if (!grant_vfs_response_page()) return 0;
    clear_page(VFS_REQUEST_VA); clear_page(VFS_RESPONSE_VA);
    const u64 self_slot = syscall0(SYSCALL_GET_PROCESS_SLOT);
    g_vfs.session_nonce = make_nonce(g_vfs.request_paddr, g_vfs.response_paddr, g_vfs.endpoint_id, self_slot);
    volatile struct fs_request_header *request = (volatile struct fs_request_header *)VFS_REQUEST_VA;
    request->magic = FS_REQUEST_MAGIC; request->version = FS_PROTOCOL_VERSION; request->op = FS_OP_CONNECT; request->request_seq = 1; request->arg0 = g_vfs.response_paddr; request->arg1 = self_slot; request->session_nonce = g_vfs.session_nonce;
    if (!share_vfs_request_page()) return 0;
    if (!signal_vfs()) return 0;
    if (!wait_vfs_response(1, FS_OP_CONNECT)) return 0;
    volatile struct fs_response_header *response = (volatile struct fs_response_header *)VFS_RESPONSE_VA;
    if (response->status != FS_STATUS_OK || response->result_token == 0) return 0;
    g_vfs.root_token = response->result_token; g_vfs.next_seq = 2; g_vfs.active = 1;
    user_log("LinuxAbiServer: vfs connect ok\n");
    return 1;
}

static int vfs_request(u16 op, u64 token, u64 offset, u32 length, const char *path) {
    if (!g_vfs.active) return 0;
    clear_page(VFS_REQUEST_VA); clear_page(VFS_RESPONSE_VA);
    volatile struct fs_request_header *request = (volatile struct fs_request_header *)VFS_REQUEST_VA;
    const u64 seq = g_vfs.next_seq++;
    request->magic = FS_REQUEST_MAGIC; request->version = FS_PROTOCOL_VERSION; request->op = op; request->request_seq = seq; request->object_token = token; request->offset = offset; request->length = length; request->session_nonce = g_vfs.session_nonce;
    if (path != 0) {
        u64 len = cstr_len(path);
        if (len > FS_MAX_PATH_BYTES) return 0;
        request->path_bytes = (u16)len;
        volatile u8 *payload = (volatile u8 *)(VFS_REQUEST_VA + FS_REQUEST_HEADER_BYTES);
        for (u64 i = 0; i < len; i++) payload[i] = (u8)path[i];
    }
    if (!signal_vfs()) return 0;
    return wait_vfs_response(seq, op);
}

static void init_fds(void) { for (u64 i = 0; i < 32; i++) g_fds[i].kind = FD_UNUSED; g_fds[0].kind = FD_STDIO; g_fds[1].kind = FD_STDIO; g_fds[2].kind = FD_STDIO; }
static int alloc_fd(void) { for (int i = 3; i < 32; i++) if (g_fds[i].kind == FD_UNUSED) return i; return -1; }
static int fd_valid(u64 fd) { return fd < 32 && g_fds[fd].kind != FD_UNUSED; }

static int vfs_lookup_stat(const char *path, u64 *token_out, struct fs_stat_record *stat_out, u64 *file_bytes_out, u8 *kind_out) {
    if (!vfs_request(FS_OP_LOOKUP, g_vfs.root_token, 0, 0, path)) return 0;
    volatile struct fs_response_header *response = (volatile struct fs_response_header *)VFS_RESPONSE_VA;
    if (response->status != FS_STATUS_OK || response->result_token == 0) return 0;
    const u64 token = response->result_token;
    if (!vfs_request(FS_OP_STAT, token, 0, 0, 0)) return 0;
    response = (volatile struct fs_response_header *)VFS_RESPONSE_VA;
    if (response->status != FS_STATUS_OK || response->inline_bytes < FS_STAT_RECORD_BYTES) return 0;
    volatile struct fs_stat_record *record = (volatile struct fs_stat_record *)(VFS_RESPONSE_VA + FS_RESPONSE_HEADER_BYTES);
    token_out[0] = token; stat_out->object_kind = record->object_kind; stat_out->size_bytes = record->size_bytes; stat_out->mode_bits = record->mode_bits; stat_out->mtime_unix_sec = record->mtime_unix_sec;
    *file_bytes_out = record->size_bytes; *kind_out = response->object_kind != FS_OBJECT_NONE ? response->object_kind : record->object_kind;
    return 1;
}

static u32 linux_mode_from_fs(u32 fs_mode, u8 kind) { u32 perm = (kind == FS_OBJECT_DIRECTORY || kind == FS_OBJECT_MOUNT) ? 0555 : 0444; u32 type = (kind == FS_OBJECT_DIRECTORY || kind == FS_OBJECT_MOUNT) ? 0040000 : 0100000; if ((fs_mode & FS_DIR_MODE) != 0) type = 0040000; if ((fs_mode & FS_FILE_MODE) != 0) type = 0100000; return type | perm; }
static void fill_linux_stat(struct linux_stat *st, const struct fs_stat_record *rec, u64 size, u8 kind) {
    u8 *p = (u8 *)st; for (u64 i = 0; i < sizeof(*st); i++) p[i] = 0;
    st->st_nlink = (kind == FS_OBJECT_DIRECTORY || kind == FS_OBJECT_MOUNT) ? 2 : 1; st->st_mode = linux_mode_from_fs(rec->mode_bits, kind); st->st_size = (i64)size; st->st_blksize = 4096; st->st_blocks = (i64)((size + 511) / 512); st->st_mtime = (i64)rec->mtime_unix_sec; st->st_atime = st->st_mtime; st->st_ctime = st->st_mtime;
}

static struct ipc_message handle_openat(const struct trap_request *req, int old_open) {
    char path[256];
    const u64 path_ptr = old_open ? req->args[0] : req->args[1];
    const u64 flags = old_open ? req->args[1] : req->args[2];
    if ((flags & O_ACCMODE) != O_RDONLY) return reply(errno_acces(), 0);
    if (!copy_cstr_from_target(path_ptr, path, sizeof(path))) return reply(errno_fault(), 0);
    if (cstr_len(path) > FS_MAX_PATH_BYTES) return reply(errno_nametoolong(), 0);
    struct fs_stat_record rec; u64 token = 0; u64 size = 0; u8 kind = FS_OBJECT_NONE;
    if (!vfs_lookup_stat(path, &token, &rec, &size, &kind)) return reply(errno_noent(), 0);
    const int fd = alloc_fd(); if (fd < 0) return reply(errno_busy(), 0);
    if (kind == FS_OBJECT_DIRECTORY || kind == FS_OBJECT_MOUNT) {
        g_fds[fd].kind = FD_DIR; g_fds[fd].token = token; g_fds[fd].offset = 0; g_fds[fd].size = 0; g_fds[fd].mode_bits = rec.mode_bits; g_fds[fd].object_kind = kind;
        return reply((u64)fd, 0);
    }
    if ((flags & O_DIRECTORY) != 0) return reply(errno_notdir(), 0);
    if (!vfs_request(FS_OP_OPEN, token, 0, 0, 0)) return reply(errno_io(), 0);
    volatile struct fs_response_header *response = (volatile struct fs_response_header *)VFS_RESPONSE_VA;
    if (response->status != FS_STATUS_OK || response->result_token == 0) return reply(errno_acces(), 0);
    g_fds[fd].kind = FD_FILE; g_fds[fd].token = response->result_token; g_fds[fd].offset = 0; g_fds[fd].size = response->file_bytes != 0 ? response->file_bytes : size; g_fds[fd].mode_bits = rec.mode_bits; g_fds[fd].object_kind = FS_OBJECT_FILE;
    return reply((u64)fd, 0);
}

static struct ipc_message handle_read(const struct trap_request *req) {
    const u64 fd = req->args[0]; const u64 dst = req->args[1]; const u64 len = req->args[2];
    if (!fd_valid(fd)) return reply(errno_badf(), 0); if (len == 0) return reply(0, 0); if (g_fds[fd].kind == FD_STDIO) return reply(0, 0); if (g_fds[fd].kind != FD_FILE) return reply(errno_badf(), 0);
    u64 copied = 0;
    while (copied < len) {
        u64 request_len = min_u64(len - copied, FS_RESPONSE_PAYLOAD_BYTES);
        if (!vfs_request(FS_OP_READ, g_fds[fd].token, g_fds[fd].offset, (u32)request_len, 0)) return copied != 0 ? reply(copied, 0) : reply(errno_io(), 0);
        volatile struct fs_response_header *response = (volatile struct fs_response_header *)VFS_RESPONSE_VA;
        if (response->status != FS_STATUS_OK) return copied != 0 ? reply(copied, 0) : reply(errno_io(), 0);
        if (response->inline_bytes == 0) break;
        if (copy_to_target(dst + copied, (const void *)(VFS_RESPONSE_VA + FS_RESPONSE_HEADER_BYTES), response->inline_bytes) != response->inline_bytes) return reply(errno_fault(), 0);
        copied += response->inline_bytes; g_fds[fd].offset += response->inline_bytes;
        if (response->inline_bytes < request_len) break;
    }
    return reply(copied, 0);
}

static struct ipc_message handle_write(const struct trap_request *req) {
    const u64 fd = req->args[0]; const u64 src = req->args[1]; const u64 len = req->args[2];
    if (fd == 1 || fd == 2) {
        char buf[129]; u64 done = 0;
        while (done < len) { u64 chunk = min_u64(len - done, 128); if (copy_from_target(src + done, buf, chunk) != chunk) break; user_log_len(buf, chunk); done += chunk; }
        return reply(len, 0);
    }
    return reply(errno_badf(), 0);
}

static struct ipc_message handle_fstat(const struct trap_request *req) {
    const u64 fd = req->args[0]; const u64 stat_va = req->args[1]; if (!fd_valid(fd)) return reply(errno_badf(), 0);
    struct fs_stat_record rec; rec.object_kind = g_fds[fd].object_kind; rec.size_bytes = g_fds[fd].size; rec.mode_bits = g_fds[fd].mode_bits; rec.mtime_unix_sec = 0;
    if (g_fds[fd].kind == FD_STDIO) { rec.object_kind = FS_OBJECT_FILE; rec.size_bytes = 0; rec.mode_bits = FS_FILE_MODE; }
    struct linux_stat st; fill_linux_stat(&st, &rec, rec.size_bytes, rec.object_kind);
    if (copy_to_target(stat_va, &st, sizeof(st)) != sizeof(st)) return reply(errno_fault(), 0);
    return reply(0, 0);
}

static struct ipc_message handle_newfstatat(const struct trap_request *req, int old_stat) {
    const u64 path_ptr = old_stat ? req->args[0] : req->args[1]; const u64 stat_va = old_stat ? req->args[1] : req->args[2]; const u64 flags = old_stat ? 0 : req->args[3];
    if (path_ptr == 0 || (flags & AT_EMPTY_PATH) != 0) { struct trap_request f = *req; f.args[0] = old_stat ? 0 : req->args[0]; f.args[1] = stat_va; return handle_fstat(&f); }
    char path[256]; if (!copy_cstr_from_target(path_ptr, path, sizeof(path))) return reply(errno_fault(), 0); if (cstr_len(path) > FS_MAX_PATH_BYTES) return reply(errno_nametoolong(), 0);
    struct fs_stat_record rec; u64 token = 0; u64 size = 0; u8 kind = FS_OBJECT_NONE; if (!vfs_lookup_stat(path, &token, &rec, &size, &kind)) return reply(errno_noent(), 0); (void)token;
    struct linux_stat st; fill_linux_stat(&st, &rec, size, kind); if (copy_to_target(stat_va, &st, sizeof(st)) != sizeof(st)) return reply(errno_fault(), 0); return reply(0, 0);
}

static struct ipc_message handle_getdents64(const struct trap_request *req) {
    const u64 fd = req->args[0]; const u64 dst = req->args[1]; const u64 len = req->args[2]; if (!fd_valid(fd) || g_fds[fd].kind != FD_DIR) return reply(errno_badf(), 0);
    u64 written = 0;
    while (written + 32 <= len) {
        if (!vfs_request(FS_OP_READDIR, g_fds[fd].token, g_fds[fd].offset, 0, 0)) return written != 0 ? reply(written, 0) : reply(errno_io(), 0);
        volatile struct fs_response_header *response = (volatile struct fs_response_header *)VFS_RESPONSE_VA;
        if (response->status == FS_STATUS_END_OF_DIR) break; if (response->status != FS_STATUS_OK || response->inline_bytes < FS_DIRENT_RECORD_BYTES) return written != 0 ? reply(written, 0) : reply(errno_io(), 0);
        volatile struct fs_dirent_record *record = (volatile struct fs_dirent_record *)(VFS_RESPONSE_VA + FS_RESPONSE_HEADER_BYTES); if (response->inline_bytes < FS_DIRENT_RECORD_BYTES + record->name_bytes) return reply(errno_io(), 0);
        u64 reclen = align_up(19 + record->name_bytes + 1, 8); if (written + reclen > len) break;
        u8 out[320]; for (u64 i = 0; i < sizeof(out); i++) out[i] = 0;
        *((u64 *)(out + 0)) = 1; *((i64 *)(out + 8)) = (i64)record->next_cursor; *((u16 *)(out + 16)) = (u16)reclen; out[18] = (record->object_kind == FS_OBJECT_DIRECTORY || record->object_kind == FS_OBJECT_MOUNT) ? DT_DIR : DT_REG;
        volatile u8 *name = (volatile u8 *)(VFS_RESPONSE_VA + FS_RESPONSE_HEADER_BYTES + FS_DIRENT_RECORD_BYTES); for (u64 i = 0; i < record->name_bytes && 19 + i < sizeof(out); i++) out[19 + i] = name[i];
        if (copy_to_target(dst + written, out, reclen) != reclen) return reply(errno_fault(), 0);
        written += reclen; g_fds[fd].offset = record->next_cursor;
        if (record->next_cursor == 0) break;
    }
    return reply(written, 0);
}

static struct ipc_message handle_lseek(const struct trap_request *req) {
    const u64 fd = req->args[0]; const i64 off = (i64)req->args[1]; const u64 whence = req->args[2]; if (!fd_valid(fd)) return reply(errno_badf(), 0);
    i64 base = 0; if (whence == SEEK_SET) base = 0; else if (whence == SEEK_CUR) base = (i64)g_fds[fd].offset; else if (whence == SEEK_END) base = (i64)g_fds[fd].size; else return reply(errno_inval(), 0);
    i64 next = base + off; if (next < 0) return reply(errno_inval(), 0); g_fds[fd].offset = (u64)next; return reply((u64)next, 0);
}

static struct ipc_message handle_close(const struct trap_request *req) { const u64 fd = req->args[0]; if (fd >= 32 || g_fds[fd].kind == FD_UNUSED) return reply(errno_badf(), 0); if (fd > 2) g_fds[fd].kind = FD_UNUSED; return reply(0, 0); }
static struct ipc_message handle_access(const struct trap_request *req) { char path[256]; if (!copy_cstr_from_target(req->args[0], path, sizeof(path))) return reply(errno_fault(), 0); struct fs_stat_record rec; u64 token = 0, size = 0; u8 kind = 0; return reply(vfs_lookup_stat(path, &token, &rec, &size, &kind) ? 0 : errno_noent(), 0); }

static u64 page_up(u64 value) { return (value + PAGE_BYTES - 1) & ~(u64)(PAGE_BYTES - 1); }
static struct ipc_message handle_mmap(const struct trap_request *req) {
    enum { PROT_READ = 0x1, PROT_WRITE = 0x2, PROT_EXEC = 0x4, MAP_SHARED = 0x01, MAP_PRIVATE = 0x02, MAP_SHARED_VALIDATE = 0x03, MAP_TYPE = 0x0F, MAP_FIXED = 0x10, MAP_ANONYMOUS = 0x20, MAP_FIXED_NOREPLACE = 0x100000 };
    const u64 requested_va = req->args[0]; const u64 len = req->args[1]; u64 prot = req->args[2] & (PROT_READ | PROT_WRITE | PROT_EXEC); const u64 flags = req->args[3]; const u64 fd = req->args[4]; const u64 offset = req->args[5]; const u64 map_type = flags & MAP_TYPE;
    if (len == 0) return reply(errno_inval(), 0); if (map_type != MAP_PRIVATE && map_type != MAP_SHARED && map_type != MAP_SHARED_VALIDATE) return reply(errno_inval(), 0); if ((offset & (PAGE_BYTES - 1)) != 0) return reply(errno_inval(), 0); if ((flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) != 0 && (requested_va == 0 || (requested_va & (PAGE_BYTES - 1)) != 0)) return reply(errno_inval(), 0); if ((flags & MAP_ANONYMOUS) == 0) return fd <= 2 ? reply(errno_acces(), 0) : reply(errno_badf(), 0); if (prot == 0) return reply(errno_inval(), 0); if ((prot & PROT_WRITE) != 0) prot |= PROT_READ; if ((prot & PROT_WRITE) != 0 && (prot & PROT_EXEC) != 0) return reply(errno_inval(), 0);
    const u64 size = page_up(len); const u64 page_count = size / PAGE_BYTES; const u64 target_va = ((flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) != 0) ? requested_va : mmap_next_va; const u64 map_status = map_reply_target_pages(target_va, page_count, prot);
    if (map_status == SYSCALL_OK) { if ((flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) == 0) mmap_next_va += size; return reply(target_va, 0); }
    return reply(errno_nomem(), 0);
}
static struct ipc_message handle_brk(const struct trap_request *req) { if (req->args[0] == 0) return reply(brk_next_va, 0); if (req->args[0] > brk_next_va) { u64 from = page_up(brk_next_va); u64 to = page_up(req->args[0]); if (to > from && map_reply_target_pages(from, (to - from) / PAGE_BYTES, 0x3) != SYSCALL_OK) return reply(brk_next_va, 0); } brk_next_va = req->args[0]; return reply(brk_next_va, 0); }

void linux_abi_main(void) {
    const struct ipc_message cfg = wait_ipc();
    if (cfg.status != SYSCALL_OK || cfg.request_va == 0) { user_log("LinuxAbiServer: config IPC invalid\n"); for (;;) __asm__ volatile("pause"); }
    trap_request_page_va = cfg.request_va;
    const u64 request_page_status = alloc_map_pages(trap_request_page_va, 1, 0x1);
    if (request_page_status != SYSCALL_OK) { user_log("LinuxAbiServer: request page map failed\n"); user_log_hex_value(request_page_status); for (;;) __asm__ volatile("pause"); }
    init_fds();
    if (!connect_vfs_from_registry()) user_log("LinuxAbiServer: vfs connect failed\n");
    user_log("LinuxAbiServer: started\n");
    struct ipc_message msg = reply(0, 0);
    for (;;) {
        if (msg.status != SYSCALL_OK) { msg = wait_ipc(); continue; }
        if (msg.request_va != trap_request_page_va) { user_log("LinuxAbiServer: bad request va\n"); msg = reply(errno_inval(), 0); continue; }
        const struct trap_request *req = (const struct trap_request *)trap_request_page_va;
        if (req->magic != TRAP_MAGIC || req->version != TRAP_VERSION) { user_log("LinuxAbiServer: bad request header\n"); msg = reply(errno_inval(), 0); continue; }
        log_syscall_nr(req->nr);
        switch (req->nr) {
        case LINUX_SYS_READ: msg = handle_read(req); break;
        case LINUX_SYS_WRITE: msg = handle_write(req); break;
        case LINUX_SYS_OPEN: msg = handle_openat(req, 1); break;
        case LINUX_SYS_OPENAT: msg = handle_openat(req, 0); break;
        case LINUX_SYS_CLOSE: msg = handle_close(req); break;
        case LINUX_SYS_STAT: case LINUX_SYS_LSTAT: msg = handle_newfstatat(req, 1); break;
        case LINUX_SYS_FSTAT: msg = handle_fstat(req); break;
        case LINUX_SYS_NEWFSTATAT: msg = handle_newfstatat(req, 0); break;
        case LINUX_SYS_GETDENTS64: msg = handle_getdents64(req); break;
        case LINUX_SYS_LSEEK: msg = handle_lseek(req); break;
        case LINUX_SYS_ACCESS: msg = handle_access(req); break;
        case LINUX_SYS_MMAP: msg = handle_mmap(req); break;
        case LINUX_SYS_BRK: msg = handle_brk(req); break;
        case LINUX_SYS_MUNMAP: msg = reply(0, 0); break;
        case LINUX_SYS_RT_SIGACTION: case LINUX_SYS_RT_SIGPROCMASK: case LINUX_SYS_IOCTL: case LINUX_SYS_ARCH_PRCTL: case LINUX_SYS_FUTEX: case LINUX_SYS_SET_TID_ADDRESS: case LINUX_SYS_SET_ROBUST_LIST: case LINUX_SYS_PRLIMIT64: case LINUX_SYS_RSEQ: msg = reply(0, 0); break;
        case LINUX_SYS_GETPID: case LINUX_SYS_GETTID: msg = reply(1, 0); break;
        case LINUX_SYS_GETRANDOM: msg = reply(errno_again(), 0); break;
        case LINUX_SYS_EXIT: case LINUX_SYS_EXIT_GROUP: user_log("LinuxAbiServer: exit\n"); msg = reply(0, TRAP_RESPONSE_FLAG_EXIT); break;
        default: user_log("LinuxAbiServer: unhandled syscall\n"); user_log_hex_value(req->nr); msg = reply(errno_nosys(), 0); break;
        }
    }
}
