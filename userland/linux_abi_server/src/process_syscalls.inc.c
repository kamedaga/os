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
        if (g_processes[i].exec_pending || g_processes[i].principal == 0) return 1;
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

static int write_wait_status_to_current(u64 status_va, u32 exit_code) {
    if (status_va == 0) return 1;
    const u32 code = (exit_code & 0xffu) << 8;
    return copy_to_target(status_va, &code, sizeof(code)) == sizeof(code);
}

static int write_wait_status_to_trap_target(u64 principal, u64 status_va, u32 exit_code) {
    if (status_va == 0) return 1;
    const u32 code = (exit_code & 0xffu) << 8;
    return copy_to_trap_target(principal, status_va, &code, sizeof(code)) == sizeof(code);
}

static int reap_exited_child_for_current(struct linux_process_state *proc, i64 pid, u64 status_va, u64 *child_out, int *fault) {
    *fault = 0;
    for (u64 i = 0; i < LINUX_CHILD_MAX; i++) {
        if (!proc->child_used[i]) continue;
        const u64 child = proc->child_slot[i];
        if (!wait_pid_matches_child(pid, child)) continue;
        u32 recorded_status = 0;
        if (take_process_exit_record(child, &recorded_status)) {
            if (!write_wait_status_to_current(status_va, recorded_status)) {
                *fault = 1;
                return 0;
            }
            struct linux_process_state *recorded_proc = process_state_for_pid(child);
            if (recorded_proc) recorded_proc->used = 0;
            proc->child_used[i] = 0;
            *child_out = child;
            return 1;
        }
        struct linux_process_state *child_proc = process_state_for_pid(child);
        if (child_proc && child_proc->exec_pending) continue;
        const u64 child_principal = child_proc ? child_proc->principal : child;
        const u64 st = syscall1(SYSCALL_GET_PROCESS_STATUS, child_principal);
        if ((st & 0xff) == 1) continue;
        const u32 exit_code = child_proc ? child_proc->exit_status : 0;
        if (!write_wait_status_to_current(status_va, exit_code)) {
            *fault = 1;
            return 0;
        }
        proc->child_used[i] = 0;
        if (child_proc) child_proc->used = 0;
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
            struct linux_process_state *child_proc = process_state_for_pid(child_slot);
            u32 exit_code = child_proc ? child_proc->exit_status : 0;
            (void)take_process_exit_record(child_slot, &exit_code);
            if (!write_wait_status_to_trap_target(proc->principal, proc->wait_status_va, exit_code)) result = errno_fault();
            proc->child_used[i] = 0;
            if (child_proc) child_proc->used = 0;
            proc->wait_pending = 0;
            proc->wait_pid = 0;
            proc->wait_status_va = 0;
            const u64 reply_status = reply_trap_target(proc->principal, result, 0);
            if (reply_status != SYSCALL_OK) {
                user_log("LinuxAbiServer: wait reply failed=");
                user_log_hex_value(reply_status);
            }
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

static int defer_trap_target_start(u64 child_slot) {
    for (u64 i = 0; i < LINUX_PROCESS_MAX; i++) {
        if (g_deferred_start_used[i]) continue;
        g_deferred_start_used[i] = 1;
        g_deferred_start_principal[i] = child_slot;
        return 1;
    }
    return 0;
}

static void start_deferred_trap_targets(void) {
    for (u64 i = 0; i < LINUX_PROCESS_MAX; i++) {
        if (!g_deferred_start_used[i]) continue;
        const u64 child_slot = g_deferred_start_principal[i];
        g_deferred_start_used[i] = 0;
        const u64 status = start_trap_target(child_slot);
        if (status != SYSCALL_OK) {
            user_log("LinuxAbiServer: deferred start failed=");
            user_log_hex_value(status);
        }
    }
}

static void copy_process_state_for_fork(struct linux_process_state *child, const struct linux_process_state *parent, u64 child_principal) {
    child->used = 1;
    child->exec_pending = 0;
    child->exit_status = 0;
    child->pid = child_principal;
    child->tid = child_principal;
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
    child->clear_child_tid = 0;
    for (u64 i = 0; i < 65; i++) {
        child->sig_handler[i] = parent->sig_handler[i];
        child->sig_flags[i] = parent->sig_flags[i];
    }
    for (u64 fd = 0; fd < 32; fd++) {
        copy_fd_entry(&child->fds[fd], &parent->fds[fd]);
        pipe_ref_fd(&child->fds[fd]);
    }
}

static void copy_process_state_for_clone_thread(struct linux_process_state *child, const struct linux_process_state *parent, u64 child_principal, u64 clear_child_tid) {
    copy_process_state_for_fork(child, parent, child_principal);
    child->pid = parent->pid;
    child->tid = child_principal;
    child->clear_child_tid = clear_child_tid;
}

static int supported_clone_thread_flags(u64 flags) {
    const u64 signal = flags & 0xffu;
    const u64 required = (u64)CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD | CLONE_SETTLS;
    const u64 supported = required | CLONE_SYSVSEM | CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID | CLONE_CHILD_SETTID | CLONE_DETACHED;
    if (signal != 0) return 0;
    if ((flags & required) != required) return 0;
    if ((flags & ~(supported | (u64)0xff)) != 0) return 0;
    return 1;
}

static struct ipc_message handle_clone_thread(const struct trap_request *req) {
    const u64 flags = req->args[0];
    const u64 child_stack = req->args[1];
    const u64 parent_tidptr = req->args[2];
    const u64 child_tidptr = req->args[3];
    const u64 tls = req->args[4];
    if (child_stack == 0 || tls == 0 || !supported_clone_thread_flags(flags)) return reply(errno_inval(), 0);

    const u64 spawned = clone_reply_target(child_stack, tls);
    const u64 child_slot = decode_spawned_process_slot(spawned);
    if (child_slot == 0) {
        user_log("LinuxAbiServer: clone thread spawn failed\n");
        user_log_hex_value(spawned);
        return reply(errno_busy(), 0);
    }
    struct linux_process_state *child = process_state_for(child_slot);
    if (!child) {
        user_log("LinuxAbiServer: clone thread state failed\n");
        user_log_hex_value(child_slot);
        return reply(errno_busy(), 0);
    }
    u64 child_request_va = 0;
    if (!ensure_child_trap_request_page(child_slot, &child_request_va)) {
        user_log("LinuxAbiServer: clone thread request page failed\n");
        user_log_hex_value(child_slot);
        return reply(errno_busy(), 0);
    }

    copy_process_state_for_clone_thread(child, g_proc, child_slot, (flags & CLONE_CHILD_CLEARTID) != 0 ? child_tidptr : 0);
    const u32 child_tid32 = (u32)child_slot;
    if ((flags & CLONE_PARENT_SETTID) != 0 && parent_tidptr != 0) {
        if (copy_to_target(parent_tidptr, &child_tid32, sizeof(child_tid32)) != sizeof(child_tid32)) {
            user_log("LinuxAbiServer: clone parent_tid write failed\n");
            user_log_hex_value(parent_tidptr);
            return reply(errno_fault(), 0);
        }
    }
    if ((flags & CLONE_CHILD_SETTID) != 0 && child_tidptr != 0) {
        if (copy_to_trap_target(child_slot, child_tidptr, &child_tid32, sizeof(child_tid32)) != sizeof(child_tid32)) {
            user_log("LinuxAbiServer: clone child_tid write failed\n");
            user_log_hex_value(child_tidptr);
            return reply(errno_fault(), 0);
        }
    }
    if (!defer_trap_target_start(child_slot)) {
        user_log("LinuxAbiServer: clone thread defer start failed\n");
        user_log_hex_value(child_slot);
        return reply(errno_busy(), 0);
    }
    prime_reply_return_signal();
    return reply(child_slot, 0);
}

static struct ipc_message handle_fork_like(const struct trap_request *req, int clone_form) {
    if (clone_form) {
        const u64 flags = req->args[0];
        const u64 child_stack = req->args[1];
        if ((flags & CLONE_THREAD) != 0) return handle_clone_thread(req);
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
    if (!defer_trap_target_start(child_slot)) return reply(errno_busy(), 0);
    return reply(child_slot, 0);
}

static struct ipc_message handle_set_tid_address(const struct trap_request *req) {
    if (g_proc) g_proc->clear_child_tid = req->args[0];
    return reply(g_proc && g_proc->tid != 0 ? g_proc->tid : 1, 0);
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
    detach_reply_token();
    return wait_ipc();
}
