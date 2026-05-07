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
    request->magic = FS_REQUEST_MAGIC; request->version = FS_PROTOCOL_VERSION; request->op = FS_OP_CONNECT; request->arg0 = g_vfs.response_paddr; request->arg1 = self_slot; request->session_nonce = g_vfs.session_nonce;
    __asm__ volatile("" ::: "memory");
    request->request_seq = 1;
    if (!share_vfs_request_page()) return 0;
    if (!wait_vfs_response(1, FS_OP_CONNECT)) return 0;
    volatile struct fs_response_header *response = (volatile struct fs_response_header *)VFS_RESPONSE_VA;
    if (response->status != FS_STATUS_OK || response->result_token == 0) return 0;
    g_vfs.root_token = response->result_token; g_vfs.next_seq = 2; g_vfs.active = 1;
    user_log("LinuxAbiServer: vfs connect ok\n");
    return 1;
}

static int vfs_request_full(u16 op, u64 token, u64 offset, u32 length, u32 flags, const char *path, u64 inline_src, u16 inline_bytes) {
    if (!g_vfs.active) return 0;
    if (inline_bytes > PAGE_BYTES - FS_REQUEST_HEADER_BYTES) return 0;
    g_prof.vfs_requests++;
    if (op < FS_PROFILE_OP_COUNT) g_prof.vfs_op_counts[op]++;
    if (op == FS_OP_READ) g_prof.vfs_read_request_bytes += length;
    if (op == FS_OP_WRITE) {
        g_prof.vfs_write_request_bytes += length;
        g_prof.vfs_inline_write_bytes += inline_bytes;
    }
    clear_page(VFS_REQUEST_VA); clear_page(VFS_RESPONSE_VA);
    volatile struct fs_request_header *request = (volatile struct fs_request_header *)VFS_REQUEST_VA;
    const u64 seq = g_vfs.next_seq++;
    request->magic = FS_REQUEST_MAGIC; request->version = FS_PROTOCOL_VERSION; request->op = op; request->object_token = token; request->offset = offset; request->length = length; request->flags = flags; request->session_nonce = g_vfs.session_nonce;
    u16 path_len = 0;
    if (path != 0) {
        u64 len = cstr_len(path);
        if (len > FS_MAX_PATH_BYTES) return 0;
        path_len = (u16)len;
        request->path_bytes = path_len;
        volatile u8 *payload = (volatile u8 *)(VFS_REQUEST_VA + FS_REQUEST_HEADER_BYTES);
        for (u64 i = 0; i < len; i++) payload[i] = (u8)path[i];
    }
    if (inline_bytes != 0) {
        if ((u64)path_len + inline_bytes > PAGE_BYTES - FS_REQUEST_HEADER_BYTES) return 0;
        request->inline_bytes = inline_bytes;
        volatile u8 *payload = (volatile u8 *)((u64)VFS_REQUEST_VA + FS_REQUEST_HEADER_BYTES + path_len);
        if (copy_from_target(inline_src, (void *)payload, inline_bytes) != inline_bytes) return 0;
    }
    __asm__ volatile("" ::: "memory");
    request->request_seq = seq;
    if (!signal_vfs()) return 0;
    return wait_vfs_response(seq, op);
}

static int vfs_request(u16 op, u64 token, u64 offset, u32 length, const char *path) {
    return vfs_request_full(op, token, offset, length, 0, path, 0, 0);
}

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
    if (!signal_vfs()) return 0;
    return wait_vfs_response(seq, FS_OP_RENAME);
}

static u64 vfs_write_from_target(u64 token, u64 offset, u64 src, u64 len, int *fault) {
    *fault = 0;
    u64 written = 0;
    while (written < len) {
        u64 chunk = min_u64(len - written, PAGE_BYTES - FS_REQUEST_HEADER_BYTES);
        if (chunk > 0xffff) chunk = 0xffff;
        if (!vfs_request_full(FS_OP_WRITE, token, offset + written, (u32)chunk, 0, 0, src + written, (u16)chunk)) {
            *fault = 1;
            return written;
        }
        volatile struct fs_response_header *response = (volatile struct fs_response_header *)VFS_RESPONSE_VA;
        if (response->status != FS_STATUS_OK) return written;
        written += chunk;
    }
    return written;
}

