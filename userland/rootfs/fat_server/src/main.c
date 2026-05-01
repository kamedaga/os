#include "fs_protocol.h"

enum {
    SYSCALL_LOG = 0x9,
    SYSCALL_MAP_PAGE = 0x2,
    SYSCALL_WAIT_EVENT = 0x17,
    SYSCALL_INSTALL_ENDPOINT = 0x26,
    SYSCALL_ACCEPT_CAP_TRANSFER = 0x2A,
    SYSCALL_SIGNAL_ENDPOINT = 0x2C,
    SYSCALL_OK = 0,
    CAP_TRANSFER_ID_MIN = 0x1000,
    FAT_CONFIG_VA = 0x3C002000,
    FAT_REQUEST_VA = 0x27000000,
    FAT_RESPONSE_VA = 0x27001000,
    FAT_REPLY_ENDPOINT_ID = 0xE8,
    FAT_TOKEN_TAG = 1ULL << 63,
    FAT_ROOT_OBJECT_ID = 1,
    FAT_PROBE_OBJECT_ID = 2,
    FAT_DIR_MODE = 0x4000,
};

static const char g_probe_name[] = "fat_probe";
static const u16 g_probe_name_len = 9;

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

static struct fat_session g_session;
static u64 g_endpoint_id;

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

static void clear_page(u64 va) {
    volatile u64 *p = (volatile u64 *)va;
    for (u64 i = 0; i < 512; i++) p[i] = 0;
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
    if (object_id == FAT_ROOT_OBJECT_ID || object_id == FAT_PROBE_OBJECT_ID) return object_id;
    return 0;
}

static int is_root_token(u64 token) {
    return token == root_token();
}

static int is_dir_token(u64 token) {
    return object_id_from_token(token) != 0;
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
        user_log("FatServer: invalid connect request\n");
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
    user_log("FatServer: session connect ok\n");
}

void fat_server_main(void) {
    user_log("FatServer: skeleton started\n");
    volatile u64 *config = (volatile u64 *)FAT_CONFIG_VA;
    g_endpoint_id = config[0];
    if (g_endpoint_id != 0) {
        user_log("FatServer: endpoint ready\n");
        config[2] = 1;
    } else {
        user_log("FatServer: endpoint missing\n");
    }

    for (;;) {
        const u64 received = wait_event();
        if (received >= CAP_TRANSFER_ID_MIN) handle_connect_transfer(received);
        handle_fs_request();
    }
}
