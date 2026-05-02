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
    request->magic = FS_REQUEST_MAGIC; request->version = FS_PROTOCOL_VERSION; request->op = FS_OP_CONNECT; request->request_seq = 1; request->arg0 = g_vfs.response_paddr; request->arg1 = self_slot; request->session_nonce = g_vfs.session_nonce;
    if (!share_vfs_request_page()) return 0;
    if (!wait_vfs_response(1, FS_OP_CONNECT)) return 0;
    volatile struct fs_response_header *response = (volatile struct fs_response_header *)VFS_RESPONSE_VA;
    if (response->status != FS_STATUS_OK || response->result_token == 0) return 0;
    g_vfs.root_token = response->result_token; g_vfs.next_seq = 2; g_vfs.active = 1;
    user_log("LinuxAbiServer: vfs connect ok\n");
    return 1;
}

static int vfs_request(u16 op, u64 token, u64 offset, u32 length, const char *path) {
    if (!g_vfs.active) return 0;
    clear_page(VFS_REQUEST_VA); clear_page(VFS_RESPONSE_VA);
    volatile struct fs_request_header *request = (volatile struct fs_request_header *)VFS_REQUEST_VA;
    const u64 seq = g_vfs.next_seq++;
    request->magic = FS_REQUEST_MAGIC; request->version = FS_PROTOCOL_VERSION; request->op = op; request->request_seq = seq; request->object_token = token; request->offset = offset; request->length = length; request->session_nonce = g_vfs.session_nonce;
    if (path != 0) {
        u64 len = cstr_len(path);
        if (len > FS_MAX_PATH_BYTES) return 0;
        request->path_bytes = (u16)len;
        volatile u8 *payload = (volatile u8 *)(VFS_REQUEST_VA + FS_REQUEST_HEADER_BYTES);
        for (u64 i = 0; i < len; i++) payload[i] = (u8)path[i];
    }
    if (!signal_vfs()) return 0;
    return wait_vfs_response(seq, op);
}

