static void write_client_response(u16 op, u64 seq, i32 status, u32 inline_bytes, u64 arg0, u64 arg1) {
    volatile struct console_response_header *response = response_at(TTY_CLIENT_RESPONSE_VA);
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
    __asm__ volatile("" ::: "memory");
    response->response_seq = seq;
    if (g_client.reply_endpoint_id != 0) (void)signal_endpoint(g_client.reply_endpoint_id);
}

static void handle_client_connect_transfer(u64 transfer_id) {
    const u64 request_paddr = syscall1(SYSCALL_ACCEPT_CAP_TRANSFER, transfer_id);
    if (request_paddr < PAGE_BYTES) return;
    if (syscall3(SYSCALL_MAP_PAGE, TTY_CLIENT_REQUEST_VA, request_paddr, 0) != SYSCALL_OK) return;
    volatile struct console_request_header *request = request_at(TTY_CLIENT_REQUEST_VA);
    if (request->magic != CONSOLE_REQUEST_MAGIC ||
        request->version != CONSOLE_PROTOCOL_VERSION ||
        request->op != CONSOLE_OP_CONNECT ||
        request->request_seq == 0 ||
        request->arg0 < PAGE_BYTES ||
        request->session_nonce == 0) {
        user_log("TtyService: invalid connect request\n");
        return;
    }
    if (syscall3(SYSCALL_MAP_PAGE, TTY_CLIENT_RESPONSE_VA, request->arg0, 1) != SYSCALL_OK) return;
    clear_page(TTY_CLIENT_RESPONSE_VA);
    g_client.active = 1;
    g_client.request_paddr = request_paddr;
    g_client.response_paddr = request->arg0;
    g_client.session_nonce = request->session_nonce;
    g_client.reply_endpoint_id = install_endpoint(TTY_REPLY_ENDPOINT_ID, request->arg1) ? TTY_REPLY_ENDPOINT_ID : 0;
    g_client.last_completed_seq = 0;
    tty_core_reset_session();
    write_client_response(CONSOLE_OP_CONNECT, request->request_seq, CONSOLE_STATUS_OK, 0, 0, 0);
    g_client.last_completed_seq = request->request_seq;
    user_log("TtyService: session connect ok\n");
}

static void handle_client_request(void) {
    if (!g_client.active) return;
    volatile struct console_request_header *request = request_at(TTY_CLIENT_REQUEST_VA);
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
        volatile u8 *payload = payload_at(TTY_CLIENT_REQUEST_VA, CONSOLE_REQUEST_HEADER_BYTES);
        const u64 done = tty_write_from_client_payload(payload, len);
        write_client_response(CONSOLE_OP_WRITE, seq, done == len ? CONSOLE_STATUS_OK : CONSOLE_STATUS_IO_ERROR, 0, done, 0);
    } else if (request->op == CONSOLE_OP_GET_ATTR) {
        volatile u8 *payload = payload_at(TTY_CLIENT_RESPONSE_VA, CONSOLE_RESPONSE_HEADER_BYTES);
        const u32 bytes = tty_export_attr(payload, CONSOLE_RESPONSE_PAYLOAD_BYTES);
        write_client_response(CONSOLE_OP_GET_ATTR, seq, CONSOLE_STATUS_OK, bytes, tty_columns(), tty_rows());
    } else if (request->op == CONSOLE_OP_SET_ATTR) {
        volatile u8 *payload = payload_at(TTY_CLIENT_REQUEST_VA, CONSOLE_REQUEST_HEADER_BYTES);
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
