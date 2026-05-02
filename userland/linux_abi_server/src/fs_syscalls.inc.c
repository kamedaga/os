static u32 linux_mode_from_fs(u32 fs_mode, u8 kind) { u32 perm = (kind == FS_OBJECT_DIRECTORY || kind == FS_OBJECT_MOUNT) ? 0555 : 0444; u32 type = (kind == FS_OBJECT_DIRECTORY || kind == FS_OBJECT_MOUNT) ? 0040000 : 0100000; if ((fs_mode & FS_DIR_MODE) != 0) type = 0040000; if ((fs_mode & FS_FILE_MODE) != 0) type = 0100000; return type | perm; }
static void fill_linux_stat(struct linux_stat *st, const struct fs_stat_record *rec, u64 size, u8 kind) {
    u8 *p = (u8 *)st; for (u64 i = 0; i < sizeof(*st); i++) p[i] = 0;
    st->st_nlink = (kind == FS_OBJECT_DIRECTORY || kind == FS_OBJECT_MOUNT) ? 2 : 1; st->st_mode = linux_mode_from_fs(rec->mode_bits, kind); st->st_size = (i64)size; st->st_blksize = 4096; st->st_blocks = (i64)((size + 511) / 512); st->st_mtime = (i64)rec->mtime_unix_sec; st->st_atime = st->st_mtime; st->st_ctime = st->st_mtime;
}

static void fd_set_path(struct fd_entry *fd, const char *path) {
    u64 len = cstr_len(path);
    if (len > FS_MAX_PATH_BYTES) len = FS_MAX_PATH_BYTES;
    fd->path_len = (u16)len;
    for (u64 i = 0; i < len; i++) fd->path[i] = path[i];
    fd->path[len] = 0;
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

static int resolve_path_at(u64 dirfd, const char *path, char *out) {
    if (path[0] == '/') return normalize_path("/", path, out);
    if (dirfd == AT_FDCWD_U64) return normalize_path(g_cwd, path, out);
    if (!fd_valid(dirfd) || g_fds[dirfd].kind != FD_DIR || g_fds[dirfd].path_len == 0) return 0;
    return normalize_path(g_fds[dirfd].path, path, out);
}

static int path_is_dev_file(const char *path) {
    return path[0] == '/' && path[1] == 'd' && path[2] == 'e' && path[3] == 'v' && path[4] == '/';
}

static struct ipc_message handle_openat(const struct trap_request *req, int old_open) {
    char path[256];
    char resolved[FS_MAX_PATH_BYTES + 1];
    const u64 dirfd = old_open ? AT_FDCWD_U64 : req->args[0];
    const u64 path_ptr = old_open ? req->args[0] : req->args[1];
    const u64 flags = old_open ? req->args[1] : req->args[2];
    const u64 access_mode = flags & O_ACCMODE;
    if (access_mode != O_RDONLY && access_mode != O_WRONLY && access_mode != O_RDWR) return reply(errno_acces(), 0);
    if ((flags & (O_CREAT | O_TRUNC)) != 0 && access_mode == O_RDONLY) return reply(errno_acces(), 0);
    if (!copy_cstr_from_target(path_ptr, path, sizeof(path))) return reply(errno_fault(), 0);
    if (path[0] == 0) return reply(errno_noent(), 0);
    if (!resolve_path_at(dirfd, path, resolved)) return reply(errno_nametoolong(), 0);
    struct fs_stat_record rec; u64 token = 0; u64 size = 0; u8 kind = FS_OBJECT_NONE;
    if (!vfs_lookup_stat(resolved, &token, &rec, &size, &kind)) {
        if ((flags & O_CREAT) == 0) return reply(errno_noent(), 0);
        if (!vfs_create_path(resolved, 0)) return reply(errno_io(), 0);
        volatile struct fs_response_header *created = (volatile struct fs_response_header *)VFS_RESPONSE_VA;
        if (created->status != FS_STATUS_OK || created->result_token == 0) return reply(errno_acces(), 0);
        token = created->result_token;
        size = created->file_bytes;
        kind = created->object_kind;
        rec.object_kind = kind;
        rec.size_bytes = size;
        rec.mode_bits = FS_FILE_MODE;
        rec.mtime_unix_sec = 0;
    } else if ((flags & O_TRUNC) != 0 && access_mode != O_RDONLY && !path_is_dev_file(resolved)) {
        if (!vfs_create_path(resolved, 1)) return reply(errno_io(), 0);
        volatile struct fs_response_header *created = (volatile struct fs_response_header *)VFS_RESPONSE_VA;
        if (created->status != FS_STATUS_OK || created->result_token == 0) return reply(errno_acces(), 0);
        token = created->result_token;
        size = 0;
        kind = created->object_kind;
    }
    const int fd = alloc_fd(); if (fd < 0) return reply(errno_busy(), 0);
    if (kind == FS_OBJECT_DIRECTORY || kind == FS_OBJECT_MOUNT) {
        g_fds[fd].kind = FD_DIR; g_fds[fd].token = token; g_fds[fd].offset = 0; g_fds[fd].size = 0; g_fds[fd].mode_bits = rec.mode_bits; g_fds[fd].object_kind = kind;
        fd_set_path(&g_fds[fd], resolved);
        return reply((u64)fd, 0);
    }
    if ((flags & O_DIRECTORY) != 0) return reply(errno_notdir(), 0);
    if (!vfs_request(FS_OP_OPEN, token, 0, 0, 0)) return reply(errno_io(), 0);
    volatile struct fs_response_header *response = (volatile struct fs_response_header *)VFS_RESPONSE_VA;
    if (response->status != FS_STATUS_OK || response->result_token == 0) return reply(errno_acces(), 0);
    g_fds[fd].kind = FD_FILE; g_fds[fd].token = response->result_token; g_fds[fd].offset = 0; g_fds[fd].size = response->file_bytes != 0 ? response->file_bytes : size; g_fds[fd].mode_bits = rec.mode_bits; g_fds[fd].object_kind = FS_OBJECT_FILE;
    fd_set_path(&g_fds[fd], resolved);
    return reply((u64)fd, 0);
}

static struct ipc_message handle_read(const struct trap_request *req) {
    const u64 fd = req->args[0]; const u64 dst = req->args[1]; const u64 len = req->args[2];
    if (!fd_valid(fd)) return reply(errno_badf(), 0);
    if (len == 0) return reply(0, 0);
    if (g_fds[fd].kind == FD_STDIO) return reply(0, 0);
    if (g_fds[fd].kind == FD_PIPE_READ) {
        const u8 pipe_id = g_fds[fd].pipe_id;
        if (pipe_id >= PIPE_MAX || !g_pipes[pipe_id].used) return reply(errno_badf(), 0);
        struct pipe_entry *pipe = &g_pipes[pipe_id];
        if (pipe->len == 0 && (pipe->write_refs != 0 || pipe_has_live_writer(pipe_id))) {
            if (pipe->pending_read) return reply(errno_again(), 0);
            pipe->pending_read = 1;
            pipe->pending_principal = req->caller_principal;
            pipe->pending_dst = dst;
            pipe->pending_len = len;
            return wait_ipc();
        }
        int fault = 0; const u64 n = pipe_read_to_target(fd, dst, len, &fault); return reply(fault ? errno_fault() : n, 0);
    }
    if (g_fds[fd].kind != FD_FILE) return reply(errno_badf(), 0);
    u64 copied = 0;
    while (copied < len) {
        u64 request_len = min_u64(len - copied, FS_RESPONSE_PAYLOAD_BYTES);
        if (!vfs_request(FS_OP_READ, g_fds[fd].token, g_fds[fd].offset, (u32)request_len, 0)) return copied != 0 ? reply(copied, 0) : reply(errno_io(), 0);
        volatile struct fs_response_header *response = (volatile struct fs_response_header *)VFS_RESPONSE_VA;
        if (response->status != FS_STATUS_OK) return copied != 0 ? reply(copied, 0) : reply(errno_io(), 0);
        if (response->inline_bytes == 0) break;
        if (copy_to_target(dst + copied, (const void *)(VFS_RESPONSE_VA + FS_RESPONSE_HEADER_BYTES), response->inline_bytes) != response->inline_bytes) return reply(errno_fault(), 0);
        copied += response->inline_bytes; g_fds[fd].offset += response->inline_bytes;
        if (response->inline_bytes < request_len) break;
    }
    return reply(copied, 0);
}

static u64 read_fd_at_to_target(const struct fd_entry *fd, u64 file_offset, u64 dst, u64 len, int *fault) {
    u64 copied = 0;
    *fault = 0;
    while (copied < len && file_offset + copied < fd->size) {
        u64 request_len = min_u64(len - copied, FS_RESPONSE_PAYLOAD_BYTES);
        const u64 remaining = fd->size - (file_offset + copied);
        if (request_len > remaining) request_len = remaining;
        if (!vfs_request(FS_OP_READ, fd->token, file_offset + copied, (u32)request_len, 0)) break;
        volatile struct fs_response_header *response = (volatile struct fs_response_header *)VFS_RESPONSE_VA;
        if (response->status != FS_STATUS_OK) break;
        if (response->inline_bytes == 0) break;
        if (copy_to_target(dst + copied, (const void *)(VFS_RESPONSE_VA + FS_RESPONSE_HEADER_BYTES), response->inline_bytes) != response->inline_bytes) {
            *fault = 1;
            break;
        }
        copied += response->inline_bytes;
        if (response->inline_bytes < request_len) break;
    }
    return copied;
}

static struct ipc_message handle_pread64(const struct trap_request *req) {
    const u64 fd = req->args[0]; const u64 dst = req->args[1]; const u64 len = req->args[2]; const u64 offset = req->args[3];
    if (!fd_valid(fd)) return reply(errno_badf(), 0); if (len == 0) return reply(0, 0); if (g_fds[fd].kind != FD_FILE) return reply(errno_badf(), 0);
    int fault = 0; const u64 copied = read_fd_at_to_target(&g_fds[fd], offset, dst, len, &fault);
    if (fault) return reply(errno_fault(), 0);
    return copied != 0 ? reply(copied, 0) : reply(0, 0);
}

static struct ipc_message handle_write(const struct trap_request *req) {
    const u64 fd = req->args[0]; const u64 src = req->args[1]; const u64 len = req->args[2];
    if (fd_valid(fd) && g_fds[fd].kind == FD_PIPE_WRITE) {
        const u8 pipe_id = g_fds[fd].pipe_id;
        int fault = 0; const u64 n = pipe_write_from_target(fd, src, len, &fault);
        if (!fault && (i64)n > 0) try_satisfy_pending_pipe_read(pipe_id);
        return reply(fault ? errno_fault() : n, 0);
    }
    if (fd_valid(fd) && g_fds[fd].kind == FD_FILE) {
        if (len == 0) return reply(0, 0);
        int fault = 0;
        const u64 n = vfs_write_from_target(g_fds[fd].token, g_fds[fd].offset, src, len, &fault);
        if (fault) return reply(errno_fault(), 0);
        if (n != 0) {
            g_fds[fd].offset += n;
            if (g_fds[fd].offset > g_fds[fd].size) g_fds[fd].size = g_fds[fd].offset;
            return reply(n, 0);
        }
        return reply(errno_io(), 0);
    }
    if (fd == 1 || fd == 2) {
        char buf[129]; u64 done = 0;
        while (done < len) { u64 chunk = min_u64(len - done, 128); if (copy_from_target(src + done, buf, chunk) != chunk) break; user_log_len(buf, chunk); done += chunk; }
        return reply(len, 0);
    }
    return reply(errno_badf(), 0);
}

static struct ipc_message handle_writev(const struct trap_request *req) {
    const u64 fd = req->args[0]; const u64 iov = req->args[1]; const u64 iovcnt = req->args[2];
    if (fd_valid(fd) && g_fds[fd].kind == FD_PIPE_WRITE) {
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
        u64 total = 0;
        for (u64 i = 0; i < iovcnt; i++) {
            u64 pair[2];
            if (copy_from_target(iov + i * 16, pair, sizeof(pair)) != sizeof(pair)) return reply(errno_fault(), 0);
            int fault = 0;
            const u64 n = vfs_write_from_target(g_fds[fd].token, g_fds[fd].offset, pair[0], pair[1], &fault);
            if (fault) return reply(errno_fault(), 0);
            if (n == 0 && pair[1] != 0) return total != 0 ? reply(total, 0) : reply(errno_io(), 0);
            g_fds[fd].offset += n;
            if (g_fds[fd].offset > g_fds[fd].size) g_fds[fd].size = g_fds[fd].offset;
            total += n;
            if (n != pair[1]) break;
        }
        return reply(total, 0);
    }
    if (fd != 1 && fd != 2) return reply(errno_badf(), 0);
    if (iovcnt > 64) return reply(errno_inval(), 0);
    u64 total = 0;
    for (u64 i = 0; i < iovcnt; i++) {
        u64 pair[2];
        if (copy_from_target(iov + i * 16, pair, sizeof(pair)) != sizeof(pair)) return reply(errno_fault(), 0);
        const u64 src = pair[0]; const u64 len = pair[1];
        u64 done = 0;
        while (done < len) {
            char buf[129]; u64 chunk = min_u64(len - done, 128);
            if (copy_from_target(src + done, buf, chunk) != chunk) return reply(errno_fault(), 0);
            user_log_len(buf, chunk);
            done += chunk;
        }
        total += len;
    }
    return reply(total, 0);
}

static struct ipc_message handle_fstat(const struct trap_request *req) {
    const u64 fd = req->args[0]; const u64 stat_va = req->args[1]; if (!fd_valid(fd)) return reply(errno_badf(), 0);
    struct fs_stat_record rec; rec.object_kind = g_fds[fd].object_kind; rec.size_bytes = g_fds[fd].size; rec.mode_bits = g_fds[fd].mode_bits; rec.mtime_unix_sec = 0;
    if (g_fds[fd].kind == FD_STDIO) { rec.object_kind = FS_OBJECT_FILE; rec.size_bytes = 0; rec.mode_bits = FS_FILE_MODE; }
    struct linux_stat st; fill_linux_stat(&st, &rec, rec.size_bytes, rec.object_kind);
    if (copy_to_target(stat_va, &st, sizeof(st)) != sizeof(st)) return reply(errno_fault(), 0);
    return reply(0, 0);
}

static struct ipc_message handle_newfstatat(const struct trap_request *req, int old_stat) {
    const u64 dirfd = old_stat ? AT_FDCWD_U64 : req->args[0]; const u64 path_ptr = old_stat ? req->args[0] : req->args[1]; const u64 stat_va = old_stat ? req->args[1] : req->args[2]; const u64 flags = old_stat ? 0 : req->args[3];
    if (path_ptr == 0) { struct trap_request f = *req; f.args[0] = old_stat ? 0 : dirfd; f.args[1] = stat_va; return handle_fstat(&f); }
    char path[256]; char resolved[FS_MAX_PATH_BYTES + 1]; if (!copy_cstr_from_target(path_ptr, path, sizeof(path))) return reply(errno_fault(), 0);
    if (path[0] == 0 && (flags & AT_EMPTY_PATH) != 0) { struct trap_request f = *req; f.args[0] = old_stat ? 0 : dirfd; f.args[1] = stat_va; return handle_fstat(&f); }
    if (path[0] == 0) return reply(errno_noent(), 0);
    if (!resolve_path_at(dirfd, path, resolved)) return reply(errno_nametoolong(), 0);
    struct fs_stat_record rec; u64 token = 0; u64 size = 0; u8 kind = FS_OBJECT_NONE; if (!vfs_lookup_stat(resolved, &token, &rec, &size, &kind)) return reply(errno_noent(), 0); (void)token;
    struct linux_stat st; fill_linux_stat(&st, &rec, size, kind); if (copy_to_target(stat_va, &st, sizeof(st)) != sizeof(st)) return reply(errno_fault(), 0); return reply(0, 0);
}

static struct ipc_message handle_getdents64(const struct trap_request *req) {
    const u64 fd = req->args[0]; const u64 dst = req->args[1]; const u64 len = req->args[2]; if (!fd_valid(fd) || g_fds[fd].kind != FD_DIR) return reply(errno_badf(), 0);
    u64 written = 0;
    while (written + 32 <= len) {
        if (!vfs_request(FS_OP_READDIR, g_fds[fd].token, g_fds[fd].offset, 0, 0)) return written != 0 ? reply(written, 0) : reply(errno_io(), 0);
        volatile struct fs_response_header *response = (volatile struct fs_response_header *)VFS_RESPONSE_VA;
        if (response->status == FS_STATUS_END_OF_DIR) break; if (response->status != FS_STATUS_OK || response->inline_bytes < FS_DIRENT_RECORD_BYTES) return written != 0 ? reply(written, 0) : reply(errno_io(), 0);
        volatile struct fs_dirent_record *record = (volatile struct fs_dirent_record *)(VFS_RESPONSE_VA + FS_RESPONSE_HEADER_BYTES); if (response->inline_bytes < FS_DIRENT_RECORD_BYTES + record->name_bytes) return reply(errno_io(), 0);
        u64 reclen = align_up(19 + record->name_bytes + 1, 8); if (written + reclen > len) break;
        u8 out[320]; for (u64 i = 0; i < sizeof(out); i++) out[i] = 0;
        *((u64 *)(out + 0)) = 1; *((i64 *)(out + 8)) = (i64)record->next_cursor; *((u16 *)(out + 16)) = (u16)reclen; out[18] = (record->object_kind == FS_OBJECT_DIRECTORY || record->object_kind == FS_OBJECT_MOUNT) ? DT_DIR : DT_REG;
        volatile u8 *name = (volatile u8 *)(VFS_RESPONSE_VA + FS_RESPONSE_HEADER_BYTES + FS_DIRENT_RECORD_BYTES); for (u64 i = 0; i < record->name_bytes && 19 + i < sizeof(out); i++) out[19 + i] = name[i];
        if (copy_to_target(dst + written, out, reclen) != reclen) return reply(errno_fault(), 0);
        written += reclen; g_fds[fd].offset = record->next_cursor;
        if (record->next_cursor == 0) break;
    }
    return reply(written, 0);
}

static struct ipc_message handle_lseek(const struct trap_request *req) {
    const u64 fd = req->args[0]; const i64 off = (i64)req->args[1]; const u64 whence = req->args[2]; if (!fd_valid(fd)) return reply(errno_badf(), 0);
    if (fd_is_pipe(fd)) return reply(errno_spipe(), 0);
    i64 base = 0; if (whence == SEEK_SET) base = 0; else if (whence == SEEK_CUR) base = (i64)g_fds[fd].offset; else if (whence == SEEK_END) base = (i64)g_fds[fd].size; else return reply(errno_inval(), 0);
    i64 next = base + off; if (next < 0) return reply(errno_inval(), 0); g_fds[fd].offset = (u64)next; return reply((u64)next, 0);
}

static struct ipc_message handle_close(const struct trap_request *req) {
    const u64 fd = req->args[0];
    if (fd >= 32 || g_fds[fd].kind == FD_UNUSED) return reply(errno_badf(), 0);
    if (fd_is_pipe(fd)) close_pipe_fd(fd);
    g_fds[fd].kind = FD_UNUSED;
    return reply(0, 0);
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
    volatile struct fs_response_header *response = (volatile struct fs_response_header *)VFS_RESPONSE_VA;
    if (response->status == FS_STATUS_OK) return reply(0, 0);
    if (response->status == FS_STATUS_NOT_FOUND) return reply(errno_noent(), 0);
    if (response->status == FS_STATUS_NOT_DIR) return reply(errno_notdir(), 0);
    return reply(errno_acces(), 0);
}
static struct ipc_message handle_access(const struct trap_request *req) { char path[256]; char resolved[FS_MAX_PATH_BYTES + 1]; if (!copy_cstr_from_target(req->args[0], path, sizeof(path))) return reply(errno_fault(), 0); if (path[0] == 0) return reply(errno_noent(), 0); if (!resolve_path_at(AT_FDCWD_U64, path, resolved)) return reply(errno_nametoolong(), 0); struct fs_stat_record rec; u64 token = 0, size = 0; u8 kind = 0; return reply(vfs_lookup_stat(resolved, &token, &rec, &size, &kind) ? 0 : errno_noent(), 0); }
static struct ipc_message handle_arch_prctl(const struct trap_request *req) { if (req->args[0] != ARCH_SET_FS) return reply(errno_inval(), 0); return reply(set_target_fs_base(req->args[1]) == SYSCALL_OK ? 0 : errno_inval(), 0); }

static struct ipc_message handle_rt_sigaction(const struct trap_request *req) {
    const u64 signo = req->args[0];
    const u64 act_va = req->args[1];
    const u64 oldact_va = req->args[2];
    if (signo == 0 || signo >= 65) return reply(errno_inval(), 0);
    if (oldact_va != 0) {
        u64 out[4];
        out[0] = g_proc->sig_handler[signo];
        out[1] = g_proc->sig_flags[signo];
        out[2] = 0;
        out[3] = 0;
        if (copy_to_target(oldact_va, out, sizeof(out)) != sizeof(out)) return reply(errno_fault(), 0);
    }
    if (act_va != 0) {
        u64 in[4];
        if (copy_from_target(act_va, in, sizeof(in)) != sizeof(in)) return reply(errno_fault(), 0);
        g_proc->sig_handler[signo] = in[0];
        g_proc->sig_flags[signo] = in[1];
    }
    return reply(0, 0);
}

static struct ipc_message handle_rt_sigprocmask(const struct trap_request *req) {
    const u64 oldset_va = req->args[2];
    if (oldset_va != 0) {
        const u64 empty = 0;
        if (copy_to_target(oldset_va, &empty, sizeof(empty)) != sizeof(empty)) return reply(errno_fault(), 0);
    }
    return reply(0, 0);
}
