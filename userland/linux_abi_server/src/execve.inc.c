#define LINUX_ABI_EXECVE_PROFILE 0

static int path_has_prefix(const char *path, const char *prefix) {
    u64 i = 0;
    while (prefix[i] != 0) {
        if (path[i] != prefix[i]) return 0;
        i++;
    }
    return 1;
}

static int cacheable_readonly_path(const char *path) {
    return path_has_prefix(path, "/lib/") ||
        path_has_prefix(path, "/bin/") ||
        path_has_prefix(path, "/cmd/") ||
        path_has_prefix(path, "/sbin/");
}

static void profile_fs_read_path(const struct fd_entry *fd, u64 bytes) {
    if (bytes == 0) return;
    g_prof.fs_read_bytes += bytes;
    if (fd->path_len == 0) return;
    if (path_has_prefix(fd->path, "/cmd/") || path_has_prefix(fd->path, "/bin/") || path_has_prefix(fd->path, "/sbin/")) {
        g_prof.fs_read_cmd_bytes += bytes;
    } else if (path_has_prefix(fd->path, "/lib/")) {
        g_prof.fs_read_lib_bytes += bytes;
    } else if (path_has_prefix(fd->path, "/tmp/")) {
        g_prof.fs_read_tmp_bytes += bytes;
    } else if (path_has_prefix(fd->path, "/proc/")) {
        g_prof.fs_read_proc_bytes += bytes;
    }
}

static int cache_path_matches(const char *cached, u16 cached_len, const char *path, u64 path_len) {
    if (cached_len != path_len) return 0;
    for (u64 i = 0; i < path_len; i++) {
        if (cached[i] != path[i]) return 0;
    }
    return cached[path_len] == 0;
}

static void copy_path_to_cache(char *dst, u16 *dst_len, const char *path, u64 path_len) {
    if (path_len > FS_MAX_PATH_BYTES) path_len = FS_MAX_PATH_BYTES;
    *dst_len = (u16)path_len;
    for (u64 i = 0; i < path_len; i++) dst[i] = path[i];
    dst[path_len] = 0;
}

static struct path_cache_entry *path_cache_find(const char *path) {
    const u64 path_len = cstr_len(path);
    for (u64 i = 0; i < FILE_CACHE_MAX; i++) {
        if (!g_path_cache[i].used) continue;
        if (cache_path_matches(g_path_cache[i].path, g_path_cache[i].path_len, path, path_len)) return &g_path_cache[i];
    }
    return 0;
}

static void path_cache_store(const char *path, u64 token, const struct fs_stat_record *stat, u64 size, u8 kind) {
    if (!cacheable_readonly_path(path)) return;
    const u64 path_len = cstr_len(path);
    if (path_len == 0 || path_len > FS_MAX_PATH_BYTES) return;
    u64 slot = FILE_CACHE_MAX;
    for (u64 i = 0; i < FILE_CACHE_MAX; i++) {
        if (!g_path_cache[i].used) { slot = i; break; }
    }
    if (slot == FILE_CACHE_MAX) slot = token % FILE_CACHE_MAX;
    g_path_cache[slot].used = 1;
    g_path_cache[slot].kind = kind;
    g_path_cache[slot].token = token;
    g_path_cache[slot].size = size;
    g_path_cache[slot].stat = *stat;
    copy_path_to_cache(g_path_cache[slot].path, &g_path_cache[slot].path_len, path, path_len);
}

static void path_cache_invalidate(const char *path) {
    const u64 path_len = cstr_len(path);
    for (u64 i = 0; i < FILE_CACHE_MAX; i++) {
        if (g_path_cache[i].used && cache_path_matches(g_path_cache[i].path, g_path_cache[i].path_len, path, path_len)) g_path_cache[i].used = 0;
        if (g_file_cache[i].used && cache_path_matches(g_file_cache[i].path, g_file_cache[i].path_len, path, path_len)) g_file_cache[i].used = 0;
    }
}

static int vfs_lookup_stat(const char *path, u64 *token_out, struct fs_stat_record *stat_out, u64 *file_bytes_out, u8 *kind_out) {
    struct path_cache_entry *cached = path_cache_find(path);
    if (cached != 0) {
        g_prof.path_cache_hits++;
        *token_out = cached->token;
        *stat_out = cached->stat;
        *file_bytes_out = cached->size;
        *kind_out = cached->kind;
        return 1;
    }
    g_prof.path_cache_misses++;
    if (!vfs_request(FS_OP_LOOKUP, g_vfs.root_token, 0, 0, path)) { user_log("LinuxAbiServer: lookup request failed\n"); return 0; }
    volatile struct fs_response_header *response = (volatile struct fs_response_header *)VFS_RESPONSE_VA;
    if (response->status != FS_STATUS_OK || response->result_token == 0) return 0;
    const u64 token = response->result_token;
    const u8 lookup_kind = response->object_kind;
    const u64 lookup_file_bytes = response->file_bytes;
    if (!vfs_request(FS_OP_STAT, token, 0, 0, 0)) { user_log("LinuxAbiServer: stat request failed\n"); return 0; }
    response = (volatile struct fs_response_header *)VFS_RESPONSE_VA;
    if (response->status == FS_STATUS_OK && response->inline_bytes >= FS_STAT_RECORD_BYTES) {
        volatile struct fs_stat_record *record = (volatile struct fs_stat_record *)(VFS_RESPONSE_VA + FS_RESPONSE_HEADER_BYTES);
        const u64 file_bytes = record->size_bytes != 0 ? record->size_bytes : lookup_file_bytes;
        token_out[0] = token; stat_out->object_kind = record->object_kind; stat_out->size_bytes = file_bytes; stat_out->mode_bits = record->mode_bits; stat_out->mtime_unix_sec = record->mtime_unix_sec;
        *file_bytes_out = file_bytes; *kind_out = response->object_kind != FS_OBJECT_NONE ? response->object_kind : record->object_kind;
        path_cache_store(path, token, stat_out, file_bytes, *kind_out);
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
    path_cache_store(path, token, stat_out, lookup_file_bytes, lookup_kind);
    return 1;
}

static int vfs_lookup_file_token(const char *path, u64 *token_out, u64 *file_bytes_out) {
    struct fs_stat_record rec;
    u8 kind = FS_OBJECT_NONE;
    if (!vfs_lookup_stat(path, token_out, &rec, file_bytes_out, &kind)) return 0;
    if (kind != FS_OBJECT_FILE) return 0;
    return *file_bytes_out != 0;
}

static int is_vm_object_token(u64 token) { return (token & EXEC_IMAGE_TOKEN_TAG) != EXEC_IMAGE_TOKEN_TAG && (token & VM_OBJECT_TOKEN_TAG) != 0 && (token & ~VM_OBJECT_TOKEN_TAG) != 0; }
static int is_exec_image_token(u64 token) { return (token & EXEC_IMAGE_TOKEN_TAG) == EXEC_IMAGE_TOKEN_TAG && (token & ~EXEC_IMAGE_TOKEN_TAG) != 0; }
static u64 decode_spawned_process_slot(u64 value) { if ((value & SPAWN_RESULT_TAG) == 0) return 0; return value & SPAWN_RESULT_PROCESS_MASK; }
static void execve_profile_step(const char *step) {
    if (!LINUX_ABI_EXECVE_PROFILE) return;
    user_log("LinuxAbiServer.prof: ");
    user_log(step);
    user_log("\n");
}

static int alloc_map_range_self(u64 base_va, u64 page_count, u64 writable) {
    u64 done = 0;
    while (done < page_count) {
        const u64 chunk = min_u64(page_count - done, 64);
        if (alloc_map_pages(base_va + done * PAGE_BYTES, chunk, writable) != SYSCALL_OK) return 0;
        done += chunk;
    }
    return 1;
}

static struct file_cache_entry *file_cache_find_by_path(const char *path) {
    const u64 path_len = cstr_len(path);
    for (u64 i = 0; i < FILE_CACHE_MAX; i++) {
        if (!g_file_cache[i].used) continue;
        if (cache_path_matches(g_file_cache[i].path, g_file_cache[i].path_len, path, path_len)) return &g_file_cache[i];
    }
    return 0;
}

static int file_cache_alloc_buffer(u64 size, u64 *buffer_va_out) {
    const u64 aligned = align_up(size, PAGE_BYTES);
    if (aligned == 0 || aligned > FILE_CACHE_BYTES || g_file_cache_next_offset + aligned > FILE_CACHE_BYTES) return 0;
    const u64 va = FILE_CACHE_BASE_VA + g_file_cache_next_offset;
    if (!alloc_map_range_self(va, aligned / PAGE_BYTES, 1)) return 0;
    g_file_cache_next_offset += aligned;
    *buffer_va_out = va;
    return 1;
}

static struct file_cache_entry *file_cache_fill_from_fd(const struct fd_entry *fd) {
    if (fd->path_len == 0 || !cacheable_readonly_path(fd->path) || fd->size == 0) return 0;
    struct file_cache_entry *cached = file_cache_find_by_path(fd->path);
    if (cached != 0) {
        g_prof.file_cache_hits++;
        return cached;
    }
    g_prof.file_cache_misses++;

    u64 slot = FILE_CACHE_MAX;
    for (u64 i = 0; i < FILE_CACHE_MAX; i++) {
        if (!g_file_cache[i].used) { slot = i; break; }
    }
    if (slot == FILE_CACHE_MAX) return 0;

    u64 buffer_va = 0;
    const u64 saved_cache_offset = g_file_cache_next_offset;
    if (!file_cache_alloc_buffer(fd->size, &buffer_va)) return 0;
    u64 copied = 0;
    while (copied < fd->size) {
        u64 chunk = min_u64(fd->size - copied, FS_RESPONSE_PAYLOAD_BYTES);
        if (!vfs_request(FS_OP_READ, fd->token, copied, (u32)chunk, 0)) break;
        volatile struct fs_response_header *response = (volatile struct fs_response_header *)VFS_RESPONSE_VA;
        if (response->status != FS_STATUS_OK || response->inline_bytes == 0) break;
        volatile u8 *src = (volatile u8 *)(VFS_RESPONSE_VA + FS_RESPONSE_HEADER_BYTES);
        u8 *dst = (u8 *)(buffer_va + copied);
        for (u64 i = 0; i < response->inline_bytes; i++) dst[i] = src[i];
        copied += response->inline_bytes;
        if (response->inline_bytes < chunk) break;
    }
    if (copied != fd->size) {
        g_file_cache_next_offset = saved_cache_offset;
        return 0;
    }

    g_file_cache[slot].used = 1;
    g_file_cache[slot].kind = fd->object_kind;
    g_file_cache[slot].token = fd->token;
    g_file_cache[slot].size = fd->size;
    g_file_cache[slot].buffer_va = buffer_va;
    g_file_cache[slot].stat.object_kind = fd->object_kind;
    g_file_cache[slot].stat.size_bytes = fd->size;
    g_file_cache[slot].stat.mode_bits = fd->mode_bits;
    g_file_cache[slot].stat.mtime_unix_sec = 0;
    copy_path_to_cache(g_file_cache[slot].path, &g_file_cache[slot].path_len, fd->path, fd->path_len);
    g_prof.file_cache_fill_bytes += fd->size;
    return &g_file_cache[slot];
}

static u64 file_cache_read_to_target(const struct fd_entry *fd, u64 file_offset, u64 dst, u64 len, int *fault) {
    *fault = 0;
    struct file_cache_entry *cached = file_cache_fill_from_fd(fd);
    if (cached == 0) return 0;
    if (file_offset >= cached->size) return 0;
    u64 n = min_u64(len, cached->size - file_offset);
    u64 copied = 0;
    while (copied < n) {
        const u64 chunk = min_u64(n - copied, FS_RESPONSE_PAYLOAD_BYTES);
        if (copy_to_target(dst + copied, (const void *)(cached->buffer_va + file_offset + copied), chunk) != chunk) {
            *fault = 1;
            return copied;
        }
        copied += chunk;
    }
    profile_fs_read_path(fd, n);
    return n;
}

static int ensure_execve_scratch(void) {
    if (execve_scratch_ready) return 1;
    if (!alloc_map_range_self(EXECVE_CONFIG_VA, 1, 1)) return 0;
    if (!alloc_map_range_self(EXECVE_TABLE_VA, 1, 1)) return 0;
    execve_scratch_ready = 1;
    return 1;
}

static int ensure_execve_main_scratch(u64 file_bytes) {
    if (file_bytes == 0 || file_bytes > EXECVE_MAX_IMAGE_BYTES) return 0;
    const u64 required_pages = (file_bytes + PAGE_BYTES - 1) / PAGE_BYTES;
    if (required_pages <= execve_main_scratch_pages) return 1;
    const u64 base_va = EXECVE_MAIN_IMAGE_VA + execve_main_scratch_pages * PAGE_BYTES;
    const u64 add_pages = required_pages - execve_main_scratch_pages;
    if (!alloc_map_range_self(base_va, add_pages, 1)) return 0;
    execve_main_scratch_pages = required_pages;
    return 1;
}

static int vfs_read_file_to_buffer(const char *path, u64 buffer_va, u64 buffer_cap, u64 *file_bytes_out) {
    u64 file_token = 0; u64 file_bytes = 0;
    if (!vfs_lookup_file_token(path, &file_token, &file_bytes)) { user_log("LinuxAbiServer: vfs read lookup failed\n"); return 0; }
    if (file_bytes == 0 || file_bytes > buffer_cap) { user_log("LinuxAbiServer: vfs read stat invalid\n"); user_log_hex_value(file_bytes); return 0; }
    if (buffer_va == EXECVE_MAIN_IMAGE_VA && !ensure_execve_main_scratch(file_bytes)) { user_log("LinuxAbiServer: execve main scratch failed\n"); return 0; }
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

static int exec_cache_path_matches(const char *path, u64 len) {
    if (g_cached_exec_vm_token == 0 || g_cached_exec_path_len != len) return 0;
    for (u64 i = 0; i < len; i++) {
        if (g_cached_exec_path[i] != path[i]) return 0;
    }
    return g_cached_exec_path[len] == 0;
}

static void invalidate_exec_cache(void) {
    g_cached_exec_vm_token = 0;
    g_cached_exec_file_bytes = 0;
    g_cached_exec_path_len = 0;
    g_cached_exec_path[0] = 0;
}

static void invalidate_exec_cache_for_path(const char *path) {
    path_cache_invalidate(path);
    if (g_cached_exec_vm_token == 0) return;
    if (exec_cache_path_matches(path, cstr_len(path))) invalidate_exec_cache();
}

static int get_exec_vm_for_path(const char *path, u64 *vm_token_out, u64 *file_bytes_out) {
    const u64 path_len = cstr_len(path);
    if (exec_cache_path_matches(path, path_len)) {
        execve_profile_step("main cache hit");
        *vm_token_out = g_cached_exec_vm_token;
        *file_bytes_out = g_cached_exec_file_bytes;
        return 1;
    }

    u64 file_bytes = 0;
    execve_profile_step("main read begin");
    if (!vfs_read_file_to_buffer(path, EXECVE_MAIN_IMAGE_VA, EXECVE_MAX_IMAGE_BYTES, &file_bytes)) return 0;
    execve_profile_step("main read done");
    execve_profile_step("main vm install begin");
    const u64 vm_token = install_vm_object_from_buffer(EXECVE_MAIN_IMAGE_VA, file_bytes);
    if (vm_token == 0) return 0;
    execve_profile_step("main vm install done");

    g_cached_exec_vm_token = vm_token;
    g_cached_exec_file_bytes = file_bytes;
    g_cached_exec_path_len = (u16)path_len;
    for (u64 i = 0; i < path_len; i++) g_cached_exec_path[i] = path[i];
    g_cached_exec_path[path_len] = 0;

    *vm_token_out = vm_token;
    *file_bytes_out = file_bytes;
    return 1;
}

static u64 get_exec_loader_exec_token(void) {
    if (g_exec_loader_exec_token != 0) {
        execve_profile_step("loader exec cache hit");
        return g_exec_loader_exec_token;
    }
    execve_profile_step("loader exec install begin");
    g_exec_loader_exec_token = install_exec_image_from_vm(g_exec_loader_vm_token);
    execve_profile_step("loader exec install done");
    return g_exec_loader_exec_token;
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
            if (copy_from_target(argv_va + argv_count * 8, &ptr, 8) != 8) {
                user_log("LinuxAbiServer: execve argv ptr copy failed idx=");
                user_log_hex_value(argv_count);
                user_log("LinuxAbiServer: execve argv ptr va=");
                user_log_hex_value(argv_va + argv_count * 8);
                return 0;
            }
            if (ptr == 0) break;
            if (!append_target_cstr_arg(cfg, &cursor, ptr, &cfg->argv_offsets[argv_count], &cfg->argv_bytes[argv_count])) {
                user_log("LinuxAbiServer: execve argv string copy failed idx=");
                user_log_hex_value(argv_count);
                user_log("LinuxAbiServer: execve argv string va=");
                user_log_hex_value(ptr);
                return 0;
            }
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
            if (copy_from_target(envp_va + envp_count * 8, &ptr, 8) != 8) {
                break;
            }
            if (ptr == 0) break;
            if (!append_target_cstr_arg(cfg, &cursor, ptr, &cfg->envp_offsets[envp_count], &cfg->envp_bytes[envp_count])) {
                break;
            }
            envp_count++;
        }
    }
    cfg->envp_count = (u16)envp_count;
    return 1;
}

static int spawn_exec_loader_for_execve(u64 caller_principal, const char *path, u64 argv_va, u64 envp_va, u64 *spawned_principal_out) {
    execve_profile_step("scratch begin");
    if (!ensure_execve_scratch()) { user_log("LinuxAbiServer: execve scratch failed\n"); return 0; }
    execve_profile_step("scratch done");
    u64 abi_request_va = 0;
    if (!ensure_child_trap_request_page(caller_principal, &abi_request_va)) {
        user_log("LinuxAbiServer: execve request page failed\n");
        return 0;
    }

    u64 main_bytes = 0;
    if (!is_vm_object_token(g_exec_loader_vm_token)) { user_log("LinuxAbiServer: execve loader token absent\n"); return 0; }
    if (!is_vm_object_token(g_standard_interpreter_vm_token) || g_standard_interpreter_bytes == 0) { user_log("LinuxAbiServer: execve interpreter token absent\n"); return 0; }
    u64 main_vm_token = 0;
    if (!get_exec_vm_for_path(path, &main_vm_token, &main_bytes)) { user_log("LinuxAbiServer: execve main vm failed\n"); return 0; }
    execve_profile_step("main vm done");
    const u64 loader_exec_token = get_exec_loader_exec_token();
    if (loader_exec_token == 0) { user_log("LinuxAbiServer: execve loader exec install failed\n"); return 0; }
    execve_profile_step("loader exec done");

    execve_profile_step("config begin");
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
    cfg->abi_trap_request_page_va = abi_request_va;
    cfg->fs_endpoint_id = g_vfs.endpoint_id;
    cfg->fs_compat_process_slot = g_vfs.process_slot;
    if (!configure_exec_args_from_target(cfg, path, argv_va, envp_va)) { user_log("LinuxAbiServer: execve argv copy failed\n"); return 0; }
    execve_profile_step("argv done");

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
    execve_profile_step("table done");

    const u64 flags = SPAWN_FLAG_BOOTSTRAP_EXTENDED_DESCRIPTOR_TABLE | SPAWN_FLAG_CHILD_BOOTSTRAP_OWNER;
    execve_profile_step("spawn begin");
    const u64 spawned = syscall4(SYSCALL_SPAWN_EXEC, loader_exec_token, EXECVE_TABLE_VA, 0, flags);
    execve_profile_step("spawn syscall done");
    const u64 spawned_principal = decode_spawned_process_slot(spawned);
    if (spawned_principal == 0) {
        user_log("LinuxAbiServer: execve spawn failed ret=");
        user_log_hex_value(spawned);
        return 0;
    }
    *spawned_principal_out = spawned_principal;
    return 1;
}

static struct ipc_message handle_execve(const struct trap_request *req) {
    char path[256];
    if (!copy_cstr_from_target(req->args[0], path, sizeof(path))) return reply(errno_fault(), 0);
    if (cstr_len(path) > FS_MAX_PATH_BYTES) return reply(errno_nametoolong(), 0);
    profile_clear();
    g_exec_path_len = (u16)cstr_len(path);
    for (u16 i = 0; i < g_exec_path_len; i++) g_exec_path[i] = path[i];
    g_exec_path[g_exec_path_len] = 0;
    const u64 old_principal = req->caller_principal;
    u64 spawned_principal = 0;
    if (!spawn_exec_loader_for_execve(req->caller_principal, path, req->args[1], req->args[2], &spawned_principal)) return reply(errno_io(), 0);
    if (g_proc) {
        g_proc->principal = 0;
        g_proc->exec_pending = 1;
        g_proc->exec_pending_principal = spawned_principal;
        if (g_root_linux_principal_set && g_root_linux_principal == old_principal) g_root_linux_principal = 0;
    }
    reset_exec_runtime_state();
    return reply(0, TRAP_RESPONSE_FLAG_EXIT);
}
