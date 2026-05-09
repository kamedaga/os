static int install_fs_endpoint(const struct vfs_client *client) {
    return syscall3(SYSCALL_INSTALL_ENDPOINT, 0, client->endpoint_id, client->process_slot) == SYSCALL_OK;
}

static int grant_fs_response_page(struct vfs_client *client) {
    u64 ret = syscall3(SYSCALL_GRANT_CAP_ON_ENDPOINT, client->response_paddr, client->endpoint_id, PAGE_RIGHT_CPU_READ | PAGE_RIGHT_CPU_WRITE);
    if (ret == SYSCALL_OK) return 1;
    if (ret == SYSCALL_ERR_ENDPOINT && install_fs_endpoint(client)) {
        ret = syscall3(SYSCALL_GRANT_CAP_ON_ENDPOINT, client->response_paddr, client->endpoint_id, PAGE_RIGHT_CPU_READ | PAGE_RIGHT_CPU_WRITE);
    }
    return ret == SYSCALL_OK;
}

static int share_fs_request_page(struct vfs_client *client) {
    u64 ret = syscall2(SYSCALL_SHARE_CAP, client->request_paddr, client->endpoint_id);
    if (ret == SYSCALL_OK) return 1;
    if (ret == SYSCALL_ERR_ENDPOINT && install_fs_endpoint(client)) {
        ret = syscall2(SYSCALL_SHARE_CAP, client->request_paddr, client->endpoint_id);
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

static int connect_fs_from_registry(u64 service_kind, struct vfs_client *client, u64 request_va, u64 response_va, const char *label) {
    struct service_entry entry;
    if (!find_service(service_kind, &entry)) return 0;
    client->endpoint_id = entry.endpoint_id;
    client->process_slot = entry.process_slot;
    client->request_paddr = syscall0(SYSCALL_ALLOC_PAGE);
    client->response_paddr = syscall0(SYSCALL_ALLOC_PAGE);
    if (client->request_paddr < 0x1000 || client->response_paddr < 0x1000) return 0;
    if (syscall3(SYSCALL_MAP_PAGE, request_va, client->request_paddr, 1) != SYSCALL_OK) return 0;
    if (syscall3(SYSCALL_MAP_PAGE, response_va, client->response_paddr, 1) != SYSCALL_OK) return 0;
    if (!grant_fs_response_page(client)) return 0;
    clear_page(request_va);
    clear_page(response_va);
    const u64 self_slot = syscall0(SYSCALL_GET_PROCESS_SLOT);
    client->session_nonce = make_nonce(client->request_paddr, client->response_paddr, client->endpoint_id, self_slot);
    volatile struct fs_request_header *request = (volatile struct fs_request_header *)request_va;
    request->magic = FS_REQUEST_MAGIC; request->version = FS_PROTOCOL_VERSION; request->op = FS_OP_CONNECT; request->arg0 = client->response_paddr; request->arg1 = self_slot; request->session_nonce = client->session_nonce;
    __asm__ volatile("" ::: "memory");
    request->request_seq = 1;
    if (!share_fs_request_page(client)) return 0;
    if (!wait_fs_response_at(response_va, 1, FS_OP_CONNECT)) return 0;
    volatile struct fs_response_header *response = (volatile struct fs_response_header *)response_va;
    if (response->status != FS_STATUS_OK || response->result_token == 0) return 0;
    client->root_token = response->result_token;
    client->next_seq = 2;
    client->active = 1;
    user_log(label);
    return 1;
}

static int connect_vfs_from_registry(void) {
    return connect_fs_from_registry(SERVICE_KIND_VFS, &g_vfs, VFS_REQUEST_VA, VFS_RESPONSE_VA, "LinuxAbiServer: vfs connect ok\n");
}

static int fs_request_full(struct vfs_client *client, u64 request_va, u64 response_va, u16 op, u64 token, u64 offset, u32 length, u32 flags, const char *path, u64 inline_src, u16 inline_bytes) {
    if (!client->active) return 0;
    if (inline_bytes > PAGE_BYTES - FS_REQUEST_HEADER_BYTES) return 0;
    g_prof.vfs_requests++;
    if (op < FS_PROFILE_OP_COUNT) g_prof.vfs_op_counts[op]++;
    if (op == FS_OP_READ) g_prof.vfs_read_request_bytes += length;
    if (op == FS_OP_WRITE) {
        g_prof.vfs_write_request_bytes += length;
        g_prof.vfs_inline_write_bytes += inline_bytes;
    }
    clear_page(request_va);
    clear_page(response_va);
    volatile struct fs_request_header *request = (volatile struct fs_request_header *)request_va;
    const u64 seq = client->next_seq++;
    request->magic = FS_REQUEST_MAGIC; request->version = FS_PROTOCOL_VERSION; request->op = op; request->object_token = token; request->offset = offset; request->length = length; request->flags = flags; request->session_nonce = client->session_nonce;
    u16 path_len = 0;
    if (path != 0) {
        u64 len = cstr_len(path);
        if (len > FS_MAX_PATH_BYTES) return 0;
        path_len = (u16)len;
        request->path_bytes = path_len;
        volatile u8 *payload = (volatile u8 *)(request_va + FS_REQUEST_HEADER_BYTES);
        for (u64 i = 0; i < len; i++) payload[i] = (u8)path[i];
    }
    if (inline_bytes != 0) {
        if ((u64)path_len + inline_bytes > PAGE_BYTES - FS_REQUEST_HEADER_BYTES) return 0;
        request->inline_bytes = inline_bytes;
        volatile u8 *payload = (volatile u8 *)(request_va + FS_REQUEST_HEADER_BYTES + path_len);
        if (copy_from_target(inline_src, (void *)payload, inline_bytes) != inline_bytes) return 0;
    }
    __asm__ volatile("" ::: "memory");
    request->request_seq = seq;
    if (!signal_fs(client)) return 0;
    return wait_fs_response_at(response_va, seq, op);
}

static int vfs_request_full(u16 op, u64 token, u64 offset, u32 length, u32 flags, const char *path, u64 inline_src, u16 inline_bytes) {
    return fs_request_full(&g_vfs, VFS_REQUEST_VA, VFS_RESPONSE_VA, op, token, offset, length, flags, path, inline_src, inline_bytes);
}

static int vfs_request(u16 op, u64 token, u64 offset, u32 length, const char *path) {
    return vfs_request_full(op, token, offset, length, 0, path, 0, 0);
}

static u64 g_last_vfs_write_status = FS_STATUS_OK;
static u64 g_last_vfs_write_offset = 0;
static u64 g_last_vfs_write_length = 0;

static int vfs_create_path(const char *path, int truncate_existing) {
    const u32 flags = truncate_existing ? FS_CREATE_FLAG_TRUNCATE : 0;
    return vfs_request_full(FS_OP_CREATE, g_vfs.root_token, 0, 0, flags, path, 0, 0);
}

static int vfs_rename_paths(const char *old_path, const char *new_path) {
    if (!g_vfs.active) return 0;
    const u64 old_len = cstr_len(old_path);
    const u64 new_len = cstr_len(new_path);
    if (old_len == 0 || new_len == 0 || old_len > FS_MAX_PATH_BYTES || new_len > FS_MAX_PATH_BYTES) return 0;
    if (old_len + new_len > PAGE_BYTES - FS_REQUEST_HEADER_BYTES) return 0;
    g_prof.vfs_requests++;
    if (FS_OP_RENAME < FS_PROFILE_OP_COUNT) g_prof.vfs_op_counts[FS_OP_RENAME]++;
    clear_page(VFS_REQUEST_VA); clear_page(VFS_RESPONSE_VA);
    volatile struct fs_request_header *request = (volatile struct fs_request_header *)VFS_REQUEST_VA;
    const u64 seq = g_vfs.next_seq++;
    request->magic = FS_REQUEST_MAGIC;
    request->version = FS_PROTOCOL_VERSION;
    request->op = FS_OP_RENAME;
    request->object_token = g_vfs.root_token;
    request->path_bytes = (u16)old_len;
    request->inline_bytes = (u16)new_len;
    request->session_nonce = g_vfs.session_nonce;
    volatile u8 *payload = (volatile u8 *)(VFS_REQUEST_VA + FS_REQUEST_HEADER_BYTES);
    for (u64 i = 0; i < old_len; i++) payload[i] = (u8)old_path[i];
    for (u64 i = 0; i < new_len; i++) payload[old_len + i] = (u8)new_path[i];
    __asm__ volatile("" ::: "memory");
    request->request_seq = seq;
    if (!signal_fs(&g_vfs)) return 0;
    return wait_vfs_response(seq, FS_OP_RENAME);
}

static u64 vfs_write_from_target(u64 token, u64 offset, u64 src, u64 len, int *fault) {
    *fault = 0;
    g_last_vfs_write_status = FS_STATUS_OK;
    g_last_vfs_write_offset = offset;
    g_last_vfs_write_length = len;
    u64 written = 0;
    while (written < len) {
        u64 chunk = min_u64(len - written, PAGE_BYTES - FS_REQUEST_HEADER_BYTES);
        if (chunk > 0xffff) chunk = 0xffff;
        if (!vfs_request_full(FS_OP_WRITE, token, offset + written, (u32)chunk, 0, 0, src + written, (u16)chunk)) {
            g_last_vfs_write_status = 0xffffffffffffffffULL;
            g_last_vfs_write_offset = offset + written;
            g_last_vfs_write_length = chunk;
            *fault = 1;
            return written;
        }
        volatile struct fs_response_header *response = (volatile struct fs_response_header *)VFS_RESPONSE_VA;
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

