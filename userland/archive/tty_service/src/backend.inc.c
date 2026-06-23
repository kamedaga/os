static int find_service(u64 kind, struct service_entry *out) {
    volatile struct service_registry_page *page = (volatile struct service_registry_page *)SERVICE_REGISTRY_SHADOW_VA;
    if (page->magic != SERVICE_REGISTRY_MAGIC || page->version != SERVICE_REGISTRY_VERSION) return 0;
    for (u64 i = 0; i < page->entry_count && i < SERVICE_REGISTRY_MAX_ENTRIES; i++) {
        if (page->entries[i].kind != kind) continue;
        out->kind = page->entries[i].kind;
        out->process_slot = page->entries[i].process_slot;
        out->endpoint_id = page->entries[i].endpoint_id;
        out->flags = page->entries[i].flags;
        return out->endpoint_id != 0 && out->process_slot != 0;
    }
    return 0;
}

static int install_endpoint(u64 endpoint_id, u64 process_slot) {
    return syscall3(SYSCALL_INSTALL_ENDPOINT, 0, endpoint_id, process_slot) == SYSCALL_OK;
}

static int signal_endpoint(u64 endpoint_id) {
    return syscall2(SYSCALL_SIGNAL_ENDPOINT, endpoint_id, 0) == SYSCALL_OK;
}

static int create_console_ipc_buffers(void) {
    const u64 owner_rights = IPC_BUFFER_RIGHT_READ | IPC_BUFFER_RIGHT_WRITE | IPC_BUFFER_RIGHT_MAP | IPC_BUFFER_RIGHT_GRANT;
    g_console.request_token = create_ipc_buffer_from_page(g_console.request_paddr, owner_rights, IPC_BUFFER_ROLE_REQUEST);
    g_console.response_token = create_ipc_buffer_from_page(g_console.response_paddr, owner_rights, IPC_BUFFER_ROLE_RESPONSE);
    return is_ipc_buffer_token(g_console.request_token) && is_ipc_buffer_token(g_console.response_token);
}

static u64 grant_console_response_buffer(u64 endpoint_id, u64 process_slot) {
    const u64 rights = IPC_BUFFER_RIGHT_READ | IPC_BUFFER_RIGHT_WRITE | IPC_BUFFER_RIGHT_MAP;
    u64 token = grant_ipc_buffer_on_endpoint(g_console.response_token, endpoint_id, rights);
    if (is_ipc_buffer_token(token)) return token;
    if (install_endpoint(endpoint_id, process_slot)) {
        token = grant_ipc_buffer_on_endpoint(g_console.response_token, endpoint_id, rights);
    }
    return is_ipc_buffer_token(token) ? token : 0;
}

static int share_console_request_buffer(u64 endpoint_id, u64 process_slot) {
    const u64 rights = IPC_BUFFER_RIGHT_READ | IPC_BUFFER_RIGHT_MAP;
    u64 ret = share_ipc_buffer_on_endpoint(g_console.request_token, endpoint_id, rights);
    if (ret == SYSCALL_OK) return 1;
    if (install_endpoint(endpoint_id, process_slot)) ret = share_ipc_buffer_on_endpoint(g_console.request_token, endpoint_id, rights);
    return ret == SYSCALL_OK;
}

static u64 make_nonce(u64 request_token, u64 response_token, u64 endpoint_id, u64 process_slot) {
    u64 nonce = request_token ^ ((response_token << 17) | (response_token >> 47)) ^ ((endpoint_id << 7) | (endpoint_id >> 57)) ^ process_slot ^ 0x5454595345525631ULL;
    return nonce == 0 ? 1 : nonce;
}

static volatile struct console_request_header *request_at(u64 va) {
    return (volatile struct console_request_header *)va;
}

static volatile struct console_response_header *response_at(u64 va) {
    return (volatile struct console_response_header *)va;
}

static volatile u8 *payload_at(u64 va, u64 header_bytes) {
    return (volatile u8 *)(va + header_bytes);
}

static int wait_console_response(u64 expected_seq, u16 expected_op, u64 poll_limit) {
    volatile struct console_response_header *response = response_at(g_console.response_va);
    for (u64 i = 0; poll_limit == 0 || i < poll_limit; i++) {
        if (response->response_seq == expected_seq) {
            __sync_synchronize();
            return response->magic == CONSOLE_RESPONSE_MAGIC &&
                response->version == CONSOLE_PROTOCOL_VERSION &&
                response->op == expected_op;
        }
        (void)syscall2(SYSCALL_WAIT_EVENT, 0, 1);
    }
    return 0;
}

enum {
    TTY_BACKEND_TX_RING_BYTES = 1048576,
};

static u8 g_backend_tx_ring[TTY_BACKEND_TX_RING_BYTES];
static u64 g_backend_tx_head;
static u64 g_backend_tx_len;
static char g_backend_tx_flush_buf[CONSOLE_REQUEST_PAYLOAD_BYTES];
static u64 g_backend_tx_pending_seq;
static u64 g_backend_tx_pending_chunk;

static void backend_tx_drop_oldest(u64 bytes) {
    if (bytes > g_backend_tx_len) bytes = g_backend_tx_len;
    g_backend_tx_head = (g_backend_tx_head + bytes) % TTY_BACKEND_TX_RING_BYTES;
    g_backend_tx_len -= bytes;
}

static int backend_flush_output_once(void);
static void backend_flush_output_budget(u64 budget);

static int console_response_ready(u64 expected_seq, u16 expected_op) {
    volatile struct console_response_header *response = response_at(g_console.response_va);
    return response->response_seq == expected_seq &&
        response->magic == CONSOLE_RESPONSE_MAGIC &&
        response->version == CONSOLE_PROTOCOL_VERSION &&
        response->op == expected_op;
}

static int backend_console_request_pending(void) {
    return g_backend_tx_pending_seq != 0;
}

static int backend_tx_push_byte(u8 byte) {
    if (g_backend_tx_len >= TTY_BACKEND_TX_HIGH_WATER_BYTES) {
        (void)backend_flush_output_once();
        if (g_backend_tx_len >= TTY_BACKEND_TX_HIGH_WATER_BYTES) return 0;
    }
    if (g_backend_tx_len >= TTY_BACKEND_TX_RING_BYTES) {
        (void)backend_flush_output_once();
        if (g_backend_tx_len >= TTY_BACKEND_TX_RING_BYTES) return 0;
    }
    const u64 tail = (g_backend_tx_head + g_backend_tx_len) % TTY_BACKEND_TX_RING_BYTES;
    g_backend_tx_ring[tail] = byte;
    g_backend_tx_len++;
    return 1;
}

static u64 backend_tx_copy_head(char *dst, u64 max_len) {
    const u64 n = g_backend_tx_len < max_len ? g_backend_tx_len : max_len;
    for (u64 i = 0; i < n; i++) dst[i] = (char)g_backend_tx_ring[(g_backend_tx_head + i) % TTY_BACKEND_TX_RING_BYTES];
    return n;
}

static int console_begin_request(u16 op, u32 length, u32 flags, const char *inline_src, u32 inline_bytes, u64 *seq_out) {
    if (!g_console.active || inline_bytes > CONSOLE_REQUEST_PAYLOAD_BYTES) return 0;
    clear_page(g_console.request_va);
    clear_page(g_console.response_va);
    volatile struct console_request_header *request = request_at(g_console.request_va);
    const u64 seq = g_console.next_seq++;
    request->magic = CONSOLE_REQUEST_MAGIC;
    request->version = CONSOLE_PROTOCOL_VERSION;
    request->op = op;
    request->session_nonce = g_console.session_nonce;
    request->length = length;
    request->flags = flags;
    if (inline_bytes != 0) {
        volatile u8 *payload = payload_at(g_console.request_va, CONSOLE_REQUEST_HEADER_BYTES);
        for (u32 i = 0; i < inline_bytes; i++) payload[i] = (u8)inline_src[i];
    }
    __sync_synchronize();
    request->request_seq = seq;
    __sync_synchronize();
    if (!signal_endpoint(g_console.endpoint_id)) return 0;
    *seq_out = seq;
    return 1;
}

static int backend_connect_console(void) {
    struct service_entry entry;
    if (!find_service(SERVICE_KIND_CONSOLE, &entry)) return 0;
    g_console.endpoint_id = entry.endpoint_id;
    g_console.process_slot = entry.process_slot;
    g_console.request_paddr = syscall0(SYSCALL_ALLOC_PAGE);
    g_console.response_paddr = syscall0(SYSCALL_ALLOC_PAGE);
    if (g_console.request_paddr < PAGE_BYTES || g_console.response_paddr < PAGE_BYTES) return 0;
    g_console.request_va = map_page_anywhere(g_console.request_paddr, 1);
    g_console.response_va = map_page_anywhere(g_console.response_paddr, 1);
    if (g_console.request_va < PAGE_BYTES || g_console.response_va < PAGE_BYTES) return 0;
    if (!create_console_ipc_buffers()) return 0;
    const u64 remote_response_token = grant_console_response_buffer(g_console.endpoint_id, g_console.process_slot);
    if (!is_ipc_buffer_token(remote_response_token)) return 0;

    clear_page(g_console.request_va);
    clear_page(g_console.response_va);
    const u64 self_slot = syscall0(SYSCALL_GET_PROCESS_SLOT);
    g_console.session_nonce = make_nonce(g_console.request_token, g_console.response_token, g_console.endpoint_id, self_slot);
    volatile struct console_request_header *request = request_at(g_console.request_va);
    request->magic = CONSOLE_REQUEST_MAGIC;
    request->version = CONSOLE_PROTOCOL_VERSION;
    request->op = CONSOLE_OP_CONNECT;
    request->session_nonce = g_console.session_nonce;
    request->arg0 = remote_response_token;
    request->arg1 = self_slot;
    __sync_synchronize();
    request->request_seq = 1;
    __sync_synchronize();
    if (!share_console_request_buffer(g_console.endpoint_id, g_console.process_slot)) return 0;
    if (!wait_console_response(1, CONSOLE_OP_CONNECT, 8192)) return 0;
    volatile struct console_response_header *response = response_at(g_console.response_va);
    if (response->status != CONSOLE_STATUS_OK) return 0;
    g_console.next_seq = 2;
    g_console.active = 1;
    g_backend_tx_head = 0;
    g_backend_tx_len = 0;
    g_backend_tx_pending_seq = 0;
    g_backend_tx_pending_chunk = 0;
    user_log("TtyService: console connect ok\n");
    return 1;
}

static u64 backend_write_bytes(const char *bytes, u64 len) {
    for (u64 i = 0; i < len; i++) {
        if (!backend_tx_push_byte((u8)bytes[i])) return i;
    }
    return len;
}

static int backend_flush_output_once(void) {
    if (g_backend_tx_pending_seq != 0) {
        if (!console_response_ready(g_backend_tx_pending_seq, CONSOLE_OP_WRITE)) return 0;
        volatile struct console_response_header *response = response_at(g_console.response_va);
        const u64 chunk = g_backend_tx_pending_chunk;
        const u64 accepted = response->arg0 <= chunk ? response->arg0 : 0;
        if (accepted != 0) backend_tx_drop_oldest(accepted);
        g_backend_tx_pending_seq = 0;
        g_backend_tx_pending_chunk = 0;
        return accepted != 0 || response->status == CONSOLE_STATUS_OK || response->status == CONSOLE_STATUS_AGAIN;
    }
    if (g_backend_tx_len == 0) return 0;
    const u64 chunk = backend_tx_copy_head(g_backend_tx_flush_buf, CONSOLE_REQUEST_PAYLOAD_BYTES);
    if (chunk == 0) return 0;
    u64 seq = 0;
    if (!console_begin_request(CONSOLE_OP_WRITE, (u32)chunk, 0, g_backend_tx_flush_buf, (u32)chunk, &seq)) return 0;
    g_backend_tx_pending_seq = seq;
    g_backend_tx_pending_chunk = chunk;
    return 1;
}

static void backend_flush_output_budget(u64 budget) {
    for (u64 i = 0; i < budget; i++) {
        if (!backend_flush_output_once()) return;
    }
}

static int backend_output_pending(void) {
    return g_backend_tx_len != 0 || g_backend_tx_pending_seq != 0;
}

static int backend_write_ready(void) {
    backend_flush_output_budget(TTY_FLUSH_ACTIVE_BUDGET);
    return g_backend_tx_len < TTY_BACKEND_TX_HIGH_WATER_BYTES &&
        g_backend_tx_len < TTY_BACKEND_TX_RING_BYTES;
}

static int backend_get_winsize(u16 *columns_out, u16 *rows_out) {
    if (backend_console_request_pending()) return 0;
    u64 seq = 0;
    if (!console_begin_request(CONSOLE_OP_GET_ATTR, 0, 0, 0, 0, &seq)) return 0;
    if (!wait_console_response(seq, CONSOLE_OP_GET_ATTR, 8192)) return 0;
    volatile struct console_response_header *response = response_at(g_console.response_va);
    if (response->status != CONSOLE_STATUS_OK) return 0;
    if (response->arg0 == 0 || response->arg1 == 0 || response->arg0 > 0xffff || response->arg1 > 0xffff) return 0;
    *columns_out = (u16)response->arg0;
    *rows_out = (u16)response->arg1;
    return 1;
}

static u64 backend_try_read_raw(void) {
    if (backend_console_request_pending()) {
        (void)backend_flush_output_once();
        if (backend_console_request_pending()) return 0;
    }
    u64 seq = 0;
    if (!console_begin_request(CONSOLE_OP_READ, CONSOLE_RESPONSE_PAYLOAD_BYTES,
            CONSOLE_REQUEST_FLAG_NONBLOCK, 0, 0, &seq)) {
        return 0;
    }
    if (!wait_console_response(seq, CONSOLE_OP_READ, 8192)) return 0;
    volatile struct console_response_header *response = response_at(g_console.response_va);
    if (response->status == CONSOLE_STATUS_AGAIN) return 0;
    if (response->status != CONSOLE_STATUS_OK) return 0;
    return min_u64(response->inline_bytes, CONSOLE_RESPONSE_PAYLOAD_BYTES);
}
