static void close_all_process_fds(struct linux_process_state *proc) {
    if (!proc) return;
    for (u64 fd = 0; fd < 32; fd++) {
        struct fd_entry *entry = &proc->fds[fd];
        if (entry->kind == FD_UNUSED) continue;
        const int is_pipe = fd_entry_is_pipe(entry);
        if (is_pipe) close_pipe_entry(entry);
        if (entry->kind == FD_SOCKET) net_close_udp(entry->token);
        if (fd > 2 || is_pipe) {
            entry->kind = FD_UNUSED;
            entry->fd_flags = 0;
            entry->desc_flags = 0;
        }
    }
    remove_pipe_waiters_for_principal(proc->principal);
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
        if (g_pipes[i].read_refs != 0 || g_pipes[i].write_refs != 0 || g_pipes[i].pending_read || g_pipes[i].pending_write) return 1;
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

static int write_wait_status_to_current(u64 status_va, u32 wait_status) {
    if (status_va == 0) return 1;
    return copy_to_target(status_va, &wait_status, sizeof(wait_status)) == sizeof(wait_status);
}

static int write_wait_status_to_trap_target(u64 principal, u64 status_va, u32 wait_status) {
    if (status_va == 0) return 1;
    return copy_to_trap_target(principal, status_va, &wait_status, sizeof(wait_status)) == sizeof(wait_status);
}

static void wait_child_principal_settled(u64 child_principal) {
    if (child_principal == 0) return;
    for (u64 i = 0; i < 64; i++) {
        const u64 st = syscall1(SYSCALL_GET_PROCESS_STATUS, child_principal);
        if ((st & 0xffu) != 1) break;
        __asm__ volatile("pause" ::: "memory");
    }
}

static void wait_child_slot_settled(u64 child_slot) {
    struct linux_process_state *child_proc = process_state_for_pid(child_slot);
    if (!child_proc) return;
    wait_child_principal_settled(child_proc->principal);
}

static void wait_debug_event(const char *event, u64 principal, u64 pid, u64 child);

static int reap_exited_child_for_current(struct linux_process_state *proc, i64 pid, u64 status_va, u64 *child_out, int *fault) {
    *fault = 0;
    for (u64 i = 0; i < LINUX_CHILD_MAX; i++) {
        if (!proc->child_used[i]) continue;
        const u64 child = proc->child_slot[i];
        if (!wait_pid_matches_child(pid, child)) continue;
        u32 recorded_status = 0;
        u64 recorded_principal = 0;
        if (take_process_exit_record_with_principal(child, &recorded_status, &recorded_principal)) {
            wait_debug_event("reap_record", proc->principal, proc->pid, child);
            wait_child_principal_settled(recorded_principal);
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
        wait_debug_event("reap_status", proc->principal, proc->pid, child);
        wait_child_slot_settled(child);
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

static void log_wait_state_for_miss(u64 child_slot);

static void wait_debug_event(const char *event, u64 principal, u64 pid, u64 child) {
    (void)event;
    (void)principal;
    (void)pid;
    (void)child;
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
            u64 child_principal = child_proc ? child_proc->principal : 0;
            u64 recorded_principal = 0;
            if (take_process_exit_record_with_principal(child_slot, &exit_code, &recorded_principal) && recorded_principal != 0) child_principal = recorded_principal;
            wait_child_principal_settled(child_principal);
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
            wait_debug_event("satisfied", proc->principal, proc->pid, child_slot);
            satisfied = 1;
            break;
        }
    }
    if (!satisfied) log_wait_state_for_miss(child_slot);
    return satisfied;
}

static int child_has_exit_record(u64 child_slot) {
    for (u64 i = 0; i < LINUX_PROCESS_MAX; i++) {
        if (g_exit_record_used[i] && g_exit_record_pid[i] == child_slot) return 1;
    }
    return 0;
}

static int child_slot_reclaimable(u64 child_slot) {
    if (child_has_exit_record(child_slot)) return 1;
    struct linux_process_state *child_proc = process_state_for_pid(child_slot);
    if (!child_proc) return 1;
    if (child_proc->exec_pending || child_proc->principal == 0) return 0;
    const u64 st = syscall1(SYSCALL_GET_PROCESS_STATUS, child_proc->principal);
    return (st & 0xffu) != 1;
}

static void compact_exited_child_slots(struct linux_process_state *proc) {
    if (!proc) return;
    for (u64 i = 0; i < LINUX_CHILD_MAX; i++) {
        if (!proc->child_used[i]) continue;
        const u64 child = proc->child_slot[i];
        if (!child_slot_reclaimable(child)) continue;
        discard_process_exit_records(child);
        struct linux_process_state *child_proc = process_state_for_pid(child);
        if (child_proc && !child_proc->exec_pending) child_proc->used = 0;
        proc->child_used[i] = 0;
    }
}

static int add_child_slot(struct linux_process_state *proc, u64 child_slot) {
    for (u64 i = 0; i < LINUX_CHILD_MAX; i++) {
        if (proc->child_used[i]) continue;
        proc->child_used[i] = 1;
        proc->child_slot[i] = child_slot;
        return 1;
    }
    compact_exited_child_slots(proc);
    for (u64 i = 0; i < LINUX_CHILD_MAX; i++) {
        if (proc->child_used[i]) continue;
        proc->child_used[i] = 1;
        proc->child_slot[i] = child_slot;
        return 1;
    }
    return 0;
}

static void log_wait_state_for_miss(u64 child_slot) {
    (void)child_slot;
}

static int defer_trap_target_start(u64 child_slot, u64 child_token) {
    for (u64 i = 0; i < LINUX_PROCESS_MAX; i++) {
        if (g_deferred_start_used[i]) continue;
        g_deferred_start_used[i] = 1;
        g_deferred_start_principal[i] = child_slot;
        g_deferred_start_token[i] = child_token;
        return 1;
    }
    return 0;
}

static void start_deferred_trap_targets(void) {
    for (u64 i = 0; i < LINUX_PROCESS_MAX; i++) {
        if (!g_deferred_start_used[i]) continue;
        const u64 child_slot = g_deferred_start_principal[i];
        const u64 child_token = g_deferred_start_token[i];
        g_deferred_start_used[i] = 0;
        g_deferred_start_token[i] = 0;
        const u64 status = start_trap_target(child_token != 0 ? child_token : child_slot);
        if (status != SYSCALL_OK) {
            user_log("LinuxAbiServer: deferred start failed=");
            user_log_hex_value(status);
        }
    }
}

static void copy_process_state_for_fork(struct linux_process_state *child, const struct linux_process_state *parent, u64 child_pid, u64 child_principal) {
    child->used = 1;
    child->exec_pending = 0;
    child->exec_pending_principal = 0;
    child->exit_status = 0;
    child->pid = child_pid;
    child->tid = child_pid;
    child->pgid = parent->pgid;
    child->principal = child_principal;
    child->target_token = 0;
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
    child->profile_enabled = parent->profile_enabled;
    child->sigaltstack_sp = parent->sigaltstack_sp;
    child->sigaltstack_size = parent->sigaltstack_size;
    child->sigaltstack_flags = parent->sigaltstack_flags;
    for (u64 i = 0; i < 65; i++) {
        child->sig_handler[i] = parent->sig_handler[i];
        child->sig_flags[i] = parent->sig_flags[i];
    }
    for (u64 fd = 0; fd < 32; fd++) {
        copy_fd_entry(&child->fds[fd], &parent->fds[fd]);
        pipe_ref_fd(&child->fds[fd]);
    }
}

static void copy_process_state_for_clone_thread(struct linux_process_state *child, const struct linux_process_state *parent, u64 child_tid, u64 child_principal, u64 clear_child_tid) {
    copy_process_state_for_fork(child, parent, child_tid, child_principal);
    child->pid = parent->pid;
    child->tid = child_tid;
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
    const u64 child_slot = delegate_target_token_slot(spawned);
    if (child_slot == 0) {
        user_log("LinuxAbiServer: clone thread spawn failed\n");
        user_log_hex_value(spawned);
        return reply(errno_busy(), 0);
    }
    const u64 child_tid = child_slot;
    struct linux_process_state *child = process_state_for(child_slot);
    if (!child) {
        user_log("LinuxAbiServer: clone thread state failed\n");
        user_log_hex_value(child_slot);
        exit_trap_target_no_wait(child_slot);
        return reply(errno_busy(), 0);
    }
    child->target_token = spawned;
    u64 child_request_va = 0;
    if (!ensure_child_trap_request_page(child_slot, &child_request_va)) {
        user_log("LinuxAbiServer: clone thread request page failed\n");
        user_log_hex_value(child_slot);
        return reply(errno_busy(), 0);
    }

    copy_process_state_for_clone_thread(child, g_proc, child_tid, child_slot, (flags & CLONE_CHILD_CLEARTID) != 0 ? child_tidptr : 0);
    child->target_token = spawned;
    const u32 child_tid32 = (u32)child_tid;
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
    if (!defer_trap_target_start(child_slot, spawned)) {
        user_log("LinuxAbiServer: clone thread defer start failed\n");
        user_log_hex_value(child_slot);
        return reply(errno_busy(), 0);
    }
    prime_reply_return_signal();
    return reply(child_tid, 0);
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
    const u64 child_slot = delegate_target_token_slot(spawned);
    if (child_slot == 0) {
        user_log("LinuxAbiServer: fork spawn failed=");
        user_log_hex_value(spawned);
        return reply(errno_busy(), 0);
    }
    const u64 child_pid = child_slot;
    discard_process_exit_records(child_pid);
    remove_child_slot(g_proc, child_pid);
    struct linux_process_state *child = process_state_for(child_slot);
    if (!child) {
        user_log("LinuxAbiServer: fork state failed child=");
        user_log_dec_value(child_slot);
        user_log("\n");
        exit_trap_target_no_wait(child_slot);
        return reply(errno_busy(), 0);
    }
    child->target_token = spawned;
    u64 child_request_va = 0;
    if (!ensure_child_trap_request_page(child_slot, &child_request_va)) {
        user_log("LinuxAbiServer: fork request page failed child=");
        user_log_dec_value(child_slot);
        user_log("\n");
        return reply(errno_busy(), 0);
    }
    copy_process_state_for_fork(child, g_proc, child_pid, child_slot);
    child->target_token = spawned;
    if (!add_child_slot(g_proc, child_pid)) {
        user_log("LinuxAbiServer: child table full child=");
        user_log_dec_value(child_pid);
        user_log("\n");
        exit_trap_target_no_wait(child_slot);
        child->used = 0;
        return reply(errno_busy(), 0);
    }
    if (!defer_trap_target_start(child_slot, spawned)) {
        user_log("LinuxAbiServer: fork defer failed child=");
        user_log_dec_value(child_pid);
        user_log("\n");
        remove_child_slot(g_proc, child_pid);
        exit_trap_target_no_wait(child_slot);
        child->used = 0;
        return reply(errno_busy(), 0);
    }
    return reply(child_pid, 0);
}

static struct ipc_message handle_set_tid_address(const struct trap_request *req) {
    if (g_proc) g_proc->clear_child_tid = req->args[0];
    return reply(g_proc && g_proc->tid != 0 ? g_proc->tid : 1, 0);
}

static struct ipc_message handle_getpgid(const struct trap_request *req) {
    const u64 requested = req->args[0];
    if (!g_proc) return reply(errno_child(), 0);
    if (requested == 0 || requested == g_proc->pid || requested == g_proc->tid) return reply(g_proc->pgid, 0);
    for (u64 i = 0; i < LINUX_PROCESS_MAX; i++) {
        const struct linux_process_state *proc = &g_processes[i];
        if (!proc->used) continue;
        if (proc->pid == requested || proc->tid == requested) return reply(proc->pgid, 0);
    }
    return reply(errno_noent(), 0);
}

static struct ipc_message handle_setpgid(const struct trap_request *req) {
    const u64 pid = req->args[0];
    const u64 pgid = req->args[1];
    if (!g_proc) return reply(errno_child(), 0);
    struct linux_process_state *target = 0;
    if (pid == 0 || pid == g_proc->pid || pid == g_proc->tid) target = g_proc;
    for (u64 i = 0; i < LINUX_PROCESS_MAX; i++) {
        struct linux_process_state *proc = &g_processes[i];
        if (!proc->used) continue;
        if (pid != 0 && (proc->pid == pid || proc->tid == pid)) {
            target = proc;
            break;
        }
    }
    if (!target) return reply(errno_noent(), 0);
    const u64 new_pgid = pgid == 0 ? target->pid : pgid;
    if (new_pgid == 0) return reply(errno_inval(), 0);
    const u64 target_pid = target->pid;
    for (u64 i = 0; i < LINUX_PROCESS_MAX; i++) {
        struct linux_process_state *proc = &g_processes[i];
        if (!proc->used || proc->pid != target_pid) continue;
        proc->pgid = new_pgid;
    }
    return reply(0, 0);
}

static void deliver_tty_signal(u64 signo) {
    if (signo == 0 || signo >= 65) return;
    int delivered = 0;
    for (u64 waiter_index = 0; waiter_index < LINUX_PROCESS_MAX; waiter_index++) {
        struct linux_process_state *waiter = &g_processes[waiter_index];
        if (!waiter->used || !waiter->wait_pending) continue;
        for (u64 child_index = 0; child_index < LINUX_CHILD_MAX; child_index++) {
            if (!waiter->child_used[child_index]) continue;
            const u64 child_pid = waiter->child_slot[child_index];
            if (!wait_pid_matches_child(waiter->wait_pid, child_pid)) continue;
            struct linux_process_state *proc = process_state_for_pid(child_pid);
            if (!proc || !proc->used || proc->exec_pending || proc->principal == 0) continue;
            proc->exit_status = (u32)(signo & 0x7fu);
            record_process_exit_for_principal(proc->pid, proc->principal, proc->exit_status);
            remove_futex_waiters_for_principal(proc->principal);
            if (proc->clear_child_tid != 0) {
                const u32 zero = 0;
                (void)copy_to_trap_target(proc->principal, proc->clear_child_tid, &zero, sizeof(zero));
            }
            (void)reply_trap_target(proc->principal, 0, TRAP_RESPONSE_FLAG_EXIT);
            close_all_process_fds(proc);
            proc->used = 0;
            delivered = 1;
        }
    }
    if (delivered) {
        for (u64 i = 0; i < LINUX_PROCESS_MAX; i++) {
            if (g_processes[i].used && g_processes[i].wait_pending) {
                for (u64 child_index = 0; child_index < LINUX_CHILD_MAX; child_index++) {
                    if (g_processes[i].child_used[child_index]) {
                        (void)satisfy_pending_waiters_for_child(g_processes[i].child_slot[child_index]);
                    }
                }
            }
        }
        prime_reply_return_signal();
    }
}

static struct ipc_message handle_kill(const struct trap_request *req) {
    const i64 pid = (i64)req->args[0];
    const u64 sig = req->args[1];
    if (sig >= 65) return reply(errno_inval(), 0);
    if (pid == 0 || pid == -1) return reply(0, 0);
    const u64 abs_pid = pid < 0 ? (u64)(-pid) : (u64)pid;
    if (abs_pid == 0) return reply(0, 0);
    for (u64 i = 0; i < LINUX_PROCESS_MAX; i++) {
        const struct linux_process_state *proc = &g_processes[i];
        if (!proc->used) continue;
        if (proc->pid == abs_pid || proc->tid == abs_pid) return reply(0, 0);
    }
    return reply(errno_noent(), 0);
}

static int linux_signal_is_ignored_by_default(u64 sig) {
    return sig == SIGCHLD || sig == SIGURG || sig == SIGWINCH;
}

static int linux_signal_is_job_control_only(u64 sig) {
    return sig == SIGCONT || sig == SIGSTOP || sig == SIGTSTP || sig == SIGTTIN || sig == SIGTTOU;
}

static int linux_signal_default_terminates(u64 sig) {
    if (sig == 0) return 0;
    if (linux_signal_is_ignored_by_default(sig)) return 0;
    if (linux_signal_is_job_control_only(sig)) return 0;
    return 1;
}

static struct abi_handler_result terminate_linux_process_by_signal(const struct trap_request *req, struct linux_process_state *target, u64 sig) {
    if (!target || sig == 0 || sig >= 65) return abi_reply_now(errno_inval(), 0);
    const u64 target_pid = target->pid;
    const u64 current_principal = req->caller_principal;
    const int exits_current = g_proc && g_proc->pid == target_pid;
    const u32 wait_status = (u32)(sig & 0x7fu);
    int record_exit = 0;

    for (u64 i = 0; i < LINUX_PROCESS_MAX; i++) {
        struct linux_process_state *proc = &g_processes[i];
        if (!proc->used || proc->pid != target_pid) continue;
        proc->exit_status = wait_status;
        if (proc->clear_child_tid != 0) {
            const u32 zero = 0;
            if (proc->principal == current_principal) {
                (void)copy_to_target(proc->clear_child_tid, &zero, sizeof(zero));
                (void)wake_futex_waiters(proc->pid, proc->clear_child_tid, 1);
            } else {
                (void)copy_to_trap_target(proc->principal, proc->clear_child_tid, &zero, sizeof(zero));
            }
        }
        remove_futex_waiters_for_principal(proc->principal);
        if (proc->principal != current_principal) {
            const u64 reply_status = reply_trap_target(proc->principal, 0, TRAP_RESPONSE_FLAG_EXIT);
            if (reply_status == SYSCALL_OK) proc->used = 0;
        }
        record_exit = 1;
    }

    if (record_exit) record_process_exit_for_principal(target_pid, target->principal, wait_status);
    if (exits_current) {
        close_all_process_fds(g_proc);
        if (g_proc) g_proc->used = 0;
        prime_reply_return_signal();
        exit_trap_target_no_wait(current_principal);
        (void)satisfy_pending_waiters_for_child(target_pid);
        return abi_wait_next();
    }

    close_all_process_fds(target);
    target->used = 0;
    prime_reply_return_signal();
    (void)satisfy_pending_waiters_for_child(target_pid);
    return abi_reply_now(0, 0);
}

static struct abi_handler_result handle_thread_signal(u64 tgid, u64 tid, u64 sig, const struct trap_request *req) {
    if (sig >= 65) return abi_reply_now(errno_inval(), 0);
    struct linux_process_state *target = process_state_for_tid(tid);
    if (!target) return abi_reply_now(errno_noent(), 0);
    if (tgid != 0 && target->pid != tgid) return abi_reply_now(errno_noent(), 0);
    if (sig == 0) return abi_reply_now(0, 0);
    if (target->sig_handler[sig] == 1) return abi_reply_now(0, 0);
    if (target->sig_handler[sig] != 0) return abi_reply_now(0, 0);
    if (!linux_signal_default_terminates(sig)) return abi_reply_now(0, 0);
    return terminate_linux_process_by_signal(req, target, sig);
}

static struct abi_handler_result handle_tkill(const struct trap_request *req) {
    return handle_thread_signal(0, req->args[0], req->args[1], req);
}

static struct abi_handler_result handle_tgkill(const struct trap_request *req) {
    return handle_thread_signal(req->args[0], req->args[1], req->args[2], req);
}

static struct abi_handler_result handle_current_exit(const struct trap_request *req, u64 syscall_profile_start_tick, int *syscall_profile_recorded) {
    const u64 exiting_principal = req->caller_principal;
    const u64 exiting_token = req->thread_id;
    struct linux_process_state *exiting_proc = g_proc;
    if (exiting_proc && exiting_proc->profile_enabled) {
        const u64 syscall_profile_end_tick = syscall0(SYSCALL_GET_TICK_COUNT);
        profile_record_syscall_ticks(req->nr, syscall_profile_end_tick - syscall_profile_start_tick);
        *syscall_profile_recorded = 1;
        profile_report_and_reset();
        exiting_proc->profile_enabled = 0;
    } else {
        profile_clear();
    }

    const u64 exiting_pid = exiting_proc ? exiting_proc->pid : exiting_principal;
    const int exiting_thread = exiting_proc && exiting_proc->tid != exiting_proc->pid;
    const int exit_group = req->nr == LINUX_SYS_EXIT_GROUP;
    const int process_exits = exit_group || !exiting_thread;
    int satisfy_waiters_after_exit_reply = 0;
    if (g_root_linux_principal_set && (exiting_principal == g_root_linux_principal || exiting_pid == g_root_linux_principal)) {
        user_log("LinuxAbiServer: root exit nr=");
        user_log_dec_value(req->nr);
        user_log(" principal=");
        user_log_hex_value(exiting_principal);
        user_log("LinuxAbiServer: root exit pid=");
        user_log_hex_value(exiting_pid);
        user_log("LinuxAbiServer: root exit status=");
        user_log_hex_value(req->args[0] & 0xffu);
        user_log("LinuxAbiServer: root exit group=");
        user_log_dec_value(exit_group);
        user_log(" process=");
        user_log_dec_value(process_exits);
        user_log("\n");
    }

    if (exiting_proc) exiting_proc->exit_status = (u32)((req->args[0] & 0xffu) << 8);
    if (exit_group && exiting_proc) {
        for (u64 i = 0; i < LINUX_PROCESS_MAX; i++) {
            struct linux_process_state *thread_proc = &g_processes[i];
            if (!thread_proc->used || thread_proc == exiting_proc || thread_proc->pid != exiting_pid) continue;
            thread_proc->exit_status = exiting_proc->exit_status;
            if (thread_proc->clear_child_tid != 0) {
                const u32 zero = 0;
                (void)copy_to_trap_target(thread_proc->principal, thread_proc->clear_child_tid, &zero, sizeof(zero));
            }
            remove_futex_waiters_for_principal(thread_proc->principal);
            const u64 reply_status = reply_trap_target(thread_proc->principal, 0, TRAP_RESPONSE_FLAG_EXIT);
            if (reply_status == SYSCALL_OK) {
                thread_proc->used = 0;
            }
        }
    }

    if (exiting_proc && exiting_proc->clear_child_tid != 0) {
        const u32 zero = 0;
        (void)copy_to_target(exiting_proc->clear_child_tid, &zero, sizeof(zero));
        (void)wake_futex_waiters(exiting_pid, exiting_proc->clear_child_tid, 1);
    }
    if (process_exits) {
        if (exiting_proc) record_process_exit_for_principal(exiting_pid, exiting_principal, exiting_proc->exit_status);
        wait_debug_event("exit_record", exiting_principal, exiting_pid, exiting_pid);
        close_all_process_fds(g_proc);
        satisfy_waiters_after_exit_reply = 1;
    }

    remove_futex_waiters_for_principal(exiting_principal);
    if (exiting_proc) exiting_proc->used = 0;
    prime_reply_return_signal();
    exit_trap_target_no_wait(exiting_token);
    if (satisfy_waiters_after_exit_reply) (void)satisfy_pending_waiters_for_child(exiting_pid);
    return abi_exit_current(exiting_principal);
}

static void finish_current_exit_after_wait(u64 exiting_principal) {
    int root_exited = g_root_linux_principal_set && exiting_principal == g_root_linux_principal;
    if (!root_exited && g_root_linux_principal_set) {
        const u64 root_status = syscall1(SYSCALL_GET_PROCESS_STATUS, g_root_linux_principal);
        root_exited = (root_status & 0xff) != 1;
    }
    if (root_exited && !has_live_linux_process_state() && !has_open_pipe_state() && !has_known_child_slots()) {
        process_exit(0);
    }
}

static struct abi_handler_result handle_wait4(const struct trap_request *req) {
    const i64 pid = (i64)req->args[0];
    const u64 status_va = req->args[1];
    const u64 options = req->args[2];
    wait_debug_event("wait4", req->caller_principal, g_proc ? g_proc->pid : 0, (u64)pid);
    const u64 supported_options = (u64)WNOHANG | (u64)WUNTRACED | (u64)WCONTINUED;
    if ((options & ~supported_options) != 0) return abi_reply_now(errno_inval(), 0);
    u64 child = 0;
    int fault = 0;
    if (reap_exited_child_for_current(g_proc, pid, status_va, &child, &fault)) return abi_reply_now(child, 0);
    if (fault) return abi_reply_now(errno_fault(), 0);
    if ((options & WNOHANG) != 0) return abi_reply_now(0, 0);
    int has_matching_child = 0;
    for (u64 i = 0; i < LINUX_CHILD_MAX; i++) {
        if (g_proc->child_used[i] && wait_pid_matches_child(pid, g_proc->child_slot[i])) has_matching_child = 1;
    }
    if (!has_matching_child) return abi_reply_now(errno_child(), 0);
    if (g_proc->wait_pending) return abi_reply_now(errno_again(), 0);
    g_proc->wait_pending = 1;
    g_proc->wait_pid = pid;
    g_proc->wait_status_va = status_va;
    wait_debug_event("pending", req->caller_principal, g_proc->pid, (u64)pid);
    const u64 detach_status = detach_reply_token();
    if (detach_status != SYSCALL_OK) {
        g_proc->wait_pending = 0;
        g_proc->wait_pid = 0;
        g_proc->wait_status_va = 0;
        return abi_reply_now(errno_again(), 0);
    }
    return abi_pending();
}
