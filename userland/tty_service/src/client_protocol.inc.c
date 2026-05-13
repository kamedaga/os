static void write_client_response(u16 op, u64 seq, i32 status, u32 inline_bytes, u64 arg0, u64 arg1) {
    volatile struct console_response_header *response = response_at(g_client.response_va);
    response->magic = CONSOLE_RESPONSE_MAGIC;
    response->version = CONSOLE_PROTOCOL_VERSION;
    response->op = op;
    response->status = status;
    response->result_flags = 0;
    response->inline_bytes = inline_bytes;
    response->reserved0 = 0;
    response->arg0 = arg0;
    response->arg1 = arg1;
    response->reserved1 = 0;
    response->reserved2 = 0;
    __sync_synchronize();
    response->response_seq = seq;
    __sync_synchronize();
    if (g_client.reply_endpoint_id != 0) (void)signal_endpoint(g_client.reply_endpoint_id);
}

static void finish_client_connect(
    volatile struct console_request_header *request,
    u64 request_va,
    u64 response_va,
    u64 request_paddr,
    u64 response_paddr,
    u64 request_token,
    u64 response_token
) {
    clear_page(response_va);
    g_client.active = 1;
    g_client.request_paddr = request_paddr;
    g_client.response_paddr = response_paddr;
    g_client.request_token = request_token;
    g_client.response_token = response_token;
    g_client.request_va = request_va;
    g_client.response_va = response_va;
    g_client.session_nonce = request->session_nonce;
    g_client.reply_endpoint_id = install_endpoint(TTY_REPLY_ENDPOINT_ID, request->arg1) ? TTY_REPLY_ENDPOINT_ID : 0;
    g_client.last_completed_seq = 0;
    tty_core_reset_session();
    write_client_response(CONSOLE_OP_CONNECT, request->request_seq, CONSOLE_STATUS_OK, 0, 0, 0);
    g_client.last_completed_seq = request->request_seq;
}

static int handle_client_connect_token(u64 request_token) {
    const u64 request_va = map_ipc_buffer_anywhere(request_token, 0);
    if (request_va < PAGE_BYTES) return 0;
    volatile struct console_request_header *request = request_at(request_va);
    if (request->magic != CONSOLE_REQUEST_MAGIC ||
        request->version != CONSOLE_PROTOCOL_VERSION ||
        request->op != CONSOLE_OP_CONNECT ||
        request->request_seq == 0 ||
        !is_ipc_buffer_token(request->arg0) ||
        request->session_nonce == 0) {
        user_log("TtyService: invalid ipc-buffer connect request\n");
        return 0;
    }
    const u64 response_token = request->arg0;
    const u64 response_va = map_ipc_buffer_anywhere(response_token, 1);
    if (response_va < PAGE_BYTES) return 0;
    finish_client_connect(request, request_va, response_va, 0, 0, request_token, response_token);
    user_log("TtyService: ipc-buffer session connect ok\n");
    return 1;
}

static void handle_client_connect_paddr_transfer(u64 transfer_id) {
    const u64 request_paddr = syscall1(SYSCALL_ACCEPT_CAP_TRANSFER, transfer_id);
    if (request_paddr < PAGE_BYTES) return;
    const u64 request_va = map_page_anywhere(request_paddr, 0);
    if (request_va < PAGE_BYTES) return;
    volatile struct console_request_header *request = request_at(request_va);
    if (request->magic != CONSOLE_REQUEST_MAGIC ||
        request->version != CONSOLE_PROTOCOL_VERSION ||
        request->op != CONSOLE_OP_CONNECT ||
        request->request_seq == 0 ||
        request->arg0 < PAGE_BYTES ||
        request->session_nonce == 0) {
        user_log("TtyService: invalid connect request\n");
        return;
    }
    const u64 response_va = map_page_anywhere(request->arg0, 1);
    if (response_va < PAGE_BYTES) return;
    finish_client_connect(request, request_va, response_va, request_paddr, request->arg0, 0, 0);
    user_log("TtyService: session connect ok\n");
}

static void handle_client_connect_transfer(u64 transfer_id) {
    const u64 request_token = accept_ipc_buffer_transfer(transfer_id);
    if (is_ipc_buffer_token(request_token) && handle_client_connect_token(request_token)) return;
    handle_client_connect_paddr_transfer(transfer_id);
}

static void handle_client_request(void) {
    if (!g_client.active) return;
    volatile struct console_request_header *request = request_at(g_client.request_va);
    if (request->magic != CONSOLE_REQUEST_MAGIC || request->version != CONSOLE_PROTOCOL_VERSION) return;
    const u64 seq = request->request_seq;
    if (seq == 0 || seq <= g_client.last_completed_seq) return;
    if (request->session_nonce != g_client.session_nonce) return;

    if (request->op == CONSOLE_OP_READ) {
        u8 signo = 0;
        const u64 n = tty_read_to_client_payload(request->length, &signo);
        if (signo != 0) {
            write_client_response(CONSOLE_OP_READ, seq, CONSOLE_STATUS_INTERRUPTED, 0, signo, 0);
        } else {
            write_client_response(CONSOLE_OP_READ, seq, CONSOLE_STATUS_OK, (u32)n, n, 0);
        }
    } else if (request->op == CONSOLE_OP_WRITE) {
        const u64 len = min_u64(request->length, CONSOLE_REQUEST_PAYLOAD_BYTES);
        volatile u8 *payload = payload_at(g_client.request_va, CONSOLE_REQUEST_HEADER_BYTES);
        const u64 done = tty_write_from_client_payload(payload, len);
        write_client_response(CONSOLE_OP_WRITE, seq, done == len ? CONSOLE_STATUS_OK : CONSOLE_STATUS_IO_ERROR, 0, done, 0);
    } else if (request->op == CONSOLE_OP_GET_ATTR) {
        volatile u8 *payload = payload_at(g_client.response_va, CONSOLE_RESPONSE_HEADER_BYTES);
        const u32 bytes = tty_export_attr(payload, CONSOLE_RESPONSE_PAYLOAD_BYTES);
        write_client_response(CONSOLE_OP_GET_ATTR, seq, CONSOLE_STATUS_OK, bytes, tty_columns(), tty_rows());
    } else if (request->op == CONSOLE_OP_SET_ATTR) {
        volatile u8 *payload = payload_at(g_client.request_va, CONSOLE_REQUEST_HEADER_BYTES);
        const u64 len = min_u64(request->length, CONSOLE_REQUEST_PAYLOAD_BYTES);
        write_client_response(CONSOLE_OP_SET_ATTR, seq, tty_import_attr(payload, len) ? CONSOLE_STATUS_OK : CONSOLE_STATUS_INVALID, 0, 0, 0);
    } else if (request->op == CONSOLE_OP_GET_SIGNAL) {
        const u8 signo = tty_take_signal();
        if (signo != 0) {
            write_client_response(CONSOLE_OP_GET_SIGNAL, seq, CONSOLE_STATUS_OK, 0, signo, 0);
        } else {
            write_client_response(CONSOLE_OP_GET_SIGNAL, seq, CONSOLE_STATUS_AGAIN, 0, 0, 0);
        }
    } else {
        write_client_response(request->op, seq, CONSOLE_STATUS_INVALID, 0, 0, 0);
    }
    g_client.last_completed_seq = seq;
}
