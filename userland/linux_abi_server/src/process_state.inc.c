static int initial_stdio_should_use_tty(void) {
    return g_console.active;
}

static void init_process_fds(struct linux_process_state *proc) {
    for (u64 i = 0; i < 32; i++) {
        proc->fds[i].kind = FD_UNUSED;
        proc->fds[i].fd_flags = 0;
        proc->fds[i].desc_flags = 0;
    }
    const int use_tty = initial_stdio_should_use_tty();
    const enum fd_kind stdio_kind = use_tty ? FD_TTY : FD_STDIO;
    proc->fds[0].kind = stdio_kind; proc->fds[0].fd_flags = 0; proc->fds[0].desc_flags = 0;
    proc->fds[1].kind = stdio_kind; proc->fds[1].fd_flags = 0; proc->fds[1].desc_flags = 0;
    proc->fds[2].kind = stdio_kind; proc->fds[2].fd_flags = 0; proc->fds[2].desc_flags = 0;
}
static void copy_fd_entry(struct fd_entry *dst, const struct fd_entry *src) {
    dst->kind = src->kind;
    dst->token = src->token;
    dst->offset = src->offset;
    dst->size = src->size;
    dst->mode_bits = src->mode_bits;
    dst->fd_flags = src->fd_flags;
    dst->desc_flags = src->desc_flags;
    dst->object_kind = src->object_kind;
    dst->pipe_id = src->pipe_id;
    dst->socket_connected = src->socket_connected;
    dst->socket_type = src->socket_type;
    dst->socket_connecting = src->socket_connecting;
    dst->socket_reserved0 = src->socket_reserved0;
    dst->path_len = src->path_len;
    dst->socket_local_port = src->socket_local_port;
    dst->socket_remote_port = src->socket_remote_port;
    dst->socket_local_ip = src->socket_local_ip;
    dst->socket_remote_ip = src->socket_remote_ip;
    for (u16 i = 0; i <= src->path_len && i <= FS_MAX_PATH_BYTES; i++) dst->path[i] = src->path[i];
}
static void init_process_state(struct linux_process_state *proc, u64 principal) {
    proc->used = 1; proc->exec_pending = 0; proc->exit_status = 0; proc->pid = principal; proc->tid = principal; proc->pgid = principal; proc->principal = principal; init_process_fds(proc);
    proc->target_token = 0;
    proc->exec_pending_principal = 0;
    proc->mmap_next_va = 0x31000000ULL;
    proc->brk_next_va = 0x38000000ULL;
    for (u64 i = 0; i < VM_REGION_MAX; i++) proc->regions[i].used = 0;
    proc->cwd[0] = '/'; proc->cwd[1] = 0; proc->cwd_len = 1;
    for (u64 i = 0; i < LINUX_CHILD_MAX; i++) proc->child_used[i] = 0;
    proc->wait_pending = 0;
    proc->wait_pid = 0;
    proc->wait_status_va = 0;
    proc->clear_child_tid = 0;
    proc->profile_enabled = 0;
    proc->sigaltstack_sp = 0;
    proc->sigaltstack_size = 0;
    proc->sigaltstack_flags = SS_DISABLE;
    for (u64 i = 0; i < 65; i++) {
        proc->sig_handler[i] = 0;
        proc->sig_flags[i] = 0;
    }
}
static void init_process_tables(void) {
    for (u64 i = 0; i < LINUX_PROCESS_MAX; i++) {
        g_processes[i].used = 0;
        g_exit_record_used[i] = 0;
        g_exit_record_principal[i] = 0;
        g_deferred_start_used[i] = 0;
        g_deferred_start_token[i] = 0;
    }
    for (u64 i = 0; i < PIPE_MAX; i++) g_pipes[i].used = 0;
    for (u64 i = 0; i < FUTEX_WAITER_MAX; i++) g_futex_waiters[i].used = 0;
}
static struct linux_process_state *process_state_for(u64 principal) {
    if (principal == 0) return 0;
    for (u64 i = 0; i < LINUX_PROCESS_MAX; i++) {
        if (!g_processes[i].used || !g_processes[i].exec_pending) continue;
        if (g_processes[i].exec_pending_principal != principal) continue;
        for (u64 stale = 0; stale < LINUX_PROCESS_MAX; stale++) {
            if (stale == i) continue;
            if (g_processes[stale].used && !g_processes[stale].exec_pending && g_processes[stale].principal == principal) {
                g_processes[stale].used = 0;
            }
        }
        g_processes[i].principal = principal;
        g_processes[i].target_token = 0;
        g_processes[i].exec_pending = 0;
        g_processes[i].exec_pending_principal = 0;
        return &g_processes[i];
    }
    for (u64 i = 0; i < LINUX_PROCESS_MAX; i++) {
        if (!g_processes[i].used || g_processes[i].exec_pending || g_processes[i].principal != principal) continue;
        return &g_processes[i];
    }
    for (u64 i = 0; i < LINUX_PROCESS_MAX; i++) { if (g_processes[i].used) continue; init_process_state(&g_processes[i], principal); return &g_processes[i]; }
    return 0;
}

static struct linux_process_state *find_process_state_for_principal(u64 principal) {
    if (principal == 0) return 0;
    for (u64 i = 0; i < LINUX_PROCESS_MAX; i++) {
        if (!g_processes[i].used) continue;
        if (!g_processes[i].exec_pending && g_processes[i].principal == principal) return &g_processes[i];
        if (g_processes[i].exec_pending && g_processes[i].exec_pending_principal == principal) return &g_processes[i];
    }
    return 0;
}

static u64 delegate_target_token_slot(u64 token) {
    if ((token & DELEGATE_TARGET_TOKEN_TAG_MASK) != DELEGATE_TARGET_TOKEN_TAG) return 0;
    const u64 slot = token & DELEGATE_TARGET_SLOT_MASK;
    const u64 generation = (token >> DELEGATE_TARGET_GENERATION_SHIFT) & DELEGATE_TARGET_GENERATION_MASK;
    return slot != 0 && generation != 0 ? slot : 0;
}

static int delegate_target_token_matches_slot(u64 token, u64 slot) {
    return slot != 0 && delegate_target_token_slot(token) == slot;
}

static u64 target_token_for_principal(u64 principal) {
    const u64 token_slot = delegate_target_token_slot(principal);
    if (token_slot != 0) return principal;
    struct linux_process_state *proc = find_process_state_for_principal(principal);
    if (proc && delegate_target_token_matches_slot(proc->target_token, principal)) return proc->target_token;
    return 0;
}

static u64 terminate_process_for_sigpipe(u64 principal) {
    struct linux_process_state *proc = find_process_state_for_principal(principal);
    if (!proc) return 0;
    const u64 pid = proc->pid;
    proc->exit_status = SIGPIPE;
    record_process_exit_for_principal(pid, principal, proc->exit_status);
    close_all_process_fds(proc);
    proc->used = 0;
    return pid;
}
static int fd_entry_is_pipe(const struct fd_entry *entry) { return entry != 0 && (entry->kind == FD_PIPE_READ || entry->kind == FD_PIPE_WRITE); }

static void refresh_pipe_refs_from_process_fds(void) {
    for (u64 pipe_id = 0; pipe_id < PIPE_MAX; pipe_id++) {
        if (!g_pipes[pipe_id].used) continue;
        g_pipes[pipe_id].read_refs = 0;
        g_pipes[pipe_id].write_refs = 0;
    }
    for (u64 proc_index = 0; proc_index < LINUX_PROCESS_MAX; proc_index++) {
        struct linux_process_state *proc = &g_processes[proc_index];
        if (!proc->used) continue;
        for (u64 fd = 0; fd < 32; fd++) {
            const struct fd_entry *entry = &proc->fds[fd];
            if (!fd_entry_is_pipe(entry) || entry->pipe_id >= PIPE_MAX || !g_pipes[entry->pipe_id].used) continue;
            if (entry->kind == FD_PIPE_READ) g_pipes[entry->pipe_id].read_refs++;
            if (entry->kind == FD_PIPE_WRITE) g_pipes[entry->pipe_id].write_refs++;
        }
    }
    for (u64 pipe_id = 0; pipe_id < PIPE_MAX; pipe_id++) {
        struct pipe_entry *pipe = &g_pipes[pipe_id];
        if (!pipe->used) continue;
        if (pipe->read_refs == 0 && pipe->write_refs == 0 && !pipe->pending_read && !pipe->pending_write) pipe->used = 0;
    }
}

static int alloc_pipe_slot(void) {
    refresh_pipe_refs_from_process_fds();
    for (u64 i = 0; i < PIPE_MAX; i++) if (!g_pipes[i].used) return (int)i;
    return -1;
}
static int fd_is_pipe(u64 fd) { return g_proc != 0 && fd < 32 && fd_entry_is_pipe(&g_fds[fd]); }

static void pipe_ref_fd(const struct fd_entry *fd) {
    if (fd->pipe_id >= PIPE_MAX || !g_pipes[fd->pipe_id].used) return;
    if (fd->kind == FD_PIPE_READ) g_pipes[fd->pipe_id].read_refs++;
    if (fd->kind == FD_PIPE_WRITE) g_pipes[fd->pipe_id].write_refs++;
}

static int pipe_has_live_writer(u8 pipe_id);
static int pipe_has_live_reader(u8 pipe_id);
static void try_satisfy_pending_pipe_read(u8 pipe_id);
static void try_satisfy_pending_pipe_write(u8 pipe_id);
static void defer_pipe_wake(u8 pipe_id);

static void pipe_debug_event(const char *event, u8 pipe_id, u64 principal) {
    (void)event;
    (void)pipe_id;
    (void)principal;
}

static void remove_pipe_waiters_for_principal(u64 principal) {
    if (principal == 0) return;
    for (u8 pipe_id = 0; pipe_id < PIPE_MAX; pipe_id++) {
        struct pipe_entry *pipe = &g_pipes[pipe_id];
        if (!pipe->used) continue;
        int changed = 0;
        if (pipe->pending_read && pipe->pending_principal == principal) {
            pipe->pending_read = 0;
            pipe->pending_principal = 0;
            pipe->pending_dst = 0;
            pipe->pending_len = 0;
            changed = 1;
        }
        if (pipe->pending_write && pipe->pending_write_principal == principal) {
            pipe->pending_write = 0;
            pipe->pending_write_atomic = 0;
            pipe->pending_write_principal = 0;
            pipe->pending_write_token = 0;
            pipe->pending_write_len = 0;
            changed = 1;
        }
        if (!changed) continue;
        if (!pipe_has_live_reader(pipe_id) && !pipe_has_live_writer(pipe_id) && !pipe->pending_read && !pipe->pending_write) {
            pipe->used = 0;
        } else {
            defer_pipe_wake(pipe_id);
        }
    }
}

static void defer_pipe_wake(u8 pipe_id) {
    if (pipe_id < PIPE_MAX) g_deferred_pipe_wake_mask |= (u32)(1u << pipe_id);
    prime_reply_return_signal();
}

static void flush_deferred_pipe_wakes(void) {
    const u32 mask = g_deferred_pipe_wake_mask;
    g_deferred_pipe_wake_mask = 0;
    for (u8 pipe_id = 0; pipe_id < PIPE_MAX; pipe_id++) {
        if ((mask & (u32)(1u << pipe_id)) == 0) continue;
        try_satisfy_pending_pipe_write(pipe_id);
        try_satisfy_pending_pipe_read(pipe_id);
    }
}

static void close_pipe_entry(struct fd_entry *entry) {
    if (!fd_entry_is_pipe(entry)) return;
    const u8 pipe_id = entry->pipe_id;
    if (pipe_id >= PIPE_MAX || !g_pipes[pipe_id].used) return;
    pipe_debug_event(entry->kind == FD_PIPE_READ ? "close_r" : "close_w", pipe_id, g_proc ? g_proc->principal : 0);
    if (entry->kind == FD_PIPE_READ && g_pipes[pipe_id].read_refs != 0) g_pipes[pipe_id].read_refs--;
    if (entry->kind == FD_PIPE_WRITE && g_pipes[pipe_id].write_refs != 0) g_pipes[pipe_id].write_refs--;
    defer_pipe_wake(pipe_id);
    if (g_pipes[pipe_id].read_refs == 0 && g_pipes[pipe_id].write_refs == 0 && !g_pipes[pipe_id].pending_read && !g_pipes[pipe_id].pending_write) g_pipes[pipe_id].used = 0;
}

static void close_pipe_fd(u64 fd) {
    if (!fd_is_pipe(fd)) return;
    close_pipe_entry(&g_fds[fd]);
}

static void try_satisfy_pending_pipe_read(u8 pipe_id) {
    if (pipe_id >= PIPE_MAX || !g_pipes[pipe_id].used) return;
    struct pipe_entry *pipe = &g_pipes[pipe_id];
    if (!pipe->pending_read) return;
    pipe_debug_event("satisfy_read_begin", pipe_id, pipe->pending_principal);
    if (pipe->len == 0 && pipe_has_live_writer(pipe_id)) return;
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
    if (!pipe_has_live_reader(pipe_id) && !pipe_has_live_writer(pipe_id) && !pipe->pending_write) pipe->used = 0;
    const u64 reply_status = reply_trap_target(principal, result, 0);
    (void)reply_status;
    pipe_debug_event("satisfy_read_done", pipe_id, principal);
}

static void try_satisfy_pending_pipe_write(u8 pipe_id) {
    if (pipe_id >= PIPE_MAX || !g_pipes[pipe_id].used) return;
    struct pipe_entry *pipe = &g_pipes[pipe_id];
    if (!pipe->pending_write) return;
    const u64 principal = pipe->pending_write_principal;
    const u64 token = pipe->pending_write_token;
    pipe_debug_event("satisfy_write_begin", pipe_id, principal);
    u64 result = 0;
    u64 exited_pid = 0;
    if (!pipe_has_live_reader(pipe_id)) {
        exited_pid = terminate_process_for_sigpipe(principal);
        result = 0;
    } else {
        const u64 space = PIPE_BUFFER_BYTES - pipe->len;
        if (space == 0 || (pipe->pending_write_atomic && space < pipe->pending_write_len)) return;
        const u64 n = min_u64(pipe->pending_write_len, space);
        u64 done = 0;
        while (done < n) {
            const u64 tail = (pipe->head + pipe->len) % PIPE_BUFFER_BYTES;
            const u64 contiguous = min_u64(n - done, PIPE_BUFFER_BYTES - tail);
            for (u64 i = 0; i < contiguous; i++) pipe->bytes[tail + i] = pipe->pending_write_bytes[done + i];
            pipe->len += contiguous;
            done += contiguous;
        }
        result = n;
    }
    pipe->pending_write = 0;
    pipe->pending_write_atomic = 0;
    pipe->pending_write_principal = 0;
    pipe->pending_write_token = 0;
    pipe->pending_write_len = 0;
    if (!pipe_has_live_reader(pipe_id) && !pipe_has_live_writer(pipe_id) && !pipe->pending_read) pipe->used = 0;
    if (!pipe_has_live_reader(pipe_id)) {
        const u64 reply_status = reply_trap_target(token != 0 ? token : principal, 0, TRAP_RESPONSE_FLAG_EXIT);
        (void)reply_status;
        if (exited_pid != 0) (void)satisfy_pending_waiters_for_child(exited_pid);
    } else {
        const u64 reply_status = reply_trap_target(principal, result, 0);
        (void)reply_status;
        try_satisfy_pending_pipe_read(pipe_id);
    }
    pipe_debug_event("satisfy_write_done", pipe_id, principal);
}

static u64 pipe_read_to_target(u64 fd, u64 dst, u64 len, int *fault) {
    *fault = 0;
    if (fd >= 32 || g_fds[fd].kind != FD_PIPE_READ) return errno_badf();
    const u8 pipe_id = g_fds[fd].pipe_id;
    if (pipe_id >= PIPE_MAX || !g_pipes[pipe_id].used) return errno_badf();
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
    defer_pipe_wake(pipe_id);
    return n;
}

static u64 pipe_write_from_target(u64 fd, u64 src, u64 len, int *fault) {
    *fault = 0;
    if (fd >= 32 || g_fds[fd].kind != FD_PIPE_WRITE) return errno_badf();
    const u8 pipe_id = g_fds[fd].pipe_id;
    if (pipe_id >= PIPE_MAX || !g_pipes[pipe_id].used) return errno_badf();
    struct pipe_entry *pipe = &g_pipes[pipe_id];
    if (!pipe_has_live_reader(pipe_id)) return errno_pipe();
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
static int fd_is_tty_like(u64 fd) { return fd_valid(fd) && g_fds[fd].kind == FD_TTY; }

static void sync_fd_to_thread_group(u64 fd) {
    if (!g_proc || g_proc->pid == 0 || fd >= 32) return;
    for (u64 i = 0; i < LINUX_PROCESS_MAX; i++) {
        struct linux_process_state *peer = &g_processes[i];
        if (!peer->used || peer == g_proc || peer->exec_pending || peer->principal == 0 || peer->pid != g_proc->pid) continue;
        copy_fd_entry(&peer->fds[fd], &g_fds[fd]);
    }
}

static int pipe_has_live_writer(u8 pipe_id) {
    if (pipe_id >= PIPE_MAX) return 0;
    for (u64 p = 0; p < LINUX_PROCESS_MAX; p++) {
        struct linux_process_state *proc = &g_processes[p];
        if (!proc->used) continue;
        for (u64 fd = 0; fd < 32; fd++) {
            if (proc->fds[fd].kind == FD_PIPE_WRITE && proc->fds[fd].pipe_id == pipe_id) return 1;
        }
    }
    return 0;
}

static int pipe_has_live_reader(u8 pipe_id) {
    if (pipe_id >= PIPE_MAX) return 0;
    for (u64 p = 0; p < LINUX_PROCESS_MAX; p++) {
        struct linux_process_state *proc = &g_processes[p];
        if (!proc->used) continue;
        for (u64 fd = 0; fd < 32; fd++) {
            if (proc->fds[fd].kind == FD_PIPE_READ && proc->fds[fd].pipe_id == pipe_id) return 1;
        }
    }
    return 0;
}
static struct linux_process_state *process_state_for_pid(u64 pid) {
    if (pid == 0) return 0;
    for (u64 i = 0; i < LINUX_PROCESS_MAX; i++) if (g_processes[i].used && g_processes[i].pid == pid) return &g_processes[i];
    return 0;
}

static struct linux_process_state *process_state_for_tid(u64 tid) {
    if (tid == 0) return 0;
    for (u64 i = 0; i < LINUX_PROCESS_MAX; i++) if (g_processes[i].used && g_processes[i].tid == tid) return &g_processes[i];
    return 0;
}

static void record_process_exit_for_principal(u64 pid, u64 principal, u32 status) {
    if (pid == 0) return;
    if (principal == 0) {
        struct linux_process_state *proc = process_state_for_pid(pid);
        if (proc) principal = proc->principal;
    }
    for (u64 i = 0; i < LINUX_PROCESS_MAX; i++) {
        if (!g_exit_record_used[i] || g_exit_record_pid[i] != pid) continue;
        if (principal != 0) g_exit_record_principal[i] = principal;
        g_exit_record_status[i] = status;
        return;
    }
    for (u64 i = 0; i < LINUX_PROCESS_MAX; i++) {
        if (g_exit_record_used[i]) continue;
        g_exit_record_used[i] = 1;
        g_exit_record_pid[i] = pid;
        g_exit_record_principal[i] = principal;
        g_exit_record_status[i] = status;
        return;
    }
}

static int take_process_exit_record_with_principal(u64 pid, u32 *status_out, u64 *principal_out) {
    if (pid == 0) return 0;
    for (u64 i = 0; i < LINUX_PROCESS_MAX; i++) {
        if (!g_exit_record_used[i] || g_exit_record_pid[i] != pid) continue;
        *status_out = g_exit_record_status[i];
        if (principal_out) *principal_out = g_exit_record_principal[i];
        g_exit_record_used[i] = 0;
        g_exit_record_principal[i] = 0;
        return 1;
    }
    return 0;
}

static int take_process_exit_record(u64 pid, u32 *status_out) {
    return take_process_exit_record_with_principal(pid, status_out, 0);
}

static void discard_process_exit_records(u64 pid) {
    u32 ignored = 0;
    while (take_process_exit_record(pid, &ignored)) {}
}

static void remove_child_slot(struct linux_process_state *proc, u64 child_slot) {
    if (!proc) return;
    for (u64 i = 0; i < LINUX_CHILD_MAX; i++) {
        if (proc->child_used[i] && proc->child_slot[i] == child_slot) proc->child_used[i] = 0;
    }
}
