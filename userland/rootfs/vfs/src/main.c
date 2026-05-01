#include "vfs_protocol.h"

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
    VFS_CONFIG_VA = 0x3C002000,
    VFS_REQUEST_VA = 0x26000000,
    VFS_RESPONSE_VA = 0x26001000,
    VFS_FAT_REQUEST_VA = 0x26100000,
    VFS_FAT_RESPONSE_VA = 0x26101000,
    VFS_REPLY_ENDPOINT_ID = 0xE0,
    VFS_TOKEN_TAG = 1ULL << 63,
    VFS_BACKEND_TOKEN_BASE = 1ULL << 40,
    VFS_ROOT_OBJECT_ID = 1,
    VFS_DEV_OBJECT_ID = 2,
    VFS_PROC_OBJECT_ID = 3,
    VFS_TMP_OBJECT_ID = 4,
    VFS_RUN_OBJECT_ID = 5,
    VFS_DEV_NULL_OBJECT_ID = 6,
    VFS_DEV_ZERO_OBJECT_ID = 7,
    VFS_DEV_NULL_OPEN_OBJECT_ID = 8,
    VFS_DEV_ZERO_OPEN_OBJECT_ID = 9,
    VFS_DIR_MODE = 0x4000,
    VFS_FILE_MODE = 0x8000,
};

struct vfs_mount_entry {
    u64 object_id;
    const char *name;
    u16 name_len;
};

struct vfs_builtin_file {
    u64 object_id;
    u64 open_object_id;
    const char *name;
    u16 name_len;
};

struct vfs_session {
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

struct vfs_backend_session {
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
    u64 next_seq;
};

static struct vfs_session g_session;
static struct vfs_backend_session g_root_backend;
static u64 g_endpoint_id;
static const struct vfs_mount_entry g_root_mounts[] = {
    { VFS_DEV_OBJECT_ID, "dev", 3 },
    { VFS_PROC_OBJECT_ID, "proc", 4 },
    { VFS_TMP_OBJECT_ID, "tmp", 3 },
    { VFS_RUN_OBJECT_ID, "run", 3 },
};
static const struct vfs_builtin_file g_dev_files[] = {
    { VFS_DEV_NULL_OBJECT_ID, VFS_DEV_NULL_OPEN_OBJECT_ID, "null", 4 },
    { VFS_DEV_ZERO_OBJECT_ID, VFS_DEV_ZERO_OPEN_OBJECT_ID, "zero", 4 },
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

static u64 token_from_object_id(u64 object_id) {
    return VFS_TOKEN_TAG | object_id;
}

static int is_cap_token(u64 token) {
    return (token & VFS_TOKEN_TAG) != 0 && (token & ~VFS_TOKEN_TAG) != 0;
}

static int is_backend_token(u64 token) {
    return is_cap_token(token) && ((token & ~VFS_TOKEN_TAG) & VFS_BACKEND_TOKEN_BASE) != 0;
}

static u64 wrap_backend_token(u64 backend_token) {
    if (!is_cap_token(backend_token)) return 0;
    return VFS_TOKEN_TAG | VFS_BACKEND_TOKEN_BASE | (backend_token & ~VFS_TOKEN_TAG);
}

static u64 unwrap_backend_token(u64 token) {
    if (!is_backend_token(token)) return 0;
    return VFS_TOKEN_TAG | ((token & ~VFS_TOKEN_TAG) & ~VFS_BACKEND_TOKEN_BASE);
}

static u64 root_token(void) {
    return token_from_object_id(VFS_ROOT_OBJECT_ID);
}

static u64 object_id_from_token(u64 token) {
    if ((token & VFS_TOKEN_TAG) == 0) return 0;
    const u64 object_id = token & ~VFS_TOKEN_TAG;
    if (object_id >= VFS_ROOT_OBJECT_ID && object_id <= VFS_DEV_ZERO_OPEN_OBJECT_ID) return object_id;
    return 0;
}

static int is_directory_object_id(u64 object_id) {
    return object_id >= VFS_ROOT_OBJECT_ID && object_id <= VFS_RUN_OBJECT_ID;
}

static int is_file_object_id(u64 object_id) {
    return object_id == VFS_DEV_NULL_OBJECT_ID || object_id == VFS_DEV_ZERO_OBJECT_ID;
}

static int is_open_file_object_id(u64 object_id) {
    return object_id == VFS_DEV_NULL_OPEN_OBJECT_ID || object_id == VFS_DEV_ZERO_OPEN_OBJECT_ID;
}

static int is_directory_token(u64 token) {
    return is_directory_object_id(object_id_from_token(token));
}

static int is_file_token(u64 token) {
    return is_file_object_id(object_id_from_token(token));
}

static int is_open_file_token(u64 token) {
    return is_open_file_object_id(object_id_from_token(token));
}

static u64 open_object_id_for_file(u64 object_id) {
    for (u64 i = 0; i < sizeof(g_dev_files) / sizeof(g_dev_files[0]); i++) {
        if (g_dev_files[i].object_id == object_id) return g_dev_files[i].open_object_id;
    }
    return 0;
}

static int is_root_path(const volatile u8 *path, u16 len) {
    if (len == 0) return 1;
    if (len == 1 && path[0] == '/') return 1;
    return 0;
}

static int path_equals(const volatile u8 *path, u16 len, const char *name, u16 name_len) {
    if (len != name_len) return 0;
    for (u16 i = 0; i < len; i++) {
        if (path[i] != (u8)name[i]) return 0;
    }
    return 1;
}

static u64 lookup_mount_name(const volatile u8 *name, u16 name_len) {
    for (u64 i = 0; i < sizeof(g_root_mounts) / sizeof(g_root_mounts[0]); i++) {
        if (path_equals(name, name_len, g_root_mounts[i].name, g_root_mounts[i].name_len)) {
            return token_from_object_id(g_root_mounts[i].object_id);
        }
    }
    return 0;
}

static u64 lookup_dev_name(const volatile u8 *name, u16 name_len) {
    for (u64 i = 0; i < sizeof(g_dev_files) / sizeof(g_dev_files[0]); i++) {
        if (path_equals(name, name_len, g_dev_files[i].name, g_dev_files[i].name_len)) {
            return token_from_object_id(g_dev_files[i].object_id);
        }
    }
    return 0;
}

static int root_mount_name_exists(const volatile u8 *name, u16 name_len) {
    for (u64 i = 0; i < sizeof(g_root_mounts) / sizeof(g_root_mounts[0]); i++) {
        if (path_equals(name, name_len, g_root_mounts[i].name, g_root_mounts[i].name_len)) return 1;
    }
    return 0;
}

static u64 lookup_path(u64 base_token, const volatile u8 *path, u16 len) {
    const u64 base_object_id = object_id_from_token(base_token);
    if (base_object_id == 0) return 0;
    if (is_root_path(path, len)) return base_token;

    u16 pos = 0;
    u64 current_object_id = base_object_id;
    if (len > 0 && path[0] == '/') {
        current_object_id = VFS_ROOT_OBJECT_ID;
        pos = 1;
    }

    while (pos < len && path[pos] == '/') pos++;
    if (pos >= len) return token_from_object_id(current_object_id);

    const u16 start = pos;
    while (pos < len && path[pos] != '/') pos++;
    const u16 component_len = pos - start;
    while (pos < len && path[pos] == '/') pos++;

    if (current_object_id == VFS_ROOT_OBJECT_ID) {
        const u64 token = lookup_mount_name(path + start, component_len);
        if (token == 0) return 0;
        current_object_id = object_id_from_token(token);
    } else if (current_object_id == VFS_DEV_OBJECT_ID) {
        const u64 token = lookup_dev_name(path + start, component_len);
        if (token == 0) return 0;
        current_object_id = object_id_from_token(token);
    } else {
        return 0;
    }

    if (pos >= len) return token_from_object_id(current_object_id);
    if (current_object_id != VFS_DEV_OBJECT_ID) return 0;

    const u16 second_start = pos;
    while (pos < len && path[pos] != '/') pos++;
    const u16 second_len = pos - second_start;
    while (pos < len && path[pos] == '/') pos++;
    if (pos < len) return 0;
    return lookup_dev_name(path + second_start, second_len);
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

static void reply_dir_lookup(u16 op, u64 seq, u64 token) {
    clear_page(g_session.response_va);
    const u8 kind = token == root_token() ? FS_OBJECT_MOUNT : FS_OBJECT_DIRECTORY;
    write_response(op, seq, FS_STATUS_OK, token, 0, 0, kind, 0);
}

static void reply_file_lookup(u16 op, u64 seq, u64 token) {
    clear_page(g_session.response_va);
    write_response(op, seq, FS_STATUS_OK, token, 0, 0, FS_OBJECT_FILE, 0);
}

static void reply_stat(u64 seq, u64 token) {
    clear_page(g_session.response_va);
    volatile struct fs_stat_record *record = (volatile struct fs_stat_record *)(g_session.response_va + FS_RESPONSE_HEADER_BYTES);
    const u64 object_id = object_id_from_token(token);
    const int is_dir = is_directory_object_id(object_id);
    record->object_kind = is_dir ? FS_OBJECT_DIRECTORY : FS_OBJECT_FILE;
    record->size_bytes = 0;
    record->mode_bits = is_dir ? VFS_DIR_MODE : VFS_FILE_MODE;
    record->reserved1 = 0;
    record->mtime_unix_sec = 0;
    record->reserved2[0] = 0;
    record->reserved2[1] = 0;
    const u8 kind = token == root_token() ? FS_OBJECT_MOUNT : record->object_kind;
    write_response(FS_OP_STAT, seq, FS_STATUS_OK, 0, 0, 0, kind, sizeof(struct fs_stat_record));
}

static void write_dirent_response(u64 seq, u64 result_token, u64 next_cursor, const char *name, u16 name_len, u8 object_kind) {
    volatile struct fs_dirent_record *record = (volatile struct fs_dirent_record *)(g_session.response_va + FS_RESPONSE_HEADER_BYTES);
    record->next_cursor = next_cursor;
    record->object_kind = object_kind;
    record->reserved0[0] = 0;
    record->reserved0[1] = 0;
    record->reserved0[2] = 0;
    record->reserved0[3] = 0;
    record->reserved0[4] = 0;
    record->reserved0[5] = 0;
    record->reserved0[6] = 0;
    record->name_bytes = name_len;
    record->reserved1 = 0;
    record->reserved2 = 0;

    volatile u8 *payload = (volatile u8 *)(g_session.response_va + FS_RESPONSE_HEADER_BYTES + FS_DIRENT_RECORD_BYTES);
    for (u16 i = 0; i < name_len; i++) payload[i] = (u8)name[i];
    write_response(
        FS_OP_READDIR,
        seq,
        FS_STATUS_OK,
        result_token,
        0,
        next_cursor,
        object_kind,
        (u16)(FS_DIRENT_RECORD_BYTES + name_len)
    );
}

static void reply_readdir(u64 seq, u64 token, u64 cursor) {
    clear_page(g_session.response_va);
    if (token == root_token()) {
        if (cursor >= sizeof(g_root_mounts) / sizeof(g_root_mounts[0])) {
            write_response(FS_OP_READDIR, seq, FS_STATUS_END_OF_DIR, 0, 0, cursor, FS_OBJECT_DIRECTORY, 0);
            return;
        }
        const struct vfs_mount_entry *entry = &g_root_mounts[cursor];
        write_dirent_response(seq, token_from_object_id(entry->object_id), cursor + 1, entry->name, entry->name_len, FS_OBJECT_DIRECTORY);
        return;
    }
    if (token == token_from_object_id(VFS_DEV_OBJECT_ID)) {
        if (cursor >= sizeof(g_dev_files) / sizeof(g_dev_files[0])) {
            write_response(FS_OP_READDIR, seq, FS_STATUS_END_OF_DIR, 0, 0, cursor, FS_OBJECT_DIRECTORY, 0);
            return;
        }
        const struct vfs_builtin_file *entry = &g_dev_files[cursor];
        write_dirent_response(seq, token_from_object_id(entry->object_id), cursor + 1, entry->name, entry->name_len, FS_OBJECT_FILE);
        return;
    }
    if (is_directory_token(token)) {
        write_response(FS_OP_READDIR, seq, FS_STATUS_END_OF_DIR, 0, 0, cursor, FS_OBJECT_DIRECTORY, 0);
        return;
    }
    write_response(FS_OP_READDIR, seq, FS_STATUS_NOT_FOUND, 0, 0, cursor, FS_OBJECT_NONE, 0);
}

static void reply_open(u64 seq, u64 token) {
    const u64 object_id = object_id_from_token(token);
    const u64 open_object_id = open_object_id_for_file(object_id);
    if (open_object_id == 0) {
        reply_status(FS_OP_OPEN, seq, FS_STATUS_NOT_FOUND);
        return;
    }
    clear_page(g_session.response_va);
    write_response(FS_OP_OPEN, seq, FS_STATUS_OK, token_from_object_id(open_object_id), 0, 0, FS_OBJECT_OPEN_FILE, 0);
}

static void reply_read(u64 seq, u64 token, u64 offset, u32 length) {
    (void)offset;
    clear_page(g_session.response_va);
    const u64 object_id = object_id_from_token(token);
    if (object_id == VFS_DEV_NULL_OPEN_OBJECT_ID) {
        write_response(FS_OP_READ, seq, FS_STATUS_OK, 0, 0, offset, FS_OBJECT_OPEN_FILE, 0);
        return;
    }
    if (object_id != VFS_DEV_ZERO_OPEN_OBJECT_ID) {
        write_response(FS_OP_READ, seq, FS_STATUS_NOT_FOUND, 0, 0, offset, FS_OBJECT_NONE, 0);
        return;
    }
    u16 bytes = (u16)length;
    if (bytes > FS_RESPONSE_PAYLOAD_BYTES) bytes = FS_RESPONSE_PAYLOAD_BYTES;
    volatile u8 *payload = (volatile u8 *)(g_session.response_va + FS_RESPONSE_HEADER_BYTES);
    for (u16 i = 0; i < bytes; i++) payload[i] = 0;
    write_response(FS_OP_READ, seq, FS_STATUS_OK, 0, 0, offset + bytes, FS_OBJECT_OPEN_FILE, bytes);
}

static void reply_write(u64 seq, u64 token, u64 offset, u32 length) {
    const u64 object_id = object_id_from_token(token);
    if (!is_open_file_object_id(object_id)) {
        reply_status(FS_OP_WRITE, seq, FS_STATUS_NOT_FOUND);
        return;
    }
    clear_page(g_session.response_va);
    write_response(
        FS_OP_WRITE,
        seq,
        FS_STATUS_OK,
        0,
        offset + length,
        offset + length,
        FS_OBJECT_OPEN_FILE,
        0
    );
}

static u64 make_backend_session_nonce(u64 request_paddr, u64 response_paddr, u64 endpoint_id, u64 process_slot) {
    u64 nonce = request_paddr ^
        ((response_paddr << 17) | (response_paddr >> 47)) ^
        ((endpoint_id << 7) | (endpoint_id >> 57)) ^
        process_slot ^
        0x9e3779b97f4a7c15ULL;
    return nonce == 0 ? 1 : nonce;
}

static int install_backend_endpoint(void) {
    if (g_root_backend.endpoint_id == 0 || g_root_backend.process_slot == 0) return 0;
    return syscall3(SYSCALL_INSTALL_ENDPOINT, 0, g_root_backend.endpoint_id, g_root_backend.process_slot) == SYSCALL_OK;
}

static int grant_backend_response_page(void) {
    u64 ret = syscall3(
        SYSCALL_GRANT_CAP_ON_ENDPOINT,
        g_root_backend.response_paddr,
        g_root_backend.endpoint_id,
        PAGE_RIGHT_CPU_READ | PAGE_RIGHT_CPU_WRITE
    );
    if (ret == SYSCALL_OK) return 1;
    if (ret == SYSCALL_ERR_ENDPOINT && install_backend_endpoint()) {
        ret = syscall3(
            SYSCALL_GRANT_CAP_ON_ENDPOINT,
            g_root_backend.response_paddr,
            g_root_backend.endpoint_id,
            PAGE_RIGHT_CPU_READ | PAGE_RIGHT_CPU_WRITE
        );
    }
    return ret == SYSCALL_OK;
}

static int share_backend_request_page(void) {
    u64 ret = syscall2(SYSCALL_SHARE_CAP, g_root_backend.request_paddr, g_root_backend.endpoint_id);
    if (ret == SYSCALL_OK) return 1;
    if (ret == SYSCALL_ERR_ENDPOINT && install_backend_endpoint()) {
        ret = syscall2(SYSCALL_SHARE_CAP, g_root_backend.request_paddr, g_root_backend.endpoint_id);
    }
    return ret == SYSCALL_OK;
}

static int signal_backend(void) {
    u64 ret = syscall2(SYSCALL_SIGNAL_ENDPOINT, g_root_backend.endpoint_id, 0);
    if (ret == SYSCALL_OK) return 1;
    if (ret == SYSCALL_ERR_ENDPOINT && install_backend_endpoint()) {
        ret = syscall2(SYSCALL_SIGNAL_ENDPOINT, g_root_backend.endpoint_id, 0);
    }
    return ret == SYSCALL_OK;
}

static int wait_backend_response(u64 expected_seq, u16 expected_op) {
    volatile struct fs_response_header *response = (volatile struct fs_response_header *)g_root_backend.response_va;
    for (u64 i = 0; i < 256; i++) {
        if (response->response_seq == expected_seq) {
            return response->magic == FS_RESPONSE_MAGIC &&
                response->version == FS_PROTOCOL_VERSION &&
                response->op == expected_op;
        }
        (void)wait_event_poll();
    }
    return 0;
}

static int connect_root_backend(u64 endpoint_id, u64 process_slot) {
    if (endpoint_id == 0 || process_slot == 0) return 0;

    g_root_backend.endpoint_id = endpoint_id;
    g_root_backend.process_slot = process_slot;
    g_root_backend.request_va = VFS_FAT_REQUEST_VA;
    g_root_backend.response_va = VFS_FAT_RESPONSE_VA;
    g_root_backend.request_paddr = syscall0(SYSCALL_ALLOC_PAGE);
    g_root_backend.response_paddr = syscall0(SYSCALL_ALLOC_PAGE);
    if (g_root_backend.request_paddr < 0x1000 || g_root_backend.response_paddr < 0x1000) return 0;
    if (syscall3(SYSCALL_MAP_PAGE, g_root_backend.request_va, g_root_backend.request_paddr, 1) != SYSCALL_OK) return 0;
    if (syscall3(SYSCALL_MAP_PAGE, g_root_backend.response_va, g_root_backend.response_paddr, 1) != SYSCALL_OK) return 0;
    if (!grant_backend_response_page()) return 0;

    clear_page(g_root_backend.request_va);
    clear_page(g_root_backend.response_va);
    const u64 process_slot_self = syscall0(SYSCALL_GET_PROCESS_SLOT);
    g_root_backend.session_nonce = make_backend_session_nonce(
        g_root_backend.request_paddr,
        g_root_backend.response_paddr,
        endpoint_id,
        process_slot_self
    );
    volatile struct fs_request_header *request = (volatile struct fs_request_header *)g_root_backend.request_va;
    request->magic = FS_REQUEST_MAGIC;
    request->version = FS_PROTOCOL_VERSION;
    request->op = FS_OP_CONNECT;
    request->object_token = 0;
    request->offset = 0;
    request->length = 0;
    request->flags = 0;
    request->path_bytes = 0;
    request->inline_bytes = 0;
    request->reserved0 = 0;
    request->arg0 = g_root_backend.response_paddr;
    request->arg1 = process_slot_self;
    request->session_nonce = g_root_backend.session_nonce;
    __asm__ volatile("" ::: "memory");
    request->request_seq = 1;

    if (!share_backend_request_page()) return 0;
    if (!wait_backend_response(1, FS_OP_CONNECT)) return 0;

    volatile struct fs_response_header *response = (volatile struct fs_response_header *)g_root_backend.response_va;
    if (response->status != FS_STATUS_OK || !is_cap_token(response->result_token)) return 0;
    g_root_backend.root_token = response->result_token;
    g_root_backend.next_seq = 2;
    g_root_backend.active = 1;
    return 1;
}

static int backend_request(
    u16 op,
    u64 backend_token,
    u64 offset,
    u32 length,
    u32 flags,
    const volatile u8 *path,
    u16 path_bytes,
    const volatile u8 *inline_payload,
    u16 inline_bytes
) {
    if (!g_root_backend.active) return 0;
    if ((u64)path_bytes + (u64)inline_bytes > FS_PAGE_BYTES - FS_REQUEST_HEADER_BYTES) return 0;
    const u64 seq = g_root_backend.next_seq++;
    clear_page(g_root_backend.request_va);
    clear_page(g_root_backend.response_va);

    volatile struct fs_request_header *request = (volatile struct fs_request_header *)g_root_backend.request_va;
    request->magic = FS_REQUEST_MAGIC;
    request->version = FS_PROTOCOL_VERSION;
    request->op = op;
    request->object_token = backend_token;
    request->offset = offset;
    request->length = length;
    request->flags = flags;
    request->path_bytes = path_bytes;
    request->inline_bytes = inline_bytes;
    request->reserved0 = 0;
    request->arg0 = 0;
    request->arg1 = 0;
    request->session_nonce = g_root_backend.session_nonce;

    volatile u8 *payload = (volatile u8 *)(g_root_backend.request_va + FS_REQUEST_HEADER_BYTES);
    for (u16 i = 0; i < path_bytes; i++) payload[i] = path[i];
    for (u16 i = 0; i < inline_bytes; i++) payload[path_bytes + i] = inline_payload[i];

    __asm__ volatile("" ::: "memory");
    request->request_seq = seq;
    if (!signal_backend()) return 0;
    return wait_backend_response(seq, op);
}

static void forward_backend_response(u16 op, u64 client_seq, u64 cursor_bias) {
    volatile struct fs_response_header *backend = (volatile struct fs_response_header *)g_root_backend.response_va;
    u16 inline_bytes = backend->inline_bytes;
    if (inline_bytes > FS_RESPONSE_PAYLOAD_BYTES) {
        reply_status(op, client_seq, FS_STATUS_INVALID);
        return;
    }

    clear_page(g_session.response_va);
    volatile u8 *dst = (volatile u8 *)(g_session.response_va + FS_RESPONSE_HEADER_BYTES);
    const volatile u8 *src = (const volatile u8 *)(g_root_backend.response_va + FS_RESPONSE_HEADER_BYTES);
    for (u16 i = 0; i < inline_bytes; i++) dst[i] = src[i];

    u64 result_token = backend->result_token == 0 ? 0 : wrap_backend_token(backend->result_token);
    u64 cursor_next = backend->cursor_next;
    if (op == FS_OP_READDIR) {
        cursor_next += cursor_bias;
        if (backend->status == FS_STATUS_OK && inline_bytes >= FS_DIRENT_RECORD_BYTES) {
            volatile struct fs_dirent_record *record = (volatile struct fs_dirent_record *)dst;
            record->next_cursor += cursor_bias;
        }
    }
    write_response(op, client_seq, backend->status, result_token, backend->file_bytes, cursor_next, backend->object_kind, inline_bytes);
}

static int backend_readdir_entry_shadowed_by_builtin_root(void) {
    volatile struct fs_response_header *backend = (volatile struct fs_response_header *)g_root_backend.response_va;
    if (backend->status != FS_STATUS_OK || backend->inline_bytes < FS_DIRENT_RECORD_BYTES) return 0;
    volatile struct fs_dirent_record *record = (volatile struct fs_dirent_record *)(g_root_backend.response_va + FS_RESPONSE_HEADER_BYTES);
    if (backend->inline_bytes < FS_DIRENT_RECORD_BYTES + record->name_bytes) return 0;
    const volatile u8 *name = (const volatile u8 *)(g_root_backend.response_va + FS_RESPONSE_HEADER_BYTES + FS_DIRENT_RECORD_BYTES);
    return root_mount_name_exists(name, record->name_bytes);
}

static void forward_backend_request(
    u16 op,
    u64 client_seq,
    u64 backend_token,
    u64 offset,
    u32 length,
    u32 flags,
    const volatile u8 *path,
    u16 path_bytes,
    const volatile u8 *inline_payload,
    u16 inline_bytes,
    u64 cursor_bias
) {
    if (!backend_request(op, backend_token, offset, length, flags, path, path_bytes, inline_payload, inline_bytes)) {
        reply_status(op, client_seq, FS_STATUS_IO_ERROR);
        return;
    }
    forward_backend_response(op, client_seq, cursor_bias);
}

static void forward_backend_root_readdir_filtered(u64 client_seq, u64 backend_cursor, u32 length, u32 flags, u64 cursor_bias) {
    for (u64 guard = 0; guard < 64; guard++) {
        if (!backend_request(
            FS_OP_READDIR,
            g_root_backend.root_token,
            backend_cursor,
            length,
            flags,
            (const volatile u8 *)0,
            0,
            (const volatile u8 *)0,
            0
        )) {
            reply_status(FS_OP_READDIR, client_seq, FS_STATUS_IO_ERROR);
            return;
        }

        volatile struct fs_response_header *backend = (volatile struct fs_response_header *)g_root_backend.response_va;
        if (backend->status != FS_STATUS_OK) {
            forward_backend_response(FS_OP_READDIR, client_seq, cursor_bias);
            return;
        }
        if (!backend_readdir_entry_shadowed_by_builtin_root()) {
            forward_backend_response(FS_OP_READDIR, client_seq, cursor_bias);
            return;
        }

        volatile struct fs_dirent_record *record = (volatile struct fs_dirent_record *)(g_root_backend.response_va + FS_RESPONSE_HEADER_BYTES);
        if (record->next_cursor <= backend_cursor) {
            reply_status(FS_OP_READDIR, client_seq, FS_STATUS_INVALID);
            return;
        }
        backend_cursor = record->next_cursor;
    }
    reply_status(FS_OP_READDIR, client_seq, FS_STATUS_BUSY);
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
        if (request->path_bytes > FS_MAX_PATH_BYTES) {
            reply_status(FS_OP_LOOKUP, seq, FS_STATUS_INVALID);
        } else {
            const volatile u8 *path = (const volatile u8 *)(g_session.request_va + FS_REQUEST_HEADER_BYTES);
            if (is_backend_token(request->object_token)) {
                forward_backend_request(
                    FS_OP_LOOKUP,
                    seq,
                    unwrap_backend_token(request->object_token),
                    0,
                    0,
                    0,
                    path,
                    request->path_bytes,
                    (const volatile u8 *)0,
                    0,
                    0
                );
            } else {
                const u64 token = lookup_path(request->object_token, path, request->path_bytes);
                if (token != 0 && is_directory_token(token)) reply_dir_lookup(FS_OP_LOOKUP, seq, token);
                else if (token != 0 && is_file_token(token)) reply_file_lookup(FS_OP_LOOKUP, seq, token);
                else if (g_root_backend.active && object_id_from_token(request->object_token) == VFS_ROOT_OBJECT_ID) {
                    forward_backend_request(
                        FS_OP_LOOKUP,
                        seq,
                        g_root_backend.root_token,
                        0,
                        0,
                        0,
                        path,
                        request->path_bytes,
                        (const volatile u8 *)0,
                        0,
                        0
                    );
                } else {
                    reply_status(FS_OP_LOOKUP, seq, FS_STATUS_NOT_FOUND);
                }
            }
        }
    } else if (request->op == FS_OP_STAT) {
        if (is_backend_token(request->object_token)) {
            forward_backend_request(
                FS_OP_STAT,
                seq,
                unwrap_backend_token(request->object_token),
                0,
                0,
                0,
                (const volatile u8 *)0,
                0,
                (const volatile u8 *)0,
                0,
                0
            );
        } else if (is_directory_token(request->object_token) || is_file_token(request->object_token) || is_open_file_token(request->object_token)) {
            reply_stat(seq, request->object_token);
        }
        else reply_status(FS_OP_STAT, seq, FS_STATUS_NOT_FOUND);
    } else if (request->op == FS_OP_READDIR) {
        if (is_backend_token(request->object_token)) {
            forward_backend_request(
                FS_OP_READDIR,
                seq,
                unwrap_backend_token(request->object_token),
                request->offset,
                request->length,
                request->flags,
                (const volatile u8 *)0,
                0,
                (const volatile u8 *)0,
                0,
                0
            );
        } else if (request->object_token == root_token() &&
            g_root_backend.active &&
            request->offset >= sizeof(g_root_mounts) / sizeof(g_root_mounts[0]))
        {
            const u64 bias = sizeof(g_root_mounts) / sizeof(g_root_mounts[0]);
            forward_backend_root_readdir_filtered(
                seq,
                request->offset - bias,
                request->length,
                request->flags,
                bias
            );
        } else if (is_directory_token(request->object_token)) reply_readdir(seq, request->object_token, request->offset);
        else reply_status(FS_OP_READDIR, seq, FS_STATUS_NOT_FOUND);
    } else if (request->op == FS_OP_CLOSE) {
        if (is_backend_token(request->object_token)) {
            forward_backend_request(
                FS_OP_CLOSE,
                seq,
                unwrap_backend_token(request->object_token),
                0,
                0,
                0,
                (const volatile u8 *)0,
                0,
                (const volatile u8 *)0,
                0,
                0
            );
        } else {
            reply_status(FS_OP_CLOSE, seq, FS_STATUS_OK);
        }
    } else if (request->op == FS_OP_STATFS) {
        if (is_backend_token(request->object_token) || (request->object_token == root_token() && g_root_backend.active)) {
            forward_backend_request(
                FS_OP_STATFS,
                seq,
                is_backend_token(request->object_token) ? unwrap_backend_token(request->object_token) : g_root_backend.root_token,
                0,
                0,
                0,
                (const volatile u8 *)0,
                0,
                (const volatile u8 *)0,
                0,
                0
            );
        } else {
            clear_page(g_session.response_va);
            write_response(FS_OP_STATFS, seq, FS_STATUS_OK, 0, 0, 0, FS_OBJECT_MOUNT, 0);
        }
    } else if (request->op == FS_OP_OPEN) {
        if (is_backend_token(request->object_token)) {
            forward_backend_request(
                request->op,
                seq,
                unwrap_backend_token(request->object_token),
                0,
                0,
                request->flags,
                (const volatile u8 *)0,
                0,
                (const volatile u8 *)0,
                0,
                0
            );
        } else if (is_directory_token(request->object_token)) reply_status(request->op, seq, FS_STATUS_IS_DIR);
        else if (is_file_token(request->object_token)) reply_open(seq, request->object_token);
        else reply_status(request->op, seq, FS_STATUS_NOT_FOUND);
    } else if (request->op == FS_OP_OPEN_EXEC) {
        if (is_backend_token(request->object_token)) {
            forward_backend_request(
                request->op,
                seq,
                unwrap_backend_token(request->object_token),
                0,
                0,
                request->flags,
                (const volatile u8 *)0,
                0,
                (const volatile u8 *)0,
                0,
                0
            );
        } else if (is_directory_token(request->object_token)) reply_status(request->op, seq, FS_STATUS_IS_DIR);
        else if (is_file_token(request->object_token)) reply_status(request->op, seq, FS_STATUS_NO_RIGHT);
        else reply_status(request->op, seq, FS_STATUS_NOT_FOUND);
    } else if (request->op == FS_OP_READ) {
        if (is_backend_token(request->object_token)) {
            forward_backend_request(
                request->op,
                seq,
                unwrap_backend_token(request->object_token),
                request->offset,
                request->length,
                request->flags,
                (const volatile u8 *)0,
                0,
                (const volatile u8 *)0,
                0,
                0
            );
        } else if (is_open_file_token(request->object_token)) reply_read(seq, request->object_token, request->offset, request->length);
        else reply_status(request->op, seq, FS_STATUS_NOT_FOUND);
    } else if (request->op == FS_OP_WRITE) {
        if (is_backend_token(request->object_token)) {
            const volatile u8 *payload = (const volatile u8 *)(g_session.request_va + FS_REQUEST_HEADER_BYTES + request->path_bytes);
            forward_backend_request(
                request->op,
                seq,
                unwrap_backend_token(request->object_token),
                request->offset,
                request->length,
                request->flags,
                (const volatile u8 *)0,
                0,
                payload,
                request->inline_bytes,
                0
            );
        } else if (is_open_file_token(request->object_token)) reply_write(seq, request->object_token, request->offset, request->length);
        else reply_status(request->op, seq, FS_STATUS_NOT_FOUND);
    } else if (request->op == FS_OP_CREATE || request->op == FS_OP_UNLINK || request->op == FS_OP_RENAME) {
        if (is_backend_token(request->object_token)) {
            const volatile u8 *path = (const volatile u8 *)(g_session.request_va + FS_REQUEST_HEADER_BYTES);
            const volatile u8 *inline_payload = path + request->path_bytes;
            forward_backend_request(
                request->op,
                seq,
                unwrap_backend_token(request->object_token),
                request->offset,
                request->length,
                request->flags,
                path,
                request->path_bytes,
                inline_payload,
                request->inline_bytes,
                0
            );
        } else if (g_root_backend.active && object_id_from_token(request->object_token) == VFS_ROOT_OBJECT_ID) {
            const volatile u8 *path = (const volatile u8 *)(g_session.request_va + FS_REQUEST_HEADER_BYTES);
            const volatile u8 *inline_payload = path + request->path_bytes;
            forward_backend_request(
                request->op,
                seq,
                g_root_backend.root_token,
                request->offset,
                request->length,
                request->flags,
                path,
                request->path_bytes,
                inline_payload,
                request->inline_bytes,
                0
            );
        } else {
            reply_status(request->op, seq, FS_STATUS_NO_RIGHT);
        }
    } else {
        reply_status(request->op, seq, FS_STATUS_NOT_SUPPORTED);
    }

    g_session.last_completed_seq = seq;
}

static void handle_connect_transfer(u64 transfer_id) {
    const u64 request_paddr = syscall2(SYSCALL_ACCEPT_CAP_TRANSFER, transfer_id, 0);
    if (request_paddr < 0x1000) return;
    if (syscall3(SYSCALL_MAP_PAGE, VFS_REQUEST_VA, request_paddr, 0) != SYSCALL_OK) return;

    volatile struct fs_request_header *request = (volatile struct fs_request_header *)VFS_REQUEST_VA;
    if (request->magic != FS_REQUEST_MAGIC ||
        request->version != FS_PROTOCOL_VERSION ||
        request->op != FS_OP_CONNECT ||
        request->request_seq == 0 ||
        request->arg0 < 0x1000 ||
        request->session_nonce == 0)
    {
        user_log("RootVfs: invalid connect request\n");
        return;
    }
    if (syscall3(SYSCALL_MAP_PAGE, VFS_RESPONSE_VA, request->arg0, 1) != SYSCALL_OK) return;

    clear_page(VFS_RESPONSE_VA);
    g_session.active = 1;
    g_session.request_va = VFS_REQUEST_VA;
    g_session.response_va = VFS_RESPONSE_VA;
    g_session.request_paddr = request_paddr;
    g_session.response_paddr = request->arg0;
    g_session.reply_endpoint_id = syscall3(SYSCALL_INSTALL_ENDPOINT, 0, VFS_REPLY_ENDPOINT_ID, request->arg1) == SYSCALL_OK
        ? VFS_REPLY_ENDPOINT_ID
        : 0;
    g_session.session_nonce = request->session_nonce;
    g_session.last_completed_seq = 0;
    g_session.root_token = root_token();
    write_response(FS_OP_CONNECT, request->request_seq, FS_STATUS_OK, g_session.root_token, 0, 0, FS_OBJECT_MOUNT, 0);
    g_session.last_completed_seq = request->request_seq;
    user_log("RootVfs: session connect ok\n");
}

void rootfs_vfs_main(void) {
    user_log("RootVfs: started\n");
    user_log("RootVfs: builtin mounts /dev /proc /tmp /run\n");
    volatile u64 *config = (volatile u64 *)VFS_CONFIG_VA;
    g_endpoint_id = config[0];
    const u64 fat_endpoint_id = config[3];
    const u64 fat_process_slot = config[4];
    if (connect_root_backend(fat_endpoint_id, fat_process_slot)) {
        user_log("RootVfs: fat backend connect ok\n");
    } else if (fat_endpoint_id != 0) {
        user_log("RootVfs: fat backend connect failed\n");
    } else {
        user_log("RootVfs: fat backend missing\n");
    }
    if (g_endpoint_id != 0) {
        user_log("RootVfs: endpoint ready\n");
        config[2] = 1;
    } else {
        user_log("RootVfs: endpoint missing\n");
    }
    for (;;) {
        const u64 received = wait_event();
        if (received >= CAP_TRANSFER_ID_MIN) handle_connect_transfer(received);
        handle_fs_request();
    }
}
