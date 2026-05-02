static void close_all_process_fds(struct linux_process_state *proc) {
    struct linux_process_state *saved = g_proc;
    g_proc = proc;
    for (u64 fd = 0; fd < 32; fd++) {
        if (g_fds[fd].kind == FD_UNUSED) continue;
        if (fd_is_pipe(fd)) close_pipe_fd(fd);
        if (fd > 2 || fd_is_pipe(fd)) g_fds[fd].kind = FD_UNUSED;
    }
    g_proc = saved;
}

static int has_live_linux_process_state(void) {
    for (u64 i = 0; i < LINUX_PROCESS_MAX; i++) {
        if (!g_processes[i].used) continue;
        const u64 st = syscall1(SYSCALL_GET_PROCESS_STATUS, g_processes[i].principal);
        if ((st & 0xff) == 1) return 1;
    }
    return 0;
}

static int has_open_pipe_state(void) {
    for (u64 i = 0; i < PIPE_MAX; i++) {
        if (!g_pipes[i].used) continue;
        if (g_pipes[i].read_refs != 0 || g_pipes[i].write_refs != 0 || g_pipes[i].pending_read) return 1;
    }
    return 0;
}

static int has_known_child_slots(void) {
    for (u64 i = 0; i < LINUX_PROCESS_MAX; i++) {
        if (!g_processes[i].used) continue;
        for (u64 child = 0; child < LINUX_CHILD_MAX; child++) {
            if (g_processes[i].child_used[child]) return 1;
        }
    }
    return 0;
}

static int wait_pid_matches_child(i64 pid, u64 child_slot) {
    if (pid > 0) return (u64)pid == child_slot;
    return 1;
}

static int write_wait_status_to_current(u64 status_va) {
    if (status_va == 0) return 1;
    const u32 code = 0;
    return copy_to_target(status_va, &code, sizeof(code)) == sizeof(code);
}

static int write_wait_status_to_trap_target(u64 principal, u64 status_va) {
    if (status_va == 0) return 1;
    const u32 code = 0;
    return copy_to_trap_target(principal, status_va, &code, sizeof(code)) == sizeof(code);
}

static int reap_exited_child_for_current(struct linux_process_state *proc, i64 pid, u64 status_va, u64 *child_out, int *fault) {
    *fault = 0;
    for (u64 i = 0; i < LINUX_CHILD_MAX; i++) {
        if (!proc->child_used[i]) continue;
        const u64 child = proc->child_slot[i];
        if (!wait_pid_matches_child(pid, child)) continue;
        const u64 st = syscall1(SYSCALL_GET_PROCESS_STATUS, child);
        if ((st & 0xff) == 1) continue;
        if (!write_wait_status_to_current(status_va)) {
            *fault = 1;
            return 0;
        }
        proc->child_used[i] = 0;
        *child_out = child;
        return 1;
    }
    return 0;
}

static int satisfy_pending_waiters_for_child(u64 child_slot) {
    int satisfied = 0;
    for (u64 p = 0; p < LINUX_PROCESS_MAX; p++) {
        struct linux_process_state *proc = &g_processes[p];
        if (!proc->used || !proc->wait_pending) continue;
        if (!wait_pid_matches_child(proc->wait_pid, child_slot)) continue;
        for (u64 i = 0; i < LINUX_CHILD_MAX; i++) {
            if (!proc->child_used[i] || proc->child_slot[i] != child_slot) continue;
            u64 result = child_slot;
            if (!write_wait_status_to_trap_target(proc->principal, proc->wait_status_va)) result = errno_fault();
            proc->child_used[i] = 0;
            proc->wait_pending = 0;
            proc->wait_pid = 0;
            proc->wait_status_va = 0;
            (void)reply_trap_target(proc->principal, result, 0);
            satisfied = 1;
            break;
        }
    }
    return satisfied;
}

static int add_child_slot(struct linux_process_state *proc, u64 child_slot) {
    for (u64 i = 0; i < LINUX_CHILD_MAX; i++) {
        if (proc->child_used[i]) continue;
        proc->child_used[i] = 1;
        proc->child_slot[i] = child_slot;
        return 1;
    }
    return 0;
}

static void copy_process_state_for_fork(struct linux_process_state *child, const struct linux_process_state *parent, u64 child_principal) {
    child->used = 1;
    child->principal = child_principal;
    child->mmap_next_va = parent->mmap_next_va;
    child->brk_next_va = parent->brk_next_va;
    for (u64 i = 0; i < VM_REGION_MAX; i++) child->regions[i] = parent->regions[i];
    child->cwd_len = parent->cwd_len;
    for (u16 i = 0; i <= parent->cwd_len && i <= FS_MAX_PATH_BYTES; i++) child->cwd[i] = parent->cwd[i];
    for (u64 i = 0; i < LINUX_CHILD_MAX; i++) child->child_used[i] = 0;
    child->wait_pending = 0;
    child->wait_pid = 0;
    child->wait_status_va = 0;
    for (u64 i = 0; i < 65; i++) {
        child->sig_handler[i] = parent->sig_handler[i];
        child->sig_flags[i] = parent->sig_flags[i];
    }
    for (u64 fd = 0; fd < 32; fd++) {
        child->fds[fd].kind = parent->fds[fd].kind;
        child->fds[fd].token = parent->fds[fd].token;
        child->fds[fd].offset = parent->fds[fd].offset;
        child->fds[fd].size = parent->fds[fd].size;
        child->fds[fd].mode_bits = parent->fds[fd].mode_bits;
        child->fds[fd].object_kind = parent->fds[fd].object_kind;
        child->fds[fd].pipe_id = parent->fds[fd].pipe_id;
        child->fds[fd].path_len = parent->fds[fd].path_len;
        for (u16 i = 0; i <= parent->fds[fd].path_len && i <= FS_MAX_PATH_BYTES; i++) child->fds[fd].path[i] = parent->fds[fd].path[i];
        pipe_ref_fd(&child->fds[fd]);
    }
}

static struct ipc_message handle_fork_like(const struct trap_request *req, int clone_form) {
    if (clone_form) {
        const u64 flags = req->args[0];
        const u64 child_stack = req->args[1];
        if (child_stack != 0) return reply(errno_inval(), 0);
        if ((flags & ~(u64)0xff) != 0) return reply(errno_inval(), 0);
        if ((flags & 0xff) != SIGCHLD && (flags & 0xff) != 0) return reply(errno_inval(), 0);
    }
    const u64 spawned = syscall0(SYSCALL_FORK_ABI_TRAP_REPLY_TARGET);
    const u64 child_slot = decode_spawned_process_slot(spawned);
    if (child_slot == 0) return reply(errno_busy(), 0);
    struct linux_process_state *child = process_state_for(child_slot);
    if (!child) return reply(errno_busy(), 0);
    u64 child_request_va = 0;
    if (!ensure_child_trap_request_page(child_slot, &child_request_va)) return reply(errno_busy(), 0);
    copy_process_state_for_fork(child, g_proc, child_slot);
    (void)add_child_slot(g_proc, child_slot);
    if (start_trap_target(child_slot) != SYSCALL_OK) return reply(errno_busy(), 0);
    return reply(child_slot, 0);
}

static struct ipc_message handle_wait4(const struct trap_request *req) {
    const i64 pid = (i64)req->args[0];
    const u64 status_va = req->args[1];
    const u64 options = req->args[2];
    if ((options & ~(u64)WNOHANG) != 0) return reply(errno_inval(), 0);
    u64 child = 0;
    int fault = 0;
    if (reap_exited_child_for_current(g_proc, pid, status_va, &child, &fault)) return reply(child, 0);
    if (fault) return reply(errno_fault(), 0);
    if ((options & WNOHANG) != 0) return reply(0, 0);
    int has_matching_child = 0;
    for (u64 i = 0; i < LINUX_CHILD_MAX; i++) {
        if (g_proc->child_used[i] && wait_pid_matches_child(pid, g_proc->child_slot[i])) has_matching_child = 1;
    }
    if (!has_matching_child) return reply(errno_child(), 0);
    if (g_proc->wait_pending) return reply(errno_again(), 0);
    g_proc->wait_pending = 1;
    g_proc->wait_pid = pid;
    g_proc->wait_status_va = status_va;
    return wait_ipc();
}
