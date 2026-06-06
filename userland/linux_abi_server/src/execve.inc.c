#define LINUX_ABI_EXECVE_PROFILE_ENV "CAPABILITYOS_EXEC_PROFILE=1"
#define LINUX_ABI_EXECVE_PROFILE_DETAIL_ENV "CAPABILITYOS_EXEC_PROFILE_DETAIL=1"
#define LINUX_ABI_EXECVE_PROFILE_VERBOSE_ENV "CAPABILITYOS_EXEC_PROFILE_VERBOSE=1"
#define LINUX_ABI_EXECVE_FAULT_TRACE_ENV "CAPABILITYOS_EXEC_FAULT_TRACE=1"

enum {
    EXEC_OPT_PATH_CACHE = 0,
    EXEC_OPT_FILE_READ_CACHE = 1,
    EXEC_OPT_MAIN_VM_CACHE = 1,
    EXEC_OPT_SERVICE_SOURCE_CACHE = 1,
};

static int g_execve_profile_enabled = 0;
static int g_execve_profile_detail = 0;
static int g_execve_profile_verbose = 0;
static int g_execve_fault_trace = 0;
static u64 g_execve_profile_start_tick = 0;
static u64 g_execve_profile_last_tick = 0;
static u64 g_execve_profile_count = 0;
static const char *g_execve_profile_path = 0;
static const char *g_execve_profile_labels[64];
static u64 g_execve_profile_tick[64];
static u64 g_execve_profile_dt[64];
static u64 g_execve_profile_total[64];
static char g_execve_path_buf[256];
static char g_execve_virtual_path_buf[FS_MAX_PATH_BYTES + 1];
static char g_execve_resolved_path_buf[FS_MAX_PATH_BYTES + 1];
static char g_execve_uutils_tool_buf[32];
static char g_execve_target_arg_buf[EXECVE_MAX_ARG_DATA_BYTES];
static char g_execve_prefix_buf[128];
static char g_execve_shebang_interpreter_buf[FS_MAX_PATH_BYTES + 1];
static char g_execve_shebang_arg_buf[FS_MAX_PATH_BYTES + 1];
static char g_execve_interpreter_resolved_buf[FS_MAX_PATH_BYTES + 1];
static char g_execve_proc_exe_path_buf[FS_MAX_PATH_BYTES + 1];

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
        path_has_prefix(path, "/sbin/") ||
        path_has_prefix(path, "/usr/bin/") ||
        path_has_prefix(path, "/usr/lib/");
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

struct file_cache_free_range {
    u64 offset;
    u64 size;
};

static struct file_cache_free_range g_file_cache_free_ranges[FILE_CACHE_MAX];
static u64 g_file_cache_free_range_count = 0;
static u64 g_file_cache_evict_cursor = 0;

static int file_cache_range_valid(u64 buffer_va, u64 size) {
    if (size == 0 || (size & (PAGE_BYTES - 1)) != 0) return 0;
    if (buffer_va < FILE_CACHE_BASE_VA) return 0;
    const u64 offset = buffer_va - FILE_CACHE_BASE_VA;
    return offset <= FILE_CACHE_BYTES && size <= FILE_CACHE_BYTES - offset;
}

static void file_cache_remove_free_range(u64 index) {
    if (index >= g_file_cache_free_range_count) return;
    for (u64 i = index + 1; i < g_file_cache_free_range_count; i++) {
        g_file_cache_free_ranges[i - 1] = g_file_cache_free_ranges[i];
    }
    g_file_cache_free_range_count--;
}

static int file_cache_add_free_range(u64 buffer_va, u64 size) {
    if (!file_cache_range_valid(buffer_va, size)) return 0;
    u64 merged_offset = buffer_va - FILE_CACHE_BASE_VA;
    u64 merged_size = size;
    u64 i = 0;
    while (i < g_file_cache_free_range_count) {
        const u64 range_offset = g_file_cache_free_ranges[i].offset;
        const u64 range_size = g_file_cache_free_ranges[i].size;
        const u64 range_end = range_offset + range_size;
        const u64 merged_end = merged_offset + merged_size;
        if (range_end < merged_offset || merged_end < range_offset) {
            i++;
            continue;
        }
        if (range_offset < merged_offset) merged_offset = range_offset;
        const u64 new_end = range_end > merged_end ? range_end : merged_end;
        merged_size = new_end - merged_offset;
        file_cache_remove_free_range(i);
    }
    if (g_file_cache_free_range_count >= FILE_CACHE_MAX) return 0;
    g_file_cache_free_ranges[g_file_cache_free_range_count].offset = merged_offset;
    g_file_cache_free_ranges[g_file_cache_free_range_count].size = merged_size;
    g_file_cache_free_range_count++;
    return 1;
}

static void file_cache_clear_slot(u64 slot) {
    if (slot >= FILE_CACHE_MAX) return;
    g_file_cache[slot].used = 0;
    g_file_cache[slot].kind = 0;
    g_file_cache[slot].path_len = 0;
    g_file_cache[slot].token = 0;
    g_file_cache[slot].size = 0;
    g_file_cache[slot].file_offset = 0;
    g_file_cache[slot].cached_size = 0;
    g_file_cache[slot].buffer_va = 0;
    g_file_cache[slot].vm_token = 0;
    g_file_cache[slot].path[0] = 0;
}

static int file_cache_release_slot(u64 slot) {
    if (slot >= FILE_CACHE_MAX) return 0;
    if (!g_file_cache[slot].used) {
        file_cache_clear_slot(slot);
        return 1;
    }
    const u64 buffer_va = g_file_cache[slot].buffer_va;
    const u64 cached_size = g_file_cache[slot].cached_size;
    if (cached_size != 0 && !file_cache_add_free_range(buffer_va, cached_size)) return 0;
    file_cache_clear_slot(slot);
    g_prof.file_cache_evictions++;
    return 1;
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
        if (g_file_cache[i].used && cache_path_matches(g_file_cache[i].path, g_file_cache[i].path_len, path, path_len)) (void)file_cache_release_slot(i);
    }
}

static u32 fs_mode_bits_for_kind(u8 kind, u32 fallback) {
    if (kind == FS_OBJECT_SYMLINK) return FS_SYMLINK_MODE;
    if (kind == FS_OBJECT_DIRECTORY || kind == FS_OBJECT_MOUNT) return FS_DIR_MODE;
    if (kind == FS_OBJECT_FILE) return FS_FILE_MODE;
    return fallback;
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
        const u8 effective_kind = lookup_kind != FS_OBJECT_NONE ? lookup_kind :
            (response->object_kind != FS_OBJECT_NONE ? response->object_kind : record->object_kind);
        const u64 file_bytes = record->size_bytes != 0 ? record->size_bytes : lookup_file_bytes;
        token_out[0] = token; stat_out->object_kind = effective_kind; stat_out->size_bytes = file_bytes; stat_out->mode_bits = fs_mode_bits_for_kind(effective_kind, record->mode_bits); stat_out->mtime_unix_sec = record->mtime_unix_sec;
        *file_bytes_out = file_bytes; *kind_out = effective_kind;
        path_cache_store(path, token, stat_out, file_bytes, effective_kind);
        return 1;
    }
    if (response->status != FS_STATUS_OK || lookup_kind == FS_OBJECT_NONE) { user_log("LinuxAbiServer: stat status failed\n"); user_log_hex_value((u64)(u32)response->status); return 0; }
    token_out[0] = token;
    stat_out->object_kind = lookup_kind;
    stat_out->size_bytes = lookup_file_bytes;
    stat_out->mode_bits = fs_mode_bits_for_kind(lookup_kind, FS_FILE_MODE);
    stat_out->mtime_unix_sec = 0;
    *file_bytes_out = lookup_file_bytes;
    *kind_out = lookup_kind;
    path_cache_store(path, token, stat_out, lookup_file_bytes, lookup_kind);
    return 1;
}

static int execve_read_symlink_target(const char *path, char *target, u64 target_capacity, u64 *target_len_out) {
    *target_len_out = 0;
    if (target_capacity == 0) return 0;
    struct fs_stat_record rec;
    u64 token = 0;
    u64 size = 0;
    u8 kind = FS_OBJECT_NONE;
    if (!vfs_lookup_stat(path, &token, &rec, &size, &kind)) return 0;
    if (kind != FS_OBJECT_SYMLINK) return 0;
    u32 request_len = (u32)target_capacity;
    if (request_len > FS_RESPONSE_PAYLOAD_BYTES) request_len = FS_RESPONSE_PAYLOAD_BYTES;
    if (!vfs_request(FS_OP_READ, token, 0, request_len, 0)) return 0;
    volatile struct fs_response_header *response = (volatile struct fs_response_header *)vfs_response_addr();
    if (response->status != FS_STATUS_OK || response->inline_bytes > request_len) return 0;
    volatile u8 *payload = (volatile u8 *)vfs_response_payload();
    for (u16 i = 0; i < response->inline_bytes; i++) target[i] = (char)payload[i];
    target[response->inline_bytes] = 0;
    *target_len_out = response->inline_bytes;
    (void)rec;
    (void)size;
    return 1;
}

static int execve_parent_path(const char *path, char *parent) {
    u64 last_slash = 0;
    for (u64 i = 0; path[i] != 0; i++) {
        if (path[i] == '/') last_slash = i;
    }
    if (last_slash == 0) {
        parent[0] = '/';
        parent[1] = 0;
        return 1;
    }
    if (last_slash > FS_MAX_PATH_BYTES) return 0;
    for (u64 i = 0; i < last_slash; i++) parent[i] = path[i];
    parent[last_slash] = 0;
    return 1;
}

static int execve_resolve_symlink_target(const char *link_path, char *out) {
    char target[FS_MAX_PATH_BYTES + 1];
    u64 target_len = 0;
    if (!execve_read_symlink_target(link_path, target, sizeof(target) - 1, &target_len)) return 0;
    if (target_len == 0) return 0;
    if (target[0] == '/') return normalize_path("/", target, out);
    char parent[FS_MAX_PATH_BYTES + 1];
    if (!execve_parent_path(link_path, parent)) return 0;
    return normalize_path(parent, target, out);
}

static int vfs_lookup_file_token(const char *path, u64 *token_out, u64 *file_bytes_out) {
    char current[FS_MAX_PATH_BYTES + 1];
    if (!normalize_path("/", path, current)) return 0;
    for (u32 depth = 0; depth < 16; depth++) {
        struct fs_stat_record rec;
        u8 kind = FS_OBJECT_NONE;
        if (!vfs_lookup_stat(current, token_out, &rec, file_bytes_out, &kind)) return 0;
        if (kind == FS_OBJECT_FILE) return *file_bytes_out != 0;
        if (kind != FS_OBJECT_SYMLINK) return 0;
        char next[FS_MAX_PATH_BYTES + 1];
        if (!execve_resolve_symlink_target(current, next)) return 0;
        for (u64 i = 0; i <= FS_MAX_PATH_BYTES; i++) {
            current[i] = next[i];
            if (next[i] == 0) break;
        }
    }
    return 0;
}

static int execve_resolve_file_path(const char *path, char *resolved_out) {
    char current[FS_MAX_PATH_BYTES + 1];
    if (!normalize_path("/", path, current)) return 0;
    for (u32 depth = 0; depth < 16; depth++) {
        struct fs_stat_record rec;
        u64 token = 0;
        u64 file_bytes = 0;
        u8 kind = FS_OBJECT_NONE;
        if (!vfs_lookup_stat(current, &token, &rec, &file_bytes, &kind)) return 0;
        (void)token;
        (void)rec;
        (void)file_bytes;
        if (kind == FS_OBJECT_FILE) {
            const u64 len = cstr_len(current);
            if (len > FS_MAX_PATH_BYTES) return 0;
            for (u64 i = 0; i <= len; i++) resolved_out[i] = current[i];
            return 1;
        }
        if (kind != FS_OBJECT_SYMLINK) return 0;
        char next[FS_MAX_PATH_BYTES + 1];
        if (!execve_resolve_symlink_target(current, next)) return 0;
        for (u64 i = 0; i <= FS_MAX_PATH_BYTES; i++) {
            current[i] = next[i];
            if (next[i] == 0) break;
        }
    }
    return 0;
}

static int is_vm_object_token(u64 token) { return (token & VM_OBJECT_TOKEN_TAG) != 0 && (token & ~VM_OBJECT_TOKEN_TAG) != 0; }
static int track_process_vm_object_token(u64 token) {
    if (!g_proc || !is_vm_object_token(token)) return 0;
    for (u64 i = 0; i < g_proc->vm_object_token_count; i++) {
        if (g_proc->vm_object_tokens[i] == token) return 1;
    }
    if (g_proc->vm_object_token_count >= LINUX_PROCESS_VM_OBJECT_TOKEN_MAX) return 0;
    g_proc->vm_object_tokens[g_proc->vm_object_token_count++] = token;
    return 1;
}
static void release_process_vm_object_tokens(struct linux_process_state *proc) {
    if (!proc) return;
    for (u64 i = 0; i < proc->vm_object_token_count; i++) {
        if (is_vm_object_token(proc->vm_object_tokens[i])) (void)drop_vm_object_token(proc->vm_object_tokens[i]);
        proc->vm_object_tokens[i] = 0;
    }
    proc->vm_object_token_count = 0;
}
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
        if (g_file_cache[i].file_offset != 0) continue;
        if (g_file_cache[i].cached_size < align_up(g_file_cache[i].size, PAGE_BYTES)) continue;
        if (cache_path_matches(g_file_cache[i].path, g_file_cache[i].path_len, path, path_len)) return &g_file_cache[i];
    }
    return 0;
}

static int file_cache_take_free_buffer(u64 aligned, u64 *buffer_va_out) {
    for (u64 i = 0; i < g_file_cache_free_range_count; i++) {
        if (g_file_cache_free_ranges[i].size < aligned) continue;
        const u64 offset = g_file_cache_free_ranges[i].offset;
        g_file_cache_free_ranges[i].offset += aligned;
        g_file_cache_free_ranges[i].size -= aligned;
        if (g_file_cache_free_ranges[i].size == 0) file_cache_remove_free_range(i);
        *buffer_va_out = FILE_CACHE_BASE_VA + offset;
        g_prof.file_cache_reuse_bytes += aligned;
        return 1;
    }
    return 0;
}

static int file_cache_alloc_buffer(u64 size, u64 *buffer_va_out) {
    const u64 aligned = align_up(size, PAGE_BYTES);
    if (aligned == 0 || aligned > FILE_CACHE_BYTES) return 0;
    if (file_cache_take_free_buffer(aligned, buffer_va_out)) return 1;
    if (g_file_cache_next_offset + aligned > FILE_CACHE_BYTES) return 0;
    const u64 va = FILE_CACHE_BASE_VA + g_file_cache_next_offset;
    const u64 page_count = aligned / PAGE_BYTES;
    u64 mapped = 0;
    while (mapped < page_count) {
        const u64 chunk = min_u64(page_count - mapped, 64);
        if (alloc_map_pages(va + mapped * PAGE_BYTES, chunk, 1) != SYSCALL_OK) return 0;
        mapped += chunk;
    }
    g_file_cache_next_offset += aligned;
    *buffer_va_out = va;
    return 1;
}

static int file_cache_evict_one_for_size(u64 aligned) {
    (void)aligned;
    for (u64 attempts = 0; attempts < FILE_CACHE_MAX; attempts++) {
        const u64 slot = (g_file_cache_evict_cursor + attempts) % FILE_CACHE_MAX;
        if (!g_file_cache[slot].used) continue;
        if (!file_cache_release_slot(slot)) continue;
        g_file_cache_evict_cursor = (slot + 1) % FILE_CACHE_MAX;
        return 1;
    }
    return 0;
}

static int file_cache_find_slot_or_evict(u64 *slot_out) {
    for (u64 i = 0; i < FILE_CACHE_MAX; i++) {
        if (!g_file_cache[i].used) {
            *slot_out = i;
            return 1;
        }
    }
    for (u64 attempts = 0; attempts < FILE_CACHE_MAX; attempts++) {
        const u64 slot = (g_file_cache_evict_cursor + attempts) % FILE_CACHE_MAX;
        if (!g_file_cache[slot].used) continue;
        if (!file_cache_release_slot(slot)) continue;
        g_file_cache_evict_cursor = (slot + 1) % FILE_CACHE_MAX;
        *slot_out = slot;
        return 1;
    }
    return 0;
}

static int file_cache_should_fill_for_read(const struct fd_entry *fd, u64 file_offset, u64 len) {
    if (len == 0 || fd->size == 0 || file_offset >= fd->size) return 0;
    if (fd->size <= 1024 * 1024) return 1;
    const u64 remaining = fd->size - file_offset;
    if (len >= remaining) return 1;
    if (len >= 4 * 1024 * 1024 && len >= fd->size / 2) return 1;
    return 0;
}

static struct file_cache_entry *file_cache_fill_from_fd(const struct fd_entry *fd, int allow_mmap_cow) {
    if (!EXEC_OPT_FILE_READ_CACHE && !(allow_mmap_cow && LINUX_ENABLE_FILE_VM_OBJECT_MMAP)) return 0;
    if (fd->path_len == 0) {
        g_prof.file_cache_fill_fail_no_path++;
        return 0;
    }
    if (!cacheable_readonly_path(fd->path)) {
        g_prof.file_cache_fill_fail_uncacheable++;
        return 0;
    }
    if (fd->size == 0) {
        g_prof.file_cache_fill_fail_size++;
        return 0;
    }
    struct file_cache_entry *cached = file_cache_find_by_path(fd->path);
    if (cached != 0) {
        g_prof.file_cache_hits++;
        return cached;
    }
    g_prof.file_cache_misses++;

    u64 slot = 0;
    if (!file_cache_find_slot_or_evict(&slot)) {
        g_prof.file_cache_fill_fail_slot++;
        return 0;
    }

    u64 buffer_va = 0;
    if (!file_cache_alloc_buffer(fd->size, &buffer_va)) {
        const u64 aligned = align_up(fd->size, PAGE_BYTES);
        while (!file_cache_alloc_buffer(fd->size, &buffer_va)) {
            if (!file_cache_evict_one_for_size(aligned)) {
                g_prof.file_cache_fill_fail_alloc++;
                return 0;
            }
        }
    }
    u64 copied = 0;
    while (copied < fd->size) {
        u64 chunk = min_u64(fd->size - copied, FS_BULK_READ_BYTES);
        u64 bulk_bytes = 0;
        if (vfs_read_bulk_to_buffer(fd->token, copied, (u32)chunk, buffer_va + copied, &bulk_bytes) && bulk_bytes != 0) {
            copied += bulk_bytes;
            continue;
        }
        chunk = min_u64(fd->size - copied, FS_RESPONSE_PAYLOAD_BYTES);
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
        (void)file_cache_add_free_range(buffer_va, align_up(fd->size, PAGE_BYTES));
        g_prof.file_cache_fill_fail_read++;
        return 0;
    }
    const u64 aligned_size = align_up(fd->size, PAGE_BYTES);
    for (u64 i = fd->size; i < aligned_size; i++) {
        ((u8 *)buffer_va)[i] = 0;
    }

    g_file_cache[slot].used = 1;
    g_file_cache[slot].kind = fd->object_kind;
    g_file_cache[slot].token = fd->token;
    g_file_cache[slot].size = fd->size;
    g_file_cache[slot].file_offset = 0;
    g_file_cache[slot].cached_size = align_up(fd->size, PAGE_BYTES);
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
    struct file_cache_entry *cached = fd->path_len != 0 ? file_cache_find_by_path(fd->path) : 0;
    if (cached == 0) {
        if (!file_cache_should_fill_for_read(fd, file_offset, len)) return 0;
        cached = file_cache_fill_from_fd(fd, 0);
    }
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

static u64 file_cache_vm_object_token_for_fd(const struct fd_entry *fd) {
    if (!LINUX_ENABLE_FILE_VM_OBJECT_MMAP) return 0;
    if (fd->size == 0) return 0;
    if (align_up(fd->size, PAGE_BYTES) > 65535ULL * PAGE_BYTES) return 0;
    if (!vfs_request(FS_OP_OPEN_EXEC, fd->token, 0, 0, 0)) return 0;
    volatile struct fs_response_header *response = (volatile struct fs_response_header *)vfs_response_addr();
    if (response->status != FS_STATUS_OK) return 0;
    if (!is_vm_object_token(response->arg0)) {
        g_prof.file_vm_object_mmap_install_fail++;
        return 0;
    }
    return response->arg0;
}

static int file_vm_object_map_to_target(const struct fd_entry *fd, u64 file_offset, u64 dst, u64 size, u64 prot) {
    if (!LINUX_ENABLE_FILE_VM_OBJECT_MMAP) return 0;
    if ((file_offset & (PAGE_BYTES - 1)) != 0 || (dst & (PAGE_BYTES - 1)) != 0 || (size & (PAGE_BYTES - 1)) != 0) return 0;

    const u64 vm_token = file_cache_vm_object_token_for_fd(fd);
    if (!is_vm_object_token(vm_token)) return 0;

    const u64 object_pages = align_up(fd->size, PAGE_BYTES) / PAGE_BYTES;
    const u64 object_page_offset = file_offset / PAGE_BYTES;
    if (object_page_offset >= object_pages) {
        (void)drop_vm_object_token(vm_token);
        return 0;
    }
    const u64 requested_pages = size / PAGE_BYTES;
    const u64 available_pages = object_pages - object_page_offset;
    const u64 map_pages = min_u64(requested_pages, available_pages);
    if (map_pages == 0) {
        (void)drop_vm_object_token(vm_token);
        return 0;
    }
    const u64 map_prot = prot & ~0x2ULL;
    if (map_vm_object_to_reply_target(vm_token, object_page_offset, dst, map_pages, map_prot) != SYSCALL_OK) {
        (void)drop_vm_object_token(vm_token);
        g_prof.file_vm_object_mmap_map_fail++;
        return 0;
    }
    if (requested_pages > map_pages) {
        const u64 tail_pages = requested_pages - map_pages;
        const u64 tail_va = dst + map_pages * PAGE_BYTES;
        if (map_zeroed_target_pages_chunked(tail_va, tail_pages, prot) != SYSCALL_OK) {
            (void)unmap_reply_target_pages(dst, map_pages);
            (void)drop_vm_object_token(vm_token);
            g_prof.file_vm_object_mmap_map_fail++;
            return 0;
        }
        g_prof.file_vm_object_mmap_tail_pages += tail_pages;
    }
    if (!track_process_vm_object_token(vm_token)) {
        (void)unmap_reply_target_pages(dst, requested_pages);
        (void)drop_vm_object_token(vm_token);
        g_prof.file_vm_object_mmap_map_fail++;
        return 0;
    }

    const u64 readable = file_offset < fd->size ? min_u64(size, fd->size - file_offset) : 0;
    profile_fs_read_path(fd, readable);
    g_prof.file_vm_object_mmap_mapped++;
    g_prof.file_vm_object_mmap_pages += map_pages;
    return 1;
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
    const u64 token = syscall3(SYSCALL_CREATE_VM_OBJECT_FROM_CURRENT_PAGES, buffer_va, file_bytes, 0xF);
    if (!is_vm_object_token(token)) return 0;
    const u64 page_count = align_up(file_bytes, PAGE_BYTES) / PAGE_BYTES;
    if (!alloc_map_range_self(buffer_va, page_count, 1)) return 0;
    return token;
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

static int exec_path_eq_literal(const char *path, const char *literal) {
    u64 i = 0;
    while (path[i] != 0 && literal[i] != 0) {
        if (path[i] != literal[i]) return 0;
        i++;
    }
    return path[i] == 0 && literal[i] == 0;
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
    if (exec_path_eq_literal(path, "/lib/ld-musl-x86_64.so.1")) {
        g_standard_interpreter_dirty = 1;
    }
    const u64 path_len = cstr_len(path);
    struct exec_cache_entry *entry = exec_cache_find(path, path_len);
    if (entry != 0) entry->used = 0;
}

static int ensure_standard_interpreter_scratch(void) {
    if (execve_ld_scratch_ready) return 1;
    const u64 pages = (EXECVE_MAX_LD_BYTES + PAGE_BYTES - 1) / PAGE_BYTES;
    if (!alloc_map_range_self(EXECVE_LD_IMAGE_VA, pages, 1)) return 0;
    execve_ld_scratch_ready = 1;
    return 1;
}

static int ensure_standard_interpreter_current(void) {
    if (!g_standard_interpreter_dirty &&
        is_vm_object_token(g_standard_interpreter_vm_token) &&
        g_standard_interpreter_bytes != 0) {
        return 1;
    }
    if (!ensure_standard_interpreter_scratch()) {
        user_log("LinuxAbiServer: ld scratch failed\n");
        return 0;
    }
    u64 file_bytes = 0;
    if (!vfs_read_file_to_buffer("/lib/ld-musl-x86_64.so.1", EXECVE_LD_IMAGE_VA, EXECVE_MAX_LD_BYTES, &file_bytes)) {
        user_log("LinuxAbiServer: ld reload read failed\n");
        return 0;
    }
    const u64 vm_token = install_vm_object_from_buffer(EXECVE_LD_IMAGE_VA, file_bytes);
    if (vm_token == 0) {
        user_log("LinuxAbiServer: ld reload vm install failed\n");
        return 0;
    }
    g_standard_interpreter_vm_token = vm_token;
    g_standard_interpreter_bytes = file_bytes;
    g_standard_interpreter_dirty = 0;
    user_log("LinuxAbiServer: ld reloaded\n");
    return 1;
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

static const char *kernel_syscall_status_name(u64 status) {
    if (status == SYSCALL_OK) return "ok";
    if (status == SYSCALL_ERR_INVALID) return "invalid";
    if (status == SYSCALL_ERR_NOT_READY) return "not_ready";
    if (status == SYSCALL_ERR_ALLOC) return "alloc";
    if (status == SYSCALL_ERR_MAP) return "map";
    if (status == SYSCALL_ERR_SEND) return "send";
    if (status == SYSCALL_ERR_ENDPOINT) return "endpoint";
    if (status == SYSCALL_ERR_GRANT) return "grant";
    return "unknown";
}

static void log_exec_grant_failure(const char *label, u64 status) {
    user_log("LinuxAbiServer: ");
    user_log(label);
    user_log(" failed status=");
    user_log(kernel_syscall_status_name(status));
    user_log(" raw=");
    user_log_hex_value(status);
}

static u64 grant_vm_object_to_exec(u64 vm_token, u64 *status_out) {
    const u64 granted = syscall3(SYSCALL_GRANT_VM_OBJECT, vm_token, g_exec_service_slot, VM_RIGHT_READ_MAP);
    if (status_out != 0) *status_out = granted;
    return is_vm_object_token(granted) ? granted : 0;
}

static u64 grant_standard_interpreter_to_exec_service(void) {
    if (g_exec_service_interpreter_granted_slot == g_exec_service_slot &&
        g_exec_service_interpreter_source_token == g_standard_interpreter_vm_token &&
        is_vm_object_token(g_exec_service_interpreter_granted_token)) {
        return g_exec_service_interpreter_granted_token;
    }
    u64 status = 0;
    const u64 granted = grant_vm_object_to_exec(g_standard_interpreter_vm_token, &status);
    if (granted == 0) log_exec_grant_failure("exec service interpreter grant", status);
    if (granted == 0) return 0;
    g_exec_service_interpreter_granted_slot = g_exec_service_slot;
    g_exec_service_interpreter_source_token = g_standard_interpreter_vm_token;
    g_exec_service_interpreter_granted_token = granted;
    return granted;
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
    execve_profile_step("exec service lookup begin");
    if (!ensure_exec_service_scratch()) return 0;
    struct service_entry entry;
    if (!find_service(SERVICE_KIND_EXEC, &entry)) {
        user_log("LinuxAbiServer: exec service registry missing\n");
        return 0;
    }
    g_exec_service_slot = entry.process_slot;
    execve_profile_step("exec service lookup done");
    return 1;
}

static void exec_launch_full_fence(void) {
    __sync_synchronize();
}

static void exec_launch_publish_request_op(volatile struct exec_launch_request *request, u64 op) {
    exec_launch_full_fence();
    request->op = op;
    exec_launch_full_fence();
}

static void exec_launch_sequence_acquire(void) {
    while (__sync_lock_test_and_set(&g_exec_launch_sequence_lock, 1) != 0) {
        wait_without_consuming_ipc();
        __asm__ volatile("pause" ::: "memory");
    }
    exec_launch_full_fence();
}

static void exec_launch_sequence_release(void) {
    exec_launch_full_fence();
    __sync_lock_release(&g_exec_launch_sequence_lock);
}

static int send_exec_launch_request(struct exec_bootstrap_config *cfg, struct exec_cache_entry *entry, u64 main_vm_token, u64 *spawned_principal_out) {
    g_exec_launch_pending_start_seq = 0;
    if (!ensure_exec_service_pages()) { user_log("LinuxAbiServer: exec service pages failed\n"); return 0; }
    execve_profile_step("exec service pages done");
    if (!ensure_standard_interpreter_current()) return 0;
    if (!start_exec_service()) return 0;
    execve_profile_step("exec service started");

    const int exec_has_source = EXEC_OPT_SERVICE_SOURCE_CACHE && entry != 0 && entry->exec_service_cached != 0;
    cfg->executable_vm_token = 0;
    if (!exec_has_source) {
        execve_profile_step("exec service grant begin");
        u64 grant_status = 0;
        cfg->executable_vm_token = grant_vm_object_to_exec(main_vm_token, &grant_status);
        execve_profile_step("exec service grant done");
        if (cfg->executable_vm_token == 0) {
            log_exec_grant_failure("exec service grant", grant_status);
            return 0;
        }
    }
    execve_profile_step("exec service interpreter grant begin");
    cfg->interpreter_vm_token = grant_standard_interpreter_to_exec_service();
    execve_profile_step("exec service interpreter grant done");
    if (cfg->interpreter_vm_token == 0) {
        return 0;
    }

    const u64 request_addr = exec_launch_request_addr();
    const u64 response_addr = exec_launch_response_addr();
    if (request_addr < PAGE_BYTES || response_addr < PAGE_BYTES) return 0;
    clear_page(request_addr);
    clear_page(response_addr);
    struct exec_launch_request *request = (struct exec_launch_request *)request_addr;
    const u64 request_seq = g_exec_service_seq++;
    request->magic = EXEC_LAUNCH_REQUEST_MAGIC;
    request->version = EXEC_LAUNCH_VERSION;
    {
        const u8 *src = (const u8 *)cfg;
        u8 *dst = (u8 *)&request->config;
        for (u64 i = 0; i < sizeof(*cfg); i++) dst[i] = src[i];
    }
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
                exec_launch_publish_request_op(request, EXEC_LAUNCH_OP_START);
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
        exec_launch_publish_request_op(request, EXEC_LAUNCH_OP_START);
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
            exec_launch_full_fence();
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
            g_exec_launch_pending_start_seq = request_seq;
            execve_profile_step("exec service reply done");
            return 1;
        }
        if ((attempts & 0x3ffu) == 0) wait_without_consuming_ipc();
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

static int start_prepared_exec_launch(u64 expected_child_process_slot) {
    const u64 request_seq = g_exec_launch_pending_start_seq;
    g_exec_launch_pending_start_seq = 0;
    if (request_seq == 0 || expected_child_process_slot == 0) return 0;
    const u64 request_addr = exec_launch_request_addr();
    const u64 response_addr = exec_launch_response_addr();
    if (request_addr < PAGE_BYTES || response_addr < PAGE_BYTES) return 0;
    volatile struct exec_launch_request *request = (volatile struct exec_launch_request *)request_addr;
    volatile struct exec_launch_response *response = (volatile struct exec_launch_response *)response_addr;
    if (request->magic != EXEC_LAUNCH_REQUEST_MAGIC ||
        request->version != EXEC_LAUNCH_VERSION ||
        request->seq != request_seq) {
        user_log("LinuxAbiServer: exec start request mismatch\n");
        return 0;
    }
    exec_launch_publish_request_op(request, EXEC_LAUNCH_OP_START_READY);
    const u64 signal_status = syscall2(SYSCALL_SIGNAL_ENDPOINT, EXEC_LAUNCH_ENDPOINT_ID, 0);
    if (signal_status != SYSCALL_OK && signal_status != SYSCALL_ERR_ENDPOINT) {
        user_log("LinuxAbiServer: exec start signal failed=");
        user_log_hex_value(signal_status);
        return 0;
    }
    for (u64 attempts = 0; attempts < 2000000; attempts++) {
        if (response->magic == EXEC_LAUNCH_RESPONSE_MAGIC &&
            response->version == EXEC_LAUNCH_VERSION &&
            response->op == EXEC_LAUNCH_OP_STARTED &&
            response->seq == request_seq) {
            exec_launch_full_fence();
            if (response->status == EXEC_LAUNCH_STATUS_OK &&
                response->child_process_slot == expected_child_process_slot) {
                return 1;
            }
            user_log("LinuxAbiServer: exec start failed status=");
            user_log_hex_value(response->status);
            return 0;
        }
        if ((attempts & 0x3ffu) == 0) wait_without_consuming_ipc();
        __asm__ volatile("pause" ::: "memory");
    }
    user_log("LinuxAbiServer: exec start timeout\n");
    return 0;
}

static void reset_exec_runtime_state(void) {
    if (!g_proc) return;
    clear_tracked_target_ranges();
    release_process_vm_object_tokens(g_proc);
    g_profile_trace_verbose = 0;
    g_mmap_next_va = g_mmap_base_va;
    g_brk_next_va = g_brk_initial_va;
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

static int target_env_has_exec_fault_trace(u64 envp_va) {
    if (envp_va == 0) return 0;
    for (u64 i = 0; i < EXECVE_MAX_ENVP; i++) {
        u64 ptr = 0;
        if (copy_from_target(envp_va + i * 8, &ptr, 8) != 8) return 0;
        if (ptr == 0) return 0;
        char value[64];
        if (!copy_cstr_from_target(ptr, value, sizeof(value))) continue;
        if (exec_cstr_eq(value, LINUX_ABI_EXECVE_FAULT_TRACE_ENV)) return 1;
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
    char *temp = g_execve_target_arg_buf;
    if (!copy_cstr_from_target(target_va, temp, sizeof(g_execve_target_arg_buf))) {
        u64 vfork_parent = g_proc ? g_proc->vfork_parent_principal : 0;
        const struct trap_request *req = abi_current_request();
        if (vfork_parent == 0 && req != 0) vfork_parent = vfork_parent_for_principal(req->caller_principal);
        if (!copy_cstr_from_trap_target(vfork_parent, target_va, temp, sizeof(g_execve_target_arg_buf))) {
            user_log("LinuxAbiServer: execve cstr copy failed va=");
            user_log_hex_value(target_va);
            return 0;
        }
    }
    return append_exec_arg(cfg, cursor, temp, cstr_len(temp), offset_out, len_out);
}

static int append_target_argv_tail(struct exec_bootstrap_config *cfg, u16 *cursor, u64 argv_va, u64 source_index, u64 *argv_count) {
    if (argv_va == 0) return 1;
    for (;;) {
        u64 ptr = 0;
        if (copy_from_target(argv_va + source_index * 8, &ptr, 8) != 8) {
            u64 vfork_parent = g_proc ? g_proc->vfork_parent_principal : 0;
            const struct trap_request *req = abi_current_request();
            if (vfork_parent == 0 && req != 0) vfork_parent = vfork_parent_for_principal(req->caller_principal);
            if (copy_from_trap_target(vfork_parent, argv_va + source_index * 8, &ptr, 8) != 8) {
                user_log("LinuxAbiServer: execve argv ptr copy failed va=");
                user_log_hex_value(argv_va + source_index * 8);
                return 0;
            }
        }
        if (ptr == 0) return 1;
        if (*argv_count >= EXECVE_MAX_ARGV) {
            user_log("LinuxAbiServer: execve argv limit exceeded\n");
            return 0;
        }
        if (!append_target_cstr_arg(cfg, cursor, ptr, &cfg->argv_offsets[*argv_count], &cfg->argv_bytes[*argv_count])) return 0;
        (*argv_count)++;
        source_index++;
    }
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
        if (!append_target_argv_tail(cfg, &cursor, argv_va, source_index, &argv_count)) return 0;
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
                u64 vfork_parent = g_proc ? g_proc->vfork_parent_principal : 0;
                const struct trap_request *req = abi_current_request();
                if (vfork_parent == 0 && req != 0) vfork_parent = vfork_parent_for_principal(req->caller_principal);
                if (copy_from_trap_target(vfork_parent, envp_va + envp_count * 8, &ptr, 8) != 8) {
                    break;
                }
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

static int configure_exec_args_for_shebang(
    struct exec_bootstrap_config *cfg,
    const char *execfn,
    const char *interpreter_argv0,
    const char *interpreter_arg,
    const char *script_path,
    u64 argv_va,
    u64 envp_va
) {
    u16 cursor = 0;
    if (!append_exec_arg(cfg, &cursor, execfn, cstr_len(execfn), &cfg->execfn_offset, &cfg->execfn_bytes)) return 0;
    u64 argv_count = 0;
    if (!append_exec_arg(cfg, &cursor, interpreter_argv0, cstr_len(interpreter_argv0), &cfg->argv_offsets[argv_count], &cfg->argv_bytes[argv_count])) return 0;
    argv_count++;
    if (interpreter_arg != 0 && interpreter_arg[0] != 0) {
        if (argv_count >= EXECVE_MAX_ARGV) return 0;
        if (!append_exec_arg(cfg, &cursor, interpreter_arg, cstr_len(interpreter_arg), &cfg->argv_offsets[argv_count], &cfg->argv_bytes[argv_count])) return 0;
        argv_count++;
    }
    if (argv_count >= EXECVE_MAX_ARGV) return 0;
    if (!append_exec_arg(cfg, &cursor, script_path, cstr_len(script_path), &cfg->argv_offsets[argv_count], &cfg->argv_bytes[argv_count])) return 0;
    argv_count++;
    if (!append_target_argv_tail(cfg, &cursor, argv_va, 1, &argv_count)) return 0;
    cfg->argv_count = (u16)argv_count;

    u64 envp_count = 0;
    if (envp_va != 0) {
        while (envp_count < EXECVE_MAX_ENVP) {
            u64 ptr = 0;
            if (copy_from_target(envp_va + envp_count * 8, &ptr, 8) != 8) {
                u64 vfork_parent = g_proc ? g_proc->vfork_parent_principal : 0;
                const struct trap_request *req = abi_current_request();
                if (vfork_parent == 0 && req != 0) vfork_parent = vfork_parent_for_principal(req->caller_principal);
                if (copy_from_trap_target(vfork_parent, envp_va + envp_count * 8, &ptr, 8) != 8) break;
            }
            if (ptr == 0) break;
            if (!append_target_cstr_arg(cfg, &cursor, ptr, &cfg->envp_offsets[envp_count], &cfg->envp_bytes[envp_count])) break;
            envp_count++;
        }
    }
    cfg->envp_count = (u16)envp_count;
    return 1;
}

static int launch_exec_for_execve(u64 caller_principal, const char *path, u64 argv_va, u64 envp_va, const char *argv0_override, u64 *spawned_principal_out) {
    execve_profile_step("scratch begin");
    if (!ensure_execve_scratch()) { user_log("LinuxAbiServer: execve scratch failed\n"); return 0; }
    execve_profile_step("scratch done");
    if (!is_vm_object_token(g_exec_vm_token)) { user_log("LinuxAbiServer: execve exec token absent\n"); return 0; }
    if (!ensure_standard_interpreter_current()) return 0;
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
    populate_exec_layout_config(cfg);
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

static int launch_exec_for_shebang(
    u64 caller_principal,
    const char *interpreter_load_path,
    const char *interpreter_argv0,
    const char *interpreter_arg,
    const char *script_path,
    u64 argv_va,
    u64 envp_va,
    u64 *spawned_principal_out
) {
    if (!ensure_execve_scratch()) return 0;
    if (!is_vm_object_token(g_exec_vm_token)) return 0;
    if (!ensure_standard_interpreter_current()) return 0;
    if (!is_vm_object_token(g_standard_interpreter_vm_token) || g_standard_interpreter_bytes == 0) return 0;
    if (!start_exec_service()) return 0;
    clear_page(EXECVE_CONFIG_VA);
    struct exec_bootstrap_config *cfg = (struct exec_bootstrap_config *)EXECVE_CONFIG_VA;
    if (!configure_exec_args_for_shebang(cfg, script_path, interpreter_argv0, interpreter_arg, script_path, argv_va, envp_va)) return 0;

    u64 abi_request_va = 0;
    if (!ensure_child_trap_request_page(caller_principal, &abi_request_va)) return 0;
    u64 main_bytes = 0;
    u64 main_vm_token = 0;
    if (!get_exec_vm_for_path(interpreter_load_path, &main_vm_token, &main_bytes)) return 0;

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
    populate_exec_layout_config(cfg);

    struct exec_cache_entry *entry = EXEC_OPT_SERVICE_SOURCE_CACHE ? exec_cache_find(interpreter_load_path, cstr_len(interpreter_load_path)) : 0;
    int send_result = send_exec_launch_request(cfg, entry, main_vm_token, spawned_principal_out);
    if (send_result == 2) send_result = send_exec_launch_request(cfg, entry, main_vm_token, spawned_principal_out);
    return send_result == 1;
}

static void close_cloexec_fds_for_execve(void) {
    if (!g_proc) return;
    for (u64 fd = 0; fd < LINUX_FD_MAX; fd++) {
        struct fd_entry *entry = &g_fds[fd];
        if (entry->kind == FD_UNUSED || (entry->fd_flags & FD_INTERNAL_CLOEXEC) == 0) continue;
        if (fd_entry_is_pipe(entry)) close_pipe_entry(entry);
        if (entry->kind == FD_SOCKET) close_socket_entry(entry);
        entry->kind = FD_UNUSED;
        entry->fd_flags = 0;
    }
}

static void prepare_process_for_exec_replacement(u64 old_principal, u64 spawned_principal) {
    if (!g_proc) return;
    close_cloexec_fds_for_execve();
    clear_process_timers(g_proc);
    reset_signal_dispositions_for_exec(g_proc);
    g_proc->exec_pending = 1;
    g_proc->exec_pending_principal = spawned_principal;
    if (g_root_linux_principal_set && g_root_linux_principal == old_principal) g_root_linux_principal = 0;
    reset_exec_runtime_state();
}

static void rollback_process_exec_replacement(u64 old_principal) {
    if (g_proc) {
        g_proc->principal = old_principal;
        g_proc->exec_pending = 0;
        g_proc->exec_pending_principal = 0;
    }
    if (!g_root_linux_principal_set || g_root_linux_principal == 0) {
        g_root_linux_principal = old_principal;
        g_root_linux_principal_set = 1;
    }
}

static struct ipc_message commit_exec_replacement(u64 old_principal, u64 spawned_principal) {
    prepare_process_for_exec_replacement(old_principal, spawned_principal);
    if (!start_prepared_exec_launch(spawned_principal)) {
        user_log("LinuxAbiServer: exec start failed before old exit\n");
        exec_launch_sequence_release();
        rollback_process_exec_replacement(old_principal);
        return reply(errno_io(), 0);
    }
    exit_trap_target_no_wait(old_principal);
    exec_launch_sequence_release();
    reply_vfork_parent_if_any(g_proc);
    return wait_ipc();
}

static int read_exec_prefix_from_token(u64 token, char *buf, u16 cap, u16 *bytes_out) {
    *bytes_out = 0;
    if (cap == 0) return 0;
    if (!vfs_request(FS_OP_OPEN, token, 0, 0, 0)) return 0;
    volatile struct fs_response_header *response = (volatile struct fs_response_header *)vfs_response_addr();
    if (response->status != FS_STATUS_OK || response->result_token == 0) return 0;
    const u64 open_token = response->result_token;
    u32 request_len = cap;
    if (request_len > FS_RESPONSE_PAYLOAD_BYTES) request_len = FS_RESPONSE_PAYLOAD_BYTES;
    if (!vfs_request(FS_OP_READ, open_token, 0, request_len, 0)) {
        (void)vfs_request(FS_OP_CLOSE, open_token, 0, 0, 0);
        return 0;
    }
    response = (volatile struct fs_response_header *)vfs_response_addr();
    if (response->status != FS_STATUS_OK || response->inline_bytes > request_len) {
        (void)vfs_request(FS_OP_CLOSE, open_token, 0, 0, 0);
        return 0;
    }
    volatile u8 *payload = (volatile u8 *)vfs_response_payload();
    for (u16 i = 0; i < response->inline_bytes; i++) buf[i] = (char)payload[i];
    *bytes_out = response->inline_bytes;
    (void)vfs_request(FS_OP_CLOSE, open_token, 0, 0, 0);
    return 1;
}

static int parse_shebang(const char *buf, u16 len, char *interpreter, char *arg) {
    interpreter[0] = 0;
    arg[0] = 0;
    if (len < 3 || buf[0] != '#' || buf[1] != '!') return 0;
    u16 pos = 2;
    while (pos < len && (buf[pos] == ' ' || buf[pos] == '\t')) pos++;
    u16 out = 0;
    while (pos < len && buf[pos] != 0 && buf[pos] != '\n' && buf[pos] != '\r' && buf[pos] != ' ' && buf[pos] != '\t') {
        if (out + 1 >= FS_MAX_PATH_BYTES) return 0;
        interpreter[out++] = buf[pos++];
    }
    interpreter[out] = 0;
    while (pos < len && (buf[pos] == ' ' || buf[pos] == '\t')) pos++;
    out = 0;
    while (pos < len && buf[pos] != 0 && buf[pos] != '\n' && buf[pos] != '\r') {
        if (out + 1 >= FS_MAX_PATH_BYTES) return 0;
        arg[out++] = buf[pos++];
    }
    arg[out] = 0;
    return interpreter[0] != 0;
}

static struct ipc_message handle_execve(const struct trap_request *req) {
    char *path = g_execve_path_buf;
    if (!copy_cstr_from_target(req->args[0], path, sizeof(g_execve_path_buf))) return reply(errno_fault(), 0);
    if (cstr_len(path) > FS_MAX_PATH_BYTES) return reply(errno_nametoolong(), 0);
    char *virtual_path = g_execve_virtual_path_buf;
    char *resolved_path = g_execve_resolved_path_buf;
    if (!resolve_virtual_path_at(AT_FDCWD_U64, path, virtual_path)) return reply(errno_nametoolong(), 0);
    if (!map_virtual_path_to_host(virtual_path, resolved_path)) return reply(errno_nametoolong(), 0);
    g_execve_profile_enabled = target_env_has_exec_profile(req->args[2]);
    g_execve_profile_detail = target_env_has_exec_profile_detail(req->args[2]);
    g_execve_profile_verbose = target_env_has_exec_profile_verbose(req->args[2]);
    g_execve_fault_trace = target_env_has_exec_fault_trace(req->args[2]);
    if (g_execve_profile_verbose) g_profile_trace_verbose = 1;
    const char *load_path = resolved_path;
    char *uutils_tool = g_execve_uutils_tool_buf;
    const char *uutils_tool_ptr = 0;
    const char *argv0_override = 0;
    if (chroot_is_default() && maybe_uutils_applet_exec(path, uutils_tool, sizeof(g_execve_uutils_tool_buf))) {
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
        g_proc->fault_trace_enabled = (u8)g_execve_fault_trace;
    }
    execve_profile_begin(load_path);
    execve_profile_step("entry");
    const char *proc_exe_path = load_path;
    if (execve_resolve_file_path(load_path, g_execve_proc_exe_path_buf)) proc_exe_path = g_execve_proc_exe_path_buf;
    g_exec_path_len = (u16)cstr_len(proc_exe_path);
    for (u16 i = 0; i < g_exec_path_len; i++) g_exec_path[i] = proc_exe_path[i];
    g_exec_path[g_exec_path_len] = 0;
    u64 exec_probe_token = 0;
    u64 exec_probe_bytes = 0;
    if (!vfs_lookup_file_token(resolved_path, &exec_probe_token, &exec_probe_bytes)) return reply(errno_noent(), 0);
    char *exec_prefix = g_execve_prefix_buf;
    u16 exec_prefix_len = 0;
    char *shebang_interpreter = g_execve_shebang_interpreter_buf;
    char *shebang_arg = g_execve_shebang_arg_buf;
    int exec_prefix_ok = read_exec_prefix_from_token(exec_probe_token, exec_prefix, sizeof(g_execve_prefix_buf), &exec_prefix_len);
    int shebang_ok = exec_prefix_ok ? parse_shebang(exec_prefix, exec_prefix_len, shebang_interpreter, shebang_arg) : 0;
    if (shebang_ok)
    {
        char *interpreter_resolved = g_execve_interpreter_resolved_buf;
        if (!resolve_path_at(AT_FDCWD_U64, shebang_interpreter, interpreter_resolved)) return reply(errno_noent(), 0);
        const char *interpreter_load_path = interpreter_resolved;
        const char *interpreter_argv0 = shebang_interpreter;
        u64 interpreter_probe_token = 0;
        u64 interpreter_probe_bytes = 0;
        if (!vfs_lookup_file_token(interpreter_resolved, &interpreter_probe_token, &interpreter_probe_bytes) &&
            !chroot_is_default() &&
            exec_cstr_eq(shebang_interpreter, "/bin/sh"))
        {
            interpreter_load_path = "/bin/sh";
        }
        const u64 old_principal = req->caller_principal;
        u64 spawned_principal = 0;
        exec_launch_sequence_acquire();
        if (!launch_exec_for_shebang(
            req->caller_principal,
            interpreter_load_path,
            interpreter_argv0,
            shebang_arg[0] != 0 ? shebang_arg : 0,
            virtual_path,
            req->args[1],
            req->args[2],
            &spawned_principal
        )) {
            exec_launch_sequence_release();
            reply_vfork_parent_if_any(g_proc);
            return reply(errno_io(), 0);
        }
        return commit_exec_replacement(old_principal, spawned_principal);
    }
    if (uutils_tool_ptr != 0 && !vfs_lookup_file_token(load_path, &exec_probe_token, &exec_probe_bytes)) return reply(errno_noent(), 0);
    execve_profile_step("probe lookup done");
    const u64 old_principal = req->caller_principal;
    u64 spawned_principal = 0;
    exec_launch_sequence_acquire();
    if (!launch_exec_for_execve(req->caller_principal, load_path, req->args[1], req->args[2], argv0_override, &spawned_principal)) {
        exec_launch_sequence_release();
        reply_vfork_parent_if_any(g_proc);
        return reply(errno_io(), 0);
    }
    execve_profile_step("spawn exec done");
    execve_profile_flush();
    return commit_exec_replacement(old_principal, spawned_principal);
}
