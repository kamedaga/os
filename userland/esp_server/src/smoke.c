#include "fs_server_abi.h"

enum {
    SYSCALL_SIGNAL_ENDPOINT = 0x2C,
    SYSCALL_GET_PROCESS_SLOT = 0x2E,
    SYSCALL_INSTALL_ENDPOINT = 0x26,
    SYSCALL_WAIT_EVENT = 0x17,
    SYSCALL_LOG = 0x9,
    SYSCALL_ALLOC_MAP_PAGES_ANYWHERE = 0x5D,
    SYSCALL_CREATE_IPC_BUFFER_FROM_PAGE = 0x5E,
    SYSCALL_GRANT_IPC_BUFFER_ON_ENDPOINT = 0x5F,
    SYSCALL_SHARE_IPC_BUFFER_ON_ENDPOINT = 0x60,
    SYSCALL_OK = 0,
    SYSCALL_ERR_ENDPOINT = 9,

    IPC_BUFFER_TOKEN_TAG = 0xA000000000000000ULL,
    IPC_BUFFER_TOKEN_MASK = 0x0FFFFFFFFFFFFFFFULL,
    IPC_BUFFER_RIGHT_READ = 0x1,
    IPC_BUFFER_RIGHT_WRITE = 0x2,
    IPC_BUFFER_RIGHT_MAP = 0x4,
    IPC_BUFFER_RIGHT_GRANT = 0x8,
    IPC_BUFFER_ROLE_REQUEST = 1,
    IPC_BUFFER_ROLE_RESPONSE = 2,

    SERVICE_REGISTRY_MAGIC = 0x53525643u,
    SERVICE_REGISTRY_VERSION = 1,
    SERVICE_KIND_VFS = 2,
    SERVICE_FLAG_PROCESS_SLOT_COMPAT = 1,
    SERVICE_REGISTRY_PAGE_VA = 0x3C2C0000,

    RESPONSE_POLL_LIMIT = 2048,
};

struct service_registry_entry {
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
    struct service_registry_entry entries[8];
};

struct fs_smoke_client {
    u64 endpoint_id;
    u64 server_process_slot;
    u64 request_va;
    u64 response_va;
    u64 request_paddr;
    u64 response_paddr;
    u64 request_token;
    u64 response_token;
    u64 session_nonce;
    u64 mount_token;
    u64 next_seq;
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

static u64 syscall0(u64 nr) {
    u64 ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(nr)
        : "rcx", "rdx", "r8", "r9", "r10", "r11", "memory");
    return ret;
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

static void clear_page(u64 va) {
    volatile u64 *p = (volatile u64 *)va;
    for (u64 i = 0; i < 512; i++) p[i] = 0;
}

static void copy_to_volatile(volatile u8 *dst, const u8 *src, u64 len) {
    for (u64 i = 0; i < len; i++) dst[i] = src[i];
}

static int find_esp_fs_service(u64 *endpoint_id, u64 *process_slot) {
    volatile struct service_registry_page *page = (volatile struct service_registry_page *)SERVICE_REGISTRY_PAGE_VA;
    if (page->magic != SERVICE_REGISTRY_MAGIC || page->version != SERVICE_REGISTRY_VERSION) return 0;
    u64 count = page->entry_count;
    if (count > 8) count = 8;
    for (u64 i = 0; i < count; i++) {
        volatile struct service_registry_entry *entry = &page->entries[i];
        if (entry->kind != SERVICE_KIND_VFS) continue;
        *endpoint_id = entry->endpoint_id;
        *process_slot = ((entry->flags & SERVICE_FLAG_PROCESS_SLOT_COMPAT) != 0) ? entry->process_slot : 0;
        return *endpoint_id != 0;
    }
    return 0;
}

static int install_compat_endpoint_if_needed(u64 endpoint_id, u64 process_slot) {
    if (process_slot == 0) return 0;
    return syscall3(SYSCALL_INSTALL_ENDPOINT, 0, endpoint_id, process_slot) == SYSCALL_OK;
}

static int is_ipc_buffer_token(u64 token) {
    return (token & ~IPC_BUFFER_TOKEN_MASK) == IPC_BUFFER_TOKEN_TAG && (token & IPC_BUFFER_TOKEN_MASK) != 0;
}

static u64 create_ipc_buffer_from_page(u64 paddr, u64 rights_bits, u64 role) {
    return syscall3(SYSCALL_CREATE_IPC_BUFFER_FROM_PAGE, paddr, rights_bits, role);
}

static u64 grant_ipc_buffer_on_endpoint(u64 token, u64 endpoint_id, u64 rights_bits, u64 process_slot) {
    u64 ret = syscall3(SYSCALL_GRANT_IPC_BUFFER_ON_ENDPOINT, token, endpoint_id, rights_bits);
    if (is_ipc_buffer_token(ret)) return ret;
    if (ret != SYSCALL_ERR_ENDPOINT) return 0;
    if (!install_compat_endpoint_if_needed(endpoint_id, process_slot)) return 0;
    ret = syscall3(SYSCALL_GRANT_IPC_BUFFER_ON_ENDPOINT, token, endpoint_id, rights_bits);
    return is_ipc_buffer_token(ret) ? ret : 0;
}

static int share_ipc_buffer_on_endpoint(u64 token, u64 endpoint_id, u64 rights_bits, u64 process_slot) {
    u64 ret = syscall3(SYSCALL_SHARE_IPC_BUFFER_ON_ENDPOINT, token, endpoint_id, rights_bits);
    if (ret == SYSCALL_OK) return 1;
    if (ret != SYSCALL_ERR_ENDPOINT) return 0;
    if (!install_compat_endpoint_if_needed(endpoint_id, process_slot)) return 0;
    return syscall3(SYSCALL_SHARE_IPC_BUFFER_ON_ENDPOINT, token, endpoint_id, rights_bits) == SYSCALL_OK;
}

static u64 make_session_nonce(u64 request_token, u64 response_token, u64 endpoint_id, u64 process_slot) {
    u64 nonce = request_token ^ ((response_token << 17) | (response_token >> 47)) ^
        ((endpoint_id << 7) | (endpoint_id >> 57)) ^ process_slot ^ 0x6d6f6b655f667331ULL;
    return nonce == 0 ? 1 : nonce;
}

static int wait_response(struct fs_smoke_client *client, u64 seq, u16 op) {
    volatile struct fs_response_header *response = (volatile struct fs_response_header *)client->response_va;
    for (u64 i = 0; i < RESPONSE_POLL_LIMIT; i++) {
        if (response->response_seq == seq) {
            return response->magic == FS_RESPONSE_MAGIC &&
                response->version == FS_PROTOCOL_VERSION &&
                response->op == op &&
                response->status == FS_STATUS_OK;
        }
        (void)syscall2(SYSCALL_WAIT_EVENT, 0, 1);
    }
    return 0;
}

static u64 begin_request(struct fs_smoke_client *client, u16 op, u64 object_token, u64 offset, u32 length, const char *path) {
    clear_page(client->request_va);
    volatile struct fs_request_header *request = (volatile struct fs_request_header *)client->request_va;
    const u16 path_len = path ? (u16)cstr_len(path) : 0;
    request->magic = FS_REQUEST_MAGIC;
    request->version = FS_PROTOCOL_VERSION;
    request->op = op;
    request->object_token = object_token;
    request->offset = offset;
    request->length = length;
    request->flags = 0;
    request->path_bytes = path_len;
    request->inline_bytes = 0;
    request->reserved0 = 0;
    request->arg0 = 0;
    request->arg1 = 0;
    request->session_nonce = client->session_nonce;
    if (path_len != 0) copy_to_volatile((volatile u8 *)(client->request_va + FS_REQUEST_HEADER_BYTES), (const u8 *)path, path_len);
    const u64 seq = client->next_seq++;
    __asm__ volatile("" ::: "memory");
    request->request_seq = seq;
    (void)syscall2(SYSCALL_SIGNAL_ENDPOINT, client->endpoint_id, 0);
    return seq;
}

static int connect_esp_fs(struct fs_smoke_client *client) {
    u64 endpoint_id = 0;
    u64 process_slot = 0;
    if (!find_esp_fs_service(&endpoint_id, &process_slot)) return 0;

    u64 paddrs[2] = {0, 0};
    const u64 request_va = syscall3(SYSCALL_ALLOC_MAP_PAGES_ANYWHERE, 2, 1, (u64)paddrs);
    const u64 response_va = request_va + FS_PAGE_BYTES;
    if (request_va < FS_PAGE_BYTES) return 0;
    if (paddrs[0] < 0x1000 || paddrs[1] < 0x1000) return 0;
    const u64 owner_rights = IPC_BUFFER_RIGHT_READ | IPC_BUFFER_RIGHT_WRITE | IPC_BUFFER_RIGHT_MAP | IPC_BUFFER_RIGHT_GRANT;
    const u64 request_token = create_ipc_buffer_from_page(paddrs[0], owner_rights, IPC_BUFFER_ROLE_REQUEST);
    const u64 response_token = create_ipc_buffer_from_page(paddrs[1], owner_rights, IPC_BUFFER_ROLE_RESPONSE);
    if (!is_ipc_buffer_token(request_token) || !is_ipc_buffer_token(response_token)) return 0;
    const u64 remote_response_token = grant_ipc_buffer_on_endpoint(
        response_token,
        endpoint_id,
        IPC_BUFFER_RIGHT_READ | IPC_BUFFER_RIGHT_WRITE | IPC_BUFFER_RIGHT_MAP,
        process_slot
    );
    if (!is_ipc_buffer_token(remote_response_token)) return 0;

    const u64 self_slot = syscall0(SYSCALL_GET_PROCESS_SLOT);
    const u64 nonce = make_session_nonce(request_token, response_token, endpoint_id, self_slot);
    clear_page(request_va);
    clear_page(response_va);
    volatile struct fs_request_header *request = (volatile struct fs_request_header *)request_va;
    request->magic = FS_REQUEST_MAGIC;
    request->version = FS_PROTOCOL_VERSION;
    request->op = FS_OP_CONNECT;
    request->request_seq = 1;
    request->arg0 = remote_response_token;
    request->arg1 = self_slot;
    request->session_nonce = nonce;

    if (!share_ipc_buffer_on_endpoint(request_token, endpoint_id, IPC_BUFFER_RIGHT_READ | IPC_BUFFER_RIGHT_MAP, process_slot)) return 0;
    client->request_va = request_va;
    client->response_va = response_va;
    if (!wait_response(client, 1, FS_OP_CONNECT)) return 0;

    volatile struct fs_response_header *response = (volatile struct fs_response_header *)client->response_va;
    client->endpoint_id = endpoint_id;
    client->server_process_slot = process_slot;
    client->request_paddr = paddrs[0];
    client->response_paddr = paddrs[1];
    client->request_token = request_token;
    client->response_token = response_token;
    client->session_nonce = nonce;
    client->mount_token = response->result_token;
    client->next_seq = 2;
    return client->mount_token != 0 && response->object_kind == FS_OBJECT_MOUNT;
}

void esp_server_smoke_main(void) {
    user_log("EspSmoke: started\n");
    struct fs_smoke_client client;
    if (!connect_esp_fs(&client)) {
        user_log("EspSmoke: connect ESP fs failed\n");
        return;
    }
    user_log("EspSmoke: connect ESP fs ok\n");

    u64 seq = begin_request(&client, FS_OP_LOOKUP, client.mount_token, 0, 0, "/EFI/BOOT/BOOTFS.IMG");
    if (!wait_response(&client, seq, FS_OP_LOOKUP)) {
        user_log("EspSmoke: lookup BOOTFS failed\n");
        return;
    }
    volatile struct fs_response_header *response = (volatile struct fs_response_header *)client.response_va;
    const u64 file_token = response->result_token;
    if (file_token == 0 || response->object_kind != FS_OBJECT_FILE) {
        user_log("EspSmoke: lookup BOOTFS invalid\n");
        return;
    }
    user_log("EspSmoke: lookup BOOTFS ok\n");

    seq = begin_request(&client, FS_OP_STAT, file_token, 0, 0, 0);
    if (!wait_response(&client, seq, FS_OP_STAT)) {
        user_log("EspSmoke: stat BOOTFS failed\n");
        return;
    }
    user_log("EspSmoke: stat BOOTFS ok\n");

    seq = begin_request(&client, FS_OP_OPEN, file_token, 0, 0, 0);
    if (!wait_response(&client, seq, FS_OP_OPEN)) {
        user_log("EspSmoke: open BOOTFS failed\n");
        return;
    }
    response = (volatile struct fs_response_header *)client.response_va;
    const u64 open_token = response->result_token;
    if (open_token == 0 || response->object_kind != FS_OBJECT_OPEN_FILE) {
        user_log("EspSmoke: open BOOTFS invalid\n");
        return;
    }
    user_log("EspSmoke: open BOOTFS ok\n");

    seq = begin_request(&client, FS_OP_READ, open_token, 0, 64, 0);
    if (!wait_response(&client, seq, FS_OP_READ)) {
        user_log("EspSmoke: read BOOTFS failed\n");
        return;
    }
    response = (volatile struct fs_response_header *)client.response_va;
    if (response->inline_bytes == 0) {
        user_log("EspSmoke: read BOOTFS empty\n");
        return;
    }
    user_log("EspSmoke: read BOOTFS ok\n");
}
