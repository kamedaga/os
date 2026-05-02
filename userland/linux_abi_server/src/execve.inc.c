static int vfs_lookup_stat(const char *path, u64 *token_out, struct fs_stat_record *stat_out, u64 *file_bytes_out, u8 *kind_out) {
    if (!vfs_request(FS_OP_LOOKUP, g_vfs.root_token, 0, 0, path)) { user_log("LinuxAbiServer: lookup request failed\n"); return 0; }
    volatile struct fs_response_header *response = (volatile struct fs_response_header *)VFS_RESPONSE_VA;
    if (response->status != FS_STATUS_OK || response->result_token == 0) { user_log("LinuxAbiServer: lookup status failed\n"); user_log_hex_value((u64)(u32)response->status); return 0; }
    const u64 token = response->result_token;
    const u8 lookup_kind = response->object_kind;
    const u64 lookup_file_bytes = response->file_bytes;
    if (!vfs_request(FS_OP_STAT, token, 0, 0, 0)) { user_log("LinuxAbiServer: stat request failed\n"); return 0; }
    response = (volatile struct fs_response_header *)VFS_RESPONSE_VA;
    if (response->status == FS_STATUS_OK && response->inline_bytes >= FS_STAT_RECORD_BYTES) {
        volatile struct fs_stat_record *record = (volatile struct fs_stat_record *)(VFS_RESPONSE_VA + FS_RESPONSE_HEADER_BYTES);
        token_out[0] = token; stat_out->object_kind = record->object_kind; stat_out->size_bytes = record->size_bytes; stat_out->mode_bits = record->mode_bits; stat_out->mtime_unix_sec = record->mtime_unix_sec;
        *file_bytes_out = record->size_bytes; *kind_out = response->object_kind != FS_OBJECT_NONE ? response->object_kind : record->object_kind;
        return 1;
    }
    if (response->status != FS_STATUS_OK || lookup_kind == FS_OBJECT_NONE) { user_log("LinuxAbiServer: stat status failed\n"); user_log_hex_value((u64)(u32)response->status); return 0; }
    token_out[0] = token;
    stat_out->object_kind = lookup_kind;
    stat_out->size_bytes = lookup_file_bytes;
    stat_out->mode_bits = (lookup_kind == FS_OBJECT_DIRECTORY || lookup_kind == FS_OBJECT_MOUNT) ? FS_DIR_MODE : FS_FILE_MODE;
    stat_out->mtime_unix_sec = 0;
    *file_bytes_out = lookup_file_bytes;
    *kind_out = lookup_kind;
    return 1;
}

static int vfs_lookup_file_token(const char *path, u64 *token_out, u64 *file_bytes_out) {
    if (!vfs_request(FS_OP_LOOKUP, g_vfs.root_token, 0, 0, path)) return 0;
    volatile struct fs_response_header *response = (volatile struct fs_response_header *)VFS_RESPONSE_VA;
    if (response->status != FS_STATUS_OK || response->result_token == 0) return 0;
    const u8 kind = response->object_kind;
    if (kind != FS_OBJECT_FILE) return 0;
    *token_out = response->result_token;
    *file_bytes_out = response->file_bytes;
    return response->file_bytes != 0;
}

static int is_vm_object_token(u64 token) { return (token & EXEC_IMAGE_TOKEN_TAG) != EXEC_IMAGE_TOKEN_TAG && (token & VM_OBJECT_TOKEN_TAG) != 0 && (token & ~VM_OBJECT_TOKEN_TAG) != 0; }
static int is_exec_image_token(u64 token) { return (token & EXEC_IMAGE_TOKEN_TAG) == EXEC_IMAGE_TOKEN_TAG && (token & ~EXEC_IMAGE_TOKEN_TAG) != 0; }
static u64 decode_spawned_process_slot(u64 value) { if ((value & SPAWN_RESULT_TAG) == 0) return 0; return value & SPAWN_RESULT_PROCESS_MASK; }

static int alloc_map_range_self(u64 base_va, u64 page_count, u64 writable) {
    u64 done = 0;
    while (done < page_count) {
        const u64 chunk = min_u64(page_count - done, 64);
        if (alloc_map_pages(base_va + done * PAGE_BYTES, chunk, writable) != SYSCALL_OK) return 0;
        done += chunk;
    }
    return 1;
}

static int ensure_execve_scratch(void) {
    if (execve_scratch_ready) return 1;
    if (!alloc_map_range_self(EXECVE_MAIN_IMAGE_VA, EXECVE_MAX_IMAGE_BYTES / PAGE_BYTES, 1)) return 0;
    if (!alloc_map_range_self(EXECVE_LD_IMAGE_VA, EXECVE_MAX_LD_BYTES / PAGE_BYTES, 1)) return 0;
    if (!alloc_map_range_self(EXECVE_CONFIG_VA, 1, 1)) return 0;
    if (!alloc_map_range_self(EXECVE_TABLE_VA, 1, 1)) return 0;
    execve_scratch_ready = 1;
    return 1;
}

static int vfs_read_file_to_buffer(const char *path, u64 buffer_va, u64 buffer_cap, u64 *file_bytes_out) {
    u64 file_token = 0; u64 file_bytes = 0;
    if (!vfs_lookup_file_token(path, &file_token, &file_bytes)) { user_log("LinuxAbiServer: vfs read lookup failed\n"); return 0; }
    if (file_bytes == 0 || file_bytes > buffer_cap) { user_log("LinuxAbiServer: vfs read stat invalid\n"); user_log_hex_value(file_bytes); return 0; }
    if (!vfs_request(FS_OP_OPEN, file_token, 0, 0, 0)) { user_log("LinuxAbiServer: vfs read open request failed\n"); return 0; }
    volatile struct fs_response_header *response = (volatile struct fs_response_header *)VFS_RESPONSE_VA;
    if (response->status != FS_STATUS_OK || response->result_token == 0) { user_log("LinuxAbiServer: vfs read open failed\n"); user_log_hex_value((u64)(u32)response->status); return 0; }
    const u64 open_token = response->result_token;
    u64 copied = 0;
    int ok = 1;
    while (copied < file_bytes) {
        const u64 chunk = min_u64(file_bytes - copied, FS_RESPONSE_PAYLOAD_BYTES);
        if (!vfs_request(FS_OP_READ, open_token, copied, (u32)chunk, 0)) { user_log("LinuxAbiServer: vfs read request failed\n"); ok = 0; break; }
        response = (volatile struct fs_response_header *)VFS_RESPONSE_VA;
        if (response->status != FS_STATUS_OK || response->inline_bytes == 0) { user_log("LinuxAbiServer: vfs read chunk failed\n"); user_log_hex_value((u64)(u32)response->status); ok = 0; break; }
        volatile u8 *src = (volatile u8 *)(VFS_RESPONSE_VA + FS_RESPONSE_HEADER_BYTES);
        u8 *dst = (u8 *)(buffer_va + copied);
        for (u64 i = 0; i < response->inline_bytes; i++) dst[i] = src[i];
        copied += response->inline_bytes;
    }
    (void)vfs_request(FS_OP_CLOSE, open_token, 0, 0, 0);
    if (!ok) return 0;
    *file_bytes_out = file_bytes;
    return 1;
}

static u64 install_vm_object_from_buffer(u64 buffer_va, u64 file_bytes) {
    const u64 token = syscall3(SYSCALL_INSTALL_VM_OBJECT, buffer_va, file_bytes, VM_RIGHT_READ_MAP_GRANT);
    return is_vm_object_token(token) ? token : 0;
}

static u64 install_exec_image_from_vm(u64 vm_token) {
    const u64 token = syscall2(SYSCALL_INSTALL_EXEC_IMAGE, vm_token, EXEC_RIGHT_EXEC_GRANT);
    return is_exec_image_token(token) ? token : 0;
}

static void reset_exec_runtime_state(void) {
    if (!g_proc) return;
    g_mmap_next_va = 0x31000000ULL;
    g_brk_next_va = 0x38000000ULL;
    for (u64 i = 0; i < VM_REGION_MAX; i++) g_regions[i].used = 0;
}

static int append_exec_arg(struct exec_loader_config *cfg, u16 *cursor, const char *value, u64 len, u16 *offset_out, u16 *len_out) {
    if (len == 0 || len > 0xffff) return 0;
    if ((u64)*cursor + len > EXECVE_MAX_ARG_DATA_BYTES) return 0;
    for (u64 i = 0; i < len; i++) cfg->arg_data[(u64)*cursor + i] = (u8)value[i];
    *offset_out = *cursor;
    *len_out = (u16)len;
    *cursor = (u16)((u64)*cursor + len);
    cfg->arg_data_bytes = *cursor;
    return 1;
}

static int append_target_cstr_arg(struct exec_loader_config *cfg, u16 *cursor, u64 target_va, u16 *offset_out, u16 *len_out) {
    char temp[256];
    if (!copy_cstr_from_target(target_va, temp, sizeof(temp))) return 0;
    return append_exec_arg(cfg, cursor, temp, cstr_len(temp), offset_out, len_out);
}

static int configure_exec_args_from_target(struct exec_loader_config *cfg, const char *path, u64 argv_va, u64 envp_va) {
    u16 cursor = 0;
    if (!append_exec_arg(cfg, &cursor, path, cstr_len(path), &cfg->execfn_offset, &cfg->execfn_bytes)) return 0;

    u64 argv_count = 0;
    if (argv_va != 0) {
        while (argv_count < EXECVE_MAX_ARGV) {
            u64 ptr = 0;
            if (copy_from_target(argv_va + argv_count * 8, &ptr, 8) != 8) return 0;
            if (ptr == 0) break;
            if (!append_target_cstr_arg(cfg, &cursor, ptr, &cfg->argv_offsets[argv_count], &cfg->argv_bytes[argv_count])) return 0;
            argv_count++;
        }
    }
    if (argv_count == 0) {
        if (!append_exec_arg(cfg, &cursor, path, cstr_len(path), &cfg->argv_offsets[0], &cfg->argv_bytes[0])) return 0;
        argv_count = 1;
    }
    cfg->argv_count = (u16)argv_count;

    u64 envp_count = 0;
    if (envp_va != 0) {
        while (envp_count < EXECVE_MAX_ENVP) {
            u64 ptr = 0;
            if (copy_from_target(envp_va + envp_count * 8, &ptr, 8) != 8) return 0;
            if (ptr == 0) break;
            if (!append_target_cstr_arg(cfg, &cursor, ptr, &cfg->envp_offsets[envp_count], &cfg->envp_bytes[envp_count])) return 0;
            envp_count++;
        }
    }
    cfg->envp_count = (u16)envp_count;
    return 1;
}

static int spawn_exec_loader_for_execve(const char *path, u64 argv_va, u64 envp_va) {
    if (!ensure_execve_scratch()) { user_log("LinuxAbiServer: execve scratch failed\n"); return 0; }

    u64 main_bytes = 0;
    if (!is_vm_object_token(g_exec_loader_vm_token)) { user_log("LinuxAbiServer: execve loader token absent\n"); return 0; }
    if (!is_vm_object_token(g_standard_interpreter_vm_token) || g_standard_interpreter_bytes == 0) { user_log("LinuxAbiServer: execve interpreter token absent\n"); return 0; }
    if (!vfs_read_file_to_buffer(path, EXECVE_MAIN_IMAGE_VA, EXECVE_MAX_IMAGE_BYTES, &main_bytes)) { user_log("LinuxAbiServer: execve main read failed\n"); return 0; }
    const u64 main_vm_token = install_vm_object_from_buffer(EXECVE_MAIN_IMAGE_VA, main_bytes);
    if (main_vm_token == 0) { user_log("LinuxAbiServer: execve vm install failed\n"); return 0; }
    const u64 loader_exec_token = install_exec_image_from_vm(g_exec_loader_vm_token);
    if (loader_exec_token == 0) { user_log("LinuxAbiServer: execve loader exec install failed\n"); return 0; }

    clear_page(EXECVE_CONFIG_VA);
    clear_page(EXECVE_TABLE_VA);
    struct exec_loader_config *cfg = (struct exec_loader_config *)EXECVE_CONFIG_VA;
    cfg->magic = EXEC_LOADER_BOOTSTRAP_MAGIC;
    cfg->version = EXEC_LOADER_BOOTSTRAP_VERSION;
    cfg->executable_file_bytes = main_bytes;
    cfg->interpreter_file_bytes = g_standard_interpreter_bytes;
    cfg->abi_trap_endpoint_id = LINUX_ABI_ENDPOINT_ID;
    cfg->abi_trap_endpoint_process_slot = syscall0(SYSCALL_GET_PROCESS_SLOT);
    cfg->abi_trap_flavor = 1;
    cfg->abi_trap_request_page_va = trap_request_page_va;
    cfg->fs_endpoint_id = g_vfs.endpoint_id;
    cfg->fs_compat_process_slot = g_vfs.process_slot;
    if (!configure_exec_args_from_target(cfg, path, argv_va, envp_va)) { user_log("LinuxAbiServer: execve argv copy failed\n"); return 0; }

    struct bootstrap_descriptor_table *table = (struct bootstrap_descriptor_table *)EXECVE_TABLE_VA;
    table->page_count = 1;
    table->cap_count = 2;
    table->page_descriptors[0].source_va = EXECVE_CONFIG_VA;
    table->page_descriptors[0].target_va = EXEC_LOADER_CONFIG_TARGET_VA;
    table->cap_descriptors[0].source_token = main_vm_token;
    table->cap_descriptors[0].target_token_va = EXEC_LOADER_CONFIG_TARGET_VA + OFFSETOF(struct exec_loader_config, executable_vm_token);
    table->cap_descriptors[0].rights_bits = VM_RIGHT_READ_MAP;
    table->cap_descriptors[0].kind = BOOTSTRAP_CAP_KIND_VM_OBJECT;
    table->cap_descriptors[1].source_token = g_standard_interpreter_vm_token;
    table->cap_descriptors[1].target_token_va = EXEC_LOADER_CONFIG_TARGET_VA + OFFSETOF(struct exec_loader_config, interpreter_vm_token);
    table->cap_descriptors[1].rights_bits = VM_RIGHT_READ_MAP;
    table->cap_descriptors[1].kind = BOOTSTRAP_CAP_KIND_VM_OBJECT;

    const u64 flags = SPAWN_FLAG_BOOTSTRAP_EXTENDED_DESCRIPTOR_TABLE | SPAWN_FLAG_CHILD_BOOTSTRAP_OWNER;
    const u64 spawned = syscall4(SYSCALL_SPAWN_EXEC, loader_exec_token, EXECVE_TABLE_VA, 0, flags);
    if (decode_spawned_process_slot(spawned) == 0) {
        user_log("LinuxAbiServer: execve spawn failed ret=");
        user_log_hex_value(spawned);
        return 0;
    }
    user_log("LinuxAbiServer: execve spawned\n");
    return 1;
}

static struct ipc_message handle_execve(const struct trap_request *req) {
    char path[256];
    if (!copy_cstr_from_target(req->args[0], path, sizeof(path))) return reply(errno_fault(), 0);
    if (cstr_len(path) > FS_MAX_PATH_BYTES) return reply(errno_nametoolong(), 0);
    user_log("LinuxAbiServer: execve ");
    user_log(path);
    user_log("\n");
    if (!spawn_exec_loader_for_execve(path, req->args[1], req->args[2])) return reply(errno_io(), 0);
    reset_exec_runtime_state();
    return reply(0, TRAP_RESPONSE_FLAG_EXIT);
}
