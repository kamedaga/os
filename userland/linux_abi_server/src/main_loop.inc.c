static void signal_bootstrap_ready(volatile struct linux_abi_bootstrap_config *cfg) {
    const u64 endpoint_id = cfg->ready_endpoint_id != 0 ? cfg->ready_endpoint_id : LINUX_ABI_READY_ENDPOINT_ID;
    if (endpoint_id == 0 || cfg->ready_process_slot == 0) return;
    if (syscall3(SYSCALL_INSTALL_ENDPOINT, 0, endpoint_id, cfg->ready_process_slot) == SYSCALL_OK) {
        (void)syscall2(SYSCALL_SIGNAL_ENDPOINT, endpoint_id, 0);
    }
}

struct linux_syscall_dispatch_result {
    struct ipc_message msg;
    int profile_recorded;
};

struct linux_syscall_profile_scope {
    int enabled;
    int trace_enabled;
    u64 profile_start_tick;
    u64 trace_start_tick;
};

static struct linux_syscall_profile_scope linux_syscall_profile_begin(const struct trap_request *req) {
    struct linux_syscall_profile_scope scope;
    scope.enabled = g_proc->profile_enabled != 0;
    scope.trace_enabled = profile_trace_enabled();
    if (scope.enabled) profile_count_syscall(req->nr);
    scope.profile_start_tick = scope.enabled ? syscall0(SYSCALL_GET_TICK_COUNT) : 0;
    scope.trace_start_tick = scope.trace_enabled ? syscall0(SYSCALL_GET_TICK_COUNT) : 0;
    return scope;
}

static void linux_syscall_profile_finish(const struct trap_request *req, const struct linux_syscall_profile_scope *scope, int profile_recorded) {
    if (scope->trace_enabled) {
        profile_trace_syscall_span(req->nr, req->caller_principal, scope->trace_start_tick, syscall0(SYSCALL_GET_TICK_COUNT));
    }
    if (scope->enabled && !profile_recorded) {
        const u64 syscall_profile_end_tick = syscall0(SYSCALL_GET_TICK_COUNT);
        profile_record_syscall_ticks(req->nr, syscall_profile_end_tick - scope->profile_start_tick);
    }
}

static int wait_diag_enabled(void) {
    for (u64 i = 0; i < LINUX_PROCESS_MAX; i++) {
        if (g_processes[i].used && g_processes[i].profile_progress_enabled) return 1;
    }
    return 0;
}

static u64 count_wait_pending_processes(void) {
    u64 count = 0;
    for (u64 i = 0; i < LINUX_PROCESS_MAX; i++) {
        if (g_processes[i].used && g_processes[i].wait_pending) count++;
    }
    return count;
}

static u64 count_exec_pending_processes(void) {
    u64 count = 0;
    for (u64 i = 0; i < LINUX_PROCESS_MAX; i++) {
        if (g_processes[i].used && g_processes[i].exec_pending) count++;
    }
    return count;
}

static u64 count_sleep_waiters(void) {
    u64 count = 0;
    for (u64 i = 0; i < LINUX_SLEEP_WAITER_MAX; i++) {
        if (g_sleep_waiters[i].used) count++;
    }
    return count;
}

static u64 count_epoll_waiters(void) {
    u64 count = 0;
    for (u64 i = 0; i < LINUX_EPOLL_WAITER_MAX; i++) {
        if (g_epoll_waiters[i].used) count++;
    }
    return count;
}

static u64 process_child_slot_count(const struct linux_process_state *proc) {
    if (!proc) return 0;
    u64 count = 0;
    for (u64 i = 0; i < LINUX_CHILD_MAX; i++) {
        if (proc->child_used[i]) count++;
    }
    return count;
}

static void log_wait_diag_child_slots(const struct linux_process_state *proc) {
    if (!proc) return;
    u64 logged = 0;
    for (u64 i = 0; i < LINUX_CHILD_MAX && logged < 12; i++) {
        if (!proc->child_used[i]) continue;
        const u64 child = proc->child_slot[i];
        struct linux_process_state *child_proc = process_state_for_pid(child);
        const u64 child_principal = child_proc ? (child_proc->exec_pending ? child_proc->exec_pending_principal : child_proc->principal) : child;
        u64 status = 0;
        if (child_principal != 0) status = syscall1(SYSCALL_GET_PROCESS_STATUS, child_principal) & 0xff;
        user_log("LinuxAbiServer.wait_diag.child parent=");
        user_log_dec_value(proc->pid);
        user_log(" child=");
        user_log_dec_value(child);
        user_log(" has_state=");
        user_log_dec_value(child_proc ? 1 : 0);
        if (child_proc) {
            user_log(" principal=");
            user_log_dec_value(child_proc->principal);
            user_log(" pending_principal=");
            user_log_dec_value(child_proc->exec_pending_principal);
            user_log(" exec_pending=");
            user_log_dec_value(child_proc->exec_pending);
            if (child_proc->exec_path_len != 0) {
                user_log(" exe=");
                user_log(child_proc->exec_path);
            }
            user_log(" exit=");
            user_log_hex_value(child_proc->exit_status);
        }
        user_log(" status=");
        user_log_dec_value(status);
        user_log("\n");
        logged++;
    }
}

static int process_group_has_futex_waiter(u64 pid) {
    if (pid == 0) return 0;
    for (u64 i = 0; i < FUTEX_WAITER_MAX; i++) {
        const struct futex_waiter *waiter = &g_futex_waiters[i];
        if (!waiter->used) continue;
        const struct linux_process_state *proc = process_state_for(waiter->principal);
        if (proc && proc->pid == pid) return 1;
    }
    return 0;
}

static void log_wait_diag_periodic(void) {
    static u64 last_tick = 0;
    if (!wait_diag_enabled()) return;
    const u64 now = syscall0(SYSCALL_GET_TICK_COUNT);
    if (last_tick != 0 && now - last_tick < 5000) return;
    last_tick = now;

    user_log("LinuxAbiServer.wait_diag tick=");
    user_log_dec_value(now);
    user_log(" live=");
    user_log_dec_value(count_live_linux_process_states());
    user_log(" wait_pending=");
    user_log_dec_value(count_wait_pending_processes());
    user_log(" exec_pending=");
    user_log_dec_value(count_exec_pending_processes());
    user_log(" sleep=");
    user_log_dec_value(count_sleep_waiters());
    user_log(" futex=");
    user_log_dec_value(count_futex_waiters());
    user_log(" epoll=");
    user_log_dec_value(count_epoll_waiters());
    user_log(" exit_records=");
    user_log_dec_value(count_exit_records());
    user_log(" child_slots=");
    user_log_dec_value(count_known_child_slots());
    user_log("\n");

    u64 logged = 0;
    for (u64 i = 0; i < LINUX_PROCESS_MAX && logged < 4; i++) {
        const struct linux_process_state *proc = &g_processes[i];
        if (!proc->used || !proc->exec_pending) continue;
        user_log("LinuxAbiServer.wait_diag.exec_pending pid=");
        user_log_dec_value(proc->pid);
        user_log(" old_principal=");
        user_log_dec_value(proc->principal);
        user_log(" expected_principal=");
        user_log_dec_value(proc->exec_pending_principal);
        if (proc->exec_pending_principal != 0) {
            user_log(" status=");
            user_log_dec_value(syscall1(SYSCALL_GET_PROCESS_STATUS, proc->exec_pending_principal) & 0xff);
        }
        if (proc->exec_path_len != 0) {
            user_log(" exe=");
            user_log(proc->exec_path);
        }
        user_log("\n");
        logged++;
    }

    logged = 0;
    for (u64 i = 0; i < LINUX_PROCESS_MAX && logged < 4; i++) {
        const struct linux_process_state *proc = &g_processes[i];
        if (!proc->used || !proc->wait_pending) continue;
        user_log("LinuxAbiServer.wait_diag.wait pid=");
        user_log_dec_value(proc->pid);
        user_log(" tid=");
        user_log_dec_value(proc->tid);
        user_log(" principal=");
        user_log_dec_value(proc->principal);
        user_log(" wait_pid=");
        user_log_dec_value((u64)proc->wait_pid);
        user_log(" waitid=");
        user_log_dec_value(proc->wait_is_waitid);
        user_log(" children=");
        user_log_dec_value(process_child_slot_count(proc));
        user_log("\n");
        log_wait_diag_child_slots(proc);
        logged++;
    }

    logged = 0;
    for (u64 i = 0; i < LINUX_PROCESS_MAX && logged < 3; i++) {
        const struct linux_process_state *proc = &g_processes[i];
        if (!proc->used || proc->wait_pending || process_child_slot_count(proc) == 0) continue;
        if (!process_group_has_futex_waiter(proc->pid)) continue;
        user_log("LinuxAbiServer.wait_diag.children pid=");
        user_log_dec_value(proc->pid);
        user_log(" tid=");
        user_log_dec_value(proc->tid);
        user_log(" principal=");
        user_log_dec_value(proc->principal);
        user_log(" children=");
        user_log_dec_value(process_child_slot_count(proc));
        if (proc->exec_path_len != 0) {
            user_log(" exe=");
            user_log(proc->exec_path);
        }
        user_log("\n");
        log_wait_diag_child_slots(proc);
        logged++;
    }

    logged = 0;
    for (u64 i = 0; i < LINUX_SLEEP_WAITER_MAX && logged < 4; i++) {
        const struct linux_sleep_waiter *waiter = &g_sleep_waiters[i];
        if (!waiter->used) continue;
        const struct linux_process_state *proc = process_state_for(waiter->principal);
        user_log("LinuxAbiServer.wait_diag.sleep principal=");
        user_log_dec_value(waiter->principal);
        if (proc) {
            user_log(" pid=");
            user_log_dec_value(proc->pid);
            user_log(" tid=");
            user_log_dec_value(proc->tid);
        }
        user_log(" deadline=");
        user_log_dec_value(waiter->deadline_tick);
        user_log(" now=");
        user_log_dec_value(now);
        user_log("\n");
        logged++;
    }

    logged = 0;
    for (u64 i = 0; i < FUTEX_WAITER_MAX && logged < 8; i++) {
        const struct futex_waiter *waiter = &g_futex_waiters[i];
        if (!waiter->used) continue;
        const struct linux_process_state *proc = process_state_for(waiter->principal);
        user_log("LinuxAbiServer.wait_diag.futex principal=");
        user_log_dec_value(waiter->principal);
        if (proc) {
            user_log(" pid=");
            user_log_dec_value(proc->pid);
            user_log(" tid=");
            user_log_dec_value(proc->tid);
            if (proc->exec_path_len != 0) {
                user_log(" exe=");
                user_log(proc->exec_path);
            }
        }
        user_log(" owner=");
        user_log_dec_value(waiter->owner_pid);
        user_log(" uaddr=");
        user_log_hex_inline(waiter->uaddr);
        user_log(" expected=");
        user_log_hex_inline(waiter->expected);
        u32 current = 0;
        if (copy_from_trap_target(waiter->principal, waiter->uaddr, &current, sizeof(current)) == sizeof(current)) {
            user_log(" current=");
            user_log_hex_inline(current);
        } else {
            user_log(" current=fault");
        }
        user_log(" deadline=");
        user_log_dec_value(waiter->deadline_tick);
        user_log(" now=");
        user_log_dec_value(now);
        user_log("\n");
        logged++;
    }
}

static void linux_abi_wait_maintenance(void) {
    try_satisfy_pending_sigwaits();
    (void)try_satisfy_pending_waiters();
    process_futex_waiters_update();
    process_sleep_waiters_update();
    process_epoll_waiters_update();
    flush_deferred_pipe_wakes();
    log_wait_diag_periodic();
}

static struct ipc_message wait_linux_abi_event(void) {
    for (;;) {
        linux_abi_wait_maintenance();
        struct ipc_message msg = wait_ipc_timeout(1);
        if (msg.status == SYSCALL_ERR_NOT_READY) continue;
        return msg;
    }
}

static u64 g_syscall_diag_logs;

static void log_syscall_diag(const char *phase, const struct trap_request *req) {
    if (!wait_diag_enabled() || g_syscall_diag_logs >= 4096) return;
    const u64 now = syscall0(SYSCALL_GET_TICK_COUNT);
    if (now < 22500) return;
    g_syscall_diag_logs++;
    user_log("LinuxAbiServer.syscall.");
    user_log(phase);
    user_log(" tick=");
    user_log_dec_value(now);
    user_log(" nr=");
    user_log_dec_value(req->nr);
    const struct linux_syscall_metadata *meta = linux_syscall_metadata_for(req->nr);
    if (meta != 0) {
        user_log(" name=");
        user_log(meta->name);
    }
    user_log(" principal=");
    user_log_dec_value(req->caller_principal);
    if (g_proc) {
        user_log(" pid=");
        user_log_dec_value(g_proc->pid);
        user_log(" tid=");
        user_log_dec_value(g_proc->tid);
    }
    user_log("\n");
}

static struct linux_syscall_dispatch_result handle_exit_syscall(const struct trap_request *req, u64 syscall_profile_start_tick) {
    struct linux_syscall_dispatch_result result = { { 0, 0, 0, 0, 0 }, 0 };
    const u64 exiting_principal = req->caller_principal;
    struct linux_process_state *exiting_proc = g_proc;
    if (exiting_proc && exiting_proc->profile_enabled) {
        const u64 syscall_profile_end_tick = syscall0(SYSCALL_GET_TICK_COUNT);
        profile_record_syscall_ticks(req->nr, syscall_profile_end_tick - syscall_profile_start_tick);
        result.profile_recorded = 1;
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
    u8 peer_should_exit[LINUX_PROCESS_MAX];
    for (u64 i = 0; i < LINUX_PROCESS_MAX; i++) peer_should_exit[i] = 0;
    profile_trace_event_u64("exit.begin principal", exiting_principal);
    if (exiting_proc) exiting_proc->exit_status = (u32)((req->args[0] & 0xffu) << 8);
    if (exit_group && exiting_proc) {
        for (u64 i = 0; i < LINUX_PROCESS_MAX; i++) {
            struct linux_process_state *thread_proc = &g_processes[i];
            if (!thread_proc->used || thread_proc == exiting_proc || thread_proc->pid != exiting_pid) continue;
            thread_proc->exit_status = exiting_proc->exit_status;
            clear_child_tid_and_wake(thread_proc, exiting_principal);
            remove_futex_waiters_for_principal(thread_proc->principal);
            remove_sleep_waiters_for_principal(thread_proc->principal);
            remove_epoll_waiters_for_principal(thread_proc->principal);
            peer_should_exit[i] = 1;
        }
    }
    clear_child_tid_and_wake(exiting_proc, exiting_principal);
    if (process_exits) {
        if (exiting_proc) record_process_exit(exiting_pid, exiting_proc->exit_status);
        profile_trace_event_u64("exit.close_fds pid", exiting_pid);
        clear_tracked_target_ranges();
        for (u64 i = 0; i < LINUX_PROCESS_MAX; i++) {
            if (!peer_should_exit[i]) continue;
            const u64 reply_status = reply_trap_target(g_processes[i].principal, 0, TRAP_RESPONSE_FLAG_EXIT);
            if (reply_status == SYSCALL_OK) g_processes[i].used = 0;
        }
        close_all_process_fds(g_proc);
        clear_thread_group_fd_tables_no_close(exiting_pid, exiting_proc);
        release_process_vm_object_tokens(exiting_proc);
        satisfy_waiters_after_exit_reply = 1;
    }
    remove_futex_waiters_for_principal(exiting_principal);
    remove_sleep_waiters_for_principal(exiting_principal);
    remove_epoll_waiters_for_principal(exiting_principal);
    reply_vfork_parent_if_any(exiting_proc);
    if (exiting_proc) exiting_proc->used = 0;
    prime_reply_return_signal();
    profile_trace_event_u64("exit.reply principal", exiting_principal);
    exit_trap_target_no_wait(exiting_principal);
    if (satisfy_waiters_after_exit_reply) {
        profile_trace_event_u64("exit.satisfy_waiters pid", exiting_pid);
        try_satisfy_pending_sigwaits();
        (void)satisfy_pending_waiters_for_child(exiting_pid);
    }
    result.msg = wait_ipc_timeout(1);
    finish_linux_abi_if_root_exited(exiting_principal);
    return result;
}

static int dispatch_fd_syscall(const struct trap_request *req, struct ipc_message *msg) {
    switch (req->nr) {
    case LINUX_SYS_READ: *msg = handle_read(req); return 1;
    case LINUX_SYS_WRITE: *msg = handle_write(req); return 1;
    case LINUX_SYS_READV: *msg = handle_readv(req); return 1;
    case LINUX_SYS_WRITEV: *msg = handle_writev(req); return 1;
    case LINUX_SYS_PWRITE64: *msg = handle_pwrite64(req); return 1;
    case LINUX_SYS_PIPE: *msg = handle_pipe2(req, 0); return 1;
    case LINUX_SYS_PIPE2: *msg = handle_pipe2(req, 1); return 1;
    case LINUX_SYS_EPOLL_CREATE1: *msg = handle_epoll_create1(req); return 1;
    case LINUX_SYS_EPOLL_CTL: *msg = handle_epoll_ctl(req); return 1;
    case LINUX_SYS_EPOLL_WAIT: *msg = handle_epoll_wait(req); return 1;
    case LINUX_SYS_EPOLL_PWAIT: *msg = handle_epoll_wait(req); return 1;
    case LINUX_SYS_EVENTFD2: *msg = handle_eventfd2(req); return 1;
    case LINUX_SYS_CLOSE: *msg = handle_close(req); return 1;
    case LINUX_SYS_DUP: *msg = handle_dup(req); return 1;
    case LINUX_SYS_DUP2: *msg = handle_dup2_like(req, 0); return 1;
    case LINUX_SYS_DUP3: *msg = handle_dup2_like(req, 1); return 1;
    case LINUX_SYS_FCNTL: *msg = handle_fcntl(req); return 1;
    case LINUX_SYS_FLOCK: *msg = handle_flock(req); return 1;
    case LINUX_SYS_FSYNC:
    case LINUX_SYS_FDATASYNC:
    case LINUX_SYS_SYNCFS: *msg = handle_fsync_like(req); return 1;
    case LINUX_SYS_FTRUNCATE: *msg = handle_ftruncate(req); return 1;
    case LINUX_SYS_SYNC: *msg = reply(0, 0); return 1;
    default: return 0;
    }
}

void linux_abi_main(void) {
    volatile struct linux_abi_bootstrap_config *cfg = (volatile struct linux_abi_bootstrap_config *)LINUX_ABI_CONFIG_TARGET_VA;
    if (cfg->magic != LINUX_ABI_BOOTSTRAP_MAGIC ||
        cfg->version != LINUX_ABI_BOOTSTRAP_VERSION ||
        cfg->abi_trap_request_page_va == 0)
    {
        user_log("LinuxAbiServer: bootstrap config invalid\n");
        for (;;) __asm__ volatile("pause");
    }
    trap_request_page_va = cfg->abi_trap_request_page_va;
    g_exec_vm_token = cfg->exec_vm_token;
    g_standard_interpreter_vm_token = cfg->standard_interpreter_vm_token;
    g_standard_interpreter_bytes = cfg->standard_interpreter_file_bytes;
    apply_linux_abi_layout_config(cfg);
    g_exec_path_len = cfg->exec_path_bytes <= FS_MAX_PATH_BYTES ? cfg->exec_path_bytes : FS_MAX_PATH_BYTES;
    for (u16 i = 0; i < g_exec_path_len; i++) g_exec_path[i] = cfg->exec_path[i];
    g_exec_path[g_exec_path_len] = 0;
    if (!linux_syscall_metadata_validate()) {
        user_log("LinuxAbiServer: syscall metadata invalid\n");
        for (;;) __asm__ volatile("pause");
    }
    const u64 request_page_status = alloc_map_pages(trap_request_page_va, 1, 0x1);
    if (request_page_status != SYSCALL_OK) { user_log("LinuxAbiServer: request page map failed\n"); user_log_hex_value(request_page_status); for (;;) __asm__ volatile("pause"); }
    if (!ensure_all_child_trap_request_pages()) { user_log("LinuxAbiServer: request page table map failed\n"); for (;;) __asm__ volatile("pause"); }
    (void)install_self_wake_endpoint();
    if (!connect_vfs_from_registry()) user_log("LinuxAbiServer: vfs connect failed\n");
    if (!connect_console_from_registry()) user_log("LinuxAbiServer: console connect skipped\n");
    init_process_tables();
    cfg->status = LINUX_ABI_BOOTSTRAP_READY;
    signal_bootstrap_ready(cfg);
    user_log("LinuxAbiServer: started\n");
    prime_reply_return_signal();
    struct ipc_message msg = reply(0, 0);
    for (;;) {
        abi_context_clear();
        linux_abi_wait_maintenance();
        if (msg.status != SYSCALL_OK) {
            msg = wait_ipc_timeout(1);
            continue;
        }
        if (msg.request_va == 0) {
            if (!g_root_linux_principal_set) {
                msg = wait_ipc_timeout(1);
                continue;
            }
            const int polled_tty = g_console.active && g_console.is_tty ? poll_tty_signal_events() : 1;
            u64 sleep_timeout = 0;
            const int sleep_pending = sleep_waiters_next_timeout(&sleep_timeout);
            u64 futex_timeout = 0;
            const int futex_pending = futex_waiters_next_timeout(&futex_timeout);
            u64 epoll_timeout = 0;
            const int epoll_pending = epoll_waiters_next_timeout(&epoll_timeout);
            int wait_pending = 0;
            u64 wait_timeout = 0;
            if (sleep_pending) {
                wait_pending = 1;
                wait_timeout = sleep_timeout;
            }
            if (futex_pending && (!wait_pending || futex_timeout < wait_timeout)) {
                wait_pending = 1;
                wait_timeout = futex_timeout;
            }
            if (epoll_pending && (!wait_pending || epoll_timeout < wait_timeout)) {
                wait_pending = 1;
                wait_timeout = epoll_timeout;
            }
            if (!polled_tty) {
                msg = wait_ipc_timeout(1);
            } else if (wait_pending) {
                (void)wait_timeout;
                msg = wait_ipc_timeout(1);
            } else {
                msg = wait_ipc_timeout(1);
            }
            continue;
        }
        if (!is_known_trap_request_page(msg.request_va)) { msg = reply(errno_inval(), 0); continue; }
        const struct trap_request req_snapshot = *(const struct trap_request *)msg.request_va;
        const struct trap_request *req = &req_snapshot;
        if (req->magic != TRAP_MAGIC || req->version != TRAP_VERSION) { user_log("LinuxAbiServer: bad request header\n"); msg = reply(errno_inval(), 0); continue; }
        struct linux_abi_context ctx;
        abi_context_enter(&ctx, process_state_for(req->caller_principal), req, req->caller_principal);
        if (!g_root_linux_principal_set) {
            g_root_linux_principal = req->caller_principal;
            g_root_linux_principal_set = 1;
        }
        if (!g_proc) {
            user_log("LinuxAbiServer: process state missing principal=");
            user_log_hex_inline(req->caller_principal);
            user_log(" live=");
            user_log_dec_value(count_live_linux_process_states());
            user_log(" children=");
            user_log_dec_value(count_known_child_slots());
            user_log("\n");
            msg = reply(errno_busy(), 0);
            continue;
        }
        process_timers_update(g_proc);
        if (req->kind == TRAP_KIND_PAGE_FAULT) {
            msg = handle_page_fault(req);
            continue;
        }
        if (req->kind == TRAP_KIND_ASYNC_SIGNAL) {
            msg = reply(req->rax, 0);
            continue;
        }
        if (req->kind != TRAP_KIND_ABI_SYSCALL) {
            msg = reply(errno_inval(), 0);
            continue;
        }
        const struct linux_syscall_profile_scope profile_scope = linux_syscall_profile_begin(req);
        if (profile_scope.trace_enabled) {
            user_log("LinuxAbiServer.trace syscall.begin nr=");
            user_log_dec_value(req->nr);
            user_log(" pid=");
            user_log_dec_value(g_proc ? g_proc->pid : 0);
            user_log(" principal=");
            user_log_dec_value(req->caller_principal);
            user_log(" a0=");
            user_log_hex_inline(req->args[0]);
            user_log(" a1=");
            user_log_hex_inline(req->args[1]);
            user_log(" a2=");
            user_log_hex_inline(req->args[2]);
            if (g_proc && g_proc->exec_path_len != 0) {
                user_log(" exe=");
                user_log(g_proc->exec_path);
            }
            user_log("\n");
        }
        int syscall_profile_recorded = 0;
        log_syscall_diag("begin", req);
        if (!dispatch_fd_syscall(req, &msg)) switch (req->nr) {
        case LINUX_SYS_OPEN: msg = handle_openat(req, 1); break;
        case LINUX_SYS_OPENAT: msg = handle_openat(req, 0); break;
        case LINUX_SYS_SOCKET: msg = handle_socket(req); break;
        case LINUX_SYS_CONNECT: msg = handle_connect_socket(req); break;
        case LINUX_SYS_BIND: msg = handle_bind_socket(req); break;
        case LINUX_SYS_SENDTO: msg = handle_sendto(req); break;
        case LINUX_SYS_SENDMSG: msg = handle_sendmsg_socket(req); break;
        case LINUX_SYS_RECVFROM: msg = handle_recvfrom(req); break;
        case LINUX_SYS_RECVMSG: msg = handle_recvmsg_socket(req); break;
        case LINUX_SYS_SHUTDOWN: msg = handle_shutdown_socket(req); break;
        case LINUX_SYS_GETSOCKNAME: msg = handle_getsockname_socket(req); break;
        case LINUX_SYS_GETPEERNAME: msg = handle_getpeername_socket(req); break;
        case LINUX_SYS_SETSOCKOPT: msg = handle_setsockopt_socket(req); break;
        case LINUX_SYS_GETSOCKOPT: msg = handle_getsockopt_socket(req); break;
        case LINUX_SYS_CLONE: msg = handle_fork_like(req, 1); break;
        case LINUX_SYS_FORK: msg = handle_fork_like(req, 0); break;
        case LINUX_SYS_VFORK: msg = handle_fork_like(req, 0); break;
        case LINUX_SYS_WAIT4: msg = handle_wait4(req); break;
        case LINUX_SYS_WAITID: msg = handle_waitid(req); break;
        case LINUX_SYS_KILL: msg = handle_kill(req); break;
        case LINUX_SYS_GETRUSAGE: msg = handle_getrusage(req); break;
        case LINUX_SYS_STAT: case LINUX_SYS_LSTAT: msg = handle_newfstatat(req, 1); break;
        case LINUX_SYS_FSTAT: msg = handle_fstat(req); break;
        case LINUX_SYS_NEWFSTATAT: msg = handle_newfstatat(req, 0); break;
        case LINUX_SYS_STATFS: msg = handle_statfs(req); break;
        case LINUX_SYS_FSTATFS: msg = handle_fstatfs(req); break;
        case LINUX_SYS_POLL: msg = handle_poll(req, 0); break;
        case LINUX_SYS_SELECT: msg = handle_select(req, 0); break;
        case LINUX_SYS_PSELECT6: msg = handle_select(req, 1); break;
        case LINUX_SYS_PPOLL: msg = handle_poll(req, 1); break;
        case LINUX_SYS_PREAD64: msg = handle_pread64(req); break;
        case LINUX_SYS_PWRITE64: msg = handle_pwrite64(req); break;
        case LINUX_SYS_COPY_FILE_RANGE: msg = handle_copy_file_range(req); break;
        case LINUX_SYS_GETDENTS64: msg = handle_getdents64(req); break;
        case LINUX_SYS_LSEEK: msg = handle_lseek(req); break;
        case LINUX_SYS_ACCESS: msg = handle_access(req); break;
        case LINUX_SYS_FACCESSAT: msg = handle_faccessat(req); break;
        case LINUX_SYS_FACCESSAT2: msg = handle_faccessat(req); break;
        case LINUX_SYS_GETCWD: msg = handle_getcwd(req); break;
        case LINUX_SYS_CHDIR: msg = handle_chdir(req); break;
        case LINUX_SYS_FCHDIR: msg = handle_fchdir(req); break;
        case LINUX_SYS_RENAME: msg = handle_renameat(req, 1, 0); break;
        case LINUX_SYS_MKDIR: msg = handle_mkdirat(req, 1); break;
        case LINUX_SYS_MKDIRAT: msg = handle_mkdirat(req, 0); break;
        case LINUX_SYS_LINK: msg = handle_linkat(req, 1); break;
        case LINUX_SYS_LINKAT: msg = handle_linkat(req, 0); break;
        case LINUX_SYS_UNLINK: msg = handle_unlinkat(req, 1); break;
        case LINUX_SYS_UNLINKAT: msg = handle_unlinkat(req, 0); break;
        case LINUX_SYS_RENAMEAT: msg = handle_renameat(req, 0, 0); break;
        case LINUX_SYS_SYMLINK: msg = handle_symlinkat(req, 1); break;
        case LINUX_SYS_SYMLINKAT: msg = handle_symlinkat(req, 0); break;
        case LINUX_SYS_READLINK: msg = handle_readlink(req); break;
        case LINUX_SYS_READLINKAT: msg = handle_readlinkat(req, 0); break;
        case LINUX_SYS_UNAME: msg = handle_uname(req); break;
        case LINUX_SYS_TIME: msg = handle_time_syscall(req); break;
        case LINUX_SYS_GETTIMEOFDAY: msg = handle_gettimeofday(req); break;
        case LINUX_SYS_SYSINFO: msg = handle_sysinfo(req); break;
        case LINUX_SYS_CLOCK_GETTIME: msg = handle_clock_gettime(req); break;
        case LINUX_SYS_CLOCK_GETRES: msg = handle_clock_getres(req); break;
        case LINUX_SYS_PAUSE: msg = handle_pause_syscall(req); break;
        case LINUX_SYS_NANOSLEEP: msg = handle_nanosleep(req); break;
        case LINUX_SYS_CLOCK_NANOSLEEP: msg = handle_clock_nanosleep(req); break;
        case LINUX_SYS_SETITIMER: msg = handle_setitimer(req); break;
        case LINUX_SYS_TIMER_CREATE: msg = handle_timer_create(req); break;
        case LINUX_SYS_TIMER_SETTIME: msg = handle_timer_settime(req); break;
        case LINUX_SYS_TIMER_GETTIME: msg = handle_timer_gettime(req); break;
        case LINUX_SYS_TIMER_DELETE: msg = handle_timer_delete(req); break;
        case LINUX_SYS_SCHED_YIELD: msg = handle_sched_yield(req); break;
        case LINUX_SYS_SCHED_GETAFFINITY: msg = handle_sched_getaffinity(req); break;
        case LINUX_SYS_MEMBARRIER: msg = handle_membarrier(req); break;
        case LINUX_SYS_EXECVE: msg = handle_execve(req); break;
        case LINUX_SYS_MMAP: msg = handle_mmap(req); break;
        case LINUX_SYS_BRK: msg = handle_brk(req); break;
        case LINUX_SYS_MPROTECT: msg = handle_mprotect(req); break;
        case LINUX_SYS_MADVISE: msg = handle_madvise(req); break;
        case LINUX_SYS_MINCORE: msg = handle_mincore(req); break;
        case LINUX_SYS_MUNMAP: msg = handle_munmap(req); break;
        case LINUX_SYS_MREMAP: msg = handle_mremap(req); break;
        case LINUX_SYS_ARCH_PRCTL: msg = handle_arch_prctl(req); break;
        case LINUX_SYS_CHROOT: msg = handle_chroot(req); break;
        case LINUX_SYS_MOUNT: case LINUX_SYS_UMOUNT2: msg = reply(errno_perm(), 0); break;
        case LINUX_SYS_RT_SIGACTION: msg = handle_rt_sigaction(req); break;
        case LINUX_SYS_RT_SIGPROCMASK: msg = handle_rt_sigprocmask(req); break;
        case LINUX_SYS_RT_SIGRETURN: msg = handle_rt_sigreturn(req); break;
        case LINUX_SYS_RT_SIGTIMEDWAIT: msg = handle_rt_sigtimedwait(req); break;
        case LINUX_SYS_RT_SIGSUSPEND: msg = handle_rt_sigsuspend(req); break;
        case LINUX_SYS_SIGALTSTACK: msg = handle_sigaltstack(req); break;
        case LINUX_SYS_SET_TID_ADDRESS: msg = handle_set_tid_address(req); break;
        case LINUX_SYS_FUTEX: msg = handle_futex(req); break;
        case LINUX_SYS_IOCTL: msg = handle_ioctl(req); break;
        case LINUX_SYS_PRLIMIT64: msg = handle_prlimit64(req); break;
        case LINUX_SYS_FALLOCATE: msg = handle_fallocate(req); break;
        case LINUX_SYS_CHMOD: case LINUX_SYS_FCHMOD: case LINUX_SYS_CHOWN: case LINUX_SYS_FCHOWN: case LINUX_SYS_LCHOWN: case LINUX_SYS_FCHOWNAT: case LINUX_SYS_FCHMODAT: case LINUX_SYS_SET_ROBUST_LIST: case LINUX_SYS_UTIMENSAT: case LINUX_SYS_RSEQ: msg = reply(0, 0); break;
        case LINUX_SYS_LISTXATTR: case LINUX_SYS_LLISTXATTR: case LINUX_SYS_FLISTXATTR: msg = reply(errno_opnotsupp(), 0); break;
        case LINUX_SYS_PIDFD_OPEN: msg = reply(errno_nosys(), 0); break;
        case LINUX_SYS_RENAMEAT2: msg = handle_renameat(req, 0, 1); break;
        case LINUX_SYS_SPLICE: msg = reply(errno_nosys(), 0); break;
        case LINUX_SYS_GETPID: msg = reply(g_proc && g_proc->pid != 0 ? g_proc->pid : 1, 0); break;
        case LINUX_SYS_GETTID: msg = reply(g_proc && g_proc->tid != 0 ? g_proc->tid : 1, 0); break;
        case LINUX_SYS_TKILL: msg = handle_tkill(req); break;
        case LINUX_SYS_TGKILL: msg = handle_tgkill(req); break;
        case LINUX_SYS_GETPPID: msg = reply(1, 0); break;
        case LINUX_SYS_SETPGID: msg = handle_setpgid(req); break;
        case LINUX_SYS_SETSID: msg = handle_setsid(req); break;
        case LINUX_SYS_GETPGID: msg = handle_getpgid(req); break;
        case LINUX_SYS_GETUID: case LINUX_SYS_GETGID: case LINUX_SYS_GETEUID: case LINUX_SYS_GETEGID: case LINUX_SYS_SETFSUID: case LINUX_SYS_SETFSGID: msg = reply(0, 0); break;
        case LINUX_SYS_UMASK: msg = reply(022, 0); break;
        case LINUX_SYS_GETRANDOM: msg = handle_getrandom(req); break;
        case LINUX_SYS_EXIT:
        case LINUX_SYS_EXIT_GROUP:
            {
                const struct linux_syscall_dispatch_result exit_result = handle_exit_syscall(req, profile_scope.profile_start_tick);
                msg = exit_result.msg;
                syscall_profile_recorded = exit_result.profile_recorded;
            }
            break;
        default: user_log("LinuxAbiServer: unhandled syscall\n"); user_log_hex_value(req->nr); msg = reply(errno_nosys(), 0); break;
        }
        log_syscall_diag("done", req);
        linux_syscall_profile_finish(req, &profile_scope, syscall_profile_recorded);
    }
}
