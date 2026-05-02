static int fd_clone_into(u64 dst, u64 src) {
    if (!fd_valid(src) || dst >= 32) return 0;
    g_fds[dst].kind = g_fds[src].kind;
    g_fds[dst].token = g_fds[src].token;
    g_fds[dst].offset = g_fds[src].offset;
    g_fds[dst].size = g_fds[src].size;
    g_fds[dst].mode_bits = g_fds[src].mode_bits;
    g_fds[dst].object_kind = g_fds[src].object_kind;
    g_fds[dst].pipe_id = g_fds[src].pipe_id;
    g_fds[dst].path_len = g_fds[src].path_len;
    for (u16 i = 0; i <= g_fds[src].path_len && i <= FS_MAX_PATH_BYTES; i++) g_fds[dst].path[i] = g_fds[src].path[i];
    pipe_ref_fd(&g_fds[dst]);
    return 1;
}

static int alloc_fd_at_least(u64 min_fd) {
    if (min_fd >= 32) return -1;
    for (u64 i = min_fd; i < 32; i++) if (g_fds[i].kind == FD_UNUSED) return (int)i;
    return -1;
}

static struct ipc_message handle_pipe2(const struct trap_request *req, int has_flags) {
    const u64 pipefd_va = req->args[0];
    const u64 flags = has_flags ? req->args[1] : 0;
    if ((flags & ~(u64)(O_CLOEXEC | O_NONBLOCK)) != 0) return reply(errno_inval(), 0);
    const int pipe_slot = alloc_pipe_slot();
    if (pipe_slot < 0) return reply(errno_busy(), 0);
    const int read_fd = alloc_fd_at_least(0);
    if (read_fd < 0) return reply(errno_busy(), 0);
    g_fds[(u64)read_fd].kind = FD_PIPE_READ;
    const int write_fd = alloc_fd_at_least(0);
    if (write_fd < 0) { g_fds[(u64)read_fd].kind = FD_UNUSED; return reply(errno_busy(), 0); }

    g_pipes[(u64)pipe_slot].used = 1;
    g_pipes[(u64)pipe_slot].pending_read = 0;
    g_pipes[(u64)pipe_slot].read_refs = 1;
    g_pipes[(u64)pipe_slot].write_refs = 1;
    g_pipes[(u64)pipe_slot].head = 0;
    g_pipes[(u64)pipe_slot].len = 0;
    g_pipes[(u64)pipe_slot].pending_principal = 0;
    g_pipes[(u64)pipe_slot].pending_dst = 0;
    g_pipes[(u64)pipe_slot].pending_len = 0;

    g_fds[(u64)read_fd].kind = FD_PIPE_READ;
    g_fds[(u64)read_fd].token = 0;
    g_fds[(u64)read_fd].offset = 0;
    g_fds[(u64)read_fd].size = 0;
    g_fds[(u64)read_fd].mode_bits = FS_FILE_MODE;
    g_fds[(u64)read_fd].object_kind = FS_OBJECT_FILE;
    g_fds[(u64)read_fd].pipe_id = (u8)pipe_slot;
    g_fds[(u64)read_fd].path_len = 0;
    g_fds[(u64)read_fd].path[0] = 0;

    g_fds[(u64)write_fd].kind = FD_PIPE_WRITE;
    g_fds[(u64)write_fd].token = 0;
    g_fds[(u64)write_fd].offset = 0;
    g_fds[(u64)write_fd].size = 0;
    g_fds[(u64)write_fd].mode_bits = FS_FILE_MODE;
    g_fds[(u64)write_fd].object_kind = FS_OBJECT_FILE;
    g_fds[(u64)write_fd].pipe_id = (u8)pipe_slot;
    g_fds[(u64)write_fd].path_len = 0;
    g_fds[(u64)write_fd].path[0] = 0;

    const u32 read_fd32 = (u32)read_fd;
    const u32 write_fd32 = (u32)write_fd;
    if (copy_to_target(pipefd_va, &read_fd32, sizeof(read_fd32)) != sizeof(read_fd32) ||
        copy_to_target(pipefd_va + sizeof(read_fd32), &write_fd32, sizeof(write_fd32)) != sizeof(write_fd32)) {
        g_fds[(u64)read_fd].kind = FD_UNUSED;
        g_fds[(u64)write_fd].kind = FD_UNUSED;
        g_pipes[(u64)pipe_slot].used = 0;
        return reply(errno_fault(), 0);
    }
    return reply(0, 0);
}

static struct ipc_message handle_dup(const struct trap_request *req) {
    const u64 oldfd = req->args[0];
    const int newfd = alloc_fd_at_least(0);
    if (newfd < 0) return reply(errno_busy(), 0);
    if (!fd_clone_into((u64)newfd, oldfd)) return reply(errno_badf(), 0);
    return reply((u64)newfd, 0);
}

static struct ipc_message handle_dup2_like(const struct trap_request *req, int dup3) {
    const u64 oldfd = req->args[0]; const u64 newfd = req->args[1];
    if (newfd >= 32) return reply(errno_badf(), 0);
    if (!fd_valid(oldfd)) return reply(errno_badf(), 0);
    if (dup3 && oldfd == newfd) return reply(errno_inval(), 0);
    if (oldfd == newfd) return reply(newfd, 0);
    if (fd_is_pipe(newfd)) close_pipe_fd(newfd);
    if (!fd_clone_into(newfd, oldfd)) return reply(errno_badf(), 0);
    return reply(newfd, 0);
}

static struct ipc_message handle_fcntl(const struct trap_request *req) {
    const u64 fd = req->args[0]; const u64 cmd = req->args[1]; const u64 arg = req->args[2];
    if (!fd_valid(fd)) return reply(errno_badf(), 0);
    if (cmd == F_DUPFD || cmd == F_DUPFD_CLOEXEC) {
        const int newfd = alloc_fd_at_least(arg);
        if (newfd < 0) return reply(errno_busy(), 0);
        if (!fd_clone_into((u64)newfd, fd)) return reply(errno_badf(), 0);
        return reply((u64)newfd, 0);
    }
    if (cmd == F_GETFD) return reply(0, 0);
    if (cmd == F_SETFD) return reply(0, 0);
    if (cmd == F_GETFL) return reply(g_fds[fd].kind == FD_PIPE_WRITE ? O_WRONLY : O_RDONLY, 0);
    if (cmd == F_SETFL) return reply(0, 0);
    return reply(errno_inval(), 0);
}
