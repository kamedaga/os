static u64 errno_from_fs_status(i32 status);

static u32 linux_mode_from_fs(u32 fs_mode, u8 kind) { u32 perm = kind == FS_OBJECT_SYMLINK ? 0777 : 0555; u32 type = (kind == FS_OBJECT_DIRECTORY || kind == FS_OBJECT_MOUNT) ? 0040000 : (kind == FS_OBJECT_SYMLINK ? 0120000 : 0100000); const u32 fs_type = fs_mode & 0xF000; if (fs_type == FS_DIR_MODE) type = 0040000; if (fs_type == FS_FILE_MODE) type = 0100000; if (fs_type == FS_SYMLINK_MODE) type = 0120000; return type | perm; }
static u64 linux_ino_from_path(const char *path) {
    u64 hash = 1469598103934665603ULL;
    for (u64 i = 0; path[i] != 0; i++) {
        hash ^= (u8)path[i];
        hash *= 1099511628211ULL;
    }
    return hash != 0 ? hash : 1;
}
static int append_dirent_name_to_path(char *out, u16 *len, volatile u8 *name, u16 name_len) {
    if (name_len == 0) return 1;
    if (name_len == 1 && name[0] == '.') return 1;
    if (name_len == 2 && name[0] == '.' && name[1] == '.') {
        while (*len > 1 && out[*len - 1] != '/') *len = (u16)(*len - 1);
        if (*len > 1) *len = (u16)(*len - 1);
        out[*len] = 0;
        return 1;
    }
    if (*len != 1) {
        if ((u64)*len + 1 > FS_MAX_PATH_BYTES) return 0;
        out[(*len)++] = '/';
    }
    if ((u64)*len + name_len > FS_MAX_PATH_BYTES) return 0;
    for (u16 i = 0; i < name_len; i++) out[*len + i] = (char)name[i];
    *len = (u16)(*len + name_len);
    out[*len] = 0;
    return 1;
}
static u64 linux_ino_from_dirent(u64 fd, volatile u8 *name, u16 name_len) {
    if (!fd_valid(fd) || g_fds[fd].path_len == 0) return 1;
    char path[FS_MAX_PATH_BYTES + 1];
    u16 len = g_fds[fd].path_len;
    if (len > FS_MAX_PATH_BYTES) return 1;
    for (u16 i = 0; i < len; i++) path[i] = g_fds[fd].path[i];
    path[len] = 0;
    if (!append_dirent_name_to_path(path, &len, name, name_len)) return 1;
    return linux_ino_from_path(path);
}
static void fill_linux_stat(struct linux_stat *st, const struct fs_stat_record *rec, u64 size, u8 kind) {
    u8 *p = (u8 *)st; for (u64 i = 0; i < sizeof(*st); i++) p[i] = 0;
    st->st_nlink = (kind == FS_OBJECT_DIRECTORY || kind == FS_OBJECT_MOUNT) ? 2 : 1; st->st_mode = linux_mode_from_fs(rec->mode_bits, kind); st->st_size = (i64)size; st->st_blksize = 4096; st->st_blocks = (i64)((size + 511) / 512); st->st_mtime = (i64)rec->mtime_unix_sec; st->st_atime = st->st_mtime; st->st_ctime = st->st_mtime;
}
static void fill_linux_stat_path(struct linux_stat *st, const struct fs_stat_record *rec, u64 size, u8 kind, const char *path) {
    fill_linux_stat(st, rec, size, kind);
    st->st_dev = 1;
    st->st_ino = linux_ino_from_path(path);
}
static void fill_linux_pipe_stat(struct linux_stat *st, u64 fd) {
    u8 *p = (u8 *)st; for (u64 i = 0; i < sizeof(*st); i++) p[i] = 0;
    st->st_dev = 1;
    st->st_ino = 0x200000ULL + fd;
    st->st_nlink = 1;
    st->st_mode = 0010000 | 0600;
    st->st_blksize = 4096;
}
static void fill_linux_statfs(struct linux_statfs *st) {
    u8 *p = (u8 *)st; for (u64 i = 0; i < sizeof(*st); i++) p[i] = 0;
    st->f_type = 0x4d44;
    st->f_bsize = 4096;
    st->f_blocks = 1024 * 1024;
    st->f_bfree = 512 * 1024;
    st->f_bavail = 512 * 1024;
    st->f_files = 65536;
    st->f_ffree = 32768;
    st->f_namelen = 255;
    st->f_frsize = 4096;
}

static void fd_set_path(struct fd_entry *fd, const char *path) {
    u64 len = cstr_len(path);
    if (len > FS_MAX_PATH_BYTES) len = FS_MAX_PATH_BYTES;
    fd->path_len = (u16)len;
    for (u64 i = 0; i < len; i++) fd->path[i] = path[i];
    fd->path[len] = 0;
}

static void remove_fd_from_current_epoll_sets(u64 fd);

static int path_should_trace_io(const char *path) {
    (void)path;
    return 0;
}

static void log_io_path(const char *prefix, const char *path) {
    user_log(prefix);
    user_log(path);
    user_log("\n");
}

static void log_fd_write_failure(u64 fd) {
    user_log("LinuxAbiServer: file write failed path=");
    if (fd_valid(fd) && g_fds[fd].path_len != 0) user_log_len(g_fds[fd].path, g_fds[fd].path_len);
    else user_log("(unknown)");
    user_log("\n");
    user_log("LinuxAbiServer: vfs write status=");
    user_log_hex_value(g_last_vfs_write_status);
    user_log("LinuxAbiServer: vfs write offset=");
    user_log_hex_value(g_last_vfs_write_offset);
    user_log("LinuxAbiServer: vfs write length=");
    user_log_hex_value(g_last_vfs_write_length);
}

static int append_path_component(char *out, u16 *len, const char *component, u16 component_len) {
    if (component_len == 0) return 1;
    if (component_len == 1 && component[0] == '.') return 1;
    if (component_len == 2 && component[0] == '.' && component[1] == '.') {
        while (*len > 1 && out[*len - 1] != '/') *len = (u16)(*len - 1);
        if (*len > 1) *len = (u16)(*len - 1);
        out[*len] = 0;
        return 1;
    }
    if (*len != 1) {
        if ((u64)*len + 1 > FS_MAX_PATH_BYTES) return 0;
        out[(*len)++] = '/';
    }
    if ((u64)*len + component_len > FS_MAX_PATH_BYTES) return 0;
    for (u16 i = 0; i < component_len; i++) out[*len + i] = component[i];
    *len = (u16)(*len + component_len);
    out[*len] = 0;
    return 1;
}

static int normalize_path(const char *base, const char *path, char *out) {
    char joined[FS_MAX_PATH_BYTES * 2 + 2];
    u16 joined_len = 0;
    if (path[0] == '/') {
        joined[joined_len++] = '/';
    } else {
        const u64 base_len = cstr_len(base);
        if (base_len == 0 || base_len > FS_MAX_PATH_BYTES) return 0;
        for (u64 i = 0; i < base_len; i++) joined[joined_len++] = base[i];
        if (joined_len == 0 || joined[joined_len - 1] != '/') joined[joined_len++] = '/';
    }
    for (u64 i = 0; path[i] != 0; i++) {
        if ((u64)joined_len + 1 >= sizeof(joined)) return 0;
        joined[joined_len++] = path[i];
    }
    joined[joined_len] = 0;

    u16 out_len = 1;
    out[0] = '/';
    out[1] = 0;
    u16 pos = 0;
    while (pos < joined_len) {
        while (pos < joined_len && joined[pos] == '/') pos++;
        const u16 start = pos;
        while (pos < joined_len && joined[pos] != '/') pos++;
        if (!append_path_component(out, &out_len, &joined[start], (u16)(pos - start))) return 0;
    }
    return 1;
}

static int chroot_is_default(void) {
    return g_proc == 0 || (g_proc->root_len == 1 && g_proc->root_path[0] == '/');
}

static int map_virtual_path_to_host(const char *path, char *out) {
    if (chroot_is_default()) return normalize_path("/", path, out);
    if (path[0] != '/') return 0;
    const u64 root_len = g_proc->root_len;
    if (root_len == 0 || root_len > FS_MAX_PATH_BYTES) return 0;
    if (path[1] == 0) {
        for (u16 i = 0; i <= g_proc->root_len; i++) out[i] = g_proc->root_path[i];
        return 1;
    }
    if (root_len + cstr_len(path) > FS_MAX_PATH_BYTES) return 0;
    for (u16 i = 0; i < g_proc->root_len; i++) out[i] = g_proc->root_path[i];
    u16 pos = g_proc->root_len;
    if (pos > 1 && out[pos - 1] == '/') pos--;
    for (u16 i = 0; path[i] != 0; i++) out[pos + i] = path[i];
    out[pos + cstr_len(path)] = 0;
    return normalize_path("/", out, out);
}

static int host_path_to_virtual(const char *host, char *out) {
    if (chroot_is_default()) return normalize_path("/", host, out);
    const u64 root_len = g_proc->root_len;
    if (root_len == 0 || root_len > FS_MAX_PATH_BYTES) return 0;
    for (u16 i = 0; i < root_len; i++) {
        if (host[i] != g_proc->root_path[i]) return 0;
    }
    if (host[root_len] == 0) {
        out[0] = '/';
        out[1] = 0;
        return 1;
    }
    if (root_len > 1 && host[root_len] != '/') return 0;
    return normalize_path("/", host + root_len, out);
}

static int resolve_virtual_path_at(u64 dirfd, const char *path, char *out) {
    if (path[0] == '/') return normalize_path("/", path, out);
    if (dirfd == AT_FDCWD_U64) return normalize_path(g_cwd, path, out);
    if (!fd_valid(dirfd) || g_fds[dirfd].kind != FD_DIR || g_fds[dirfd].path_len == 0) return 0;
    char base[FS_MAX_PATH_BYTES + 1];
    if (!host_path_to_virtual(g_fds[dirfd].path, base)) return 0;
    return normalize_path(base, path, out);
}

static int resolve_path_at(u64 dirfd, const char *path, char *out) {
    char virtual_path[FS_MAX_PATH_BYTES + 1];
    if (!resolve_virtual_path_at(dirfd, path, virtual_path)) return 0;
    return map_virtual_path_to_host(virtual_path, out);
}

static int path_is_dev_file(const char *path) {
    return path[0] == '/' && path[1] == 'd' && path[2] == 'e' && path[3] == 'v' && path[4] == '/';
}

static int path_is_dev_tty(const char *path) {
    return path[0] == '/' && path[1] == 'd' && path[2] == 'e' && path[3] == 'v' && path[4] == '/' &&
        path[5] == 't' && path[6] == 't' && path[7] == 'y' && path[8] == 0;
}

static int fs_path_eq_literal(const char *path, const char *literal) {
    u64 i = 0;
    while (path[i] != 0 && literal[i] != 0) {
        if (path[i] != literal[i]) return 0;
        i++;
    }
    return path[i] == 0 && literal[i] == 0;
}

static int path_is_proc_self_exe(const char *path) {
    return fs_path_eq_literal(path, "/proc/self/exe");
}

static int path_is_dev_random(const char *path) {
    if (!(path[0] == '/' && path[1] == 'd' && path[2] == 'e' && path[3] == 'v' && path[4] == '/')) return 0;
    if (path[5] == 'r' && path[6] == 'a' && path[7] == 'n' && path[8] == 'd' && path[9] == 'o' && path[10] == 'm' && path[11] == 0) return 1;
    return path[5] == 'u' && path[6] == 'r' && path[7] == 'a' && path[8] == 'n' && path[9] == 'd' && path[10] == 'o' && path[11] == 'm' && path[12] == 0;
}

static int fs_cpu_has_rdrand(void) {
    u32 eax = 1, ebx = 0, ecx = 0, edx = 0;
    __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
    (void)ebx;
    (void)edx;
    return (ecx & (1u << 30)) != 0;
}

static int fs_rdrand64(u64 *out) {
    unsigned char ok = 0;
    u64 value = 0;
    for (u32 attempt = 0; attempt < 16; attempt++) {
        __asm__ volatile("rdrand %0; setc %1" : "=r"(value), "=qm"(ok));
        if (ok) {
            *out = value;
            return 1;
        }
    }
    return 0;
}

static int read_symlink_target_for_follow(const char *path, char *target, u64 target_capacity, u64 *target_len_out) {
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

static int parent_path_for_link(const char *path, char *parent) {
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

static int resolve_symlink_target_for_follow(const char *link_path, char *out) {
    char target[FS_MAX_PATH_BYTES + 1];
    u64 target_len = 0;
    if (!read_symlink_target_for_follow(link_path, target, sizeof(target) - 1, &target_len)) return 0;
    if (target_len == 0) return 0;
    if (target[0] == '/') return normalize_path("/", target, out);
    char parent[FS_MAX_PATH_BYTES + 1];
    if (!parent_path_for_link(link_path, parent)) return 0;
    return normalize_path(parent, target, out);
}

static int join_path_suffix(const char *base, const char *suffix, char *out) {
    char combined[FS_MAX_PATH_BYTES + 1];
    const u64 base_len = cstr_len(base);
    const u64 suffix_len = cstr_len(suffix);
    if (base_len == 0 || base_len + suffix_len > FS_MAX_PATH_BYTES) return 0;
    u64 pos = 0;
    for (u64 i = 0; i < base_len && pos < sizeof(combined) - 1; i++) combined[pos++] = base[i];
    if (suffix_len != 0) {
        u64 suffix_pos = 0;
        if (base_len == 1 && base[0] == '/') {
            while (suffix_pos < suffix_len && suffix[suffix_pos] == '/') suffix_pos++;
        }
        for (; suffix_pos < suffix_len && pos < sizeof(combined) - 1; suffix_pos++) combined[pos++] = suffix[suffix_pos];
    }
    combined[pos] = 0;
    return normalize_path("/", combined, out);
}

static int resolve_path_symlink_components(const char *path, int follow_final, char *out) {
    char current[FS_MAX_PATH_BYTES + 1];
    if (!normalize_path("/", path, current)) return 0;
    for (u32 depth = 0; depth < 16; depth++) {
        const u64 len = cstr_len(current);
        u64 pos = current[0] == '/' ? 1 : 0;
        int restarted = 0;
        while (pos < len) {
            while (pos < len && current[pos] == '/') pos++;
            if (pos >= len) break;
            const u64 start = pos;
            while (pos < len && current[pos] != '/') pos++;
            const u64 component_end = pos;
            u64 after = pos;
            while (after < len && current[after] == '/') after++;
            const int is_final = after >= len;
            if (!is_final || follow_final) {
                char prefix[FS_MAX_PATH_BYTES + 1];
                if (component_end > FS_MAX_PATH_BYTES) return 0;
                for (u64 i = 0; i < component_end; i++) prefix[i] = current[i];
                prefix[component_end] = 0;
                struct fs_stat_record rec;
                u64 token = 0;
                u64 size = 0;
                u8 kind = FS_OBJECT_NONE;
                if (vfs_lookup_stat(prefix, &token, &rec, &size, &kind) && kind == FS_OBJECT_SYMLINK) {
                    char target[FS_MAX_PATH_BYTES + 1];
                    char next[FS_MAX_PATH_BYTES + 1];
                    if (!resolve_symlink_target_for_follow(prefix, target)) return 0;
                    if (!join_path_suffix(target, current + component_end, next)) return 0;
                    for (u64 i = 0; i <= FS_MAX_PATH_BYTES; i++) {
                        current[i] = next[i];
                        if (next[i] == 0) break;
                    }
                    restarted = 1;
                    break;
                }
                (void)start;
                (void)token;
                (void)rec;
                (void)size;
            }
            pos = after;
        }
        if (restarted) continue;
        if (len > FS_MAX_PATH_BYTES) return 0;
        for (u64 i = 0; i <= len; i++) out[i] = current[i];
        return 1;
    }
    return 0;
}

static int vfs_lookup_stat_follow_final(
    const char *path,
    int follow_final,
    char *resolved_out,
    u64 *token_out,
    struct fs_stat_record *rec_out,
    u64 *size_out,
    u8 *kind_out
) {
    char current[FS_MAX_PATH_BYTES + 1];
    if (!resolve_path_symlink_components(path, follow_final, current)) return 0;
    if (!vfs_lookup_stat(current, token_out, rec_out, size_out, kind_out)) return 0;
    const u64 len = cstr_len(current);
    if (len > FS_MAX_PATH_BYTES) return 0;
    for (u64 i = 0; i <= len; i++) resolved_out[i] = current[i];
    return 1;
}

static u64 random_read_to_target(u64 dst, u64 len, int *fault) {
    *fault = 0;
    if (len == 0) return 0;
    if (!fs_cpu_has_rdrand()) return 0;
    u64 copied = 0;
    while (copied < len) {
        u64 value = 0;
        if (!fs_rdrand64(&value)) break;
        const u64 chunk = min_u64(len - copied, sizeof(value));
        if (copy_to_target(dst + copied, &value, chunk) != chunk) {
            *fault = 1;
            break;
        }
        copied += chunk;
    }
    return copied;
}

static struct ipc_message handle_openat(const struct trap_request *req, int old_open) {
    char path[256];
    char resolved[FS_MAX_PATH_BYTES + 1];
    const u64 dirfd = old_open ? AT_FDCWD_U64 : req->args[0];
    const u64 path_ptr = old_open ? req->args[0] : req->args[1];
    const u64 flags = old_open ? req->args[1] : req->args[2];
    const u64 access_mode = flags & O_ACCMODE;
    const u32 new_fd_flags = (u32)(access_mode | (flags & (O_NONBLOCK | O_APPEND)) | ((flags & O_CLOEXEC) != 0 ? FD_INTERNAL_CLOEXEC : 0));
    if (access_mode != O_RDONLY && access_mode != O_WRONLY && access_mode != O_RDWR) return reply(errno_acces(), 0);
    if ((flags & O_TRUNC) != 0 && access_mode == O_RDONLY) return reply(errno_acces(), 0);
    if (!copy_cstr_from_target(path_ptr, path, sizeof(path))) return reply(errno_fault(), 0);
    if (path[0] == 0) return reply(errno_noent(), 0);
    if (!resolve_path_at(dirfd, path, resolved)) return reply(errno_nametoolong(), 0);
    if (((flags & (O_CREAT | O_TRUNC)) != 0 || access_mode != O_RDONLY) && path_should_trace_io(resolved)) {
        log_io_path("LinuxAbiServer: open write path=", resolved);
        user_log("LinuxAbiServer: open flags=");
        user_log_hex_value(flags);
    }
    if (path_is_dev_tty(resolved)) {
        const int fd = alloc_fd(); if (fd < 0) return reply(errno_busy(), 0);
        g_fds[fd].kind = g_console.active ? FD_TTY : FD_STDIO;
        g_fds[fd].token = 0;
        g_fds[fd].offset = 0;
        g_fds[fd].size = 0;
        g_fds[fd].fd_flags = new_fd_flags;
        g_fds[fd].mode_bits = FS_FILE_MODE;
        g_fds[fd].object_kind = FS_OBJECT_FILE;
        g_fds[fd].path_len = 0;
        g_fds[fd].path[0] = 0;
        sync_fd_to_thread_group((u64)fd);
        return reply((u64)fd, 0);
    }
    if (path_is_dev_random(resolved)) {
        const int fd = alloc_fd(); if (fd < 0) return reply(errno_busy(), 0);
        g_fds[fd].kind = FD_RANDOM;
        g_fds[fd].token = 0;
        g_fds[fd].offset = 0;
        g_fds[fd].size = 0;
        g_fds[fd].fd_flags = new_fd_flags;
        g_fds[fd].mode_bits = FS_FILE_MODE;
        g_fds[fd].object_kind = FS_OBJECT_FILE;
        fd_set_path(&g_fds[fd], resolved);
        sync_fd_to_thread_group((u64)fd);
        return reply((u64)fd, 0);
    }
    struct fs_stat_record rec; u64 token = 0; u64 size = 0; u8 kind = FS_OBJECT_NONE;
    char opened_path[FS_MAX_PATH_BYTES + 1];
    if (!vfs_lookup_stat_follow_final(resolved, (flags & O_NOFOLLOW) == 0, opened_path, &token, &rec, &size, &kind)) {
        if ((flags & O_CREAT) == 0) return reply(errno_noent(), 0);
        if (!vfs_create_path(resolved, 0)) {
            if (path_should_trace_io(resolved)) log_io_path("LinuxAbiServer: create request failed path=", resolved);
            return reply(errno_io(), 0);
        }
        volatile struct fs_response_header *created = (volatile struct fs_response_header *)vfs_response_addr();
        if (created->status != FS_STATUS_OK || created->result_token == 0) {
            if (path_should_trace_io(resolved)) {
                log_io_path("LinuxAbiServer: create failed path=", resolved);
                user_log("LinuxAbiServer: create status=");
                user_log_hex_value((u64)(u32)created->status);
            }
            return reply(errno_acces(), 0);
        }
        invalidate_exec_cache_for_path(resolved);
        token = created->result_token;
        size = created->file_bytes;
        kind = created->object_kind;
        rec.object_kind = kind;
        rec.size_bytes = size;
        rec.mode_bits = FS_FILE_MODE;
        rec.mtime_unix_sec = 0;
        for (u64 i = 0; i <= FS_MAX_PATH_BYTES; i++) {
            opened_path[i] = resolved[i];
            if (resolved[i] == 0) break;
        }
    } else if ((flags & O_TRUNC) != 0 && access_mode != O_RDONLY && !path_is_dev_file(resolved)) {
        if (!vfs_create_path(opened_path, 1)) {
            if (path_should_trace_io(opened_path)) log_io_path("LinuxAbiServer: truncate request failed path=", opened_path);
            return reply(errno_io(), 0);
        }
        volatile struct fs_response_header *created = (volatile struct fs_response_header *)vfs_response_addr();
        if (created->status != FS_STATUS_OK || created->result_token == 0) {
            if (path_should_trace_io(opened_path)) {
                log_io_path("LinuxAbiServer: truncate failed path=", opened_path);
                user_log("LinuxAbiServer: truncate status=");
                user_log_hex_value((u64)(u32)created->status);
            }
            return reply(errno_acces(), 0);
        }
        invalidate_exec_cache_for_path(opened_path);
        token = created->result_token;
        size = 0;
        kind = created->object_kind;
    }
    const int fd = alloc_fd(); if (fd < 0) return reply(errno_busy(), 0);
    if (kind == FS_OBJECT_DIRECTORY || kind == FS_OBJECT_MOUNT) {
        g_fds[fd].kind = FD_DIR; g_fds[fd].token = token; g_fds[fd].offset = 0; g_fds[fd].size = 0; g_fds[fd].fd_flags = new_fd_flags; g_fds[fd].mode_bits = rec.mode_bits; g_fds[fd].object_kind = kind;
        fd_set_path(&g_fds[fd], opened_path);
        sync_fd_to_thread_group((u64)fd);
        return reply((u64)fd, 0);
    }
    if ((flags & O_DIRECTORY) != 0) return reply(errno_notdir(), 0);
    if (access_mode == O_RDONLY && (flags & (O_CREAT | O_TRUNC)) == 0 && cacheable_readonly_path(opened_path)) {
        struct file_cache_entry *cached = file_cache_find_by_path(opened_path);
        if (cached != 0 && cached->size == size) {
            g_prof.open_cache_hits++;
            g_fds[fd].kind = FD_FILE;
            g_fds[fd].token = cached->token != 0 ? cached->token : token;
            g_fds[fd].offset = 0;
            g_fds[fd].size = size;
            g_fds[fd].fd_flags = new_fd_flags;
            g_fds[fd].mode_bits = rec.mode_bits;
            g_fds[fd].object_kind = FS_OBJECT_FILE;
            fd_set_path(&g_fds[fd], opened_path);
            sync_fd_to_thread_group((u64)fd);
            return reply((u64)fd, 0);
        }
        g_prof.open_cache_misses++;
    }
    if (!vfs_request(FS_OP_OPEN, token, 0, 0, 0)) {
        if (path_should_trace_io(opened_path)) log_io_path("LinuxAbiServer: open request failed path=", opened_path);
        return reply(errno_io(), 0);
    }
    volatile struct fs_response_header *response = (volatile struct fs_response_header *)vfs_response_addr();
    if (response->status != FS_STATUS_OK || response->result_token == 0) {
        if (path_should_trace_io(opened_path)) {
            log_io_path("LinuxAbiServer: open failed path=", opened_path);
            user_log("LinuxAbiServer: open status=");
            user_log_hex_value((u64)(u32)response->status);
        }
        return reply(errno_acces(), 0);
    }
    g_fds[fd].kind = FD_FILE; g_fds[fd].token = response->result_token; g_fds[fd].offset = 0; g_fds[fd].size = response->file_bytes != 0 ? response->file_bytes : size; g_fds[fd].fd_flags = new_fd_flags; g_fds[fd].mode_bits = rec.mode_bits; g_fds[fd].object_kind = FS_OBJECT_FILE;
    fd_set_path(&g_fds[fd], opened_path);
    sync_fd_to_thread_group((u64)fd);
    return reply((u64)fd, 0);
}

static struct ipc_message handle_read(const struct trap_request *req) {
    const u64 fd = req->args[0]; const u64 dst = req->args[1]; const u64 len = req->args[2];
    if (!fd_valid(fd)) return reply(errno_badf(), 0);
    if (len == 0) return reply(0, 0);
    if (g_fds[fd].kind == FD_STDIO) {
        (void)dst;
        return reply(0, 0);
    }
    if (g_fds[fd].kind == FD_TTY) {
        int fault = 0;
        const u64 n = console_read_to_target(dst, len, &fault, g_fds[fd].fd_flags);
        return reply(fault ? errno_fault() : n, 0);
    }
    if (g_fds[fd].kind == FD_PIPE_READ) {
        g_prof.pipe_read_calls++;
        const u8 pipe_id = g_fds[fd].pipe_id;
        if (pipe_id >= PIPE_MAX || !g_pipes[pipe_id].used) return reply(errno_badf(), 0);
        reconcile_pipe_refs();
        struct pipe_entry *pipe = &g_pipes[pipe_id];
        if (pipe->len == 0 && pipe_has_blocking_writer(pipe_id, g_proc ? g_proc->pid : 0)) {
            if (pipe->pending_read) { g_prof.pipe_read_again++; return reply(errno_again(), 0); }
            if (!fault_in_target_anon_lazy_range(dst, min_u64(len, PIPE_BUFFER_BYTES), 1)) return reply(errno_fault(), 0);
            g_prof.pipe_read_blocked++;
            pipe_trace_state("read_block", pipe_id);
            pipe->pending_read = 1;
            pipe->pending_principal = req->caller_principal;
            pipe->pending_dst = dst;
            pipe->pending_len = len;
            detach_reply_token();
            return wait_linux_abi_event();
        }
        int fault = 0; const u64 n = pipe_read_to_target(fd, dst, len, &fault); sync_fd_to_thread_group(fd); return reply(fault ? errno_fault() : n, 0);
    }
    if (g_fds[fd].kind == FD_RANDOM) {
        int fault = 0;
        const u64 n = random_read_to_target(dst, len, &fault);
        return reply(fault ? errno_fault() : (n != 0 ? n : errno_again()), 0);
    }
    if (g_fds[fd].kind == FD_EVENTFD) {
        int fault = 0;
        const u64 n = eventfd_read_to_target(fd, dst, len, &fault);
        return reply(fault ? errno_fault() : n, 0);
    }
    if (g_fds[fd].kind == FD_SOCKET) {
        return reply(socket_read_to_target(fd, dst, len), 0);
    }
    if (g_fds[fd].kind != FD_FILE) return reply(errno_badf(), 0);
    if (g_fds[fd].path_len != 0 && cacheable_readonly_path(g_fds[fd].path)) {
        int fault = 0;
        const u64 n = file_cache_read_to_target(&g_fds[fd], g_fds[fd].offset, dst, len, &fault);
        if (fault) return reply(errno_fault(), 0);
        if (n != 0 || g_fds[fd].offset >= g_fds[fd].size) {
            g_fds[fd].offset += n;
            sync_fd_to_thread_group(fd);
            return reply(n, 0);
        }
    }
    u64 copied = 0;
    while (copied < len && g_fds[fd].offset < g_fds[fd].size) {
        u64 request_len = min_u64(len - copied, FS_RESPONSE_PAYLOAD_BYTES);
        const u64 remaining = g_fds[fd].size - g_fds[fd].offset;
        if (request_len > remaining) request_len = remaining;
        if (len - copied > FS_RESPONSE_PAYLOAD_BYTES) {
            u64 bulk_len = min_u64(len - copied, FS_BULK_READ_BYTES);
            if (bulk_len > remaining) bulk_len = remaining;
            u64 bulk_bytes = 0;
            if (vfs_read_bulk_to_target(g_fds[fd].token, g_fds[fd].offset, (u32)bulk_len, dst + copied, &bulk_bytes)) {
                if (bulk_bytes == 0) break;
                profile_fs_read_path(&g_fds[fd], bulk_bytes);
                copied += bulk_bytes;
                g_fds[fd].offset += bulk_bytes;
                sync_fd_to_thread_group(fd);
                continue;
            }
        }
        if (!vfs_request(FS_OP_READ, g_fds[fd].token, g_fds[fd].offset, (u32)request_len, 0)) return copied != 0 ? reply(copied, 0) : reply(errno_io(), 0);
        volatile struct fs_response_header *response = (volatile struct fs_response_header *)vfs_response_addr();
        if (response->status != FS_STATUS_OK) return copied != 0 ? reply(copied, 0) : reply(errno_io(), 0);
        if (response->inline_bytes == 0) break;
        if (copy_to_target(dst + copied, vfs_response_payload(), response->inline_bytes) != response->inline_bytes) {
            return reply(errno_fault(), 0);
        }
        profile_fs_read_path(&g_fds[fd], response->inline_bytes);
        copied += response->inline_bytes; g_fds[fd].offset += response->inline_bytes; sync_fd_to_thread_group(fd);
        if (response->inline_bytes < request_len) break;
    }
    return reply(copied, 0);
}

static u64 read_fd_at_to_target_mode(const struct fd_entry *fd, u64 file_offset, u64 dst, u64 len, int *fault, int direct_bulk) {
    u64 copied = 0;
    *fault = 0;
    if (!direct_bulk && fd->path_len != 0 && cacheable_readonly_path(fd->path)) {
        const u64 n = file_cache_read_to_target(fd, file_offset, dst, len, fault);
        if (*fault || n != 0 || file_offset >= fd->size) return n;
    }
    while (copied < len && file_offset + copied < fd->size) {
        u64 request_len = min_u64(len - copied, FS_RESPONSE_PAYLOAD_BYTES);
        const u64 remaining = fd->size - (file_offset + copied);
        if (request_len > remaining) request_len = remaining;
        if (len - copied > FS_RESPONSE_PAYLOAD_BYTES) {
            u64 bulk_len = min_u64(len - copied, FS_BULK_READ_BYTES);
            if (bulk_len > remaining) bulk_len = remaining;
            u64 bulk_bytes = 0;
            const int bulk_ok = direct_bulk ?
                vfs_read_bulk_direct_to_target(fd->token, file_offset + copied, (u32)bulk_len, dst + copied, &bulk_bytes) :
                vfs_read_bulk_to_target(fd->token, file_offset + copied, (u32)bulk_len, dst + copied, &bulk_bytes);
            if (bulk_ok) {
                if (bulk_bytes == 0) break;
                profile_fs_read_path(fd, bulk_bytes);
                copied += bulk_bytes;
                continue;
            }
        }
        if (!vfs_request(FS_OP_READ, fd->token, file_offset + copied, (u32)request_len, 0)) break;
        volatile struct fs_response_header *response = (volatile struct fs_response_header *)vfs_response_addr();
        if (response->status != FS_STATUS_OK) break;
        if (response->inline_bytes == 0) break;
        if (copy_to_target(dst + copied, vfs_response_payload(), response->inline_bytes) != response->inline_bytes) {
            *fault = 1;
            break;
        }
        profile_fs_read_path(fd, response->inline_bytes);
        copied += response->inline_bytes;
        if (response->inline_bytes < request_len) break;
    }
    return copied;
}

static u64 read_fd_at_to_target(const struct fd_entry *fd, u64 file_offset, u64 dst, u64 len, int *fault) {
    return read_fd_at_to_target_mode(fd, file_offset, dst, len, fault, 0);
}

static u64 read_fd_at_to_fresh_target_pages(const struct fd_entry *fd, u64 file_offset, u64 dst, u64 len, int *fault) {
    return read_fd_at_to_target_mode(fd, file_offset, dst, len, fault, LINUX_ENABLE_DIRECT_MMAP_BULK);
}

static struct ipc_message handle_pread64(const struct trap_request *req) {
    const u64 fd = req->args[0]; const u64 dst = req->args[1]; const u64 len = req->args[2]; const u64 offset = req->args[3];
    if (!fd_valid(fd)) return reply(errno_badf(), 0); if (len == 0) return reply(0, 0); if (g_fds[fd].kind != FD_FILE) return reply(errno_badf(), 0);
    int fault = 0; const u64 copied = read_fd_at_to_target(&g_fds[fd], offset, dst, len, &fault);
    if (fault) return reply(errno_fault(), 0);
    return copied != 0 ? reply(copied, 0) : reply(0, 0);
}

static struct ipc_message handle_pwrite64(const struct trap_request *req) {
    const u64 fd = req->args[0]; const u64 src = req->args[1]; const u64 len = req->args[2]; const u64 offset = req->args[3];
    if (!fd_valid(fd)) return reply(errno_badf(), 0);
    if (len == 0) return reply(0, 0);
    if (g_fds[fd].kind != FD_FILE) return reply(errno_badf(), 0);
    int fault = 0;
    const u64 n = vfs_write_from_target(g_fds[fd].token, offset, src, len, &fault);
    if (fault) return reply(errno_fault(), 0);
    if (n == 0) {
        log_fd_write_failure(fd);
        return reply(errno_io(), 0);
    }
    g_prof.fs_write_bytes += n;
    invalidate_exec_cache_for_path(g_fds[fd].path);
    const u64 end = offset + n;
    if (end > g_fds[fd].size) {
        g_fds[fd].size = end;
        sync_fd_to_thread_group(fd);
    }
    return reply(n, 0);
}

static int ranges_overlap(u64 a_start, u64 a_len, u64 b_start, u64 b_len) {
    if (a_len == 0 || b_len == 0) return 0;
    u64 a_end = 0;
    u64 b_end = 0;
    if (u64_add_overflows(a_start, a_len, &a_end)) return 1;
    if (u64_add_overflows(b_start, b_len, &b_end)) return 1;
    return a_start < b_end && b_start < a_end;
}

static struct ipc_message handle_copy_file_range(const struct trap_request *req) {
    const u64 fd_in = req->args[0];
    const u64 off_in_va = req->args[1];
    const u64 fd_out = req->args[2];
    const u64 off_out_va = req->args[3];
    const u64 len = req->args[4];
    const u64 flags = req->args[5];
    if (flags != 0) return reply(errno_inval(), 0);
    if (len == 0) return reply(0, 0);
    if (!fd_valid(fd_in) || !fd_valid(fd_out)) return reply(errno_badf(), 0);
    if (g_fds[fd_in].kind != FD_FILE || g_fds[fd_out].kind != FD_FILE) return reply(errno_badf(), 0);
    if ((g_fds[fd_in].fd_flags & O_ACCMODE) == O_WRONLY) return reply(errno_badf(), 0);
    if ((g_fds[fd_out].fd_flags & O_ACCMODE) == O_RDONLY) return reply(errno_badf(), 0);
    if ((g_fds[fd_out].fd_flags & O_APPEND) != 0) return reply(errno_badf(), 0);

    u64 in_offset = g_fds[fd_in].offset;
    u64 out_offset = g_fds[fd_out].offset;
    if (off_in_va != 0) {
        i64 value = 0;
        if (copy_from_target(off_in_va, &value, sizeof(value)) != sizeof(value)) return reply(errno_fault(), 0);
        if (value < 0) return reply(errno_inval(), 0);
        in_offset = (u64)value;
    }
    if (off_out_va != 0) {
        i64 value = 0;
        if (copy_from_target(off_out_va, &value, sizeof(value)) != sizeof(value)) return reply(errno_fault(), 0);
        if (value < 0) return reply(errno_inval(), 0);
        out_offset = (u64)value;
    }
    if (in_offset >= g_fds[fd_in].size) return reply(0, 0);

    u64 requested = len;
    const u64 available = g_fds[fd_in].size - in_offset;
    if (requested > available) requested = available;
    if (g_fds[fd_in].token == g_fds[fd_out].token && ranges_overlap(in_offset, requested, out_offset, requested)) {
        return reply(errno_inval(), 0);
    }

    u64 copied = 0;
    while (copied < requested) {
        u64 chunk = requested - copied;
        if (chunk > FS_BULK_READ_BYTES) chunk = FS_BULK_READ_BYTES;
        const u64 page_count = (chunk + PAGE_BYTES - 1) / PAGE_BYTES;
        if (!ensure_fs_bulk_pages(&g_vfs, page_count)) return copied != 0 ? reply(copied, 0) : reply(errno_io(), 0);
        const u64 bulk_addr = vfs_bulk_addr();
        if (bulk_addr < PAGE_BYTES) return copied != 0 ? reply(copied, 0) : reply(errno_io(), 0);
        u64 read_bytes = 0;
        if (!vfs_read_bulk_to_buffer(g_fds[fd_in].token, in_offset + copied, (u32)chunk, bulk_addr, &read_bytes)) {
            return copied != 0 ? reply(copied, 0) : reply(errno_io(), 0);
        }
        if (read_bytes == 0) break;
        u64 written = 0;
        const u64 write_page_count = (read_bytes + PAGE_BYTES - 1) / PAGE_BYTES;
        if (!vfs_write_bulk_submit(g_fds[fd_out].token, out_offset + copied, (u32)read_bytes, write_page_count, &written)) {
            return copied != 0 ? reply(copied, 0) : reply(errno_io(), 0);
        }
        if (written == 0) {
            if (copied != 0) return reply(copied, 0);
            return reply(g_last_vfs_write_status == FS_STATUS_OK ? errno_io() : errno_from_fs_status((i32)g_last_vfs_write_status), 0);
        }
        copied += written;
        if (written != read_bytes) break;
    }

    if (copied != 0) {
        if (off_in_va != 0) {
            const i64 next = (i64)(in_offset + copied);
            if (copy_to_target(off_in_va, &next, sizeof(next)) != sizeof(next)) return reply(errno_fault(), 0);
        } else {
            g_fds[fd_in].offset = in_offset + copied;
            sync_fd_to_thread_group(fd_in);
        }
        if (off_out_va != 0) {
            const i64 next = (i64)(out_offset + copied);
            if (copy_to_target(off_out_va, &next, sizeof(next)) != sizeof(next)) return reply(errno_fault(), 0);
        } else {
            g_fds[fd_out].offset = out_offset + copied;
        }
        const u64 end = out_offset + copied;
        if (end > g_fds[fd_out].size) g_fds[fd_out].size = end;
        invalidate_exec_cache_for_path(g_fds[fd_out].path);
        sync_fd_to_thread_group(fd_out);
    }
    return reply(copied, 0);
}

static struct ipc_message handle_readv(const struct trap_request *req) {
    const u64 fd = req->args[0]; const u64 iov = req->args[1]; const u64 iovcnt = req->args[2];
    if (!fd_valid(fd)) return reply(errno_badf(), 0);
    if (iovcnt > 64) return reply(errno_inval(), 0);
    if (g_fds[fd].kind == FD_STDIO) {
        return reply(0, 0);
    }
    if (g_fds[fd].kind == FD_TTY) {
        u64 total = 0;
        for (u64 i = 0; i < iovcnt; i++) {
            u64 pair[2];
            if (copy_from_target(iov + i * 16, pair, sizeof(pair)) != sizeof(pair)) return total != 0 ? reply(total, 0) : reply(errno_fault(), 0);
            if (pair[1] == 0) continue;
            int fault = 0;
            const u64 n = console_read_to_target(pair[0], pair[1], &fault, g_fds[fd].fd_flags);
            if (fault) return total != 0 ? reply(total, 0) : reply(errno_fault(), 0);
            if ((i64)n < 0) return total != 0 ? reply(total, 0) : reply(n, 0);
            total += n;
            if (n != pair[1]) break;
        }
        return reply(total, 0);
    }
    u64 total = 0;
    if (g_fds[fd].kind == FD_PIPE_READ) {
        g_prof.pipe_read_calls++;
        const u8 pipe_id = g_fds[fd].pipe_id;
        if (pipe_id >= PIPE_MAX || !g_pipes[pipe_id].used) return reply(errno_badf(), 0);
        reconcile_pipe_refs();
        struct pipe_entry *pipe = &g_pipes[pipe_id];
        if (pipe->len == 0 && pipe_has_blocking_writer(pipe_id, g_proc ? g_proc->pid : 0)) {
            if (pipe->pending_read) { g_prof.pipe_read_again++; return reply(errno_again(), 0); }
            for (u64 i = 0; i < iovcnt; i++) {
                u64 pair[2];
                if (copy_from_target(iov + i * 16, pair, sizeof(pair)) != sizeof(pair)) return reply(errno_fault(), 0);
                if (pair[1] == 0) continue;
                if (!fault_in_target_anon_lazy_range(pair[0], min_u64(pair[1], PIPE_BUFFER_BYTES), 1)) return reply(errno_fault(), 0);
                g_prof.pipe_read_blocked++;
                pipe_trace_state("readv_block", pipe_id);
                pipe->pending_read = 1;
                pipe->pending_principal = req->caller_principal;
                pipe->pending_dst = pair[0];
                pipe->pending_len = pair[1];
                detach_reply_token();
                return wait_linux_abi_event();
            }
            return reply(0, 0);
        }
        if (pipe->len == 0) { g_prof.pipe_read_eof++; pipe_trace_state("readv_eof", pipe_id); return reply(0, 0); }
        for (u64 i = 0; i < iovcnt; i++) {
            u64 pair[2];
            if (copy_from_target(iov + i * 16, pair, sizeof(pair)) != sizeof(pair)) return total != 0 ? reply(total, 0) : reply(errno_fault(), 0);
            if (pair[1] == 0) continue;
            int fault = 0;
            const u64 n = pipe_read_to_target(fd, pair[0], pair[1], &fault);
            if (fault) return total != 0 ? reply(total, 0) : reply(errno_fault(), 0);
            sync_fd_to_thread_group(fd);
            total += n;
            if (n != pair[1]) break;
        }
        return reply(total, 0);
    }
    if (g_fds[fd].kind == FD_SOCKET) {
        for (u64 i = 0; i < iovcnt; i++) {
            u64 pair[2];
            if (copy_from_target(iov + i * 16, pair, sizeof(pair)) != sizeof(pair)) return reply(errno_fault(), 0);
            if (pair[1] == 0) continue;
            return reply(socket_read_to_target(fd, pair[0], pair[1]), 0);
        }
        return reply(0, 0);
    }
    if (g_fds[fd].kind == FD_RANDOM) {
        for (u64 i = 0; i < iovcnt; i++) {
            u64 pair[2];
            if (copy_from_target(iov + i * 16, pair, sizeof(pair)) != sizeof(pair)) return total != 0 ? reply(total, 0) : reply(errno_fault(), 0);
            if (pair[1] == 0) continue;
            int fault = 0;
            const u64 n = random_read_to_target(pair[0], pair[1], &fault);
            if (fault) return total != 0 ? reply(total, 0) : reply(errno_fault(), 0);
            total += n;
            if (n != pair[1]) break;
        }
        return total != 0 ? reply(total, 0) : reply(errno_again(), 0);
    }
    if (g_fds[fd].kind != FD_FILE) return reply(errno_badf(), 0);
    for (u64 i = 0; i < iovcnt; i++) {
        u64 pair[2];
        if (copy_from_target(iov + i * 16, pair, sizeof(pair)) != sizeof(pair)) return total != 0 ? reply(total, 0) : reply(errno_fault(), 0);
        u64 copied = 0;
        while (copied < pair[1] && g_fds[fd].offset < g_fds[fd].size) {
            if (g_fds[fd].path_len != 0 && cacheable_readonly_path(g_fds[fd].path)) {
                int fault = 0;
                const u64 n = file_cache_read_to_target(&g_fds[fd], g_fds[fd].offset, pair[0] + copied, pair[1] - copied, &fault);
                if (fault) return total != 0 ? reply(total, 0) : reply(errno_fault(), 0);
                if (n != 0 || g_fds[fd].offset >= g_fds[fd].size) {
                    copied += n;
                    total += n;
                    g_fds[fd].offset += n;
                    sync_fd_to_thread_group(fd);
                    if (n == 0 || copied < pair[1]) return reply(total, 0);
                    continue;
                }
            }
            u64 request_len = min_u64(pair[1] - copied, FS_RESPONSE_PAYLOAD_BYTES);
            const u64 remaining = g_fds[fd].size - g_fds[fd].offset;
            if (request_len > remaining) request_len = remaining;
            if (pair[1] - copied > FS_RESPONSE_PAYLOAD_BYTES) {
                u64 bulk_len = min_u64(pair[1] - copied, FS_BULK_READ_BYTES);
                if (bulk_len > remaining) bulk_len = remaining;
                u64 bulk_bytes = 0;
                if (vfs_read_bulk_to_target(g_fds[fd].token, g_fds[fd].offset, (u32)bulk_len, pair[0] + copied, &bulk_bytes)) {
                    if (bulk_bytes == 0) return reply(total, 0);
                    profile_fs_read_path(&g_fds[fd], bulk_bytes);
                    copied += bulk_bytes;
                    total += bulk_bytes;
                    g_fds[fd].offset += bulk_bytes;
                    sync_fd_to_thread_group(fd);
                    continue;
                }
            }
            if (!vfs_request(FS_OP_READ, g_fds[fd].token, g_fds[fd].offset, (u32)request_len, 0)) return total != 0 ? reply(total, 0) : reply(errno_io(), 0);
            volatile struct fs_response_header *response = (volatile struct fs_response_header *)vfs_response_addr();
            if (response->status != FS_STATUS_OK) return total != 0 ? reply(total, 0) : reply(errno_io(), 0);
            if (response->inline_bytes == 0) return reply(total, 0);
            if (copy_to_target(pair[0] + copied, vfs_response_payload(), response->inline_bytes) != response->inline_bytes) {
                return total != 0 ? reply(total, 0) : reply(errno_fault(), 0);
            }
            profile_fs_read_path(&g_fds[fd], response->inline_bytes);
            copied += response->inline_bytes;
            total += response->inline_bytes;
            g_fds[fd].offset += response->inline_bytes;
            sync_fd_to_thread_group(fd);
            if (response->inline_bytes < request_len) return reply(total, 0);
        }
        if (copied < pair[1]) return reply(total, 0);
    }
    return reply(total, 0);
}

static struct ipc_message handle_write(const struct trap_request *req) {
    const u64 fd = req->args[0]; const u64 src = req->args[1]; const u64 len = req->args[2];
    if (fd_valid(fd) && g_fds[fd].kind == FD_SOCKET) return reply(socket_write_from_target(fd, src, len), 0);
    if (fd_valid(fd) && g_fds[fd].kind == FD_PIPE_WRITE) {
        g_prof.pipe_write_calls++;
        const u8 pipe_id = g_fds[fd].pipe_id;
        int fault = 0; const u64 n = pipe_write_from_target(fd, src, len, &fault);
        if (!fault && (i64)n > 0) try_satisfy_pending_pipe_read(pipe_id);
        return reply(fault ? errno_fault() : n, 0);
    }
    if (fd_valid(fd) && g_fds[fd].kind == FD_EVENTFD) {
        int fault = 0;
        const u64 n = eventfd_write_from_target(fd, src, len, &fault);
        return reply(fault ? errno_fault() : n, 0);
    }
    if (fd_valid(fd) && g_fds[fd].kind == FD_FILE) {
        if (len == 0) return reply(0, 0);
        if ((g_fds[fd].fd_flags & O_APPEND) != 0) g_fds[fd].offset = g_fds[fd].size;
        int fault = 0;
        const u64 n = vfs_write_from_target(g_fds[fd].token, g_fds[fd].offset, src, len, &fault);
        if (fault) return reply(errno_fault(), 0);
        if (n != 0) {
            g_prof.fs_write_bytes += n;
            invalidate_exec_cache_for_path(g_fds[fd].path);
            g_fds[fd].offset += n;
            if (g_fds[fd].offset > g_fds[fd].size) g_fds[fd].size = g_fds[fd].offset;
            sync_fd_to_thread_group(fd);
            return reply(n, 0);
        }
        log_fd_write_failure(fd);
        return reply(errno_io(), 0);
    }
    if (fd_valid(fd) && g_fds[fd].kind == FD_STDIO) {
        char buf[129]; u64 done = 0;
        while (done < len) { u64 chunk = min_u64(len - done, 128); if (copy_from_target(src + done, buf, chunk) != chunk) break; user_log_len(buf, chunk); done += chunk; }
        return reply(len, 0);
    }
    if (fd_valid(fd) && g_fds[fd].kind == FD_TTY) {
        int fault = 0;
        const u64 n = console_write_from_target(src, len, &fault);
        return reply(fault ? errno_fault() : (n != 0 ? n : errno_io()), 0);
    }
    return reply(errno_badf(), 0);
}

static struct ipc_message handle_writev(const struct trap_request *req) {
    const u64 fd = req->args[0]; const u64 iov = req->args[1]; const u64 iovcnt = req->args[2];
    if (fd_valid(fd) && g_fds[fd].kind == FD_SOCKET) {
        return reply(socket_send_iov_from_target(fd, iov, iovcnt), 0);
    }
    if (fd_valid(fd) && g_fds[fd].kind == FD_PIPE_WRITE) {
        g_prof.pipe_write_calls++;
        if (iovcnt > 64) return reply(errno_inval(), 0);
        u64 total = 0;
        const u8 pipe_id = g_fds[fd].pipe_id;
        for (u64 i = 0; i < iovcnt; i++) {
            u64 pair[2];
            if (copy_from_target(iov + i * 16, pair, sizeof(pair)) != sizeof(pair)) return reply(errno_fault(), 0);
            int fault = 0; const u64 n = pipe_write_from_target(fd, pair[0], pair[1], &fault);
            if (fault) return reply(errno_fault(), 0);
            if ((i64)n < 0) return total != 0 ? reply(total, 0) : reply(n, 0);
            total += n;
            if (n != pair[1]) break;
        }
        if (total != 0) try_satisfy_pending_pipe_read(pipe_id);
        return reply(total, 0);
    }
    if (fd_valid(fd) && g_fds[fd].kind == FD_FILE) {
        if (iovcnt > 64) return reply(errno_inval(), 0);
        if ((g_fds[fd].fd_flags & O_APPEND) != 0) g_fds[fd].offset = g_fds[fd].size;
        u64 pairs[64][2];
        for (u64 i = 0; i < iovcnt; i++) {
            if (copy_from_target(iov + i * 16, pairs[i], sizeof(pairs[i])) != sizeof(pairs[i])) return reply(errno_fault(), 0);
        }
        u64 total = 0;
        u64 i = 0;
        u64 iov_offset = 0;
        while (i < iovcnt) {
            if (iov_offset >= pairs[i][1]) {
                i++;
                iov_offset = 0;
                continue;
            }
            if (iovcnt > 1 || pairs[i][1] - iov_offset > FS_RESPONSE_PAYLOAD_BYTES) {
                u64 bulk_bytes = 0;
                int bulk_fault = 0;
                if (vfs_writev_bulk_from_target(g_fds[fd].token, g_fds[fd].offset, &pairs[0][0], iovcnt, &i, &iov_offset, &bulk_bytes, &bulk_fault)) {
                    if (bulk_fault) return reply(errno_fault(), 0);
                    if (bulk_bytes == 0) {
                        if (total == 0) log_fd_write_failure(fd);
                        return total != 0 ? reply(total, 0) : reply(errno_io(), 0);
                    }
                    g_prof.fs_write_bytes += bulk_bytes;
                    invalidate_exec_cache_for_path(g_fds[fd].path);
                    g_fds[fd].offset += bulk_bytes;
                    if (g_fds[fd].offset > g_fds[fd].size) g_fds[fd].size = g_fds[fd].offset;
                    sync_fd_to_thread_group(fd);
                    total += bulk_bytes;
                    continue;
                }
            }
            int fault = 0;
            const u64 n = vfs_write_from_target(g_fds[fd].token, g_fds[fd].offset, pairs[i][0] + iov_offset, pairs[i][1] - iov_offset, &fault);
            if (fault) return reply(errno_fault(), 0);
            if (n == 0 && pairs[i][1] != iov_offset) {
                if (total == 0) log_fd_write_failure(fd);
                return total != 0 ? reply(total, 0) : reply(errno_io(), 0);
            }
            if (n != 0) {
                g_prof.fs_write_bytes += n;
                invalidate_exec_cache_for_path(g_fds[fd].path);
            }
            g_fds[fd].offset += n;
            if (g_fds[fd].offset > g_fds[fd].size) g_fds[fd].size = g_fds[fd].offset;
            sync_fd_to_thread_group(fd);
            total += n;
            iov_offset += n;
            if (iov_offset != pairs[i][1]) break;
            i++;
            iov_offset = 0;
        }
        return reply(total, 0);
    }
    if (!fd_valid(fd) || (g_fds[fd].kind != FD_STDIO && g_fds[fd].kind != FD_TTY)) return reply(errno_badf(), 0);
    if (iovcnt > 64) return reply(errno_inval(), 0);
    u64 total = 0;
    for (u64 i = 0; i < iovcnt; i++) {
        u64 pair[2];
        if (copy_from_target(iov + i * 16, pair, sizeof(pair)) != sizeof(pair)) return reply(errno_fault(), 0);
        const u64 src = pair[0]; const u64 len = pair[1];
        if (len == 0) continue;

        if (g_fds[fd].kind == FD_STDIO && !(fd_valid(0) && g_fds[0].kind == FD_TTY)) {
            u64 done = 0;
            while (done < len) {
                char buf[129]; u64 chunk = min_u64(len - done, 128);
                if (copy_from_target(src + done, buf, chunk) != chunk) return total != 0 ? reply(total, 0) : reply(errno_fault(), 0);
                user_log_len(buf, chunk);
                done += chunk;
            }
            total += len;
            continue;
        }

        if (g_fds[fd].kind == FD_STDIO && fd_valid(0) && g_fds[0].kind == FD_TTY) {
            int fault = 0;
            const u64 n = console_write_from_target(src, len, &fault);
            if (fault) return total != 0 ? reply(total, 0) : reply(errno_fault(), 0);
            if ((i64)n < 0) return total != 0 ? reply(total, 0) : reply(n, 0);
            total += n;
            if (n != len) break;
            continue;
        }

        int fault = 0;
        const u64 n = console_write_from_target(src, len, &fault);
        if (fault) return total != 0 ? reply(total, 0) : reply(errno_fault(), 0);
        if ((i64)n < 0) return total != 0 ? reply(total, 0) : reply(n, 0);
        total += n;
        if (n != len) break;
    }
    return reply(total, 0);
}

static struct ipc_message handle_fsync_like(const struct trap_request *req) {
    const u64 fd = req->args[0];
    if (!fd_valid(fd)) return reply(errno_badf(), 0);
    return reply(0, 0);
}

static struct ipc_message handle_ftruncate(const struct trap_request *req) {
    const u64 fd = req->args[0];
    const i64 length = (i64)req->args[1];
    if (length < 0) return reply(errno_inval(), 0);
    if (!fd_valid(fd) || g_fds[fd].kind != FD_FILE) return reply(errno_badf(), 0);
    if ((g_fds[fd].fd_flags & O_ACCMODE) == O_RDONLY) return reply(errno_badf(), 0);
    if (!vfs_truncate_file(g_fds[fd].token, (u64)length)) return reply(errno_io(), 0);
    volatile struct fs_response_header *response = (volatile struct fs_response_header *)vfs_response_addr();
    if (response->status != FS_STATUS_OK) return reply(errno_from_fs_status(response->status), 0);
    g_fds[fd].size = response->file_bytes;
    invalidate_exec_cache_for_path(g_fds[fd].path);
    sync_fd_to_thread_group(fd);
    return reply(0, 0);
}

static struct ipc_message handle_fallocate(const struct trap_request *req) {
    const u64 fd = req->args[0];
    const u64 mode = req->args[1];
    const i64 offset = (i64)req->args[2];
    const i64 len = (i64)req->args[3];
    if (offset < 0 || len <= 0) return reply(errno_inval(), 0);
    if (!fd_valid(fd) || g_fds[fd].kind != FD_FILE) return reply(errno_badf(), 0);
    if ((g_fds[fd].fd_flags & O_ACCMODE) == O_RDONLY) return reply(errno_badf(), 0);
    if (mode != 0) return reply(errno_opnotsupp(), 0);
    const u64 end = (u64)offset + (u64)len;
    if (end < (u64)offset) return reply(errno_fbig(), 0);
    if (end > g_fds[fd].size) {
        if (!vfs_truncate_file(g_fds[fd].token, end)) return reply(errno_io(), 0);
        volatile struct fs_response_header *response = (volatile struct fs_response_header *)vfs_response_addr();
        if (response->status != FS_STATUS_OK) return reply(errno_from_fs_status(response->status), 0);
        g_fds[fd].size = response->file_bytes;
        sync_fd_to_thread_group(fd);
    }
    invalidate_exec_cache_for_path(g_fds[fd].path);
    return reply(0, 0);
}

static struct ipc_message handle_fstat(const struct trap_request *req) {
    const u64 fd = req->args[0]; const u64 stat_va = req->args[1]; if (!fd_valid(fd)) return reply(errno_badf(), 0);
    if (fd_is_pipe(fd)) {
        struct linux_stat st;
        fill_linux_pipe_stat(&st, fd);
        return copy_to_target(stat_va, &st, sizeof(st)) == sizeof(st) ? reply(0, 0) : reply(errno_fault(), 0);
    }
    struct fs_stat_record rec; rec.object_kind = g_fds[fd].object_kind; rec.size_bytes = g_fds[fd].size; rec.mode_bits = g_fds[fd].mode_bits; rec.mtime_unix_sec = 0;
    if (g_fds[fd].kind == FD_STDIO || g_fds[fd].kind == FD_TTY || g_fds[fd].kind == FD_RANDOM) { rec.object_kind = FS_OBJECT_FILE; rec.size_bytes = 0; rec.mode_bits = FS_FILE_MODE; }
    struct linux_stat st;
    if (g_fds[fd].path_len != 0) fill_linux_stat_path(&st, &rec, rec.size_bytes, rec.object_kind, g_fds[fd].path);
    else { fill_linux_stat(&st, &rec, rec.size_bytes, rec.object_kind); st.st_dev = 1; st.st_ino = 0x100000ULL + fd; }
    if (copy_to_target(stat_va, &st, sizeof(st)) != sizeof(st)) return reply(errno_fault(), 0);
    return reply(0, 0);
}

static struct ipc_message handle_fstatfs(const struct trap_request *req) {
    const u64 fd = req->args[0];
    const u64 statfs_va = req->args[1];
    if (!fd_valid(fd)) return reply(errno_badf(), 0);
    struct linux_statfs st;
    fill_linux_statfs(&st);
    return copy_to_target(statfs_va, &st, sizeof(st)) == sizeof(st) ? reply(0, 0) : reply(errno_fault(), 0);
}

static struct ipc_message handle_statfs(const struct trap_request *req) {
    const u64 path_ptr = req->args[0];
    const u64 statfs_va = req->args[1];
    char path[256];
    char resolved[FS_MAX_PATH_BYTES + 1];
    if (!copy_cstr_from_target(path_ptr, path, sizeof(path))) return reply(errno_fault(), 0);
    if (path[0] == 0) return reply(errno_noent(), 0);
    if (!resolve_path_at(AT_FDCWD_U64, path, resolved)) return reply(errno_nametoolong(), 0);
    struct fs_stat_record rec;
    u64 token = 0, size = 0;
    u8 kind = 0;
    if (!vfs_lookup_stat(resolved, &token, &rec, &size, &kind)) return reply(errno_noent(), 0);
    (void)token; (void)rec; (void)size; (void)kind;
    struct linux_statfs st;
    fill_linux_statfs(&st);
    return copy_to_target(statfs_va, &st, sizeof(st)) == sizeof(st) ? reply(0, 0) : reply(errno_fault(), 0);
}

static struct ipc_message handle_newfstatat(const struct trap_request *req, int old_stat) {
    const u64 dirfd = old_stat ? AT_FDCWD_U64 : req->args[0]; const u64 path_ptr = old_stat ? req->args[0] : req->args[1]; const u64 stat_va = old_stat ? req->args[1] : req->args[2]; const u64 flags = old_stat ? 0 : req->args[3];
    if (path_ptr == 0) { struct trap_request f = *req; f.args[0] = old_stat ? 0 : dirfd; f.args[1] = stat_va; return handle_fstat(&f); }
    char path[256]; char resolved[FS_MAX_PATH_BYTES + 1]; if (!copy_cstr_from_target(path_ptr, path, sizeof(path))) return reply(errno_fault(), 0);
    if (path[0] == 0 && (flags & AT_EMPTY_PATH) != 0) { struct trap_request f = *req; f.args[0] = old_stat ? 0 : dirfd; f.args[1] = stat_va; return handle_fstat(&f); }
    if (path[0] == 0) return reply(errno_noent(), 0);
    if (!resolve_path_at(dirfd, path, resolved)) return reply(errno_nametoolong(), 0);
    const int nofollow = (req->nr == LINUX_SYS_LSTAT) || (!old_stat && (flags & AT_SYMLINK_NOFOLLOW) != 0);
    if (path_is_dev_tty(resolved)) {
        struct fs_stat_record rec;
        rec.object_kind = FS_OBJECT_FILE;
        rec.size_bytes = 0;
        rec.mode_bits = FS_FILE_MODE;
        rec.mtime_unix_sec = 0;
        struct linux_stat st;
        fill_linux_stat_path(&st, &rec, 0, FS_OBJECT_FILE, resolved);
        if (copy_to_target(stat_va, &st, sizeof(st)) != sizeof(st)) return reply(errno_fault(), 0);
        return reply(0, 0);
    }
    if (path_is_dev_random(resolved)) {
        struct fs_stat_record rec;
        rec.object_kind = FS_OBJECT_FILE;
        rec.size_bytes = 0;
        rec.mode_bits = FS_FILE_MODE;
        rec.mtime_unix_sec = 0;
        struct linux_stat st;
        fill_linux_stat_path(&st, &rec, 0, FS_OBJECT_FILE, resolved);
        if (copy_to_target(stat_va, &st, sizeof(st)) != sizeof(st)) return reply(errno_fault(), 0);
        return reply(0, 0);
    }
    if (path_is_proc_self_exe(resolved)) {
        struct fs_stat_record rec;
        rec.object_kind = FS_OBJECT_SYMLINK;
        rec.size_bytes = g_exec_path_len;
        rec.mode_bits = FS_SYMLINK_MODE;
        rec.mtime_unix_sec = 0;
        if (!nofollow && g_exec_path_len != 0) {
            struct fs_stat_record target_rec;
            u64 token = 0;
            u64 target_size = 0;
            u8 target_kind = FS_OBJECT_NONE;
            char target_resolved[FS_MAX_PATH_BYTES + 1];
            if (vfs_lookup_stat_follow_final(g_exec_path, 1, target_resolved, &token, &target_rec, &target_size, &target_kind)) {
                (void)token;
                struct linux_stat target_st;
                fill_linux_stat_path(&target_st, &target_rec, target_size, target_kind, target_resolved);
                if (copy_to_target(stat_va, &target_st, sizeof(target_st)) != sizeof(target_st)) return reply(errno_fault(), 0);
                return reply(0, 0);
            }
        }
        struct linux_stat st;
        fill_linux_stat_path(&st, &rec, g_exec_path_len, FS_OBJECT_SYMLINK, resolved);
        if (copy_to_target(stat_va, &st, sizeof(st)) != sizeof(st)) return reply(errno_fault(), 0);
        return reply(0, 0);
    }
    struct fs_stat_record rec; u64 token = 0; u64 size = 0; u8 kind = FS_OBJECT_NONE; char final_resolved[FS_MAX_PATH_BYTES + 1]; if (!vfs_lookup_stat_follow_final(resolved, !nofollow, final_resolved, &token, &rec, &size, &kind)) return reply(errno_noent(), 0); (void)token;
    struct linux_stat st; fill_linux_stat_path(&st, &rec, size, kind, final_resolved); if (copy_to_target(stat_va, &st, sizeof(st)) != sizeof(st)) return reply(errno_fault(), 0); return reply(0, 0);
}

static struct ipc_message handle_getdents64(const struct trap_request *req) {
    const u64 fd = req->args[0]; const u64 dst = req->args[1]; const u64 len = req->args[2]; if (!fd_valid(fd) || g_fds[fd].kind != FD_DIR) return reply(errno_badf(), 0);
    u64 written = 0;
    while (written + 32 <= len) {
        if (!vfs_request(FS_OP_READDIR, g_fds[fd].token, g_fds[fd].offset, 0, 0)) return written != 0 ? reply(written, 0) : reply(errno_io(), 0);
        volatile struct fs_response_header *response = (volatile struct fs_response_header *)vfs_response_addr();
        if (response->status == FS_STATUS_END_OF_DIR) break; if (response->status != FS_STATUS_OK || response->inline_bytes < FS_DIRENT_RECORD_BYTES) return written != 0 ? reply(written, 0) : reply(errno_io(), 0);
        volatile struct fs_dirent_record *record = (volatile struct fs_dirent_record *)vfs_response_payload(); if (response->inline_bytes < FS_DIRENT_RECORD_BYTES + record->name_bytes) return reply(errno_io(), 0);
        u64 reclen = align_up(19 + record->name_bytes + 1, 8); if (written + reclen > len) break;
        volatile u8 *name = (volatile u8 *)(vfs_response_addr() + FS_RESPONSE_HEADER_BYTES + FS_DIRENT_RECORD_BYTES);
        u8 out[320]; for (u64 i = 0; i < sizeof(out); i++) out[i] = 0;
        *((u64 *)(out + 0)) = linux_ino_from_dirent(fd, name, record->name_bytes); *((i64 *)(out + 8)) = (i64)record->next_cursor; *((u16 *)(out + 16)) = (u16)reclen; out[18] = (record->object_kind == FS_OBJECT_DIRECTORY || record->object_kind == FS_OBJECT_MOUNT) ? DT_DIR : (record->object_kind == FS_OBJECT_SYMLINK ? DT_LNK : DT_REG);
        for (u64 i = 0; i < record->name_bytes && 19 + i < sizeof(out); i++) out[19 + i] = name[i];
        if (copy_to_target(dst + written, out, reclen) != reclen) return reply(errno_fault(), 0);
        written += reclen; g_fds[fd].offset = record->next_cursor; sync_fd_to_thread_group(fd);
        if (record->next_cursor == 0) break;
    }
    return reply(written, 0);
}

static struct ipc_message handle_lseek(const struct trap_request *req) {
    const u64 fd = req->args[0]; const i64 off = (i64)req->args[1]; const u64 whence = req->args[2]; if (!fd_valid(fd)) return reply(errno_badf(), 0);
    if (fd_is_pipe(fd) || g_fds[fd].kind == FD_RANDOM) return reply(errno_spipe(), 0);
    i64 base = 0; if (whence == SEEK_SET) base = 0; else if (whence == SEEK_CUR) base = (i64)g_fds[fd].offset; else if (whence == SEEK_END) base = (i64)g_fds[fd].size; else return reply(errno_inval(), 0);
    i64 next = base + off; if (next < 0) return reply(errno_inval(), 0); g_fds[fd].offset = (u64)next; sync_fd_to_thread_group(fd); return reply((u64)next, 0);
}

static struct ipc_message handle_close(const struct trap_request *req) {
    const u64 fd = req->args[0];
    if (fd >= LINUX_FD_MAX || g_fds[fd].kind == FD_UNUSED) return reply(errno_badf(), 0);
    int close_error = 0;
    if (g_fds[fd].kind == FD_FILE && (g_fds[fd].fd_flags & O_ACCMODE) != O_RDONLY && g_fds[fd].token != 0) {
        if (!vfs_request(FS_OP_CLOSE, g_fds[fd].token, 0, 0, 0)) {
            close_error = 1;
        } else {
            volatile struct fs_response_header *response = (volatile struct fs_response_header *)vfs_response_addr();
            if (response->status != FS_STATUS_OK) close_error = 1;
        }
    }
    if (fd_is_pipe(fd)) close_pipe_fd(fd);
    if (g_fds[fd].kind == FD_SOCKET) close_socket_entry(&g_fds[fd]);
    remove_fd_from_current_epoll_sets(fd);
    g_fds[fd].kind = FD_UNUSED;
    sync_fd_to_thread_group(fd);
    return reply(close_error ? errno_io() : 0, 0);
}
static struct ipc_message handle_unlinkat(const struct trap_request *req, int old_unlink) {
    char path[256];
    char resolved[FS_MAX_PATH_BYTES + 1];
    const u64 dirfd = old_unlink ? AT_FDCWD_U64 : req->args[0];
    const u64 path_ptr = old_unlink ? req->args[0] : req->args[1];
    if (!copy_cstr_from_target(path_ptr, path, sizeof(path))) return reply(errno_fault(), 0);
    if (path[0] == 0) return reply(errno_noent(), 0);
    if (!resolve_path_at(dirfd, path, resolved)) return reply(errno_nametoolong(), 0);
    if (!vfs_request(FS_OP_UNLINK, g_vfs.root_token, 0, 0, resolved)) return reply(errno_io(), 0);
    volatile struct fs_response_header *response = (volatile struct fs_response_header *)vfs_response_addr();
    if (response->status == FS_STATUS_OK) { invalidate_exec_cache_for_path(resolved); return reply(0, 0); }
    if (response->status == FS_STATUS_NOT_FOUND) return reply(errno_noent(), 0);
    if (response->status == FS_STATUS_NOT_DIR) return reply(errno_notdir(), 0);
    return reply(errno_acces(), 0);
}

static struct ipc_message handle_mkdirat(const struct trap_request *req, int old_mkdir) {
    char path[256];
    char resolved[FS_MAX_PATH_BYTES + 1];
    const u64 dirfd = old_mkdir ? AT_FDCWD_U64 : req->args[0];
    const u64 path_ptr = old_mkdir ? req->args[0] : req->args[1];
    (void)(old_mkdir ? req->args[1] : req->args[2]);
    if (!copy_cstr_from_target(path_ptr, path, sizeof(path))) return reply(errno_fault(), 0);
    if (path[0] == 0) return reply(errno_noent(), 0);
    if (!resolve_path_at(dirfd, path, resolved)) return reply(errno_nametoolong(), 0);
    if (!vfs_create_path_with_flags(resolved, FS_CREATE_FLAG_DIRECTORY | FS_CREATE_FLAG_EXCLUSIVE)) return reply(errno_io(), 0);
    volatile struct fs_response_header *response = (volatile struct fs_response_header *)vfs_response_addr();
    if (response->status == FS_STATUS_OK) { invalidate_exec_cache_for_path(resolved); return reply(0, 0); }
    if (response->status == FS_STATUS_EXISTS) return reply(errno_exist(), 0);
    if (response->status == FS_STATUS_NOT_FOUND) return reply(errno_noent(), 0);
    if (response->status == FS_STATUS_NOT_DIR) return reply(errno_notdir(), 0);
    if (response->status == FS_STATUS_NOT_SUPPORTED) return reply(errno_opnotsupp(), 0);
    return reply(errno_acces(), 0);
}

static struct ipc_message handle_symlinkat(const struct trap_request *req, int old_symlink) {
    char target[FS_MAX_PATH_BYTES + 1];
    char path[256];
    char resolved[FS_MAX_PATH_BYTES + 1];
    const u64 target_ptr = req->args[0];
    const u64 dirfd = old_symlink ? AT_FDCWD_U64 : req->args[1];
    const u64 path_ptr = old_symlink ? req->args[1] : req->args[2];
    if (!copy_cstr_from_target(target_ptr, target, sizeof(target))) return reply(errno_nametoolong(), 0);
    if (!copy_cstr_from_target(path_ptr, path, sizeof(path))) return reply(errno_fault(), 0);
    if (target[0] == 0 || path[0] == 0) return reply(errno_noent(), 0);
    if (!resolve_path_at(dirfd, path, resolved)) return reply(errno_nametoolong(), 0);
    struct fs_stat_record rec;
    u64 token = 0;
    u64 size = 0;
    u8 kind = FS_OBJECT_NONE;
    if (vfs_lookup_stat(resolved, &token, &rec, &size, &kind)) return reply(errno_exist(), 0);
    const u64 target_len = cstr_len(target);
    if (!vfs_create_symlink_path(resolved, target, (u16)target_len)) return reply(errno_io(), 0);
    volatile struct fs_response_header *response = (volatile struct fs_response_header *)vfs_response_addr();
    if (response->status == FS_STATUS_OK) { invalidate_exec_cache_for_path(resolved); return reply(0, 0); }
    if (response->status == FS_STATUS_NOT_FOUND) return reply(errno_noent(), 0);
    if (response->status == FS_STATUS_NOT_DIR) return reply(errno_notdir(), 0);
    if (response->status == FS_STATUS_NOT_SUPPORTED) return reply(errno_opnotsupp(), 0);
    return reply(errno_acces(), 0);
}

static int fs_cstr_eq(const char *a, const char *b) {
    u64 i = 0;
    while (a[i] != 0 || b[i] != 0) {
        if (a[i] != b[i]) return 0;
        i++;
    }
    return 1;
}

static u64 errno_from_fs_status(i32 status) {
    if (status == FS_STATUS_NOT_FOUND) return errno_noent();
    if (status == FS_STATUS_NOT_DIR) return errno_notdir();
    if (status == FS_STATUS_IS_DIR) return errno_acces();
    if (status == FS_STATUS_TOO_BIG) return errno_fbig();
    if (status == FS_STATUS_NOT_SUPPORTED) return errno_opnotsupp();
    if (status == FS_STATUS_BUSY) return errno_nospc();
    if (status == FS_STATUS_IO_ERROR) return errno_io();
    if (status == FS_STATUS_NO_RIGHT) return errno_acces();
    return errno_acces();
}

static int vfs_open_file_token(u64 file_token, u64 *open_token_out) {
    *open_token_out = 0;
    if (!vfs_request(FS_OP_OPEN, file_token, 0, 0, 0)) return 0;
    volatile struct fs_response_header *response = (volatile struct fs_response_header *)vfs_response_addr();
    if (response->status != FS_STATUS_OK || response->result_token == 0) return 0;
    *open_token_out = response->result_token;
    return 1;
}

static void vfs_close_token_if_open(u64 token) {
    if (token != 0) (void)vfs_request(FS_OP_CLOSE, token, 0, 0, 0);
}

static struct ipc_message copy_regular_file_for_link(const char *old_resolved, const char *new_resolved, int follow_source) {
    struct fs_stat_record old_rec;
    u64 old_file_token = 0;
    u64 old_size = 0;
    u8 old_kind = FS_OBJECT_NONE;
    char source_resolved[FS_MAX_PATH_BYTES + 1];
    if (!vfs_lookup_stat_follow_final(old_resolved, follow_source, source_resolved, &old_file_token, &old_rec, &old_size, &old_kind)) {
        return reply(errno_noent(), 0);
    }
    if (old_kind == FS_OBJECT_DIRECTORY || old_kind == FS_OBJECT_MOUNT) return reply(errno_perm(), 0);

    struct fs_stat_record new_rec;
    u64 new_existing_token = 0;
    u64 new_existing_size = 0;
    u8 new_existing_kind = FS_OBJECT_NONE;
    if (vfs_lookup_stat(new_resolved, &new_existing_token, &new_rec, &new_existing_size, &new_existing_kind)) {
        return reply(errno_exist(), 0);
    }

    if (old_kind == FS_OBJECT_SYMLINK) {
        char target[FS_MAX_PATH_BYTES + 1];
        u64 target_len = 0;
        if (!read_symlink_target_for_follow(source_resolved, target, sizeof(target) - 1, &target_len)) return reply(errno_io(), 0);
        if (!vfs_create_symlink_path(new_resolved, target, (u16)target_len)) return reply(errno_io(), 0);
        volatile struct fs_response_header *created_link = (volatile struct fs_response_header *)vfs_response_addr();
        if (created_link->status != FS_STATUS_OK) return reply(errno_from_fs_status(created_link->status), 0);
        invalidate_exec_cache_for_path(new_resolved);
        return reply(0, 0);
    }

    if (!vfs_create_path(new_resolved, 0)) return reply(errno_io(), 0);
    volatile struct fs_response_header *created = (volatile struct fs_response_header *)vfs_response_addr();
    if (created->status != FS_STATUS_OK || created->result_token == 0) return reply(errno_from_fs_status(created->status), 0);
    const u64 new_file_token = created->result_token;

    u64 old_open_token = 0;
    u64 new_open_token = 0;
    if (!vfs_open_file_token(old_file_token, &old_open_token) ||
        !vfs_open_file_token(new_file_token, &new_open_token))
    {
        vfs_close_token_if_open(old_open_token);
        vfs_close_token_if_open(new_open_token);
        (void)vfs_request(FS_OP_UNLINK, g_vfs.root_token, 0, 0, new_resolved);
        return reply(errno_io(), 0);
    }

    u64 copied = 0;
    u8 buf[1024];
    while (copied < old_size) {
        u64 chunk = old_size - copied;
        if (chunk > sizeof(buf)) chunk = sizeof(buf);
        if (!vfs_request(FS_OP_READ, old_open_token, copied, (u32)chunk, 0)) {
            vfs_close_token_if_open(old_open_token);
            vfs_close_token_if_open(new_open_token);
            (void)vfs_request(FS_OP_UNLINK, g_vfs.root_token, 0, 0, new_resolved);
            return reply(errno_io(), 0);
        }
        volatile struct fs_response_header *read_response = (volatile struct fs_response_header *)vfs_response_addr();
        if (read_response->status != FS_STATUS_OK || read_response->inline_bytes == 0 || read_response->inline_bytes > chunk) {
            vfs_close_token_if_open(old_open_token);
            vfs_close_token_if_open(new_open_token);
            (void)vfs_request(FS_OP_UNLINK, g_vfs.root_token, 0, 0, new_resolved);
            return reply(read_response->status == FS_STATUS_OK ? errno_io() : errno_from_fs_status(read_response->status), 0);
        }
        volatile u8 *payload = (volatile u8 *)vfs_response_payload();
        const u16 read_bytes = read_response->inline_bytes;
        for (u16 i = 0; i < read_bytes; i++) buf[i] = payload[i];
        if (!vfs_write_inline_local(new_open_token, copied, buf, read_bytes)) {
            vfs_close_token_if_open(old_open_token);
            vfs_close_token_if_open(new_open_token);
            (void)vfs_request(FS_OP_UNLINK, g_vfs.root_token, 0, 0, new_resolved);
            return reply(errno_io(), 0);
        }
        volatile struct fs_response_header *write_response = (volatile struct fs_response_header *)vfs_response_addr();
        if (write_response->status != FS_STATUS_OK) {
            vfs_close_token_if_open(old_open_token);
            vfs_close_token_if_open(new_open_token);
            (void)vfs_request(FS_OP_UNLINK, g_vfs.root_token, 0, 0, new_resolved);
            return reply(errno_from_fs_status(write_response->status), 0);
        }
        copied += read_bytes;
    }

    vfs_close_token_if_open(old_open_token);
    vfs_close_token_if_open(new_open_token);
    invalidate_exec_cache_for_path(new_resolved);
    return reply(0, 0);
}

static struct ipc_message handle_linkat(const struct trap_request *req, int old_link) {
    char old_path[256];
    char new_path[256];
    char old_resolved[FS_MAX_PATH_BYTES + 1];
    char new_resolved[FS_MAX_PATH_BYTES + 1];
    const u64 old_dirfd = old_link ? AT_FDCWD_U64 : req->args[0];
    const u64 old_path_ptr = old_link ? req->args[0] : req->args[1];
    const u64 new_dirfd = old_link ? AT_FDCWD_U64 : req->args[2];
    const u64 new_path_ptr = old_link ? req->args[1] : req->args[3];
    const u64 flags = old_link ? 0 : req->args[4];
    if ((flags & ~((u64)AT_SYMLINK_FOLLOW)) != 0) return reply(errno_inval(), 0);
    if (!copy_cstr_from_target(old_path_ptr, old_path, sizeof(old_path))) return reply(errno_fault(), 0);
    if (!copy_cstr_from_target(new_path_ptr, new_path, sizeof(new_path))) return reply(errno_fault(), 0);
    if (old_path[0] == 0 || new_path[0] == 0) return reply(errno_noent(), 0);
    if (!resolve_path_at(old_dirfd, old_path, old_resolved)) return reply(errno_nametoolong(), 0);
    if (!resolve_path_at(new_dirfd, new_path, new_resolved)) return reply(errno_nametoolong(), 0);
    return copy_regular_file_for_link(old_resolved, new_resolved, (flags & AT_SYMLINK_FOLLOW) != 0);
}

static struct ipc_message handle_renameat(const struct trap_request *req, int old_rename, int has_flags) {
    enum { RENAME_NOREPLACE = 1, RENAME_EXCHANGE = 2, RENAME_WHITEOUT = 4 };
    char old_path[256];
    char new_path[256];
    char old_resolved[FS_MAX_PATH_BYTES + 1];
    char new_resolved[FS_MAX_PATH_BYTES + 1];
    const u64 old_dirfd = old_rename ? AT_FDCWD_U64 : req->args[0];
    const u64 old_path_ptr = old_rename ? req->args[0] : req->args[1];
    const u64 new_dirfd = old_rename ? AT_FDCWD_U64 : req->args[2];
    const u64 new_path_ptr = old_rename ? req->args[1] : req->args[3];
    const u64 flags = has_flags ? req->args[4] : 0;
    if ((flags & (RENAME_EXCHANGE | RENAME_WHITEOUT)) != 0) return reply(errno_opnotsupp(), 0);
    if ((flags & ~((u64)RENAME_NOREPLACE)) != 0) return reply(errno_inval(), 0);
    if (!copy_cstr_from_target(old_path_ptr, old_path, sizeof(old_path))) return reply(errno_fault(), 0);
    if (!copy_cstr_from_target(new_path_ptr, new_path, sizeof(new_path))) return reply(errno_fault(), 0);
    if (old_path[0] == 0 || new_path[0] == 0) return reply(errno_noent(), 0);
    if (!resolve_path_at(old_dirfd, old_path, old_resolved)) return reply(errno_nametoolong(), 0);
    if (!resolve_path_at(new_dirfd, new_path, new_resolved)) return reply(errno_nametoolong(), 0);
    if (fs_cstr_eq(old_resolved, new_resolved)) return reply(0, 0);
    if ((flags & RENAME_NOREPLACE) != 0) {
        struct fs_stat_record rec;
        u64 token = 0;
        u64 size = 0;
        u8 kind = FS_OBJECT_NONE;
        if (vfs_lookup_stat(new_resolved, &token, &rec, &size, &kind)) return reply(errno_exist(), 0);
    }
    if (!vfs_rename_paths(old_resolved, new_resolved)) return reply(errno_io(), 0);
    volatile struct fs_response_header *response = (volatile struct fs_response_header *)vfs_response_addr();
    if (response->status == FS_STATUS_OK) {
        invalidate_exec_cache_for_path(old_resolved);
        invalidate_exec_cache_for_path(new_resolved);
        return reply(0, 0);
    }
    if (response->status == FS_STATUS_NOT_FOUND) return reply(errno_noent(), 0);
    if (response->status == FS_STATUS_NOT_DIR) return reply(errno_notdir(), 0);
    return reply(errno_from_fs_status(response->status), 0);
}
static struct ipc_message handle_access_path(u64 dirfd, u64 path_va, u64 mode, u64 flags) {
    if ((mode & ~(u64)0x7) != 0) return reply(errno_inval(), 0);
    if ((flags & ~(u64)(AT_EACCESS | AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)) != 0) return reply(errno_inval(), 0);
    char path[256];
    char resolved[FS_MAX_PATH_BYTES + 1];
    if (!copy_cstr_from_target(path_va, path, sizeof(path))) return reply(errno_fault(), 0);
    if (path[0] == 0) {
        if ((flags & AT_EMPTY_PATH) == 0) return reply(errno_noent(), 0);
        if (!fd_valid(dirfd)) return reply(errno_badf(), 0);
        return reply(0, 0);
    }
    if (!resolve_path_at(dirfd, path, resolved)) return reply(errno_nametoolong(), 0);
    if (path_is_dev_tty(resolved)) return reply(0, 0);
    struct fs_stat_record rec;
    u64 token = 0, size = 0;
    u8 kind = 0;
    return reply(vfs_lookup_stat(resolved, &token, &rec, &size, &kind) ? 0 : errno_noent(), 0);
}

static struct ipc_message handle_access(const struct trap_request *req) {
    return handle_access_path(AT_FDCWD_U64, req->args[0], req->args[1], 0);
}

static struct ipc_message handle_faccessat(const struct trap_request *req) {
    return handle_access_path(req->args[0], req->args[1], req->args[2], req->args[3]);
}
static struct ipc_message handle_arch_prctl(const struct trap_request *req) {
    const u64 code = req->args[0];
    const u64 value = req->args[1];
    if (code == ARCH_SET_FS) {
        const u64 status = set_target_fs_base(value);
        return reply(status == SYSCALL_OK ? 0 : errno_inval(), 0);
    }
    if (code == ARCH_SET_GS) {
        const u64 status = set_target_gs_base(value);
        return reply(status == SYSCALL_OK ? 0 : errno_inval(), 0);
    }
    if (code == ARCH_GET_FS || code == ARCH_GET_GS) {
        const u64 out = code == ARCH_GET_FS ? req->fs_base : req->gs_base;
        return copy_to_target(value, &out, sizeof(out)) == sizeof(out) ? reply(0, 0) : reply(errno_fault(), 0);
    }
    return reply(errno_inval(), 0);
}

static struct ipc_message handle_rt_sigaction(const struct trap_request *req) {
    const u64 signo = req->args[0];
    const u64 act_va = req->args[1];
    const u64 oldact_va = req->args[2];
    const u64 sigset_size = req->args[3];
    if (signo == 0 || signo >= 65) return reply(errno_inval(), 0);
    if (sigset_size != sizeof(u64)) return reply(errno_inval(), 0);
    if (oldact_va != 0) {
        u64 out[4];
        out[0] = g_proc->sig_handler[signo];
        out[1] = g_proc->sig_flags[signo];
        out[2] = g_proc->sig_restorer[signo];
        out[3] = 0;
        if (copy_to_target(oldact_va, out, sizeof(out)) != sizeof(out)) return reply(errno_fault(), 0);
    }
    if (act_va != 0) {
        u64 in[4];
        if (copy_from_target(act_va, in, sizeof(in)) != sizeof(in)) return reply(errno_fault(), 0);
        g_proc->sig_handler[signo] = in[0];
        g_proc->sig_flags[signo] = in[1];
        g_proc->sig_restorer[signo] = in[2];
        if (profile_trace_enabled()) {
            profile_trace_prefix("sigaction.set");
            user_log(" signo=");
            user_log_dec_value(signo);
            user_log(" handler=");
            user_log_hex_inline(in[0]);
            user_log(" flags=");
            user_log_hex_inline(in[1]);
            user_log(" restorer=");
            user_log_hex_inline(in[2]);
            user_log("\n");
        }
    }
    return reply(0, 0);
}

static struct ipc_message handle_rt_sigreturn(const struct trap_request *req) {
    if (req->rsp == 0) return reply(errno_fault(), 0);
    struct linux_signal_frame_body body;
    if (copy_from_target(req->rsp, &body, sizeof(body)) != sizeof(body)) return reply(errno_fault(), 0);
    if (body.magic != LINUX_SIGNAL_FRAME_MAGIC) {
        return reply(errno_inval(), 0);
    }
    body.saved_context.rip = body.ucontext.uc_mcontext.rip;
    body.saved_context.rsp = body.ucontext.uc_mcontext.rsp;
    body.saved_context.rflags = body.ucontext.uc_mcontext.eflags;
    body.saved_context.rax = body.ucontext.uc_mcontext.rax;
    body.saved_context.rbx = body.ucontext.uc_mcontext.rbx;
    body.saved_context.rcx = body.ucontext.uc_mcontext.rcx;
    body.saved_context.rdx = body.ucontext.uc_mcontext.rdx;
    body.saved_context.rsi = body.ucontext.uc_mcontext.rsi;
    body.saved_context.rdi = body.ucontext.uc_mcontext.rdi;
    body.saved_context.rbp = body.ucontext.uc_mcontext.rbp;
    body.saved_context.r8 = body.ucontext.uc_mcontext.r8;
    body.saved_context.r9 = body.ucontext.uc_mcontext.r9;
    body.saved_context.r10 = body.ucontext.uc_mcontext.r10;
    body.saved_context.r11 = body.ucontext.uc_mcontext.r11;
    body.saved_context.r12 = body.ucontext.uc_mcontext.r12;
    body.saved_context.r13 = body.ucontext.uc_mcontext.r13;
    body.saved_context.r14 = body.ucontext.uc_mcontext.r14;
    body.saved_context.r15 = body.ucontext.uc_mcontext.r15;
    if (profile_trace_enabled()) {
        profile_trace_prefix("signal.return");
        user_log(" signo=");
        user_log_dec_value(body.signo);
        user_log(" rip=");
        user_log_hex_inline(body.saved_context.rip);
        user_log(" rsp=");
        user_log_hex_inline(body.saved_context.rsp);
        user_log("\n");
    }
    const u64 target = abi_reply_target_principal();
    const u64 status = reply_trap_target_context(target, &body.saved_context);
    if (status != SYSCALL_OK) return reply(errno_io(), 0);
    abi_set_reply_target_principal(0);
    (void)detach_reply_token();
    return wait_ipc_timeout(1);
}

static struct ipc_message handle_rt_sigprocmask(const struct trap_request *req) {
    const u64 how = req->args[0];
    const u64 set_va = req->args[1];
    const u64 oldset_va = req->args[2];
    const u64 sigset_size = req->args[3];
    if (sigset_size != sizeof(u64)) return reply(errno_inval(), 0);
    if (profile_trace_enabled()) {
        profile_trace_prefix("sigprocmask.args");
        user_log(" how=");
        user_log_dec_value(how);
        user_log(" set=");
        user_log_hex_inline(set_va);
        user_log(" old=");
        user_log_hex_inline(oldset_va);
        user_log(" size=");
        user_log_dec_value(sigset_size);
        user_log("\n");
    }
    if (oldset_va != 0) {
        if (copy_to_target(oldset_va, &g_proc->blocked_signals, sizeof(g_proc->blocked_signals)) != sizeof(g_proc->blocked_signals)) return reply(errno_fault(), 0);
    }
    if (set_va != 0) {
        u64 set_word = 0;
        if (copy_from_target(set_va, &set_word, sizeof(set_word)) != sizeof(set_word)) return reply(errno_fault(), 0);
        set_word &= ~linux_unblockable_signal_mask();
        if (how == SIG_BLOCK) {
            g_proc->blocked_signals |= set_word;
        } else if (how == SIG_UNBLOCK) {
            g_proc->blocked_signals &= ~set_word;
        } else if (how == SIG_SETMASK) {
            g_proc->blocked_signals = set_word;
        } else {
            return reply(errno_inval(), 0);
        }
        if (profile_trace_enabled()) {
            profile_trace_prefix("sigprocmask.state");
            user_log(" mask=");
            user_log_hex_inline(g_proc->blocked_signals);
            user_log("\n");
        }
    }
    return reply(0, 0);
}

static void fill_sigwait_info(struct linux_siginfo *info, u64 signo) {
    u8 *p = (u8 *)info;
    for (u64 i = 0; i < sizeof(*info); i++) p[i] = 0;
    info->si_signo = (i32)signo;
    info->si_code = SI_TIMER;
}

static void clear_pending_sigwait(struct linux_process_state *proc) {
    proc->sigwait_pending = 0;
    proc->sigwait_set = 0;
    proc->sigwait_info_va = 0;
    proc->sigwait_deadline_tick = 0;
}

static void try_satisfy_pending_sigwaits(void) {
    const u64 now = syscall0(SYSCALL_GET_TICK_COUNT);
    for (u64 i = 0; i < LINUX_PROCESS_MAX; i++) {
        struct linux_process_state *proc = &g_processes[i];
        if (!proc->used || proc->exec_pending || !proc->sigwait_pending) continue;
        process_timers_update(proc);
        u64 result = 0;
        u64 signo = dequeue_pending_signal_matching(proc, proc->sigwait_set);
        if (signo != 0) {
            if (proc->sigwait_info_va != 0) {
                struct linux_siginfo info;
                fill_sigwait_info(&info, signo);
                if (copy_to_trap_target(proc->principal, proc->sigwait_info_va, &info, sizeof(info)) != sizeof(info)) {
                    result = errno_fault();
                } else {
                    result = signo;
                }
            } else {
                result = signo;
            }
        } else if (proc->sigwait_deadline_tick != 0 && now >= proc->sigwait_deadline_tick) {
            result = errno_again();
        } else {
            continue;
        }
        const u64 principal = proc->principal;
        clear_pending_sigwait(proc);
        (void)reply_trap_target(principal, result, 0);
    }
}

static struct ipc_message handle_rt_sigtimedwait(const struct trap_request *req) {
    const u64 set_va = req->args[0];
    const u64 info_va = req->args[1];
    const u64 timeout_va = req->args[2];
    const u64 sigset_size = req->args[3];
    if (set_va == 0) return reply(errno_fault(), 0);
    if (sigset_size != 0 && sigset_size < sizeof(u64)) return reply(errno_inval(), 0);
    u64 set_word = 0;
    if (copy_from_target(set_va, &set_word, sizeof(set_word)) != sizeof(set_word)) return reply(errno_fault(), 0);
    process_timers_update(g_proc);
    u64 signo = dequeue_pending_signal_matching(g_proc, set_word);
    if (signo != 0) {
        if (info_va != 0) {
            struct linux_siginfo info;
            fill_sigwait_info(&info, signo);
            if (copy_to_target(info_va, &info, sizeof(info)) != sizeof(info)) return reply(errno_fault(), 0);
        }
        return reply(signo, 0);
    }
    u64 timeout_ticks = 0;
    int timed = timeout_va != 0;
    if (timeout_va != 0) {
        i64 timeout[2];
        if (copy_from_target(timeout_va, timeout, sizeof(timeout)) != sizeof(timeout)) return reply(errno_fault(), 0);
        if (timeout[0] < 0 || timeout[1] < 0 || timeout[1] >= 1000000000LL) return reply(errno_inval(), 0);
        timeout_ticks = (u64)timeout[0] * 1000ULL + ((u64)timeout[1] + 999999ULL) / 1000000ULL;
        if (timeout_ticks == 0) return reply(errno_again(), 0);
    }
    if (g_proc->sigwait_pending) return reply(errno_again(), 0);
    g_proc->sigwait_pending = 1;
    g_proc->sigwait_set = set_word;
    g_proc->sigwait_info_va = info_va;
    g_proc->sigwait_deadline_tick = timed ? syscall0(SYSCALL_GET_TICK_COUNT) + timeout_ticks : 0;
    const u64 detach_status = detach_reply_token();
    if (detach_status != SYSCALL_OK) {
        clear_pending_sigwait(g_proc);
        return reply(errno_again(), 0);
    }
    return wait_ipc_timeout(1);
}

static struct ipc_message handle_sigaltstack(const struct trap_request *req) {
    const u64 ss_va = req->args[0];
    const u64 old_ss_va = req->args[1];

    if (old_ss_va != 0) {
        struct linux_stack_t old_ss;
        old_ss.ss_sp = g_proc->sigaltstack_sp;
        old_ss.ss_flags = g_proc->sigaltstack_flags != 0 ? g_proc->sigaltstack_flags : SS_DISABLE;
        old_ss.reserved0 = 0;
        old_ss.ss_size = g_proc->sigaltstack_size;
        if (copy_to_target(old_ss_va, &old_ss, sizeof(old_ss)) != sizeof(old_ss)) return reply(errno_fault(), 0);
    }

    if (ss_va != 0) {
        struct linux_stack_t ss;
        if (copy_from_target(ss_va, &ss, sizeof(ss)) != sizeof(ss)) return reply(errno_fault(), 0);
        const u32 supported_flags = (u32)(SS_DISABLE | SS_AUTODISARM);
        if ((ss.ss_flags & ~supported_flags) != 0) return reply(errno_inval(), 0);
        if ((ss.ss_flags & SS_DISABLE) != 0) {
            g_proc->sigaltstack_sp = 0;
            g_proc->sigaltstack_size = 0;
            g_proc->sigaltstack_flags = SS_DISABLE;
        } else {
            if (ss.ss_sp == 0 || ss.ss_size < MINSIGSTKSZ) return reply(errno_inval(), 0);
            g_proc->sigaltstack_sp = ss.ss_sp;
            g_proc->sigaltstack_size = ss.ss_size;
            g_proc->sigaltstack_flags = ss.ss_flags & SS_AUTODISARM;
        }
    }

    return reply(0, 0);
}
