static int install_fs_endpoint(const struct vfs_client *client) {
    return syscall3(SYSCALL_INSTALL_ENDPOINT, 0, client->endpoint_id, client->process_slot) == SYSCALL_OK;
}

static u64 fs_request_addr(const struct vfs_client *client) { return client->request_map.addr; }
static u64 fs_response_addr(const struct vfs_client *client) { return client->response_map.addr; }
static u64 fs_bulk_addr(const struct vfs_client *client) { return client->bulk_map.addr; }
static u64 vfs_request_addr(void) { return fs_request_addr(&g_vfs); }
static u64 vfs_response_addr(void) { return fs_response_addr(&g_vfs); }
static u64 vfs_bulk_addr(void) { return fs_bulk_addr(&g_vfs); }
static void *vfs_response_payload(void) { return (void *)(vfs_response_addr() + FS_RESPONSE_HEADER_BYTES); }

static u64 grant_fs_response_buffer(struct vfs_client *client) {
    u64 ret = grant_ipc_buffer_on_endpoint(client->response_token, client->endpoint_id, IPC_BUFFER_RIGHT_READ | IPC_BUFFER_RIGHT_WRITE | IPC_BUFFER_RIGHT_MAP);
    if (is_ipc_buffer_token(ret)) return ret;
    if (ret == SYSCALL_ERR_ENDPOINT && install_fs_endpoint(client)) {
        ret = grant_ipc_buffer_on_endpoint(client->response_token, client->endpoint_id, IPC_BUFFER_RIGHT_READ | IPC_BUFFER_RIGHT_WRITE | IPC_BUFFER_RIGHT_MAP);
    }
    return is_ipc_buffer_token(ret) ? ret : 0;
}

static u64 grant_fs_bulk_buffer(struct vfs_client *client, u64 token) {
    const u64 rights = IPC_BUFFER_RIGHT_READ | IPC_BUFFER_RIGHT_WRITE | IPC_BUFFER_RIGHT_MAP | IPC_BUFFER_RIGHT_GRANT;
    u64 ret = grant_ipc_buffer_on_endpoint(token, client->endpoint_id, rights);
    if (is_ipc_buffer_token(ret)) return ret;
    if (ret == SYSCALL_ERR_ENDPOINT && install_fs_endpoint(client)) {
        ret = grant_ipc_buffer_on_endpoint(token, client->endpoint_id, rights);
    }
    return is_ipc_buffer_token(ret) ? ret : 0;
}

static u64 fs_bulk_page_count_for_length(const struct vfs_client *client, u32 length) {
    u64 page_count = (length + PAGE_BYTES - 1) / PAGE_BYTES;
    if (page_count > FS_BULK_READ_PAGE_COUNT) page_count = FS_BULK_READ_PAGE_COUNT;
    if (page_count > FS_BULK_READ_INITIAL_PAGE_COUNT) {
        u64 cap = client->bulk_page_count;
        if (cap == 0) cap = FS_BULK_READ_INITIAL_PAGE_COUNT;
        else if (cap < FS_BULK_READ_PAGE_COUNT) cap *= 2;
        if (cap > FS_BULK_READ_PAGE_COUNT) cap = FS_BULK_READ_PAGE_COUNT;
        if (page_count > cap) page_count = cap;
    }
    return page_count;
}

static int ensure_fs_bulk_pages(struct vfs_client *client, u64 page_count) {
    if (page_count == 0 || page_count > FS_BULK_READ_PAGE_COUNT) return 0;
    if (client->bulk_page_count >= page_count && client->bulk_map.addr != 0) return 1;
    const u64 start_tick = syscall0(SYSCALL_GET_TICK_COUNT);
    const u64 old_count = client->bulk_page_count;
    if (client->bulk_map.addr == 0) {
        const u64 mapped_addr = alloc_map_pages_anywhere(FS_BULK_READ_PAGE_COUNT, 1, (u64)client->bulk_paddrs);
        if (mapped_addr < PAGE_BYTES) return 0;
        client->bulk_map.addr = mapped_addr;
        client->bulk_map.page_count = FS_BULK_READ_PAGE_COUNT;
    } else if (client->bulk_map.page_count < FS_BULK_READ_PAGE_COUNT) {
        return 0;
    }
    for (u64 i = old_count; i < page_count; i++) {
        const u64 paddr = client->bulk_paddrs[i];
        if (paddr < 0x1000) return 0;
        const u64 owner_rights = IPC_BUFFER_RIGHT_READ | IPC_BUFFER_RIGHT_WRITE | IPC_BUFFER_RIGHT_MAP | IPC_BUFFER_RIGHT_GRANT;
        client->bulk_tokens[i] = create_ipc_buffer_from_page(paddr, owner_rights, IPC_BUFFER_ROLE_BULK);
        if (!is_ipc_buffer_token(client->bulk_tokens[i])) return 0;
        client->bulk_remote_tokens[i] = grant_fs_bulk_buffer(client, client->bulk_tokens[i]);
        if (!is_ipc_buffer_token(client->bulk_remote_tokens[i])) return 0;
        client->bulk_page_count = (u16)(i + 1);
    }
    if (client->bulk_page_count > old_count) {
        g_prof.vfs_bulk_cap_pages += client->bulk_page_count - old_count;
        g_prof.vfs_bulk_cap_ticks += syscall0(SYSCALL_GET_TICK_COUNT) - start_tick;
    }
    return 1;
}

static int share_fs_request_buffer(struct vfs_client *client) {
    u64 ret = share_ipc_buffer_on_endpoint(client->request_token, client->endpoint_id, IPC_BUFFER_RIGHT_READ | IPC_BUFFER_RIGHT_MAP);
    if (ret == SYSCALL_OK) return 1;
    if (ret == SYSCALL_ERR_ENDPOINT && install_fs_endpoint(client)) {
        ret = share_ipc_buffer_on_endpoint(client->request_token, client->endpoint_id, IPC_BUFFER_RIGHT_READ | IPC_BUFFER_RIGHT_MAP);
    }
    return ret == SYSCALL_OK;
}

static int signal_fs(struct vfs_client *client) {
    u64 ret = syscall2(SYSCALL_SIGNAL_ENDPOINT, client->endpoint_id, 0);
    if (ret == SYSCALL_OK) return 1;
    if (ret == SYSCALL_ERR_ENDPOINT && install_fs_endpoint(client)) {
        ret = syscall2(SYSCALL_SIGNAL_ENDPOINT, client->endpoint_id, 0);
    }
    return ret == SYSCALL_OK;
}

static int connect_fs_from_registry(u64 service_kind, struct vfs_client *client, const char *label) {
    struct service_entry entry;
    if (!find_service(service_kind, &entry)) return 0;
    client->endpoint_id = entry.endpoint_id;
    client->process_slot = entry.process_slot;
    client->request_paddr = syscall0(SYSCALL_ALLOC_PAGE);
    client->response_paddr = syscall0(SYSCALL_ALLOC_PAGE);
    if (client->request_paddr < 0x1000 || client->response_paddr < 0x1000) return 0;
    const u64 request_addr = map_page_anywhere(client->request_paddr, 1);
    const u64 response_addr = map_page_anywhere(client->response_paddr, 1);
    if (request_addr < PAGE_BYTES || response_addr < PAGE_BYTES) return 0;
    client->request_map.addr = request_addr;
    client->request_map.page_count = 1;
    client->response_map.addr = response_addr;
    client->response_map.page_count = 1;
    const u64 owner_rights = IPC_BUFFER_RIGHT_READ | IPC_BUFFER_RIGHT_WRITE | IPC_BUFFER_RIGHT_MAP | IPC_BUFFER_RIGHT_GRANT;
    client->request_token = create_ipc_buffer_from_page(client->request_paddr, owner_rights, IPC_BUFFER_ROLE_REQUEST);
    client->response_token = create_ipc_buffer_from_page(client->response_paddr, owner_rights, IPC_BUFFER_ROLE_RESPONSE);
    if (!is_ipc_buffer_token(client->request_token) || !is_ipc_buffer_token(client->response_token)) return 0;
    const u64 remote_response_token = grant_fs_response_buffer(client);
    if (!is_ipc_buffer_token(remote_response_token)) return 0;
    clear_page(request_addr);
    clear_page(response_addr);
    const u64 self_slot = syscall0(SYSCALL_GET_PROCESS_SLOT);
    client->session_nonce = make_nonce(client->request_token, client->response_token, client->endpoint_id, self_slot);
    volatile struct fs_request_header *request = (volatile struct fs_request_header *)request_addr;
    request->magic = FS_REQUEST_MAGIC; request->version = FS_PROTOCOL_VERSION; request->op = FS_OP_CONNECT; request->arg0 = remote_response_token; request->arg1 = self_slot; request->session_nonce = client->session_nonce;
    __asm__ volatile("" ::: "memory");
    request->request_seq = 1;
    if (!share_fs_request_buffer(client)) return 0;
    if (!wait_fs_response_at(response_addr, 1, FS_OP_CONNECT)) return 0;
    volatile struct fs_response_header *response = (volatile struct fs_response_header *)response_addr;
    if (response->status != FS_STATUS_OK || response->result_token == 0) return 0;
    client->root_token = response->result_token;
    client->next_seq = 2;
    client->active = 1;
    user_log(label);
    return 1;
}

static int connect_vfs_from_registry(void) {
    return connect_fs_from_registry(SERVICE_KIND_VFS, &g_vfs, "LinuxAbiServer: vfs connect ok\n");
}

static int fs_request_full(struct vfs_client *client, u16 op, u64 token, u64 offset, u32 length, u32 flags, const char *path, u64 inline_src, u16 inline_bytes) {
    if (!client->active) return 0;
    if (inline_bytes > PAGE_BYTES - FS_REQUEST_HEADER_BYTES) return 0;
    const u64 request_addr = fs_request_addr(client);
    const u64 response_addr = fs_response_addr(client);
    if (request_addr < PAGE_BYTES || response_addr < PAGE_BYTES) return 0;
    g_prof.vfs_requests++;
    if (op < FS_PROFILE_OP_COUNT) g_prof.vfs_op_counts[op]++;
    if (op == FS_OP_READ) g_prof.vfs_read_request_bytes += length;
    if (op == FS_OP_WRITE) {
        g_prof.vfs_write_request_bytes += length;
        g_prof.vfs_inline_write_bytes += inline_bytes;
    }
    const int trace_request = profile_trace_enabled();
    const u64 trace_start_tick = trace_request ? syscall0(SYSCALL_GET_TICK_COUNT) : 0;
    clear_page(request_addr);
    clear_page(response_addr);
    volatile struct fs_request_header *request = (volatile struct fs_request_header *)request_addr;
    const u64 seq = client->next_seq++;
    request->magic = FS_REQUEST_MAGIC; request->version = FS_PROTOCOL_VERSION; request->op = op; request->object_token = token; request->offset = offset; request->length = length; request->flags = flags; request->session_nonce = client->session_nonce;
    u16 path_len = 0;
    if (path != 0) {
        u64 len = cstr_len(path);
        if (len > FS_MAX_PATH_BYTES) return 0;
        path_len = (u16)len;
        request->path_bytes = path_len;
        volatile u8 *payload = (volatile u8 *)(request_addr + FS_REQUEST_HEADER_BYTES);
        for (u64 i = 0; i < len; i++) payload[i] = (u8)path[i];
    }
    if (inline_bytes != 0) {
        if ((u64)path_len + inline_bytes > PAGE_BYTES - FS_REQUEST_HEADER_BYTES) return 0;
        request->inline_bytes = inline_bytes;
        volatile u8 *payload = (volatile u8 *)(request_addr + FS_REQUEST_HEADER_BYTES + path_len);
        if (copy_from_target(inline_src, (void *)payload, inline_bytes) != inline_bytes) return 0;
    }
    __asm__ volatile("" ::: "memory");
    request->request_seq = seq;
    if (!signal_fs(client)) return 0;
    const int ok = wait_fs_response_at(response_addr, seq, op);
    if (trace_request) {
        const u64 trace_end_tick = syscall0(SYSCALL_GET_TICK_COUNT);
        user_log("LinuxAbiServer.trace tick=");
        user_log_dec_value(trace_end_tick);
        user_log(" event=vfs.request op=");
        user_log_dec_value(op);
        user_log(" seq=");
        user_log_dec_value(seq);
        user_log(" offset=");
        user_log_dec_value(offset);
        user_log(" length=");
        user_log_dec_value(length);
        user_log(" dt=");
        user_log_dec_value(trace_end_tick - trace_start_tick);
        user_log(" ok=");
        user_log_dec_value((u64)ok);
        user_log("\n");
    }
    return ok;
}

static int vfs_request_full(u16 op, u64 token, u64 offset, u32 length, u32 flags, const char *path, u64 inline_src, u16 inline_bytes) {
    return fs_request_full(&g_vfs, op, token, offset, length, flags, path, inline_src, inline_bytes);
}

static int vfs_request(u16 op, u64 token, u64 offset, u32 length, const char *path) {
    return vfs_request_full(op, token, offset, length, 0, path, 0, 0);
}

static int vfs_read_bulk_to_buffer(u64 token, u64 offset, u32 length, u64 dst_va, u64 *bytes_out) {
    *bytes_out = 0;
    if (length == 0) return 1;
    if (length > FS_BULK_READ_BYTES) length = FS_BULK_READ_BYTES;
    const u64 page_count = fs_bulk_page_count_for_length(&g_vfs, length);
    if (page_count == 0 || page_count > FS_BULK_READ_PAGE_COUNT) return 0;
    if (length > page_count * PAGE_BYTES) length = (u32)(page_count * PAGE_BYTES);
    if (!ensure_fs_bulk_pages(&g_vfs, page_count)) return 0;
    const u64 request_addr = vfs_request_addr();
    const u64 response_addr = vfs_response_addr();
    const u64 bulk_addr = vfs_bulk_addr();
    if (request_addr < PAGE_BYTES || response_addr < PAGE_BYTES || bulk_addr < PAGE_BYTES) return 0;

    g_prof.vfs_requests++;
    if (FS_OP_READ_BULK < FS_PROFILE_OP_COUNT) g_prof.vfs_op_counts[FS_OP_READ_BULK]++;
    g_prof.vfs_read_request_bytes += length;

    clear_page(request_addr);
    clear_page(response_addr);
    volatile struct fs_request_header *request = (volatile struct fs_request_header *)request_addr;
    const u64 seq = g_vfs.next_seq++;
    request->magic = FS_REQUEST_MAGIC;
    request->version = FS_PROTOCOL_VERSION;
    request->op = FS_OP_READ_BULK;
    request->object_token = token;
    request->offset = offset;
    request->length = length;
    request->flags = (u32)page_count;
    request->inline_bytes = (u16)(page_count * sizeof(u64));
    request->session_nonce = g_vfs.session_nonce;
    volatile u64 *payload = (volatile u64 *)(request_addr + FS_REQUEST_HEADER_BYTES);
    for (u64 i = 0; i < page_count; i++) payload[i] = g_vfs.bulk_remote_tokens[i];
    __asm__ volatile("" ::: "memory");
    request->request_seq = seq;
    const u64 request_start_tick = syscall0(SYSCALL_GET_TICK_COUNT);
    if (!signal_fs(&g_vfs)) return 0;
    if (!wait_fs_response_at(response_addr, seq, FS_OP_READ_BULK)) return 0;
    g_prof.vfs_bulk_request_ticks += syscall0(SYSCALL_GET_TICK_COUNT) - request_start_tick;

    volatile struct fs_response_header *response = (volatile struct fs_response_header *)response_addr;
    if (response->status != FS_STATUS_OK) return 0;
    const u64 bytes = response->arg0;
    if (bytes > length || bytes > FS_BULK_READ_BYTES) return 0;
    u8 *dst = (u8 *)dst_va;
    const volatile u8 *src = (const volatile u8 *)bulk_addr;
    const u64 copy_start_tick = syscall0(SYSCALL_GET_TICK_COUNT);
    for (u64 i = 0; i < bytes; i++) dst[i] = src[i];
    g_prof.vfs_bulk_copy_ticks += syscall0(SYSCALL_GET_TICK_COUNT) - copy_start_tick;
    *bytes_out = bytes;
    return 1;
}

static int vfs_read_bulk_to_target(u64 token, u64 offset, u32 length, u64 dst_va, u64 *bytes_out) {
    *bytes_out = 0;
    if (length == 0) return 1;
    if (length > FS_BULK_READ_BYTES) length = FS_BULK_READ_BYTES;
    const u64 page_count = fs_bulk_page_count_for_length(&g_vfs, length);
    if (page_count == 0 || page_count > FS_BULK_READ_PAGE_COUNT) return 0;
    if (length > page_count * PAGE_BYTES) length = (u32)(page_count * PAGE_BYTES);
    if (!ensure_fs_bulk_pages(&g_vfs, page_count)) return 0;
    const u64 request_addr = vfs_request_addr();
    const u64 response_addr = vfs_response_addr();
    const u64 bulk_addr = vfs_bulk_addr();
    if (request_addr < PAGE_BYTES || response_addr < PAGE_BYTES || bulk_addr < PAGE_BYTES) return 0;

    g_prof.vfs_requests++;
    if (FS_OP_READ_BULK < FS_PROFILE_OP_COUNT) g_prof.vfs_op_counts[FS_OP_READ_BULK]++;
    g_prof.vfs_read_request_bytes += length;

    clear_page(request_addr);
    clear_page(response_addr);
    volatile struct fs_request_header *request = (volatile struct fs_request_header *)request_addr;
    const u64 seq = g_vfs.next_seq++;
    request->magic = FS_REQUEST_MAGIC;
    request->version = FS_PROTOCOL_VERSION;
    request->op = FS_OP_READ_BULK;
    request->object_token = token;
    request->offset = offset;
    request->length = length;
    request->flags = (u32)page_count;
    request->inline_bytes = (u16)(page_count * sizeof(u64));
    request->session_nonce = g_vfs.session_nonce;
    volatile u64 *payload = (volatile u64 *)(request_addr + FS_REQUEST_HEADER_BYTES);
    for (u64 i = 0; i < page_count; i++) payload[i] = g_vfs.bulk_remote_tokens[i];
    __asm__ volatile("" ::: "memory");
    request->request_seq = seq;
    const u64 request_start_tick = syscall0(SYSCALL_GET_TICK_COUNT);
    if (!signal_fs(&g_vfs)) return 0;
    if (!wait_fs_response_at(response_addr, seq, FS_OP_READ_BULK)) return 0;
    g_prof.vfs_bulk_request_ticks += syscall0(SYSCALL_GET_TICK_COUNT) - request_start_tick;

    volatile struct fs_response_header *response = (volatile struct fs_response_header *)response_addr;
    if (response->status != FS_STATUS_OK) return 0;
    const u64 bytes = response->arg0;
    if (bytes > length || bytes > FS_BULK_READ_BYTES) return 0;
    const u64 copy_start_tick = syscall0(SYSCALL_GET_TICK_COUNT);
    if (copy_to_target_bulk(dst_va, (const void *)bulk_addr, bytes) != bytes) return 0;
    g_prof.vfs_bulk_copy_ticks += syscall0(SYSCALL_GET_TICK_COUNT) - copy_start_tick;
    *bytes_out = bytes;
    return 1;
}

static int vfs_read_bulk_direct_to_target(u64 token, u64 offset, u32 length, u64 dst_va, u64 *bytes_out) {
    g_prof.vfs_bulk_direct_attempts++;
    g_prof.vfs_bulk_direct_fallback_pages += fs_bulk_page_count_for_length(&g_vfs, length);
    return vfs_read_bulk_to_target(token, offset, length, dst_va, bytes_out);
}

static u64 g_last_vfs_write_status = FS_STATUS_OK;
static u64 g_last_vfs_write_offset = 0;
static u64 g_last_vfs_write_length = 0;

static int vfs_create_path_with_flags(const char *path, u32 flags) {
    return vfs_request_full(FS_OP_CREATE, g_vfs.root_token, 0, 0, flags, path, 0, 0);
}

static int vfs_create_path(const char *path, int truncate_existing) {
    return vfs_create_path_with_flags(path, truncate_existing ? FS_CREATE_FLAG_TRUNCATE : 0);
}

static int vfs_truncate_file(u64 token, u64 size) {
    return vfs_request_full(FS_OP_TRUNCATE, token, size, 0, 0, 0, 0, 0);
}

static int vfs_create_symlink_path(const char *path, const char *target, u16 target_len) {
    if (!g_vfs.active) return 0;
    const u64 path_len = cstr_len(path);
    if (path_len == 0 || path_len > FS_MAX_PATH_BYTES || target_len > FS_MAX_PATH_BYTES) return 0;
    if (path_len + target_len > PAGE_BYTES - FS_REQUEST_HEADER_BYTES) return 0;
    const u64 request_addr = vfs_request_addr();
    const u64 response_addr = vfs_response_addr();
    if (request_addr < PAGE_BYTES || response_addr < PAGE_BYTES) return 0;
    g_prof.vfs_requests++;
    if (FS_OP_CREATE < FS_PROFILE_OP_COUNT) g_prof.vfs_op_counts[FS_OP_CREATE]++;
    clear_page(request_addr);
    clear_page(response_addr);
    volatile struct fs_request_header *request = (volatile struct fs_request_header *)request_addr;
    const u64 seq = g_vfs.next_seq++;
    request->magic = FS_REQUEST_MAGIC;
    request->version = FS_PROTOCOL_VERSION;
    request->op = FS_OP_CREATE;
    request->object_token = g_vfs.root_token;
    request->flags = FS_CREATE_FLAG_SYMLINK;
    request->path_bytes = (u16)path_len;
    request->inline_bytes = target_len;
    request->session_nonce = g_vfs.session_nonce;
    volatile u8 *payload = (volatile u8 *)(request_addr + FS_REQUEST_HEADER_BYTES);
    for (u64 i = 0; i < path_len; i++) payload[i] = (u8)path[i];
    for (u16 i = 0; i < target_len; i++) payload[path_len + i] = (u8)target[i];
    __asm__ volatile("" ::: "memory");
    request->request_seq = seq;
    if (!signal_fs(&g_vfs)) return 0;
    return wait_vfs_response(seq, FS_OP_CREATE);
}

static int vfs_rename_paths(const char *old_path, const char *new_path) {
    if (!g_vfs.active) return 0;
    const u64 old_len = cstr_len(old_path);
    const u64 new_len = cstr_len(new_path);
    if (old_len == 0 || new_len == 0 || old_len > FS_MAX_PATH_BYTES || new_len > FS_MAX_PATH_BYTES) return 0;
    if (old_len + new_len > PAGE_BYTES - FS_REQUEST_HEADER_BYTES) return 0;
    const u64 request_addr = vfs_request_addr();
    const u64 response_addr = vfs_response_addr();
    if (request_addr < PAGE_BYTES || response_addr < PAGE_BYTES) return 0;
    g_prof.vfs_requests++;
    if (FS_OP_RENAME < FS_PROFILE_OP_COUNT) g_prof.vfs_op_counts[FS_OP_RENAME]++;
    clear_page(request_addr); clear_page(response_addr);
    volatile struct fs_request_header *request = (volatile struct fs_request_header *)request_addr;
    const u64 seq = g_vfs.next_seq++;
    request->magic = FS_REQUEST_MAGIC;
    request->version = FS_PROTOCOL_VERSION;
    request->op = FS_OP_RENAME;
    request->object_token = g_vfs.root_token;
    request->path_bytes = (u16)old_len;
    request->inline_bytes = (u16)new_len;
    request->session_nonce = g_vfs.session_nonce;
    volatile u8 *payload = (volatile u8 *)(request_addr + FS_REQUEST_HEADER_BYTES);
    for (u64 i = 0; i < old_len; i++) payload[i] = (u8)old_path[i];
    for (u64 i = 0; i < new_len; i++) payload[old_len + i] = (u8)new_path[i];
    __asm__ volatile("" ::: "memory");
    request->request_seq = seq;
    if (!signal_fs(&g_vfs)) return 0;
    return wait_vfs_response(seq, FS_OP_RENAME);
}

static int vfs_write_bulk_submit(u64 token, u64 offset, u32 length, u64 page_count, u64 *bytes_out) {
    *bytes_out = 0;
    if (length == 0) return 1;
    if (page_count == 0 || page_count > FS_BULK_READ_PAGE_COUNT) return 0;
    const u64 request_addr = vfs_request_addr();
    const u64 response_addr = vfs_response_addr();
    if (request_addr < PAGE_BYTES || response_addr < PAGE_BYTES) return 0;

    g_prof.vfs_requests++;
    if (FS_OP_WRITE_BULK < FS_PROFILE_OP_COUNT) g_prof.vfs_op_counts[FS_OP_WRITE_BULK]++;
    g_prof.vfs_write_request_bytes += length;

    clear_page(request_addr);
    clear_page(response_addr);
    volatile struct fs_request_header *request = (volatile struct fs_request_header *)request_addr;
    const u64 seq = g_vfs.next_seq++;
    request->magic = FS_REQUEST_MAGIC;
    request->version = FS_PROTOCOL_VERSION;
    request->op = FS_OP_WRITE_BULK;
    request->object_token = token;
    request->offset = offset;
    request->length = length;
    request->flags = (u32)page_count;
    request->inline_bytes = (u16)(page_count * sizeof(u64));
    request->session_nonce = g_vfs.session_nonce;
    volatile u64 *payload = (volatile u64 *)(request_addr + FS_REQUEST_HEADER_BYTES);
    for (u64 i = 0; i < page_count; i++) payload[i] = g_vfs.bulk_remote_tokens[i];
    __asm__ volatile("" ::: "memory");
    request->request_seq = seq;
    const u64 request_start_tick = syscall0(SYSCALL_GET_TICK_COUNT);
    if (!signal_fs(&g_vfs)) return 0;
    if (!wait_fs_response_at(response_addr, seq, FS_OP_WRITE_BULK)) return 0;
    g_prof.vfs_bulk_request_ticks += syscall0(SYSCALL_GET_TICK_COUNT) - request_start_tick;

    volatile struct fs_response_header *response = (volatile struct fs_response_header *)response_addr;
    if (response->status == FS_STATUS_NOT_SUPPORTED || response->status == FS_STATUS_INVALID) return 0;
    if (response->status != FS_STATUS_OK) {
        g_last_vfs_write_status = (u64)(u32)response->status;
        g_last_vfs_write_offset = offset;
        g_last_vfs_write_length = length;
        return 1;
    }
    const u64 bytes = response->arg0;
    if (bytes > length) return 0;
    *bytes_out = bytes;
    return 1;
}

static int vfs_write_bulk_from_target(u64 token, u64 offset, u64 src, u32 length, u64 *bytes_out, int *fault) {
    *bytes_out = 0;
    *fault = 0;
    if (length == 0) return 1;
    if (length > FS_BULK_READ_BYTES) length = FS_BULK_READ_BYTES;
    const u64 page_count = fs_bulk_page_count_for_length(&g_vfs, length);
    if (page_count == 0 || page_count > FS_BULK_READ_PAGE_COUNT) return 0;
    if (length > page_count * PAGE_BYTES) length = (u32)(page_count * PAGE_BYTES);
    if (!ensure_fs_bulk_pages(&g_vfs, page_count)) return 0;
    const u64 bulk_addr = vfs_bulk_addr();
    if (bulk_addr < PAGE_BYTES) return 0;

    const u64 copy_start_tick = syscall0(SYSCALL_GET_TICK_COUNT);
    u64 copied = 0;
    while (copied < length) {
        u64 chunk = length - copied;
        const u64 page_left = PAGE_BYTES - ((src + copied) & (PAGE_BYTES - 1));
        if (chunk > page_left) chunk = page_left;
        if (copy_from_target(src + copied, (void *)(bulk_addr + copied), chunk) != chunk) {
            *fault = 1;
            return 1;
        }
        copied += chunk;
    }
    g_prof.vfs_bulk_copy_ticks += syscall0(SYSCALL_GET_TICK_COUNT) - copy_start_tick;
    return vfs_write_bulk_submit(token, offset, length, page_count, bytes_out);
}

static int vfs_writev_bulk_from_target(u64 token, u64 offset, const u64 *pairs, u64 iovcnt, u64 *index_io, u64 *iov_offset_io, u64 *bytes_out, int *fault) {
    *bytes_out = 0;
    *fault = 0;
    if (*index_io >= iovcnt) return 1;
    if (!ensure_fs_bulk_pages(&g_vfs, FS_BULK_READ_PAGE_COUNT)) return 0;
    const u64 bulk_addr = vfs_bulk_addr();
    if (bulk_addr < PAGE_BYTES) return 0;

    u64 index = *index_io;
    u64 iov_offset = *iov_offset_io;
    u64 filled = 0;
    const u64 copy_start_tick = syscall0(SYSCALL_GET_TICK_COUNT);
    while (index < iovcnt && filled < FS_BULK_READ_BYTES) {
        const u64 src = pairs[index * 2];
        const u64 len = pairs[index * 2 + 1];
        if (iov_offset >= len) {
            index++;
            iov_offset = 0;
            continue;
        }
        u64 chunk = len - iov_offset;
        if (chunk > FS_BULK_READ_BYTES - filled) chunk = FS_BULK_READ_BYTES - filled;
        const u64 page_left = PAGE_BYTES - ((src + iov_offset) & (PAGE_BYTES - 1));
        if (chunk > page_left) chunk = page_left;
        if (copy_from_target(src + iov_offset, (void *)(bulk_addr + filled), chunk) != chunk) {
            *fault = 1;
            return 1;
        }
        filled += chunk;
        iov_offset += chunk;
    }
    g_prof.vfs_bulk_copy_ticks += syscall0(SYSCALL_GET_TICK_COUNT) - copy_start_tick;
    if (filled == 0) return 1;

    const u64 page_count = (filled + PAGE_BYTES - 1) / PAGE_BYTES;
    if (!vfs_write_bulk_submit(token, offset, (u32)filled, page_count, bytes_out)) return 0;

    u64 advance = *bytes_out;
    index = *index_io;
    iov_offset = *iov_offset_io;
    while (advance != 0 && index < iovcnt) {
        const u64 len = pairs[index * 2 + 1];
        const u64 available = iov_offset < len ? len - iov_offset : 0;
        if (advance < available) {
            iov_offset += advance;
            advance = 0;
            break;
        }
        advance -= available;
        index++;
        iov_offset = 0;
    }
    *index_io = index;
    *iov_offset_io = iov_offset;
    return 1;
}

static u64 vfs_write_from_target(u64 token, u64 offset, u64 src, u64 len, int *fault) {
    *fault = 0;
    g_last_vfs_write_status = FS_STATUS_OK;
    g_last_vfs_write_offset = offset;
    g_last_vfs_write_length = len;
    u64 written = 0;
    while (written < len) {
        if (len - written > PAGE_BYTES - FS_REQUEST_HEADER_BYTES) {
            u64 bulk_bytes = 0;
            int bulk_fault = 0;
            if (vfs_write_bulk_from_target(token, offset + written, src + written, (u32)min_u64(len - written, FS_BULK_READ_BYTES), &bulk_bytes, &bulk_fault)) {
                if (bulk_fault) {
                    *fault = 1;
                    return written;
                }
                if (bulk_bytes == 0) return written;
                written += bulk_bytes;
                continue;
            }
        }
        u64 chunk = min_u64(len - written, PAGE_BYTES - FS_REQUEST_HEADER_BYTES);
        if (chunk > 0xffff) chunk = 0xffff;
        if (!vfs_request_full(FS_OP_WRITE, token, offset + written, (u32)chunk, 0, 0, src + written, (u16)chunk)) {
            g_last_vfs_write_status = 0xffffffffffffffffULL;
            g_last_vfs_write_offset = offset + written;
            g_last_vfs_write_length = chunk;
            *fault = 1;
            return written;
        }
        volatile struct fs_response_header *response = (volatile struct fs_response_header *)vfs_response_addr();
        if (response->status != FS_STATUS_OK) {
            g_last_vfs_write_status = (u64)(u32)response->status;
            g_last_vfs_write_offset = offset + written;
            g_last_vfs_write_length = chunk;
            return written;
        }
        written += chunk;
    }
    return written;
}

static int vfs_write_inline_local(u64 token, u64 offset, const u8 *src, u16 len) {
    const u64 request_addr = vfs_request_addr();
    const u64 response_addr = vfs_response_addr();
    if (!g_vfs.active || request_addr < PAGE_BYTES || response_addr < PAGE_BYTES) return 0;
    if (len > PAGE_BYTES - FS_REQUEST_HEADER_BYTES) return 0;
    g_prof.vfs_requests++;
    if (FS_OP_WRITE < FS_PROFILE_OP_COUNT) g_prof.vfs_op_counts[FS_OP_WRITE]++;
    g_prof.vfs_write_request_bytes += len;
    g_prof.vfs_inline_write_bytes += len;
    clear_page(request_addr);
    clear_page(response_addr);
    volatile struct fs_request_header *request = (volatile struct fs_request_header *)request_addr;
    const u64 seq = g_vfs.next_seq++;
    request->magic = FS_REQUEST_MAGIC;
    request->version = FS_PROTOCOL_VERSION;
    request->op = FS_OP_WRITE;
    request->object_token = token;
    request->offset = offset;
    request->length = len;
    request->inline_bytes = len;
    request->session_nonce = g_vfs.session_nonce;
    volatile u8 *payload = (volatile u8 *)(request_addr + FS_REQUEST_HEADER_BYTES);
    for (u16 i = 0; i < len; i++) payload[i] = src[i];
    __asm__ volatile("" ::: "memory");
    request->request_seq = seq;
    if (!signal_fs(&g_vfs)) return 0;
    return wait_fs_response_at(response_addr, seq, FS_OP_WRITE);
}
