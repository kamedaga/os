#include "block_client.h"

enum {
    SYSCALL_ALLOC_MAP_PAGES = 0x0C,
    SYSCALL_GRANT_CAP = 0x08,
    SYSCALL_GRANT_CAP_ON_ENDPOINT = 0x24,
    SYSCALL_SHARE_CAP = 0x2B,
    SYSCALL_SIGNAL_ENDPOINT = 0x2C,
    SYSCALL_GET_PROCESS_SLOT = 0x2E,
    SYSCALL_INSTALL_ENDPOINT = 0x26,
    SYSCALL_WAIT_EVENT = 0x17,
    SYSCALL_OK = 0,
    SYSCALL_ERR_ENDPOINT = 9,

    PAGE_RIGHT_CPU_READ = 0x1,
    PAGE_RIGHT_CPU_WRITE = 0x2,

    SERVICE_REGISTRY_PAGE_VA = 0x3C2C0000,
    BLOCK_REQUEST_VA = 0x23000000,
    BLOCK_RESPONSE_VA = 0x23001000,
    RESPONSE_POLL_LIMIT = 512,
};

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

static u64 syscall4(u64 nr, u64 arg0, u64 arg1, u64 arg2, u64 arg3) {
    register u64 rcx __asm__("rcx") = arg3;
    u64 ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret), "+c"(rcx)
        : "a"(nr), "D"(arg0), "S"(arg1), "d"(arg2)
        : "r8", "r9", "r10", "r11", "memory");
    return ret;
}

static void clear_page(u64 va) {
    volatile u64 *p = (volatile u64 *)va;
    for (u64 i = 0; i < 512; i++) p[i] = 0;
}

static void copy_bytes_from_volatile(const volatile u8 *src, u8 *dst, u64 len) {
    for (u64 i = 0; i < len; i++) dst[i] = src[i];
}

static int find_block_service(u64 *endpoint_id, u64 *process_slot) {
    volatile struct service_registry_page *page = (volatile struct service_registry_page *)SERVICE_REGISTRY_PAGE_VA;
    if (page->magic != SERVICE_REGISTRY_MAGIC || page->version != SERVICE_REGISTRY_VERSION) return 0;
    u64 count = page->entry_count;
    if (count > 8) count = 8;
    for (u64 i = 0; i < count; i++) {
        volatile struct service_registry_entry *entry = &page->entries[i];
        if (entry->kind != SERVICE_KIND_BLOCK) continue;
        if (entry->endpoint_id == 0) return 0;
        *endpoint_id = entry->endpoint_id;
        *process_slot = ((entry->flags & SERVICE_FLAG_PROCESS_SLOT_COMPAT) != 0) ? entry->process_slot : 0;
        return 1;
    }
    return 0;
}

static int install_compat_endpoint_if_needed(u64 endpoint_id, u64 process_slot) {
    if (process_slot == 0) return 0;
    return syscall3(SYSCALL_INSTALL_ENDPOINT, 0, endpoint_id, process_slot) == SYSCALL_OK;
}

static int grant_response_cap(u64 response_paddr, u64 endpoint_id, u64 process_slot) {
    u64 ret = syscall3(SYSCALL_GRANT_CAP_ON_ENDPOINT, response_paddr, endpoint_id, PAGE_RIGHT_CPU_READ | PAGE_RIGHT_CPU_WRITE);
    if (ret == SYSCALL_OK) return 1;
    if (ret != SYSCALL_ERR_ENDPOINT) return 0;
    if (!install_compat_endpoint_if_needed(endpoint_id, process_slot)) return 0;
    return syscall3(SYSCALL_GRANT_CAP_ON_ENDPOINT, response_paddr, endpoint_id, PAGE_RIGHT_CPU_READ | PAGE_RIGHT_CPU_WRITE) == SYSCALL_OK;
}

static int share_request_cap(u64 request_paddr, u64 endpoint_id, u64 process_slot) {
    u64 ret = syscall2(SYSCALL_SHARE_CAP, request_paddr, endpoint_id);
    if (ret == SYSCALL_OK) return 1;
    if (ret != SYSCALL_ERR_ENDPOINT) return 0;
    if (!install_compat_endpoint_if_needed(endpoint_id, process_slot)) return 0;
    return syscall2(SYSCALL_SHARE_CAP, request_paddr, endpoint_id) == SYSCALL_OK;
}

static u64 make_session_nonce(u64 request_paddr, u64 response_paddr, u64 endpoint_id, u64 process_slot) {
    u64 nonce = request_paddr ^ ((response_paddr << 17) | (response_paddr >> 47)) ^
        ((endpoint_id << 7) | (endpoint_id >> 57)) ^ process_slot ^ 0x517cc1b727220a95ULL;
    return nonce == 0 ? 1 : nonce;
}

static int wait_response(struct block_client *client, u64 seq, u16 op) {
    volatile struct block_response_header *response = (volatile struct block_response_header *)client->response_va;
    for (u64 i = 0; i < RESPONSE_POLL_LIMIT; i++) {
        if (response->response_seq == seq) {
            return response->magic == BLOCK_RESPONSE_MAGIC &&
                response->version == BLOCK_PROTOCOL_VERSION &&
                response->op == op &&
                response->status == BLOCK_STATUS_OK;
        }
        (void)syscall2(SYSCALL_WAIT_EVENT, 0, 1);
    }
    return 0;
}

static u64 begin_request(struct block_client *client, u16 op, u64 object_token, u64 block_index, u32 block_count) {
    clear_page(client->request_va);
    volatile struct block_request_header *request = (volatile struct block_request_header *)client->request_va;
    request->magic = BLOCK_REQUEST_MAGIC;
    request->version = BLOCK_PROTOCOL_VERSION;
    request->op = op;
    request->object_token = object_token;
    request->block_index = block_index;
    request->block_count = block_count;
    request->flags = 0;
    request->inline_bytes = 0;
    request->reserved0 = 0;
    request->reserved1 = 0;
    request->arg0 = 0;
    request->arg1 = 0;
    request->session_nonce = client->session_nonce;
    u64 seq = client->next_seq++;
    __asm__ volatile("" ::: "memory");
    request->request_seq = seq;
    (void)syscall2(SYSCALL_SIGNAL_ENDPOINT, client->endpoint_id, 0);
    return seq;
}

int block_client_connect(struct block_client *client) {
    u64 endpoint_id = 0;
    u64 process_slot = 0;
    if (!find_block_service(&endpoint_id, &process_slot)) return 0;

    u64 paddrs[2] = {0, 0};
    if (syscall4(SYSCALL_ALLOC_MAP_PAGES, BLOCK_REQUEST_VA, 2, 1, (u64)paddrs) != SYSCALL_OK) return 0;
    if (paddrs[0] < 0x1000 || paddrs[1] < 0x1000) return 0;
    if (!grant_response_cap(paddrs[1], endpoint_id, process_slot)) return 0;

    u64 self_slot = syscall0(SYSCALL_GET_PROCESS_SLOT);
    u64 nonce = make_session_nonce(paddrs[0], paddrs[1], endpoint_id, self_slot);

    clear_page(BLOCK_REQUEST_VA);
    clear_page(BLOCK_RESPONSE_VA);
    volatile struct block_request_header *request = (volatile struct block_request_header *)BLOCK_REQUEST_VA;
    request->magic = BLOCK_REQUEST_MAGIC;
    request->version = BLOCK_PROTOCOL_VERSION;
    request->op = BLOCK_OP_CONNECT;
    request->request_seq = 1;
    request->arg0 = paddrs[1];
    request->arg1 = self_slot;
    request->session_nonce = nonce;

    if (!share_request_cap(paddrs[0], endpoint_id, process_slot)) return 0;

    client->request_va = BLOCK_REQUEST_VA;
    client->response_va = BLOCK_RESPONSE_VA;
    client->request_paddr = paddrs[0];
    client->response_paddr = paddrs[1];
    client->endpoint_id = endpoint_id;
    client->server_process_slot = process_slot;
    client->session_nonce = nonce;
    client->next_seq = 2;

    if (!wait_response(client, 1, BLOCK_OP_CONNECT)) return 0;
    volatile struct block_response_header *response = (volatile struct block_response_header *)client->response_va;
    client->root_token = response->result_token;
    client->block_size = response->arg0;
    client->capacity_blocks = response->arg1;
    return client->root_token != 0 && client->block_size != 0 && client->capacity_blocks != 0;
}

int block_client_identify(struct block_client *client) {
    u64 seq = begin_request(client, BLOCK_OP_IDENTIFY, client->root_token, 0, 0);
    if (!wait_response(client, seq, BLOCK_OP_IDENTIFY)) return 0;
    volatile struct block_response_header *response = (volatile struct block_response_header *)client->response_va;
    client->block_size = response->arg0;
    client->capacity_blocks = response->arg1;
    return client->block_size != 0 && client->capacity_blocks != 0;
}

int block_client_read_one(struct block_client *client, u64 block_index, u8 *out, u64 out_len) {
    if (!out || out_len < client->block_size || client->block_size > BLOCK_PAGE_BYTES) return 0;
    u64 seq = begin_request(client, BLOCK_OP_READ_BLOCKS, client->root_token, block_index, 1);
    if (!wait_response(client, seq, BLOCK_OP_READ_BLOCKS)) return 0;
    volatile struct block_response_header *response = (volatile struct block_response_header *)client->response_va;
    if (response->inline_bytes != client->block_size) return 0;
    const volatile u8 *payload = (const volatile u8 *)(client->response_va + sizeof(struct block_response_header));
    copy_bytes_from_volatile(payload, out, client->block_size);
    return 1;
}
