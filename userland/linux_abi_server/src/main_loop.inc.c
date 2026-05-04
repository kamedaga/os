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
    g_exec_loader_vm_token = cfg->exec_loader_vm_token;
    g_standard_interpreter_vm_token = cfg->standard_interpreter_vm_token;
    g_standard_interpreter_bytes = cfg->standard_interpreter_file_bytes;
    g_exec_path_len = cfg->exec_path_bytes <= FS_MAX_PATH_BYTES ? cfg->exec_path_bytes : FS_MAX_PATH_BYTES;
    for (u16 i = 0; i < g_exec_path_len; i++) g_exec_path[i] = cfg->exec_path[i];
    g_exec_path[g_exec_path_len] = 0;
    const u64 request_page_status = alloc_map_pages(trap_request_page_va, 1, 0x1);
    if (request_page_status != SYSCALL_OK) { user_log("LinuxAbiServer: request page map failed\n"); user_log_hex_value(request_page_status); for (;;) __asm__ volatile("pause"); }
    init_process_tables();
    (void)install_self_wake_endpoint();
    if (!connect_vfs_from_registry()) user_log("LinuxAbiServer: vfs connect failed\n");
    cfg->status = LINUX_ABI_BOOTSTRAP_READY;
    user_log("LinuxAbiServer: started\n");
    struct ipc_message msg = reply(0, 0);
    for (;;) {
        start_deferred_trap_targets();
        flush_deferred_pipe_wakes();
        if (msg.status != SYSCALL_OK) { msg = wait_ipc(); continue; }
        if (msg.request_va == 0) { msg = wait_ipc(); continue; }
        if (!is_known_trap_request_page(msg.request_va)) { msg = reply(errno_inval(), 0); continue; }
        const struct trap_request req_snapshot = *(const struct trap_request *)msg.request_va;
        const struct trap_request *req = &req_snapshot;
        if (req->magic != TRAP_MAGIC || req->version != TRAP_VERSION) { user_log("LinuxAbiServer: bad request header\n"); msg = reply(errno_inval(), 0); continue; }
        if (!g_root_linux_principal_set) {
            g_root_linux_principal = req->caller_principal;
            g_root_linux_principal_set = 1;
        }
        g_proc = process_state_for(req->caller_principal);
        if (!g_proc) { msg = reply(errno_busy(), 0); continue; }
        switch (req->nr) {
        case LINUX_SYS_READ: msg = handle_read(req); break;
        case LINUX_SYS_WRITE: msg = handle_write(req); break;
        case LINUX_SYS_WRITEV: msg = handle_writev(req); break;
        case LINUX_SYS_PIPE: msg = handle_pipe2(req, 0); break;
        case LINUX_SYS_PIPE2: msg = handle_pipe2(req, 1); break;
        case LINUX_SYS_OPEN: msg = handle_openat(req, 1); break;
        case LINUX_SYS_OPENAT: msg = handle_openat(req, 0); break;
        case LINUX_SYS_CLOSE: msg = handle_close(req); break;
        case LINUX_SYS_DUP: msg = handle_dup(req); break;
        case LINUX_SYS_DUP2: msg = handle_dup2_like(req, 0); break;
        case LINUX_SYS_DUP3: msg = handle_dup2_like(req, 1); break;
        case LINUX_SYS_CLONE: msg = handle_fork_like(req, 1); break;
        case LINUX_SYS_FORK: msg = handle_fork_like(req, 0); break;
        case LINUX_SYS_VFORK: msg = handle_fork_like(req, 0); break;
        case LINUX_SYS_WAIT4: msg = handle_wait4(req); break;
        case LINUX_SYS_FCNTL: msg = handle_fcntl(req); break;
        case LINUX_SYS_STAT: case LINUX_SYS_LSTAT: msg = handle_newfstatat(req, 1); break;
        case LINUX_SYS_FSTAT: msg = handle_fstat(req); break;
        case LINUX_SYS_NEWFSTATAT: msg = handle_newfstatat(req, 0); break;
        case LINUX_SYS_PREAD64: msg = handle_pread64(req); break;
        case LINUX_SYS_GETDENTS64: msg = handle_getdents64(req); break;
        case LINUX_SYS_LSEEK: msg = handle_lseek(req); break;
        case LINUX_SYS_ACCESS: msg = handle_access(req); break;
        case LINUX_SYS_GETCWD: msg = handle_getcwd(req); break;
        case LINUX_SYS_CHDIR: msg = handle_chdir(req); break;
        case LINUX_SYS_UNLINK: msg = handle_unlinkat(req, 1); break;
        case LINUX_SYS_UNLINKAT: msg = handle_unlinkat(req, 0); break;
        case LINUX_SYS_READLINK: msg = handle_readlink(req); break;
        case LINUX_SYS_UNAME: msg = handle_uname(req); break;
        case LINUX_SYS_CLOCK_GETTIME: msg = handle_clock_gettime(req); break;
        case LINUX_SYS_MEMBARRIER: msg = handle_membarrier(req); break;
        case LINUX_SYS_EXECVE: msg = handle_execve(req); break;
        case LINUX_SYS_MMAP: msg = handle_mmap(req); break;
        case LINUX_SYS_BRK: msg = handle_brk(req); break;
        case LINUX_SYS_MPROTECT: msg = handle_mprotect(req); break;
        case LINUX_SYS_MUNMAP: msg = handle_munmap(req); break;
        case LINUX_SYS_ARCH_PRCTL: msg = handle_arch_prctl(req); break;
        case LINUX_SYS_RT_SIGACTION: msg = handle_rt_sigaction(req); break;
        case LINUX_SYS_RT_SIGPROCMASK: msg = handle_rt_sigprocmask(req); break;
        case LINUX_SYS_SET_TID_ADDRESS: msg = handle_set_tid_address(req); break;
        case LINUX_SYS_FUTEX: msg = handle_futex(req); break;
        case LINUX_SYS_IOCTL: case LINUX_SYS_MADVISE: case LINUX_SYS_SET_ROBUST_LIST: case LINUX_SYS_PRLIMIT64: case LINUX_SYS_RSEQ: msg = reply(0, 0); break;
        case LINUX_SYS_GETPID: msg = reply(g_proc && g_proc->pid != 0 ? g_proc->pid : 1, 0); break;
        case LINUX_SYS_GETTID: msg = reply(g_proc && g_proc->tid != 0 ? g_proc->tid : 1, 0); break;
        case LINUX_SYS_GETPPID: msg = reply(1, 0); break;
        case LINUX_SYS_GETUID: case LINUX_SYS_GETGID: case LINUX_SYS_GETEUID: case LINUX_SYS_GETEGID: msg = reply(0, 0); break;
        case LINUX_SYS_UMASK: msg = reply(022, 0); break;
        case LINUX_SYS_GETRANDOM: msg = reply(errno_again(), 0); break;
        case LINUX_SYS_EXIT:
        case LINUX_SYS_EXIT_GROUP:
            {
                const u64 exiting_principal = req->caller_principal;
                struct linux_process_state *exiting_proc = g_proc;
                const u64 exiting_pid = exiting_proc ? exiting_proc->pid : exiting_principal;
                const int exiting_thread = exiting_proc && exiting_proc->tid != exiting_proc->pid;
                if (exiting_proc) exiting_proc->exit_status = (u32)(req->args[0] & 0xffu);
                if (exiting_proc && exiting_proc->clear_child_tid != 0) {
                    const u32 zero = 0;
                    (void)copy_to_target(exiting_proc->clear_child_tid, &zero, sizeof(zero));
                    (void)wake_futex_waiters(exiting_pid, exiting_proc->clear_child_tid, 1);
                }
                if (!exiting_thread) {
                    if (exiting_proc) record_process_exit(exiting_pid, exiting_proc->exit_status);
                    (void)satisfy_pending_waiters_for_child(exiting_pid);
                    close_all_process_fds(g_proc);
                }
                remove_futex_waiters_for_principal(exiting_principal);
                if (exiting_proc) exiting_proc->used = 0;
                prime_reply_return_signal();
                msg = reply(0, TRAP_RESPONSE_FLAG_EXIT);
                (void)msg;
                int root_exited = g_root_linux_principal_set && exiting_principal == g_root_linux_principal;
                if (!root_exited && g_root_linux_principal_set) {
                    const u64 root_status = syscall1(SYSCALL_GET_PROCESS_STATUS, g_root_linux_principal);
                    root_exited = (root_status & 0xff) != 1;
                }
                if (root_exited && !has_live_linux_process_state() && !has_open_pipe_state() && !has_known_child_slots()) {
                    user_log("LinuxAbiServer: companion exit\n");
                    process_exit(0);
                }
            }
            break;
        default: user_log("LinuxAbiServer: unhandled syscall\n"); user_log_hex_value(req->nr); msg = reply(errno_nosys(), 0); break;
        }
    }
}
