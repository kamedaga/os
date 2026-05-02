static void init_process_fds(struct linux_process_state *proc) { for (u64 i = 0; i < 32; i++) proc->fds[i].kind = FD_UNUSED; proc->fds[0].kind = FD_STDIO; proc->fds[1].kind = FD_STDIO; proc->fds[2].kind = FD_STDIO; }
static void init_process_state(struct linux_process_state *proc, u64 principal) {
    proc->used = 1; proc->principal = principal; init_process_fds(proc);
    proc->mmap_next_va = 0x31000000ULL;
    proc->brk_next_va = 0x38000000ULL;
    for (u64 i = 0; i < VM_REGION_MAX; i++) proc->regions[i].used = 0;
    proc->cwd[0] = '/'; proc->cwd[1] = 0; proc->cwd_len = 1;
    for (u64 i = 0; i < LINUX_CHILD_MAX; i++) proc->child_used[i] = 0;
    proc->wait_pending = 0;
    proc->wait_pid = 0;
    proc->wait_status_va = 0;
    for (u64 i = 0; i < 65; i++) {
        proc->sig_handler[i] = 0;
        proc->sig_flags[i] = 0;
    }
}
static void init_process_tables(void) { for (u64 i = 0; i < LINUX_PROCESS_MAX; i++) g_processes[i].used = 0; for (u64 i = 0; i < PIPE_MAX; i++) g_pipes[i].used = 0; }
static struct linux_process_state *process_state_for(u64 principal) {
    if (principal == 0) return 0;
    for (u64 i = 0; i < LINUX_PROCESS_MAX; i++) if (g_processes[i].used && g_processes[i].principal == principal) return &g_processes[i];
    for (u64 i = 0; i < LINUX_PROCESS_MAX; i++) { if (g_processes[i].used) continue; init_process_state(&g_processes[i], principal); return &g_processes[i]; }
    return 0;
}
static int alloc_pipe_slot(void) { for (u64 i = 0; i < PIPE_MAX; i++) if (!g_pipes[i].used) return (int)i; return -1; }
static int fd_is_pipe(u64 fd) { return g_proc != 0 && fd < 32 && (g_fds[fd].kind == FD_PIPE_READ || g_fds[fd].kind == FD_PIPE_WRITE); }

static void pipe_ref_fd(const struct fd_entry *fd) {
    if (fd->pipe_id >= PIPE_MAX || !g_pipes[fd->pipe_id].used) return;
    if (fd->kind == FD_PIPE_READ) g_pipes[fd->pipe_id].read_refs++;
    if (fd->kind == FD_PIPE_WRITE) g_pipes[fd->pipe_id].write_refs++;
}

static void try_satisfy_pending_pipe_read(u8 pipe_id);

static void close_pipe_fd(u64 fd) {
    if (!fd_is_pipe(fd)) return;
    const u8 pipe_id = g_fds[fd].pipe_id;
    if (pipe_id >= PIPE_MAX || !g_pipes[pipe_id].used) return;
    if (g_fds[fd].kind == FD_PIPE_READ && g_pipes[pipe_id].read_refs != 0) g_pipes[pipe_id].read_refs--;
    if (g_fds[fd].kind == FD_PIPE_WRITE && g_pipes[pipe_id].write_refs != 0) g_pipes[pipe_id].write_refs--;
    try_satisfy_pending_pipe_read(pipe_id);
    if (g_pipes[pipe_id].read_refs == 0 && g_pipes[pipe_id].write_refs == 0) g_pipes[pipe_id].used = 0;
}

static void try_satisfy_pending_pipe_read(u8 pipe_id) {
    if (pipe_id >= PIPE_MAX || !g_pipes[pipe_id].used) return;
    struct pipe_entry *pipe = &g_pipes[pipe_id];
    if (!pipe->pending_read) return;
    if (pipe->len == 0 && pipe->write_refs != 0) return;
    const u64 principal = pipe->pending_principal;
    const u64 dst = pipe->pending_dst;
    const u64 want = pipe->pending_len;
    u64 result = 0;

    if (pipe->len != 0) {
        const u64 n = min_u64(want, pipe->len);
        u64 done = 0;
        int fault = 0;
        while (done < n) {
            const u64 index = (pipe->head + done) % PIPE_BUFFER_BYTES;
            const u64 contiguous = min_u64(n - done, PIPE_BUFFER_BYTES - index);
            if (copy_to_trap_target(principal, dst + done, &pipe->bytes[index], contiguous) != contiguous) {
                fault = 1;
                break;
            }
            done += contiguous;
        }
        if (fault) {
            result = errno_fault();
        } else {
            pipe->head = (pipe->head + n) % PIPE_BUFFER_BYTES;
            pipe->len -= n;
            result = n;
        }
    }

    pipe->pending_read = 0;
    pipe->pending_principal = 0;
    pipe->pending_dst = 0;
    pipe->pending_len = 0;
    (void)reply_trap_target(principal, result, 0);
}

static u64 pipe_read_to_target(u64 fd, u64 dst, u64 len, int *fault) {
    *fault = 0;
    if (fd >= 32 || g_fds[fd].kind != FD_PIPE_READ) return errno_badf();
    const u8 pipe_id = g_fds[fd].pipe_id;
    if (pipe_id >= PIPE_MAX || !g_pipes[pipe_id].used || g_pipes[pipe_id].read_refs == 0) return errno_badf();
    struct pipe_entry *pipe = &g_pipes[pipe_id];
    if (len == 0) return 0;
    if (pipe->len == 0) return 0;
    const u64 n = min_u64(len, pipe->len);
    u64 done = 0;
    while (done < n) {
        const u64 index = (pipe->head + done) % PIPE_BUFFER_BYTES;
        const u64 contiguous = min_u64(n - done, PIPE_BUFFER_BYTES - index);
        if (copy_to_target(dst + done, &pipe->bytes[index], contiguous) != contiguous) { *fault = 1; return 0; }
        done += contiguous;
    }
    pipe->head = (pipe->head + n) % PIPE_BUFFER_BYTES;
    pipe->len -= n;
    return n;
}

static u64 pipe_write_from_target(u64 fd, u64 src, u64 len, int *fault) {
    *fault = 0;
    if (fd >= 32 || g_fds[fd].kind != FD_PIPE_WRITE) return errno_badf();
    const u8 pipe_id = g_fds[fd].pipe_id;
    if (pipe_id >= PIPE_MAX || !g_pipes[pipe_id].used || g_pipes[pipe_id].write_refs == 0) return errno_badf();
    struct pipe_entry *pipe = &g_pipes[pipe_id];
    if (pipe->read_refs == 0) return errno_pipe();
    if (len == 0) return 0;
    u64 written = 0;
    while (written < len && pipe->len < PIPE_BUFFER_BYTES) {
        const u64 tail = (pipe->head + pipe->len) % PIPE_BUFFER_BYTES;
        const u64 space = PIPE_BUFFER_BYTES - pipe->len;
        const u64 contiguous = min_u64(min_u64(len - written, PIPE_BUFFER_BYTES - tail), space);
        if (copy_from_target(src + written, &pipe->bytes[tail], contiguous) != contiguous) { *fault = 1; return written; }
        pipe->len += contiguous;
        written += contiguous;
    }
    if (written == 0) return errno_again();
    return written;
}
static int alloc_fd(void) { for (int i = 3; i < 32; i++) if (g_fds[i].kind == FD_UNUSED) return i; return -1; }
static int fd_valid(u64 fd) { return g_proc != 0 && fd < 32 && g_fds[fd].kind != FD_UNUSED; }
