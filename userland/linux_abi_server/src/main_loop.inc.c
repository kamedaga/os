static void signal_bootstrap_ready(volatile struct linux_abi_bootstrap_config *cfg) {
    const u64 endpoint_id = cfg->ready_endpoint_id != 0 ? cfg->ready_endpoint_id : LINUX_ABI_READY_ENDPOINT_ID;
    if (endpoint_id == 0 || cfg->ready_process_slot == 0) return;
    if (syscall3(SYSCALL_INSTALL_ENDPOINT, 0, endpoint_id, cfg->ready_process_slot) == SYSCALL_OK) {
        (void)syscall2(SYSCALL_SIGNAL_ENDPOINT, endpoint_id, 0);
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
    g_exec_path_len = cfg->exec_path_bytes <= FS_MAX_PATH_BYTES ? cfg->exec_path_bytes : FS_MAX_PATH_BYTES;
    for (u16 i = 0; i < g_exec_path_len; i++) g_exec_path[i] = cfg->exec_path[i];
    g_exec_path[g_exec_path_len] = 0;
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
        g_abi_ctx = 0;
        start_deferred_trap_targets();
        try_satisfy_pending_sigwaits();
        flush_deferred_pipe_wakes();
        if (msg.status != SYSCALL_OK) {
            msg = msg.status == SYSCALL_ERR_NOT_READY ? wait_ipc_timeout(1) : wait_ipc();
            continue;
        }
        if (msg.request_va == 0) {
            if (!g_root_linux_principal_set) {
                msg = wait_ipc_timeout(1);
                continue;
            }
            const int polled_tty = g_console.active && g_console.is_tty ? poll_tty_signal_events() : 1;
            msg = polled_tty ? wait_ipc() : wait_ipc_timeout(1);
            continue;
        }
        if (!is_known_trap_request_page(msg.request_va)) { msg = reply(errno_inval(), 0); continue; }
        const struct trap_request req_snapshot = *(const struct trap_request *)msg.request_va;
        const struct trap_request *req = &req_snapshot;
        if (req->magic != TRAP_MAGIC || req->version != TRAP_VERSION) { user_log("LinuxAbiServer: bad request header\n"); msg = reply(errno_inval(), 0); continue; }
        struct linux_abi_context ctx;
        ctx.proc = process_state_for(req->caller_principal);
        ctx.request = req;
        ctx.reply_target_principal = req->caller_principal;
        g_abi_ctx = &ctx;
        if (!g_root_linux_principal_set) {
            g_root_linux_principal = req->caller_principal;
            g_root_linux_principal_set = 1;
        }
        if (!g_proc) { msg = reply(errno_busy(), 0); continue; }
        process_timers_update(g_proc);
        if (req->kind == TRAP_KIND_PAGE_FAULT) {
            msg = handle_page_fault(req);
            continue;
        }
        if (req->kind != TRAP_KIND_ABI_SYSCALL) {
            msg = reply(errno_inval(), 0);
            continue;
        }
        const int profile_this_syscall = g_proc->profile_enabled != 0;
        const int profile_trace_this_syscall = profile_trace_enabled();
        if (profile_this_syscall) profile_count_syscall(req->nr);
        const u64 syscall_profile_start_tick = profile_this_syscall ? syscall0(SYSCALL_GET_TICK_COUNT) : 0;
        const u64 syscall_trace_start_tick = profile_trace_this_syscall ? syscall0(SYSCALL_GET_TICK_COUNT) : 0;
        int syscall_profile_recorded = 0;
        switch (req->nr) {
        case LINUX_SYS_READ: msg = handle_read(req); break;
        case LINUX_SYS_WRITE: msg = handle_write(req); break;
        case LINUX_SYS_READV: msg = handle_readv(req); break;
        case LINUX_SYS_WRITEV: msg = handle_writev(req); break;
        case LINUX_SYS_PIPE: msg = handle_pipe2(req, 0); break;
        case LINUX_SYS_PIPE2: msg = handle_pipe2(req, 1); break;
        case LINUX_SYS_EPOLL_CREATE1: msg = handle_epoll_create1(req); break;
        case LINUX_SYS_EPOLL_CTL: msg = handle_epoll_ctl(req); break;
        case LINUX_SYS_EPOLL_WAIT: msg = handle_epoll_wait(req); break;
        case LINUX_SYS_OPEN: msg = handle_openat(req, 1); break;
        case LINUX_SYS_OPENAT: msg = handle_openat(req, 0); break;
        case LINUX_SYS_CLOSE: msg = handle_close(req); break;
        case LINUX_SYS_DUP: msg = handle_dup(req); break;
        case LINUX_SYS_DUP2: msg = handle_dup2_like(req, 0); break;
        case LINUX_SYS_DUP3: msg = handle_dup2_like(req, 1); break;
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
        case LINUX_SYS_KILL: msg = handle_kill(req); break;
        case LINUX_SYS_FCNTL: msg = handle_fcntl(req); break;
        case LINUX_SYS_FLOCK: msg = handle_flock(req); break;
        case LINUX_SYS_FSYNC: case LINUX_SYS_FDATASYNC: case LINUX_SYS_SYNCFS: msg = handle_fsync_like(req); break;
        case LINUX_SYS_SYNC: msg = reply(0, 0); break;
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
        case LINUX_SYS_GETDENTS64: msg = handle_getdents64(req); break;
        case LINUX_SYS_LSEEK: msg = handle_lseek(req); break;
        case LINUX_SYS_ACCESS: msg = handle_access(req); break;
        case LINUX_SYS_FACCESSAT: msg = handle_faccessat(req); break;
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
        case LINUX_SYS_NANOSLEEP: msg = handle_nanosleep(req); break;
        case LINUX_SYS_CLOCK_NANOSLEEP: msg = handle_clock_nanosleep(req); break;
        case LINUX_SYS_SETITIMER: msg = handle_setitimer(req); break;
        case LINUX_SYS_TIMER_CREATE: msg = handle_timer_create(req); break;
        case LINUX_SYS_TIMER_SETTIME: msg = handle_timer_settime(req); break;
        case LINUX_SYS_TIMER_GETTIME: msg = handle_timer_gettime(req); break;
        case LINUX_SYS_TIMER_DELETE: msg = handle_timer_delete(req); break;
        case LINUX_SYS_SCHED_GETAFFINITY: msg = handle_sched_getaffinity(req); break;
        case LINUX_SYS_MEMBARRIER: msg = handle_membarrier(req); break;
        case LINUX_SYS_EXECVE: msg = handle_execve(req); break;
        case LINUX_SYS_MMAP: msg = handle_mmap(req); break;
        case LINUX_SYS_BRK: msg = handle_brk(req); break;
        case LINUX_SYS_MPROTECT: msg = handle_mprotect(req); break;
        case LINUX_SYS_MUNMAP: msg = handle_munmap(req); break;
        case LINUX_SYS_MREMAP: msg = handle_mremap(req); break;
        case LINUX_SYS_ARCH_PRCTL: msg = handle_arch_prctl(req); break;
        case LINUX_SYS_CHROOT: msg = handle_chroot(req); break;
        case LINUX_SYS_MOUNT: case LINUX_SYS_UMOUNT2: msg = reply(errno_perm(), 0); break;
        case LINUX_SYS_RT_SIGACTION: msg = handle_rt_sigaction(req); break;
        case LINUX_SYS_RT_SIGPROCMASK: msg = handle_rt_sigprocmask(req); break;
        case LINUX_SYS_RT_SIGRETURN: msg = handle_rt_sigreturn(req); break;
        case LINUX_SYS_RT_SIGTIMEDWAIT: msg = handle_rt_sigtimedwait(req); break;
        case LINUX_SYS_SIGALTSTACK: msg = handle_sigaltstack(req); break;
        case LINUX_SYS_SET_TID_ADDRESS: msg = handle_set_tid_address(req); break;
        case LINUX_SYS_FUTEX: msg = handle_futex(req); break;
        case LINUX_SYS_IOCTL: msg = handle_ioctl(req); break;
        case LINUX_SYS_MADVISE: case LINUX_SYS_CHMOD: case LINUX_SYS_FCHMOD: case LINUX_SYS_CHOWN: case LINUX_SYS_FCHOWN: case LINUX_SYS_LCHOWN: case LINUX_SYS_FCHOWNAT: case LINUX_SYS_FCHMODAT: case LINUX_SYS_FALLOCATE: case LINUX_SYS_SET_ROBUST_LIST: case LINUX_SYS_UTIMENSAT: case LINUX_SYS_PRLIMIT64: case LINUX_SYS_RSEQ: msg = reply(0, 0); break;
        case LINUX_SYS_RENAMEAT2: msg = handle_renameat(req, 0, 1); break;
        case LINUX_SYS_SPLICE: case LINUX_SYS_EVENTFD2: msg = reply(errno_nosys(), 0); break;
        case LINUX_SYS_GETPID: msg = reply(g_proc && g_proc->pid != 0 ? g_proc->pid : 1, 0); break;
        case LINUX_SYS_GETTID: msg = reply(g_proc && g_proc->tid != 0 ? g_proc->tid : 1, 0); break;
        case LINUX_SYS_TKILL: msg = handle_tkill(req); break;
        case LINUX_SYS_TGKILL: msg = handle_tgkill(req); break;
        case LINUX_SYS_GETPPID: msg = reply(1, 0); break;
        case LINUX_SYS_SETPGID: msg = handle_setpgid(req); break;
        case LINUX_SYS_GETPGID: msg = handle_getpgid(req); break;
        case LINUX_SYS_GETUID: case LINUX_SYS_GETGID: case LINUX_SYS_GETEUID: case LINUX_SYS_GETEGID: case LINUX_SYS_SETFSUID: case LINUX_SYS_SETFSGID: msg = reply(0, 0); break;
        case LINUX_SYS_UMASK: msg = reply(022, 0); break;
        case LINUX_SYS_GETRANDOM: msg = handle_getrandom(req); break;
        case LINUX_SYS_EXIT:
        case LINUX_SYS_EXIT_GROUP:
            {
                const u64 exiting_principal = req->caller_principal;
                struct linux_process_state *exiting_proc = g_proc;
                if (exiting_proc && exiting_proc->profile_enabled) {
                    const u64 syscall_profile_end_tick = syscall0(SYSCALL_GET_TICK_COUNT);
                    profile_record_syscall_ticks(req->nr, syscall_profile_end_tick - syscall_profile_start_tick);
                    syscall_profile_recorded = 1;
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
                int skip_kernel_exit_reclaim = 0;
                profile_trace_event_u64("exit.begin principal", exiting_principal);
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
                    if (exiting_proc) record_process_exit(exiting_pid, exiting_proc->exit_status);
                    profile_trace_event_u64("exit.close_fds pid", exiting_pid);
                    close_all_process_fds(g_proc);
                    const u64 tracked_exit_pages = exiting_proc ? vm_tracked_page_count() : 0;
                    const int tracked_unmap_ok = unmap_all_tracked_target_ranges();
                    /* Large Linux VM spaces are cheaper to reclaim from the ABI server's tracked map. */
                    skip_kernel_exit_reclaim = tracked_unmap_ok && tracked_exit_pages >= LINUX_EXIT_SKIP_RECLAIM_PAGE_THRESHOLD;
                    satisfy_waiters_after_exit_reply = 1;
                }
                remove_futex_waiters_for_principal(exiting_principal);
                if (exiting_proc) exiting_proc->used = 0;
                prime_reply_return_signal();
                if (satisfy_waiters_after_exit_reply) {
                    profile_trace_event_u64("exit.satisfy_waiters pid", exiting_pid);
                    (void)satisfy_pending_waiters_for_child(exiting_pid);
                    try_satisfy_pending_sigwaits();
                }
                profile_trace_event_u64("exit.reply principal", exiting_principal);
                const u64 exit_flags = TRAP_RESPONSE_FLAG_EXIT | (skip_kernel_exit_reclaim ? TRAP_RESPONSE_FLAG_SKIP_RECLAIM : 0);
                msg = reply_current_token(0, exit_flags);
                (void)msg;
                int root_exited = g_root_linux_principal_set && exiting_principal == g_root_linux_principal;
                if (!root_exited && g_root_linux_principal_set) {
                    const u64 root_status = syscall1(SYSCALL_GET_PROCESS_STATUS, g_root_linux_principal);
                    root_exited = (root_status & 0xff) != 1;
                }
                if (root_exited && !has_live_linux_process_state() && !has_open_pipe_state() && !has_known_child_slots()) {
                    process_exit(0);
                }
            }
            break;
        default: user_log("LinuxAbiServer: unhandled syscall\n"); user_log_hex_value(req->nr); msg = reply(errno_nosys(), 0); break;
        }
        if (profile_trace_this_syscall) {
            profile_trace_syscall_span(req->nr, req->caller_principal, syscall_trace_start_tick, syscall0(SYSCALL_GET_TICK_COUNT));
        }
        if (profile_this_syscall && !syscall_profile_recorded) {
            const u64 syscall_profile_end_tick = syscall0(SYSCALL_GET_TICK_COUNT);
            profile_record_syscall_ticks(req->nr, syscall_profile_end_tick - syscall_profile_start_tick);
        }
    }
}
