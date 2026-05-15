#define LINUX_ABI_EXECVE_PROFILE_ENV "CAPABILITYOS_EXEC_PROFILE=1"
#define LINUX_ABI_EXECVE_PROFILE_DETAIL_ENV "CAPABILITYOS_EXEC_PROFILE_DETAIL=1"
#define LINUX_ABI_EXECVE_PROFILE_VERBOSE_ENV "CAPABILITYOS_EXEC_PROFILE_VERBOSE=1"

enum {
    EXEC_OPT_PATH_CACHE = 0,
    EXEC_OPT_FILE_READ_CACHE = 0,
    EXEC_OPT_MAIN_VM_CACHE = 1,
    EXEC_OPT_SERVICE_SOURCE_CACHE = 1,
};

static int g_execve_profile_enabled = 0;
static int g_execve_profile_detail = 0;
static int g_execve_profile_verbose = 0;
static u64 g_execve_profile_start_tick = 0;
static u64 g_execve_profile_last_tick = 0;
static u64 g_execve_profile_count = 0;
static const char *g_execve_profile_path = 0;
static const char *g_execve_profile_labels[64];
static u64 g_execve_profile_tick[64];
static u64 g_execve_profile_dt[64];
static u64 g_execve_profile_total[64];

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
    if (!EXEC_OPT_PATH_CACHE) return 0;
    const u64 path_len = cstr_len(path);
    for (u64 i = 0; i < FILE_CACHE_MAX; i++) {
        if (!g_path_cache[i].used) continue;
        if (cache_path_matches(g_path_cache[i].path, g_path_cache[i].path_len, path, path_len)) return &g_path_cache[i];
    }
    return 0;
}

static void path_cache_store(const char *path, u64 token, const struct fs_stat_record *stat, u64 size, u8 kind) {
    if (!EXEC_OPT_PATH_CACHE) return;
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
    volatile struct fs_response_header *response = (volatile struct fs_response_header *)vfs_response_addr();
    if (response->status != FS_STATUS_OK || response->result_token == 0) return 0;
    const u64 token = response->result_token;
    const u8 lookup_kind = response->object_kind;
    const u64 lookup_file_bytes = response->file_bytes;
    if (!vfs_request(FS_OP_STAT, token, 0, 0, 0)) { user_log("LinuxAbiServer: stat request failed\n"); return 0; }
    response = (volatile struct fs_response_header *)vfs_response_addr();
    if (response->status == FS_STATUS_OK && response->inline_bytes >= FS_STAT_RECORD_BYTES) {
        volatile struct fs_stat_record *record = (volatile struct fs_stat_record *)vfs_response_payload();
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
static void execve_profile_begin(const char *path) {
    if (!g_execve_profile_enabled) return;
    g_execve_profile_start_tick = syscall0(SYSCALL_GET_TICK_COUNT);
    g_execve_profile_last_tick = g_execve_profile_start_tick;
    g_execve_profile_count = 0;
    g_execve_profile_path = path;
    user_log("LinuxAbiServer.exec_profile.begin path=");
    user_log(path);
    user_log(" tick=");
    user_log_dec_value(g_execve_profile_start_tick);
    user_log("\n");
}

static void execve_profile_step(const char *step) {
    if (!g_execve_profile_enabled) return;
    const u64 now = syscall0(SYSCALL_GET_TICK_COUNT);
    if (g_execve_profile_count < sizeof(g_execve_profile_dt) / sizeof(g_execve_profile_dt[0])) {
        const u64 slot = g_execve_profile_count++;
        g_execve_profile_labels[slot] = step;
        g_execve_profile_tick[slot] = now;
        g_execve_profile_dt[slot] = now - g_execve_profile_last_tick;
        g_execve_profile_total[slot] = now - g_execve_profile_start_tick;
    }
    g_execve_profile_last_tick = now;
}

static void execve_profile_flush(void) {
    if (!g_execve_profile_enabled) return;
    if (!g_execve_profile_verbose) {
        u64 max_index = 0;
        for (u64 i = 0; i < g_execve_profile_count; i++) {
            if (g_execve_profile_dt[i] > g_execve_profile_dt[max_index]) max_index = i;
        }
        const u64 total = g_execve_profile_count == 0 ? 0 : g_execve_profile_total[g_execve_profile_count - 1];
        user_log("LinuxAbiServer.exec_profile.summary path=");
        user_log(g_execve_profile_path ? g_execve_profile_path : "");
        user_log(" count=");
        user_log_dec_value(g_execve_profile_count);
        user_log(" total=");
        user_log_dec_value(total);
        user_log(" max_step=");
        user_log(g_execve_profile_count != 0 ? g_execve_profile_labels[max_index] : "none");
        user_log(" max_dt=");
        user_log_dec_value(g_execve_profile_count != 0 ? g_execve_profile_dt[max_index] : 0);
        user_log("\n");
        return;
    }
    for (u64 i = 0; i < g_execve_profile_count; i++) {
        user_log("LinuxAbiServer.exec_profile.step ");
        user_log(g_execve_profile_labels[i]);
        user_log(" tick=");
        user_log_dec_value(g_execve_profile_tick[i]);
        user_log(" dt=");
        user_log_dec_value(g_execve_profile_dt[i]);
        user_log(" total=");
        user_log_dec_value(g_execve_profile_total[i]);
        user_log("\n");
    }
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
    if (!EXEC_OPT_FILE_READ_CACHE) return 0;
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
        volatile struct fs_response_header *response = (volatile struct fs_response_header *)vfs_response_addr();
        if (response->status != FS_STATUS_OK || response->inline_bytes == 0) break;
        volatile u8 *src = (volatile u8 *)vfs_response_payload();
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
    volatile struct fs_response_header *response = (volatile struct fs_response_header *)vfs_response_addr();
    if (response->status != FS_STATUS_OK || response->result_token == 0) { user_log("LinuxAbiServer: vfs read open failed\n"); user_log_hex_value((u64)(u32)response->status); return 0; }
    const u64 open_token = response->result_token;
    u64 copied = 0;
    int ok = 1;
    while (copied < file_bytes) {
        const u64 chunk = min_u64(file_bytes - copied, FS_BULK_READ_BYTES);
        u64 bulk_bytes = 0;
        if (vfs_read_bulk_to_buffer(open_token, copied, (u32)chunk, buffer_va + copied, &bulk_bytes) && bulk_bytes != 0) {
            copied += bulk_bytes;
            continue;
        }
        const u64 fallback_chunk = min_u64(file_bytes - copied, FS_RESPONSE_PAYLOAD_BYTES);
        if (!vfs_request(FS_OP_READ, open_token, copied, (u32)fallback_chunk, 0)) { user_log("LinuxAbiServer: vfs read request failed\n"); ok = 0; break; }
        response = (volatile struct fs_response_header *)vfs_response_addr();
        if (response->status != FS_STATUS_OK || response->inline_bytes == 0) { user_log("LinuxAbiServer: vfs read chunk failed\n"); user_log_hex_value((u64)(u32)response->status); ok = 0; break; }
        volatile u8 *src = (volatile u8 *)vfs_response_payload();
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

static int cache_entry_path_matches(const struct exec_cache_entry *entry, const char *path, u64 len) {
    if (!entry->used || entry->path_len != len) return 0;
    for (u64 i = 0; i < len; i++) {
        if (entry->path[i] != path[i]) return 0;
    }
    return entry->path[len] == 0;
}

static struct exec_cache_entry *exec_cache_find(const char *path, u64 path_len) {
    if (!EXEC_OPT_MAIN_VM_CACHE && !EXEC_OPT_SERVICE_SOURCE_CACHE) return 0;
    for (u64 i = 0; i < EXEC_CACHE_MAX; i++) {
        if (cache_entry_path_matches(&g_exec_cache[i], path, path_len)) return &g_exec_cache[i];
    }
    return 0;
}

static void exec_cache_store(const char *path, u64 path_len, u64 vm_token, u64 file_bytes) {
    if (!EXEC_OPT_MAIN_VM_CACHE && !EXEC_OPT_SERVICE_SOURCE_CACHE) return;
    if (path_len == 0 || path_len > FS_MAX_PATH_BYTES) return;
    struct exec_cache_entry *entry = exec_cache_find(path, path_len);
    if (entry == 0) {
        for (u64 i = 0; i < EXEC_CACHE_MAX; i++) {
            if (!g_exec_cache[i].used) {
                entry = &g_exec_cache[i];
                break;
            }
        }
    }
    if (entry == 0) return;
    entry->used = 1;
    entry->exec_service_cached = 0;
    entry->path_len = (u16)path_len;
    entry->vm_token = vm_token;
    entry->file_bytes = file_bytes;
    for (u64 i = 0; i < path_len; i++) entry->path[i] = path[i];
    entry->path[path_len] = 0;
}

static void invalidate_exec_cache_for_path(const char *path) {
    path_cache_invalidate(path);
    const u64 path_len = cstr_len(path);
    struct exec_cache_entry *entry = exec_cache_find(path, path_len);
    if (entry != 0) entry->used = 0;
}

static int get_exec_vm_for_path(const char *path, u64 *vm_token_out, u64 *file_bytes_out) {
    const u64 path_len = cstr_len(path);
    struct exec_cache_entry *cached = exec_cache_find(path, path_len);
    if (EXEC_OPT_MAIN_VM_CACHE && cached != 0) {
        execve_profile_step("main cache hit");
        *vm_token_out = cached->vm_token;
        *file_bytes_out = cached->file_bytes;
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

    if (EXEC_OPT_MAIN_VM_CACHE || EXEC_OPT_SERVICE_SOURCE_CACHE) exec_cache_store(path, path_len, vm_token, file_bytes);

    *vm_token_out = vm_token;
    *file_bytes_out = file_bytes;
    return 1;
}

static u64 get_exec_program_token(void) {
    if (g_exec_program_token != 0) {
        execve_profile_step("exec image cache hit");
        return g_exec_program_token;
    }
    execve_profile_step("exec image install begin");
    g_exec_program_token = install_exec_image_from_vm(g_exec_vm_token);
    execve_profile_step("exec image install done");
    return g_exec_program_token;
}

static u64 grant_vm_object_to_exec(u64 vm_token) {
    const u64 granted = syscall3(SYSCALL_GRANT_VM_OBJECT, vm_token, g_exec_service_slot, VM_RIGHT_READ_MAP);
    return is_vm_object_token(granted) ? granted : 0;
}

static u64 exec_launch_request_addr(void) { return g_exec_launch_request_map.addr; }
static u64 exec_launch_response_addr(void) { return g_exec_launch_response_map.addr; }

static int ensure_exec_service_pages(void) {
    if (g_exec_launch_request_paddr < PAGE_BYTES || g_exec_launch_response_paddr < PAGE_BYTES) {
        g_exec_launch_request_paddr = syscall0(SYSCALL_ALLOC_PAGE);
        g_exec_launch_response_paddr = syscall0(SYSCALL_ALLOC_PAGE);
        if (g_exec_launch_request_paddr < PAGE_BYTES || g_exec_launch_response_paddr < PAGE_BYTES) return 0;
    }
    if (exec_launch_request_addr() < PAGE_BYTES || exec_launch_response_addr() < PAGE_BYTES) {
        const u64 request_addr = map_page_anywhere(g_exec_launch_request_paddr, 1);
        const u64 response_addr = map_page_anywhere(g_exec_launch_response_paddr, 1);
        if (request_addr < PAGE_BYTES || response_addr < PAGE_BYTES) return 0;
        g_exec_launch_request_map.addr = request_addr;
        g_exec_launch_request_map.page_count = 1;
        g_exec_launch_response_map.addr = response_addr;
        g_exec_launch_response_map.page_count = 1;
    }
    if (!is_ipc_buffer_token(g_exec_launch_request_token) || !is_ipc_buffer_token(g_exec_launch_response_token)) {
        const u64 owner_rights = IPC_BUFFER_RIGHT_READ | IPC_BUFFER_RIGHT_WRITE | IPC_BUFFER_RIGHT_MAP | IPC_BUFFER_RIGHT_GRANT;
        g_exec_launch_request_token = create_ipc_buffer_from_page(g_exec_launch_request_paddr, owner_rights, IPC_BUFFER_ROLE_REQUEST);
        g_exec_launch_response_token = create_ipc_buffer_from_page(g_exec_launch_response_paddr, owner_rights, IPC_BUFFER_ROLE_RESPONSE);
        if (!is_ipc_buffer_token(g_exec_launch_request_token) || !is_ipc_buffer_token(g_exec_launch_response_token)) return 0;
    }
    return 1;
}

static int ensure_exec_service_scratch(void) {
    if (execve_exec_service_scratch_ready) return 1;
    if (!alloc_map_range_self(EXECVE_EXEC_SERVICE_CONFIG_VA, 1, 1)) return 0;
    if (!alloc_map_range_self(EXECVE_EXEC_SERVICE_TABLE_VA, 1, 1)) return 0;
    execve_exec_service_scratch_ready = 1;
    return 1;
}

static int start_exec_service(void) {
    if (g_exec_service_slot != 0) return 1;
    execve_profile_step("exec service start begin");
    if (!ensure_exec_service_scratch()) return 0;
    const u64 exec_program_token = get_exec_program_token();
    if (exec_program_token == 0) return 0;
    clear_page(EXECVE_EXEC_SERVICE_CONFIG_VA);
    clear_page(EXECVE_EXEC_SERVICE_TABLE_VA);
    struct exec_bootstrap_config *cfg = (struct exec_bootstrap_config *)EXECVE_EXEC_SERVICE_CONFIG_VA;
    cfg->magic = EXEC_BOOTSTRAP_MAGIC;
    cfg->version = EXEC_BOOTSTRAP_VERSION;
    cfg->flags = EXEC_BOOTSTRAP_FLAG_SERVICE_MODE;
    cfg->interpreter_file_bytes = g_standard_interpreter_bytes;
    cfg->fs_endpoint_id = g_vfs.endpoint_id;
    cfg->fs_compat_process_slot = g_vfs.process_slot;

    struct bootstrap_descriptor_table *table = (struct bootstrap_descriptor_table *)EXECVE_EXEC_SERVICE_TABLE_VA;
    table->page_count = 1;
    table->cap_count = 1;
    table->page_descriptors[0].source_va = EXECVE_EXEC_SERVICE_CONFIG_VA;
    table->page_descriptors[0].target_va = EXEC_BOOTSTRAP_TARGET_VA;
    table->cap_descriptors[0].source_token = g_standard_interpreter_vm_token;
    table->cap_descriptors[0].target_token_va = EXEC_BOOTSTRAP_TARGET_VA + OFFSETOF(struct exec_bootstrap_config, interpreter_vm_token);
    table->cap_descriptors[0].rights_bits = VM_RIGHT_READ_MAP;
    table->cap_descriptors[0].kind = BOOTSTRAP_CAP_KIND_VM_OBJECT;

    const u64 flags = SPAWN_FLAG_BOOTSTRAP_EXTENDED_DESCRIPTOR_TABLE | SPAWN_FLAG_CHILD_BOOTSTRAP_OWNER;
    const u64 spawned = syscall4(SYSCALL_SPAWN_EXEC, exec_program_token, EXECVE_EXEC_SERVICE_TABLE_VA, 0, flags);
    g_exec_service_slot = decode_spawned_process_slot(spawned);
    execve_profile_step("exec service start done");
    if (g_exec_service_slot == 0) {
        user_log("LinuxAbiServer: exec service spawn failed ret=");
        user_log_hex_value(spawned);
        return 0;
    }
    return 1;
}

static int send_exec_launch_request(struct exec_bootstrap_config *cfg, struct exec_cache_entry *entry, u64 main_vm_token, u64 *spawned_principal_out) {
    if (!ensure_exec_service_pages()) { user_log("LinuxAbiServer: exec service pages failed\n"); return 0; }
    execve_profile_step("exec service pages done");
    if (!start_exec_service()) return 0;
    execve_profile_step("exec service started");

    const int exec_has_source = EXEC_OPT_SERVICE_SOURCE_CACHE && entry != 0 && entry->exec_service_cached != 0;
    cfg->executable_vm_token = 0;
    if (!exec_has_source) {
        execve_profile_step("exec service grant begin");
        cfg->executable_vm_token = grant_vm_object_to_exec(main_vm_token);
        execve_profile_step("exec service grant done");
        if (cfg->executable_vm_token == 0) {
            user_log("LinuxAbiServer: exec service grant failed\n");
            return 0;
        }
    }
    cfg->interpreter_vm_token = 0;

    const u64 request_addr = exec_launch_request_addr();
    const u64 response_addr = exec_launch_response_addr();
    if (request_addr < PAGE_BYTES || response_addr < PAGE_BYTES) return 0;
    clear_page(request_addr);
    clear_page(response_addr);
    struct exec_launch_request *request = (struct exec_launch_request *)request_addr;
    const u64 request_seq = g_exec_service_seq++;
    request->magic = EXEC_LAUNCH_REQUEST_MAGIC;
    request->version = EXEC_LAUNCH_VERSION;
    request->op = EXEC_LAUNCH_OP_START;
    {
        const u8 *src = (const u8 *)cfg;
        u8 *dst = (u8 *)&request->config;
        for (u64 i = 0; i < sizeof(*cfg); i++) dst[i] = src[i];
    }
    __asm__ volatile("" ::: "memory");
    request->seq = request_seq;

    execve_profile_step(exec_has_source ? "exec service cached send begin" : "exec service grant send begin");
    u64 install_status = syscall3(SYSCALL_INSTALL_ENDPOINT, 0, EXEC_LAUNCH_ENDPOINT_ID, g_exec_service_slot);
    execve_profile_step("exec service endpoint installed");
    u64 grant_status = 0;
    u64 share_status = 0;
    u64 attempts = 0;
    if (!g_exec_service_connected) {
        while (attempts++ < 20000) {
            grant_status = grant_ipc_buffer_on_endpoint(
                g_exec_launch_response_token,
                EXEC_LAUNCH_ENDPOINT_ID,
                IPC_BUFFER_RIGHT_READ | IPC_BUFFER_RIGHT_WRITE | IPC_BUFFER_RIGHT_MAP
            );
            if (is_ipc_buffer_token(grant_status)) {
                g_exec_launch_remote_response_token = grant_status;
                request->response_token = g_exec_launch_remote_response_token;
                __asm__ volatile("" ::: "memory");
                share_status = share_ipc_buffer_on_endpoint(
                    g_exec_launch_request_token,
                    EXEC_LAUNCH_ENDPOINT_ID,
                    IPC_BUFFER_RIGHT_READ | IPC_BUFFER_RIGHT_MAP
                );
            }
            if (is_ipc_buffer_token(grant_status) && share_status == SYSCALL_OK) {
                g_exec_service_connected = 1;
                execve_profile_step("exec service caps connected");
                break;
            }
            __asm__ volatile("pause" ::: "memory");
        }
        if (!g_exec_service_connected) {
            user_log("LinuxAbiServer: exec service signal failed install=");
            user_log_hex_value(install_status);
            user_log("LinuxAbiServer: exec service signal grant=");
            user_log_hex_value(grant_status);
            user_log("LinuxAbiServer: exec service signal share=");
            user_log_hex_value(share_status);
            return 0;
        }
    } else {
        request->response_token = g_exec_launch_remote_response_token;
        __asm__ volatile("" ::: "memory");
        u64 signal_status = syscall2(SYSCALL_SIGNAL_ENDPOINT, EXEC_LAUNCH_ENDPOINT_ID, 0);
        if (signal_status == SYSCALL_ERR_ENDPOINT && install_status == SYSCALL_OK) {
            signal_status = syscall2(SYSCALL_SIGNAL_ENDPOINT, EXEC_LAUNCH_ENDPOINT_ID, 0);
        }
        execve_profile_step("exec service signaled");
        if (signal_status != SYSCALL_OK) {
            user_log("LinuxAbiServer: exec service wake failed=");
            user_log_hex_value(signal_status);
            return 0;
        }
    }

    volatile struct exec_launch_response *response = (volatile struct exec_launch_response *)response_addr;
    execve_profile_step("exec service response wait begin");
    attempts = 0;
    while (attempts++ < 2000000) {
        if (response->magic == EXEC_LAUNCH_RESPONSE_MAGIC &&
            response->version == EXEC_LAUNCH_VERSION &&
            response->op == EXEC_LAUNCH_OP_START &&
            response->seq == request_seq) {
            if (response->status != EXEC_LAUNCH_STATUS_OK || response->child_process_slot == 0) {
                user_log("LinuxAbiServer: exec service failed status=");
                user_log_hex_value(response->status);
                if (exec_has_source && entry != 0) {
                    entry->exec_service_cached = 0;
                    return 2;
                }
                return 0;
            }
            if (EXEC_OPT_SERVICE_SOURCE_CACHE && entry != 0) entry->exec_service_cached = 1;
            *spawned_principal_out = response->child_process_slot;
            execve_profile_step("exec service reply done");
            return 1;
        }
        if ((attempts & 0x3ffu) == 0) wait_without_consuming_ipc_no_switch();
        __asm__ volatile("pause" ::: "memory");
    }
    user_log("LinuxAbiServer: exec service timeout\n");
    user_log("LinuxAbiServer: exec service timeout magic=");
    user_log_hex_value(response->magic);
    user_log("LinuxAbiServer: exec service timeout version=");
    user_log_hex_value(response->version);
    user_log("LinuxAbiServer: exec service timeout op=");
    user_log_hex_value(response->op);
    user_log("LinuxAbiServer: exec service timeout seq=");
    user_log_hex_value(response->seq);
    user_log("LinuxAbiServer: exec service timeout status=");
    user_log_hex_value(response->status);
    user_log("LinuxAbiServer: exec service timeout child=");
    user_log_hex_value(response->child_process_slot);
    user_log("LinuxAbiServer: exec service request seq=");
    user_log_hex_value(request_seq);
    return 0;
}

static void reset_exec_runtime_state(void) {
    if (!g_proc) return;
    g_mmap_next_va = 0x29000000ULL;
    g_brk_next_va = 0x38000000ULL;
    for (u64 i = 0; i < VM_REGION_MAX; i++) g_regions[i].used = 0;
}

static int exec_cstr_eq(const char *a, const char *b) {
    u64 i = 0;
    while (a[i] != 0 && b[i] != 0) {
        if (a[i] != b[i]) return 0;
        i++;
    }
    return a[i] == 0 && b[i] == 0;
}

static int target_env_has_exec_profile(u64 envp_va) {
    if (envp_va == 0) return 0;
    for (u64 i = 0; i < EXECVE_MAX_ENVP; i++) {
        u64 ptr = 0;
        if (copy_from_target(envp_va + i * 8, &ptr, 8) != 8) return 0;
        if (ptr == 0) return 0;
        char value[64];
        if (!copy_cstr_from_target(ptr, value, sizeof(value))) continue;
        if (exec_cstr_eq(value, LINUX_ABI_EXECVE_PROFILE_ENV)) return 1;
    }
    return 0;
}

static int target_env_has_exec_profile_verbose(u64 envp_va) {
    if (envp_va == 0) return 0;
    for (u64 i = 0; i < EXECVE_MAX_ENVP; i++) {
        u64 ptr = 0;
        if (copy_from_target(envp_va + i * 8, &ptr, 8) != 8) return 0;
        if (ptr == 0) return 0;
        char value[64];
        if (!copy_cstr_from_target(ptr, value, sizeof(value))) continue;
        if (exec_cstr_eq(value, LINUX_ABI_EXECVE_PROFILE_VERBOSE_ENV)) return 1;
    }
    return 0;
}

static int target_env_has_exec_profile_detail(u64 envp_va) {
    if (envp_va == 0) return 0;
    for (u64 i = 0; i < EXECVE_MAX_ENVP; i++) {
        u64 ptr = 0;
        if (copy_from_target(envp_va + i * 8, &ptr, 8) != 8) return 0;
        if (ptr == 0) return 0;
        char value[64];
        if (!copy_cstr_from_target(ptr, value, sizeof(value))) continue;
        if (exec_cstr_eq(value, LINUX_ABI_EXECVE_PROFILE_DETAIL_ENV)) return 1;
    }
    return 0;
}

static const char *path_basename(const char *path) {
    const char *base = path;
    for (u64 i = 0; path[i] != 0; i++) {
        if (path[i] == '/') base = path + i + 1;
    }
    return base;
}

static int is_uutils_tool_name(const char *name) {
    static const char *const names[] = {
        "[", "arch", "b2sum", "base32", "base64", "basename", "basenc", "cat",
        "chgrp", "chmod", "chown", "chroot", "cksum", "comm", "cp", "csplit",
        "cut", "date", "dd", "df", "dir", "dircolors", "dirname", "du", "echo",
        "env", "expand", "expr", "factor", "false", "fmt", "fold", "groups",
        "hashsum", "head", "hostid", "hostname", "id", "install", "join", "kill",
        "link", "ln", "logname", "ls", "md5sum", "mkdir", "mkfifo", "mknod",
        "mktemp", "more", "mv", "nice", "nl", "nohup", "nproc", "numfmt", "od",
        "paste", "pathchk", "pinky", "pr", "printenv", "printf", "ptx", "pwd",
        "readlink", "realpath", "rm", "rmdir", "seq", "sha1sum", "sha224sum",
        "sha256sum", "sha384sum", "sha512sum", "shred", "shuf", "sleep", "sort",
        "split", "stat", "stty", "sum", "sync", "tac", "tail", "tee", "test",
        "timeout", "touch", "tr", "true", "truncate", "tsort", "tty", "uname",
        "unexpand", "uniq", "unlink", "uptime", "users", "vdir", "wc", "who",
        "whoami", "yes",
    };
    for (u64 i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (exec_cstr_eq(name, names[i])) return 1;
    }
    return 0;
}

static int is_uutils_hot_tool_name(const char *name) {
    static const char *const names[] = {
        "[", "basename", "cat", "dirname", "echo", "false", "ls", "pwd", "test", "true",
    };
    for (u64 i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (exec_cstr_eq(name, names[i])) return 1;
    }
    return 0;
}

static int is_uutils_common_tool_name(const char *name) {
    static const char *const names[] = {
        "chmod", "cp", "cut", "date", "dd", "env", "head", "id", "ln", "mkdir",
        "mktemp", "mv", "nproc", "printf", "readlink", "realpath", "rm", "rmdir",
        "seq", "sleep", "sort", "stat", "tail", "tee", "touch", "tr", "uname",
        "uniq", "wc", "whoami", "yes",
    };
    for (u64 i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (exec_cstr_eq(name, names[i])) return 1;
    }
    return 0;
}

static int maybe_uutils_applet_exec(const char *path, char *tool_out, u64 tool_cap) {
    if (!(path_has_prefix(path, "/bin/") || path_has_prefix(path, "/usr/bin/") || path_has_prefix(path, "/usr/lib/uutils/"))) return 0;
    const char *tool = path_basename(path);
    if (!is_uutils_tool_name(tool)) return 0;
    const u64 len = cstr_len(tool);
    if (len == 0 || len >= tool_cap) return 0;
    for (u64 i = 0; i < len; i++) tool_out[i] = tool[i];
    tool_out[len] = 0;
    return 1;
}

static int append_exec_arg(struct exec_bootstrap_config *cfg, u16 *cursor, const char *value, u64 len, u16 *offset_out, u16 *len_out) {
    if (len == 0 || len > 0xffff) return 0;
    if ((u64)*cursor + len > EXECVE_MAX_ARG_DATA_BYTES) return 0;
    for (u64 i = 0; i < len; i++) cfg->arg_data[(u64)*cursor + i] = (u8)value[i];
    *offset_out = *cursor;
    *len_out = (u16)len;
    *cursor = (u16)((u64)*cursor + len);
    cfg->arg_data_bytes = *cursor;
    return 1;
}

static int append_target_cstr_arg(struct exec_bootstrap_config *cfg, u16 *cursor, u64 target_va, u16 *offset_out, u16 *len_out) {
    char temp[256];
    if (!copy_cstr_from_target(target_va, temp, sizeof(temp))) return 0;
    return append_exec_arg(cfg, cursor, temp, cstr_len(temp), offset_out, len_out);
}

static int configure_exec_args_from_target(struct exec_bootstrap_config *cfg, const char *path, u64 argv_va, u64 envp_va, const char *argv0_override) {
    u16 cursor = 0;
    if (!append_exec_arg(cfg, &cursor, path, cstr_len(path), &cfg->execfn_offset, &cfg->execfn_bytes)) return 0;

    u64 argv_count = 0;
    if (argv0_override != 0) {
        if (!append_exec_arg(cfg, &cursor, argv0_override, cstr_len(argv0_override), &cfg->argv_offsets[0], &cfg->argv_bytes[0])) return 0;
        argv_count = 1;
    }
    if (argv_va != 0) {
        u64 source_index = argv0_override != 0 ? 1 : 0;
        while (argv_count < EXECVE_MAX_ARGV) {
            u64 ptr = 0;
            if (copy_from_target(argv_va + source_index * 8, &ptr, 8) != 8) {
                user_log("LinuxAbiServer: execve argv ptr copy failed idx=");
                user_log_hex_value(source_index);
                user_log("LinuxAbiServer: execve argv ptr va=");
                user_log_hex_value(argv_va + source_index * 8);
                return 0;
            }
            if (ptr == 0) break;
            if (!append_target_cstr_arg(cfg, &cursor, ptr, &cfg->argv_offsets[argv_count], &cfg->argv_bytes[argv_count])) {
                user_log("LinuxAbiServer: execve argv string copy failed idx=");
                user_log_hex_value(source_index);
                user_log("LinuxAbiServer: execve argv string va=");
                user_log_hex_value(ptr);
                return 0;
            }
            argv_count++;
            source_index++;
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

static int spawn_exec_for_execve(u64 caller_principal, const char *path, u64 argv_va, u64 envp_va, const char *argv0_override, u64 *spawned_principal_out) {
    execve_profile_step("scratch begin");
    if (!ensure_execve_scratch()) { user_log("LinuxAbiServer: execve scratch failed\n"); return 0; }
    execve_profile_step("scratch done");
    if (!is_vm_object_token(g_exec_vm_token)) { user_log("LinuxAbiServer: execve exec token absent\n"); return 0; }
    if (!is_vm_object_token(g_standard_interpreter_vm_token) || g_standard_interpreter_bytes == 0) { user_log("LinuxAbiServer: execve interpreter token absent\n"); return 0; }
    if (!start_exec_service()) { user_log("LinuxAbiServer: execve exec service absent\n"); return 0; }
    clear_page(EXECVE_CONFIG_VA);
    struct exec_bootstrap_config *cfg = (struct exec_bootstrap_config *)EXECVE_CONFIG_VA;
    if (!configure_exec_args_from_target(cfg, path, argv_va, envp_va, argv0_override)) { user_log("LinuxAbiServer: execve argv copy failed\n"); return 0; }
    execve_profile_step("argv done");

    u64 abi_request_va = 0;
    if (!ensure_child_trap_request_page(caller_principal, &abi_request_va)) {
        user_log("LinuxAbiServer: execve request page failed\n");
        return 0;
    }

    u64 main_bytes = 0;
    u64 main_vm_token = 0;
    if (!get_exec_vm_for_path(path, &main_vm_token, &main_bytes)) { user_log("LinuxAbiServer: execve main vm failed\n"); return 0; }
    execve_profile_step("main vm done");

    execve_profile_step("config begin");
    cfg->magic = EXEC_BOOTSTRAP_MAGIC;
    cfg->version = EXEC_BOOTSTRAP_VERSION;
    cfg->executable_file_bytes = main_bytes;
    cfg->interpreter_file_bytes = g_standard_interpreter_bytes;
    cfg->abi_trap_endpoint_id = LINUX_ABI_ENDPOINT_ID;
    cfg->abi_trap_endpoint_process_slot = syscall0(SYSCALL_GET_PROCESS_SLOT);
    cfg->abi_trap_flavor = 1;
    cfg->abi_trap_request_page_va = abi_request_va;
    cfg->fs_endpoint_id = g_vfs.endpoint_id;
    cfg->fs_compat_process_slot = g_vfs.process_slot;
    execve_profile_step("config done");

    struct exec_cache_entry *entry = EXEC_OPT_SERVICE_SOURCE_CACHE ? exec_cache_find(path, cstr_len(path)) : 0;
    execve_profile_step("exec service begin");
    int send_result = send_exec_launch_request(cfg, entry, main_vm_token, spawned_principal_out);
    if (send_result == 2) {
        execve_profile_step("exec service retry begin");
        send_result = send_exec_launch_request(cfg, entry, main_vm_token, spawned_principal_out);
    }
    return send_result == 1;
}

static void close_cloexec_fds_for_execve(void) {
    if (!g_proc) return;
    for (u64 fd = 0; fd < 32; fd++) {
        struct fd_entry *entry = &g_fds[fd];
        if (entry->kind == FD_UNUSED || (entry->fd_flags & FD_INTERNAL_CLOEXEC) == 0) continue;
        if (fd_entry_is_pipe(entry)) close_pipe_entry(entry);
        if (entry->kind == FD_SOCKET) net_close_udp(entry->token);
        entry->kind = FD_UNUSED;
        entry->fd_flags = 0;
    }
}

static struct ipc_message handle_execve(const struct trap_request *req) {
    char path[256];
    if (!copy_cstr_from_target(req->args[0], path, sizeof(path))) return reply(errno_fault(), 0);
    if (cstr_len(path) > FS_MAX_PATH_BYTES) return reply(errno_nametoolong(), 0);
    g_execve_profile_enabled = target_env_has_exec_profile(req->args[2]);
    g_execve_profile_detail = target_env_has_exec_profile_detail(req->args[2]);
    g_execve_profile_verbose = target_env_has_exec_profile_verbose(req->args[2]);
    if (g_execve_profile_verbose) g_profile_trace_verbose = 1;
    const char *load_path = path;
    char uutils_tool[32];
    const char *uutils_tool_ptr = 0;
    const char *argv0_override = 0;
    if (maybe_uutils_applet_exec(path, uutils_tool, sizeof(uutils_tool))) {
        load_path = is_uutils_hot_tool_name(uutils_tool) ? "/cmd/coreutils-hot.elf" :
            (is_uutils_common_tool_name(uutils_tool) ? "/cmd/coreutils-common.elf" : "/cmd/coreutils.elf");
        uutils_tool_ptr = uutils_tool;
        argv0_override = path;
    }
    profile_clear();
    if (g_proc) {
        g_proc->profile_enabled = (u8)g_execve_profile_enabled;
        g_proc->profile_detail_enabled = (u8)g_execve_profile_detail;
        g_proc->profile_verbose_enabled = (u8)g_execve_profile_verbose;
    }
    execve_profile_begin(load_path);
    execve_profile_step("entry");
    g_exec_path_len = (u16)cstr_len(load_path);
    for (u16 i = 0; i < g_exec_path_len; i++) g_exec_path[i] = load_path[i];
    g_exec_path[g_exec_path_len] = 0;
    u64 exec_probe_token = 0;
    u64 exec_probe_bytes = 0;
    if (!vfs_lookup_file_token(path, &exec_probe_token, &exec_probe_bytes)) return reply(errno_noent(), 0);
    if (uutils_tool_ptr != 0 && !vfs_lookup_file_token(load_path, &exec_probe_token, &exec_probe_bytes)) return reply(errno_noent(), 0);
    execve_profile_step("probe lookup done");
    const u64 old_principal = req->caller_principal;
    u64 spawned_principal = 0;
    if (!spawn_exec_for_execve(req->caller_principal, load_path, req->args[1], req->args[2], argv0_override, &spawned_principal)) return reply(errno_io(), 0);
    execve_profile_step("spawn exec done");
    execve_profile_flush();
    if (g_proc) {
        close_cloexec_fds_for_execve();
        g_proc->principal = 0;
        g_proc->exec_pending = 1;
        g_proc->exec_pending_principal = spawned_principal;
        if (g_root_linux_principal_set && g_root_linux_principal == old_principal) g_root_linux_principal = 0;
    }
    reset_exec_runtime_state();
    return exit_trap_target_and_wait(old_principal);
}
