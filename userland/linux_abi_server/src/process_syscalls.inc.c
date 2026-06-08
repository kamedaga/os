static void close_all_process_fds(struct linux_process_state *proc) {
    if (!proc) return;
    for (u64 fd = 0; fd < LINUX_FD_MAX; fd++) {
        struct fd_entry *entry = &proc->fds[fd];
        if (entry->kind == FD_UNUSED) continue;
        const int is_pipe = fd_entry_is_pipe(entry);
        if (entry->kind == FD_FILE && (entry->fd_flags & O_ACCMODE) != O_RDONLY && entry->token != 0) {
            (void)vfs_request(FS_OP_CLOSE, entry->token, 0, 0, 0);
        }
        if (is_pipe) close_pipe_entry(entry);
        if (entry->kind == FD_SOCKET) close_socket_entry(entry);
        if (fd > 2 || is_pipe) entry->kind = FD_UNUSED;
    }
}

static void clear_tracked_target_ranges(void);

static int has_live_linux_process_state(void) {
    for (u64 i = 0; i < LINUX_PROCESS_MAX; i++) {
        if (!g_processes[i].used) continue;
        if (g_processes[i].exec_pending || g_processes[i].principal == 0) return 1;
        const u64 st = syscall1(SYSCALL_GET_PROCESS_STATUS, g_processes[i].principal);
        if ((st & 0xff) == 1) return 1;
    }
    return 0;
}

static u64 count_live_linux_process_states(void) {
    u64 count = 0;
    for (u64 i = 0; i < LINUX_PROCESS_MAX; i++) {
        if (!g_processes[i].used) continue;
        if (g_processes[i].exec_pending || g_processes[i].principal == 0) {
            count++;
            continue;
        }
        const u64 st = syscall1(SYSCALL_GET_PROCESS_STATUS, g_processes[i].principal);
        if ((st & 0xff) == 1) count++;
    }
    return count;
}

static u64 count_open_pipe_states(void) {
    u64 count = 0;
    for (u64 i = 0; i < PIPE_MAX; i++) {
        if (!g_pipes[i].used) continue;
        if (g_pipes[i].read_refs != 0 || g_pipes[i].write_refs != 0 || g_pipes[i].pending_read) count++;
    }
    return count;
}

static u64 count_known_child_slots(void) {
    u64 count = 0;
    for (u64 i = 0; i < LINUX_PROCESS_MAX; i++) {
        if (!g_processes[i].used) continue;
        for (u64 child = 0; child < LINUX_CHILD_MAX; child++) {
            if (g_processes[i].child_used[child]) count++;
        }
    }
    return count;
}

static u64 count_futex_waiters(void) {
    u64 count = 0;
    for (u64 i = 0; i < FUTEX_WAITER_MAX; i++) {
        if (g_futex_waiters[i].used) count++;
    }
    return count;
}

static u64 count_exit_records(void) {
    u64 count = 0;
    for (u64 i = 0; i < LINUX_PROCESS_MAX; i++) {
        if (g_exit_record_used[i]) count++;
    }
    return count;
}

static u64 count_socket_refs(void) {
    u64 count = 0;
    for (u64 i = 0; i < SOCKET_REF_MAX; i++) {
        if (g_socket_refs[i].used || g_socket_refs[i].refs != 0 || g_socket_refs[i].token != 0) count++;
    }
    return count;
}

static u64 linux_cleanup_residual_count(void) {
    return count_live_linux_process_states() +
        count_open_pipe_states() +
        count_known_child_slots() +
        count_futex_waiters() +
        count_exit_records() +
        count_socket_refs();
}

static void log_linux_cleanup_residuals(const char *label) {
    user_log("LinuxAbiServer: residual cleanup label=");
    user_log(label);
    user_log("\n");
    user_log("LinuxAbiServer: residual live_processes=");
    user_log_hex_value(count_live_linux_process_states());
    user_log("LinuxAbiServer: residual pipes=");
    user_log_hex_value(count_open_pipe_states());
    user_log("LinuxAbiServer: residual child_slots=");
    user_log_hex_value(count_known_child_slots());
    user_log("LinuxAbiServer: residual futex_waiters=");
    user_log_hex_value(count_futex_waiters());
    user_log("LinuxAbiServer: residual exit_records=");
    user_log_hex_value(count_exit_records());
    user_log("LinuxAbiServer: residual socket_refs=");
    user_log_hex_value(count_socket_refs());
}

static void finish_linux_abi_if_root_exited(u64 exiting_principal) {
    int root_exited = g_root_linux_principal_set && exiting_principal == g_root_linux_principal;
    if (!root_exited && g_root_linux_principal_set) {
        const u64 root_status = syscall1(SYSCALL_GET_PROCESS_STATUS, g_root_linux_principal);
        root_exited = (root_status & 0xff) != 1;
    }
    if (!root_exited || has_live_linux_process_state()) return;

    const u64 residuals = linux_cleanup_residual_count();
    if (residuals == 0) {
        process_exit(0);
    }
    log_linux_cleanup_residuals("root-exit");
    process_exit(2);
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

struct linux_rusage_timeval {
    i64 tv_sec;
    i64 tv_usec;
};

struct linux_rusage {
    struct linux_rusage_timeval ru_utime;
    struct linux_rusage_timeval ru_stime;
    i64 ru_maxrss;
    i64 ru_ixrss;
    i64 ru_idrss;
    i64 ru_isrss;
    i64 ru_minflt;
    i64 ru_majflt;
    i64 ru_nswap;
    i64 ru_inblock;
    i64 ru_oublock;
    i64 ru_msgsnd;
    i64 ru_msgrcv;
    i64 ru_nsignals;
    i64 ru_nvcsw;
    i64 ru_nivcsw;
};

static int zero_linux_rusage_current(u64 rusage_va) {
    if (rusage_va == 0) return 1;
    const struct linux_rusage usage = {0};
    return copy_to_target(rusage_va, &usage, sizeof(usage)) == sizeof(usage);
}

static int zero_linux_rusage_trap_target(u64 principal, u64 rusage_va) {
    if (rusage_va == 0) return 1;
    const struct linux_rusage usage = {0};
    return copy_to_trap_target(principal, rusage_va, &usage, sizeof(usage)) == sizeof(usage);
}

static struct ipc_message handle_getrusage(const struct trap_request *req) {
    const i64 who = (i64)req->args[0];
    const u64 rusage_va = req->args[1];
    if (who != 0 && who != -1 && who != 1) return reply(errno_inval(), 0);
    if (!zero_linux_rusage_current(rusage_va)) return reply(errno_fault(), 0);
    return reply(0, 0);
}

static void wait_child_slot_settled(u64 child_slot) {
    for (u64 i = 0; i < 16; i++) {
        const u64 st = syscall1(SYSCALL_GET_PROCESS_STATUS, child_slot);
        if ((st & 0xffu) != 1) break;
        wait_without_consuming_ipc_no_switch();
    }
    wait_without_consuming_ipc_no_switch();
}

static int reap_exited_child_for_current(struct linux_process_state *proc, i64 pid, u64 status_va, u64 rusage_va, u64 *child_out, int *fault) {
    *fault = 0;
    for (u64 i = 0; i < LINUX_CHILD_MAX; i++) {
        if (!proc->child_used[i]) continue;
        const u64 child = proc->child_slot[i];
        if (!wait_pid_matches_child(pid, child)) continue;
        u32 recorded_status = 0;
        if (take_process_exit_record(child, &recorded_status)) {
            wait_child_slot_settled(child);
            if (!write_wait_status_to_current(status_va, recorded_status)) {
                *fault = 1;
                return 0;
            }
            if (!zero_linux_rusage_current(rusage_va)) {
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
        wait_child_slot_settled(child);
        if (!write_wait_status_to_current(status_va, exit_code)) {
            *fault = 1;
            return 0;
        }
        if (!zero_linux_rusage_current(rusage_va)) {
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
            u32 exit_code = 0;
            if (!take_process_exit_record(child_slot, &exit_code)) {
                if (child_proc && child_proc->exec_pending) continue;
                const u64 child_principal = child_proc ? child_proc->principal : child_slot;
                const u64 st = syscall1(SYSCALL_GET_PROCESS_STATUS, child_principal);
                if ((st & 0xff) == 1) continue;
                exit_code = child_proc ? child_proc->exit_status : 0;
            }
            wait_child_slot_settled(child_slot);
            if (!write_wait_status_to_trap_target(proc->principal, proc->wait_status_va, exit_code)) result = errno_fault();
            if (!zero_linux_rusage_trap_target(proc->principal, proc->wait_rusage_va)) result = errno_fault();
            proc->child_used[i] = 0;
            if (child_proc) child_proc->used = 0;
            proc->wait_pending = 0;
            proc->wait_pid = 0;
            proc->wait_status_va = 0;
            proc->wait_rusage_va = 0;
            const u64 reply_status = reply_trap_target(proc->principal, result, 0);
            if (reply_status == SYSCALL_OK) (void)detach_reply_token();
            if (reply_status != SYSCALL_OK) {
                user_log("LinuxAbiServer: wait reply failed=");
                user_log_hex_value(reply_status);
            }
            satisfied = 1;
            break;
        }
    }
    if (!satisfied) {
        log_wait_state_for_miss(child_slot);
    }
    return satisfied;
}

static struct ipc_message terminate_current_linux_process_from_trap(
    u64 exiting_principal,
    u32 wait_status,
    u64 reply_result,
    u64 reply_flags
) {
    struct linux_process_state *exiting_proc = g_proc;
    const u64 exiting_pid = exiting_proc ? exiting_proc->pid : exiting_principal;

    if (exiting_proc) {
        exiting_proc->exit_status = wait_status;
        for (u64 i = 0; i < LINUX_PROCESS_MAX; i++) {
            struct linux_process_state *thread_proc = &g_processes[i];
            if (!thread_proc->used || thread_proc == exiting_proc || thread_proc->pid != exiting_pid) continue;
            thread_proc->exit_status = wait_status;
            if (thread_proc->clear_child_tid != 0) {
                const u32 zero = 0;
                (void)copy_to_trap_target(thread_proc->principal, thread_proc->clear_child_tid, &zero, sizeof(zero));
            }
            remove_futex_waiters_for_principal(thread_proc->principal);
            const u64 status = reply_trap_target(thread_proc->principal, 0, TRAP_RESPONSE_FLAG_EXIT);
            if (status == SYSCALL_OK) thread_proc->used = 0;
        }
        if (exiting_proc->clear_child_tid != 0) {
            const u32 zero = 0;
            (void)copy_to_target(exiting_proc->clear_child_tid, &zero, sizeof(zero));
            (void)wake_futex_waiters(exiting_pid, exiting_proc->clear_child_tid, 1);
        }
        record_process_exit(exiting_pid, exiting_proc->exit_status);
        close_all_process_fds(exiting_proc);
        clear_tracked_target_ranges();
        release_process_vm_object_tokens(exiting_proc);
        remove_futex_waiters_for_principal(exiting_principal);
        reply_vfork_parent_if_any(exiting_proc);
        exiting_proc->used = 0;
    }

    struct ipc_message msg;
    prime_reply_return_signal();
    if ((reply_flags & TRAP_RESPONSE_FLAG_EXIT) != 0) {
        exit_trap_target_no_wait(exiting_principal);
        try_satisfy_pending_sigwaits();
        (void)satisfy_pending_waiters_for_child(exiting_pid);
        msg = wait_ipc_timeout(1);
    } else {
        (void)satisfy_pending_waiters_for_child(exiting_pid);
        try_satisfy_pending_sigwaits();
        msg = reply_current_token(reply_result, reply_flags);
    }

    finish_linux_abi_if_root_exited(exiting_principal);
    return msg;
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

static void log_wait_state_for_miss(u64 child_slot) {
    (void)child_slot;
}

static void copy_process_state_for_fork_impl(struct linux_process_state *child, const struct linux_process_state *parent, u64 child_principal, int ref_pipe_fds) {
    child->used = 1;
    child->exec_pending = 0;
    child->exec_pending_principal = 0;
    child->exit_status = 0;
    child->pid = child_principal;
    child->tid = child_principal;
    child->pgid = parent->pgid;
    child->principal = child_principal;
    child->mmap_next_va = parent->mmap_next_va;
    child->brk_next_va = parent->brk_next_va;
    for (u64 i = 0; i < VM_REGION_MAX; i++) child->regions[i] = parent->regions[i];
    child->vm_object_token_count = 0;
    child->root_len = parent->root_len;
    for (u16 i = 0; i <= parent->root_len && i <= FS_MAX_PATH_BYTES; i++) child->root_path[i] = parent->root_path[i];
    child->cwd_len = parent->cwd_len;
    for (u16 i = 0; i <= parent->cwd_len && i <= FS_MAX_PATH_BYTES; i++) child->cwd[i] = parent->cwd[i];
    for (u64 i = 0; i < LINUX_CHILD_MAX; i++) child->child_used[i] = 0;
    child->wait_pending = 0;
    child->wait_pid = 0;
    child->wait_status_va = 0;
    child->wait_rusage_va = 0;
    child->sigwait_pending = 0;
    child->sigwait_set = 0;
    child->sigwait_info_va = 0;
    child->sigwait_deadline_tick = 0;
    child->clear_child_tid = 0;
    child->profile_enabled = parent->profile_enabled;
    child->profile_detail_enabled = parent->profile_detail_enabled;
    child->profile_verbose_enabled = parent->profile_verbose_enabled;
    child->fault_trace_enabled = parent->fault_trace_enabled;
    child->sigaltstack_sp = parent->sigaltstack_sp;
    child->sigaltstack_size = parent->sigaltstack_size;
    child->sigaltstack_flags = parent->sigaltstack_flags;
    child->blocked_signals = parent->blocked_signals;
    child->pending_signals = 0;
    child->timer_interrupt_signals = 0;
    child->itimer_real_expiry_tick = 0;
    child->itimer_real_interval_ticks = 0;
    for (u64 i = 0; i < LINUX_POSIX_TIMER_MAX; i++) {
        child->timers[i].used = 0;
        child->timers[i].timer_id = (i32)(i + 1);
        child->timers[i].clock_id = 0;
        child->timers[i].signo = 0;
        child->timers[i].notify = SIGEV_NONE;
        child->timers[i].value = 0;
        child->timers[i].expiry_tick = 0;
        child->timers[i].interval_ticks = 0;
        child->timers[i].overrun = 0;
    }
    for (u64 i = 0; i < 65; i++) {
        child->sig_handler[i] = parent->sig_handler[i];
        child->sig_flags[i] = parent->sig_flags[i];
        child->sig_restorer[i] = parent->sig_restorer[i];
        child->pending_signal_code[i] = 0;
    }
    for (u64 fd = 0; fd < LINUX_FD_MAX; fd++) {
        copy_fd_entry(&child->fds[fd], &parent->fds[fd]);
        if (ref_pipe_fds) {
            pipe_ref_fd(&child->fds[fd]);
            socket_ref_fd(&child->fds[fd]);
        }
    }
}

static void copy_process_state_for_fork(struct linux_process_state *child, const struct linux_process_state *parent, u64 child_principal) {
    copy_process_state_for_fork_impl(child, parent, child_principal, 1);
}

static void copy_process_state_for_clone_thread(struct linux_process_state *child, const struct linux_process_state *parent, u64 child_principal, u64 clear_child_tid) {
    copy_process_state_for_fork_impl(child, parent, child_principal, 0);
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

static int supported_clone_process_flags(u64 flags) {
    const u64 signal = flags & 0xffu;
    const u64 supported = (u64)CLONE_VM | CLONE_VFORK | CLONE_PARENT_SETTID | CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID | CLONE_DETACHED;
    if (signal != SIGCHLD && signal != 0) return 0;
    if ((flags & ~(supported | (u64)0xff)) != 0) return 0;
    return 1;
}

static int materialize_lazy_file_range(u64 start, u64 size, u64 prot);
static int materialize_file_vm_object_range(u64 start, u64 size, u64 prot);

static u16 read_le16(const u8 *bytes, u64 off) {
    return (u16)bytes[off] | ((u16)bytes[off + 1] << 8);
}

static u32 read_le32(const u8 *bytes, u64 off) {
    return (u32)bytes[off] |
        ((u32)bytes[off + 1] << 8) |
        ((u32)bytes[off + 2] << 16) |
        ((u32)bytes[off + 3] << 24);
}

static u64 read_le64(const u8 *bytes, u64 off) {
    u64 value = 0;
    for (u64 i = 0; i < 8; i++) value |= (u64)bytes[off + i] << (i * 8);
    return value;
}

static int materialize_all_spawn_regions(int share_vm) {
    for (;;) {
        int found = 0;
        for (u64 i = 0; i < VM_REGION_MAX; i++) {
            if (!g_regions[i].used || !g_regions[i].file_lazy) continue;
            found = 1;
            if (!materialize_lazy_file_range(g_regions[i].start, g_regions[i].size, g_regions[i].prot)) return 0;
            break;
        }
        if (!found) break;
    }
    if (!share_vm) return 1;
    for (;;) {
        int found = 0;
        for (u64 i = 0; i < VM_REGION_MAX; i++) {
            if (!g_regions[i].used || !g_regions[i].file_vm_object || (g_regions[i].prot & 0x2) == 0) continue;
            found = 1;
            if (!materialize_file_vm_object_range(g_regions[i].start, g_regions[i].size, g_regions[i].prot)) return 0;
            break;
        }
        if (!found) return 1;
    }
}

static int copy_source_page_to_process(u64 process_token, u64 source_process_slot, u64 va, u64 prot) {
    const u64 map_status = alloc_map_pages_to_process(process_token, va, 1, prot);
    if (map_status != SYSCALL_OK && map_status != SYSCALL_ERR_MAP) {
        user_log("LinuxAbiServer: clone map page failed va=");
        user_log_hex_value(va);
        user_log("LinuxAbiServer: clone map status=");
        user_log_hex_value(map_status);
        return 0;
    }
    const u64 copy_status = copy_from_process_to_process(process_token, source_process_slot, va, va, PAGE_BYTES);
    if (copy_status == SYSCALL_OK) return 1;

    u8 page[PAGE_BYTES];
    u64 copied = 0;
    while (copied < PAGE_BYTES) {
        const u64 chunk = 512;
        if (copy_from_target(va + copied, page + copied, chunk) != chunk) break;
        copied += chunk;
    }
    if (copied == PAGE_BYTES && copy_to_process(process_token, va, (u64)page, PAGE_BYTES) == SYSCALL_OK) {
        user_log("LinuxAbiServer: clone process copy fallback va=");
        user_log_hex_value(va);
        return 1;
    }

    user_log("LinuxAbiServer: clone process copy failed va=");
    user_log_hex_value(va);
    user_log("LinuxAbiServer: clone process copy status=");
    user_log_hex_value(copy_status);
    user_log("LinuxAbiServer: clone process copy source=");
    user_log_hex_value(source_process_slot);
    return 0;
}

static u64 vm_tracked_page_prot_or(u64 va, u64 fallback) {
    for (u64 i = 0; i < VM_REGION_MAX; i++) {
        if (!g_regions[i].used) continue;
        const u64 rs = g_regions[i].start;
        const u64 re = rs + g_regions[i].size;
        if (va >= rs && va < re) return g_regions[i].prot;
    }
    return fallback;
}

static int copy_present_pages_to_process(u64 process_token, u64 source_process_slot, u64 start, u64 end, u64 prot) {
    for (u64 va = start; va < end; va += PAGE_BYTES) {
        u8 probe[16];
        if (copy_from_target(va, probe, sizeof(probe)) != sizeof(probe)) continue;
        if (!copy_source_page_to_process(process_token, source_process_slot, va, vm_tracked_page_prot_or(va, prot))) return 0;
    }
    return 1;
}

static int share_present_pages_to_process(u64 process_token, u64 source_process_slot, u64 start, u64 end, u64 prot) {
    for (u64 va = start; va < end; va += PAGE_BYTES) {
        u8 probe[16];
        if (copy_from_target(va, probe, sizeof(probe)) != sizeof(probe)) continue;
        const u64 page_prot = vm_tracked_page_prot_or(va, prot);
        const u64 status = share_process_pages_to_process(process_token, source_process_slot, va, 1, page_prot);
        if (status != SYSCALL_OK && status != SYSCALL_ERR_MAP) {
            user_log("LinuxAbiServer: clone share page failed va=");
            user_log_hex_value(va);
            user_log("LinuxAbiServer: clone share prot=");
            user_log_hex_value(page_prot);
            user_log("LinuxAbiServer: clone share status=");
            user_log_hex_value(status);
            return 0;
        }
    }
    return 1;
}

static int copy_elf_image_pages_to_process(u64 process_token, u64 source_process_slot, u64 base, u64 *next_base_out) {
    enum { PT_LOAD = 1, PF_X = 1, PF_W = 2 };
    u8 ehdr[64];
    *next_base_out = 0;
    if (copy_from_target(base, ehdr, sizeof(ehdr)) != sizeof(ehdr)) return 1;
    if (ehdr[0] != 0x7f || ehdr[1] != 'E' || ehdr[2] != 'L' || ehdr[3] != 'F') return 1;
    if (ehdr[4] != 2 || ehdr[5] != 1 || ehdr[6] != 1) return 0;
    const u64 phoff = read_le64(ehdr, 32);
    const u16 phentsize = read_le16(ehdr, 54);
    const u16 phnum = read_le16(ehdr, 56);
    if (phentsize < 56 || phnum > 64) return 0;

    u64 image_end = base;
    u64 max_align = PAGE_BYTES;
    for (u64 i = 0; i < phnum; i++) {
        u8 phdr[56];
        if (copy_from_target(base + phoff + i * phentsize, phdr, sizeof(phdr)) != sizeof(phdr)) return 0;
        if (read_le32(phdr, 0) != PT_LOAD) continue;
        const u32 flags = read_le32(phdr, 4);
        const u64 vaddr = read_le64(phdr, 16);
        const u64 memsz = read_le64(phdr, 40);
        const u64 align = read_le64(phdr, 48);
        if (memsz == 0) continue;
        if (align > max_align) max_align = align;
        const u64 seg_start = base + (vaddr & ~(u64)(PAGE_BYTES - 1));
        const u64 seg_end = align_up(base + vaddr + memsz, PAGE_BYTES);
        if (seg_end > image_end) image_end = seg_end;
        u64 prot = 0x1;
        if ((flags & PF_W) != 0) {
            prot |= 0x2;
        } else if ((flags & PF_X) != 0) {
            prot |= 0x4;
        }
        if (!copy_present_pages_to_process(process_token, source_process_slot, seg_start, seg_end, prot)) return 0;
    }
    *next_base_out = align_up(image_end, max_align);
    return 1;
}

static int share_elf_image_pages_to_process(u64 process_token, u64 source_process_slot, u64 base, u64 *next_base_out) {
    enum { PT_LOAD = 1, PF_X = 1, PF_W = 2 };
    u8 ehdr[64];
    *next_base_out = 0;
    if (copy_from_target(base, ehdr, sizeof(ehdr)) != sizeof(ehdr)) return 1;
    if (ehdr[0] != 0x7f || ehdr[1] != 'E' || ehdr[2] != 'L' || ehdr[3] != 'F') return 1;
    if (ehdr[4] != 2 || ehdr[5] != 1 || ehdr[6] != 1) return 0;
    const u64 phoff = read_le64(ehdr, 32);
    const u16 phentsize = read_le16(ehdr, 54);
    const u16 phnum = read_le16(ehdr, 56);
    if (phentsize < 56 || phnum > 64) return 0;

    u64 image_end = base;
    u64 max_align = PAGE_BYTES;
    for (u64 i = 0; i < phnum; i++) {
        u8 phdr[56];
        if (copy_from_target(base + phoff + i * phentsize, phdr, sizeof(phdr)) != sizeof(phdr)) return 0;
        if (read_le32(phdr, 0) != PT_LOAD) continue;
        const u32 flags = read_le32(phdr, 4);
        const u64 vaddr = read_le64(phdr, 16);
        const u64 memsz = read_le64(phdr, 40);
        const u64 align = read_le64(phdr, 48);
        if (memsz == 0) continue;
        if (align > max_align) max_align = align;
        const u64 seg_start = base + (vaddr & ~(u64)(PAGE_BYTES - 1));
        const u64 seg_end = align_up(base + vaddr + memsz, PAGE_BYTES);
        if (seg_end > image_end) image_end = seg_end;
        u64 prot = 0x1;
        if ((flags & PF_W) != 0) {
            prot |= 0x2;
        } else if ((flags & PF_X) != 0) {
            prot |= 0x4;
        }
        if (!share_present_pages_to_process(process_token, source_process_slot, seg_start, seg_end, prot)) return 0;
    }
    *next_base_out = align_up(image_end, max_align);
    return 1;
}

static int copy_loaded_elf_images_to_process(u64 process_token, u64 source_process_slot) {
    u64 base = g_et_dyn_base_va;
    const u64 scan_end = g_et_dyn_base_va + 0x04000000ULL;
    for (u64 i = 0; i < 4 && base < scan_end; i++) {
        u64 next_base = 0;
        if (!copy_elf_image_pages_to_process(process_token, source_process_slot, base, &next_base)) return 0;
        if (next_base <= base) break;
        base = next_base;
    }
    return 1;
}

static int share_loaded_elf_images_to_process(u64 process_token, u64 source_process_slot) {
    u64 base = g_et_dyn_base_va;
    const u64 scan_end = g_et_dyn_base_va + 0x04000000ULL;
    for (u64 i = 0; i < 4 && base < scan_end; i++) {
        u64 next_base = 0;
        if (!share_elf_image_pages_to_process(process_token, source_process_slot, base, &next_base)) return 0;
        if (next_base <= base) break;
        base = next_base;
    }
    return 1;
}

static int copy_current_vm_to_process(u64 process_token, u64 source_process_slot, u64 parent_rsp, u64 child_rsp, int share_vm) {
    if (!materialize_all_spawn_regions(share_vm)) return 0;
    for (u64 i = 0; i < VM_REGION_MAX; i++) {
        if (!g_regions[i].used || g_regions[i].file_lazy) continue;
        if (!g_regions[i].file_backed && g_regions[i].prot == 0) continue;
        const u64 start = g_regions[i].start;
        const u64 page_count = align_up(g_regions[i].size, PAGE_BYTES) / PAGE_BYTES;
        if (share_vm) {
            if (!share_present_pages_to_process(process_token, source_process_slot, start, start + page_count * PAGE_BYTES, g_regions[i].prot)) return 0;
            continue;
        }
        u64 mapped = 0;
        while (mapped < page_count) {
            const u64 batch = min_u64(page_count - mapped, 64);
            if (alloc_map_pages_to_process(process_token, start + mapped * PAGE_BYTES, batch, g_regions[i].prot) != SYSCALL_OK) return 0;
            mapped += batch;
        }
        for (u64 page_index = 0; page_index < page_count; page_index++) {
            const u64 va = start + page_index * PAGE_BYTES;
            if (!copy_source_page_to_process(process_token, source_process_slot, va, g_regions[i].prot)) return 0;
        }
    }
    if (share_vm) {
        if (!share_loaded_elf_images_to_process(process_token, source_process_slot)) return 0;
    } else {
        if (!copy_present_pages_to_process(process_token, source_process_slot, g_user_low_va, g_user_low_va + 0x00800000ULL, 0x3)) return 0;
        if (!copy_loaded_elf_images_to_process(process_token, source_process_slot)) return 0;
    }
    const u64 parent_stack_page = parent_rsp & ~(u64)(PAGE_BYTES - 1);
    const u64 parent_stack_start = parent_stack_page > 0x200000 ? parent_stack_page - 0x200000 : 0;
    if (share_vm) {
        if (!share_present_pages_to_process(process_token, source_process_slot, parent_stack_start, parent_stack_page + 0x200000, 0x3)) return 0;
    } else if (!copy_present_pages_to_process(process_token, source_process_slot, parent_stack_start, parent_stack_page + 0x200000, 0x3)) return 0;
    if (child_rsp != 0) {
        const u64 child_stack_page = child_rsp & ~(u64)(PAGE_BYTES - 1);
        const u64 child_stack_start = child_stack_page > 0x200000 ? child_stack_page - 0x200000 : 0;
        if (share_vm) {
            if (!share_present_pages_to_process(process_token, source_process_slot, child_stack_start, child_stack_page + 0x200000, 0x3)) return 0;
        } else if (!copy_present_pages_to_process(process_token, source_process_slot, child_stack_start, child_stack_page + 0x200000, 0x3)) return 0;
    }
    return 1;
}

struct abi_child_spawn {
    u64 token;
    u64 slot;
};

static struct abi_child_spawn prepare_abi_child_from_request(const struct trap_request *req, u64 child_rsp, u64 child_fs_base, int share_vm) {
    struct abi_child_spawn failed = {0, 0};
    const u64 process_token = create_suspended_process();
    const u64 token_slot = process_builder_token_slot(process_token);
    if (token_slot == 0) return failed;

    u64 request_va = 0;
    if (!ensure_child_trap_request_page_mapped(token_slot, &request_va)) {
        abort_process(process_token);
        return failed;
    }
    if (set_process_abi_trap_delegate(process_token, LINUX_ABI_ENDPOINT_ID, syscall0(SYSCALL_GET_PROCESS_SLOT), 1, request_va) != SYSCALL_OK) {
        abort_process(process_token);
        return failed;
    }
    if (!copy_current_vm_to_process(process_token, req->caller_principal, req->rsp, child_rsp, share_vm)) {
        abort_process(process_token);
        return failed;
    }

    struct abi_trap_user_context ctx;
    fill_user_context_from_request(&ctx, req, 0);
    if (child_rsp != 0) ctx.rsp = child_rsp;
    if (child_fs_base != 0) ctx.fs_base = child_fs_base;
    if (set_process_initial_context(process_token, &ctx) != SYSCALL_OK) {
        abort_process(process_token);
        return failed;
    }
    struct abi_child_spawn spawn = {process_token, token_slot};
    return spawn;
}

static int start_prepared_abi_child(struct abi_child_spawn spawn) {
    if (spawn.token == 0 || spawn.slot == 0) return 0;
    const u64 started = decode_spawned_process_slot(start_process(spawn.token));
    return started == spawn.slot;
}

static struct ipc_message handle_clone_thread(const struct trap_request *req) {
    const u64 flags = req->args[0];
    const u64 child_stack = req->args[1];
    const u64 parent_tidptr = req->args[2];
    const u64 child_tidptr = req->args[3];
    const u64 tls = req->args[4];
    if (child_stack == 0 || tls == 0 || !supported_clone_thread_flags(flags)) return reply(errno_inval(), 0);

    struct abi_child_spawn spawn = prepare_abi_child_from_request(req, child_stack, tls, 1);
    const u64 child_slot = spawn.slot;
    if (child_slot == 0) {
        user_log("LinuxAbiServer: clone thread spawn failed\n");
        return reply(errno_busy(), 0);
    }
    struct linux_process_state *child = alloc_process_state_for_new_principal(child_slot);
    if (!child) {
        user_log("LinuxAbiServer: clone thread state failed\n");
        user_log_hex_value(child_slot);
        abort_process(spawn.token);
        return reply(errno_busy(), 0);
    }
    copy_process_state_for_clone_thread(child, g_proc, child_slot, (flags & CLONE_CHILD_CLEARTID) != 0 ? child_tidptr : 0);
    const u32 child_tid32 = (u32)child_slot;
    if ((flags & CLONE_PARENT_SETTID) != 0 && parent_tidptr != 0) {
        if (copy_to_target(parent_tidptr, &child_tid32, sizeof(child_tid32)) != sizeof(child_tid32)) {
            user_log("LinuxAbiServer: clone parent_tid write failed\n");
            user_log_hex_value(parent_tidptr);
            abort_process(spawn.token);
            child->used = 0;
            return reply(errno_fault(), 0);
        }
    }
    if ((flags & CLONE_CHILD_SETTID) != 0 && child_tidptr != 0) {
        if (copy_to_trap_target(child_slot, child_tidptr, &child_tid32, sizeof(child_tid32)) != sizeof(child_tid32)) {
            user_log("LinuxAbiServer: clone child_tid write failed\n");
            user_log_hex_value(child_tidptr);
            abort_process(spawn.token);
            child->used = 0;
            return reply(errno_fault(), 0);
        }
    }
    if (!start_prepared_abi_child(spawn)) {
        user_log("LinuxAbiServer: clone thread start failed\n");
        user_log_hex_value(child_slot);
        abort_process(spawn.token);
        child->used = 0;
        return reply(errno_busy(), 0);
    }
    prime_reply_return_signal();
    return reply(child_slot, 0);
}

static struct ipc_message handle_fork_like(const struct trap_request *req, int clone_form) {
    u64 child_stack = 0;
    u64 parent_tidptr = 0;
    u64 child_tidptr = 0;
    u64 flags = SIGCHLD;
    if (clone_form) {
        flags = req->args[0];
        child_stack = req->args[1];
        parent_tidptr = req->args[2];
        child_tidptr = req->args[3];
        if ((flags & CLONE_THREAD) != 0) return handle_clone_thread(req);
        if (!supported_clone_process_flags(flags)) return reply(errno_inval(), 0);
    }
    const int vfork_form = clone_form && (flags & CLONE_VFORK) != 0;
    const int share_vm = clone_form && (flags & CLONE_VM) != 0;
    struct abi_child_spawn spawn = prepare_abi_child_from_request(req, child_stack, 0, share_vm);
    const u64 child_slot = spawn.slot;
    if (child_slot == 0) return reply(errno_busy(), 0);
    const u64 child_pid = alloc_linux_pid();
    if (child_pid == 0) {
        abort_process(spawn.token);
        return reply(errno_busy(), 0);
    }
    discard_process_exit_records(child_pid);
    remove_child_slot(g_proc, child_pid);
    struct linux_process_state *child = alloc_process_state_for_new_principal(child_slot);
    if (!child) {
        abort_process(spawn.token);
        return reply(errno_busy(), 0);
    }
    copy_process_state_for_fork(child, g_proc, child_pid);
    child->principal = child_slot;
    if (clone_form && (flags & CLONE_CHILD_CLEARTID) != 0) child->clear_child_tid = child_tidptr;
    if (vfork_form) {
        child->vfork_parent_principal = req->caller_principal;
        child->vfork_parent_result = child_pid;
        set_vfork_parent_for_principal(child_slot, req->caller_principal, child_pid);
    }
    (void)add_child_slot(g_proc, child_pid);
    const u32 child_pid32 = (u32)child_pid;
    if (clone_form && (flags & CLONE_PARENT_SETTID) != 0 && parent_tidptr != 0) {
        if (copy_to_target(parent_tidptr, &child_pid32, sizeof(child_pid32)) != sizeof(child_pid32)) {
            abort_process(spawn.token);
            remove_child_slot(g_proc, child_pid);
            child->used = 0;
            return reply(errno_fault(), 0);
        }
    }
    if (clone_form && (flags & CLONE_CHILD_SETTID) != 0 && child_tidptr != 0) {
        if (copy_to_trap_target(child_slot, child_tidptr, &child_pid32, sizeof(child_pid32)) != sizeof(child_pid32)) {
            abort_process(spawn.token);
            remove_child_slot(g_proc, child_pid);
            child->used = 0;
            return reply(errno_fault(), 0);
        }
    }
    if (!start_prepared_abi_child(spawn)) {
        abort_process(spawn.token);
        remove_child_slot(g_proc, child_pid);
        child->used = 0;
        return reply(errno_busy(), 0);
    }
    if (vfork_form) {
        (void)detach_reply_token();
        return wait_ipc();
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
            record_process_exit(proc->pid, proc->exit_status);
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

static int linux_signal_default_terminates(u64 sig);
static struct ipc_message terminate_linux_process_by_signal(const struct trap_request *req, struct linux_process_state *target, u64 sig);

static void trace_signal_target(const char *event, const struct linux_process_state *target, u64 sig) {
    if (!profile_trace_enabled()) return;
    profile_trace_prefix(event);
    user_log(" sig=");
    user_log_dec_value(sig);
    if (target) {
        user_log(" target_pid=");
        user_log_dec_value(target->pid);
        user_log(" target_tid=");
        user_log_dec_value(target->tid);
        user_log(" target_principal=");
        user_log_dec_value(target->principal);
        user_log(" handler=");
        user_log_hex_inline(sig < 65 ? target->sig_handler[sig] : 0);
        user_log(" mask=");
        user_log_hex_inline(target->blocked_signals);
        user_log(" pending=");
        user_log_hex_inline(target->pending_signals);
    }
    user_log("\n");
}

static struct ipc_message handle_kill(const struct trap_request *req) {
    const i64 pid = (i64)req->args[0];
    const u64 sig = req->args[1];
    if (sig >= 65) return reply(errno_inval(), 0);
    if (pid == 0 || pid == -1) return reply(0, 0);
    const u64 abs_pid = pid < 0 ? (u64)(-pid) : (u64)pid;
    if (abs_pid == 0) return reply(0, 0);
    struct linux_process_state *target = 0;
    for (u64 i = 0; i < LINUX_PROCESS_MAX; i++) {
        struct linux_process_state *proc = &g_processes[i];
        if (!proc->used) continue;
        if (proc->pid == abs_pid || proc->tid == abs_pid) {
            target = proc;
            break;
        }
    }
    if (target) {
        trace_signal_target("kill.target", target, sig);
        if (sig == 0) return reply(0, 0);
        if (target->sig_handler[sig] == 1) return reply(0, 0);
        if (target->sig_handler[sig] != 0) {
            queue_process_signal_code(target, sig, SI_USER);
            prime_reply_return_signal();
            return reply(0, 0);
        }
        if (!linux_signal_default_terminates(sig)) return reply(0, 0);
        return terminate_linux_process_by_signal(req, target, sig);
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

static struct ipc_message terminate_linux_process_by_signal(const struct trap_request *req, struct linux_process_state *target, u64 sig) {
    if (!target || sig == 0 || sig >= 65) return reply(errno_inval(), 0);
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

    if (record_exit) record_process_exit(target_pid, wait_status);
    if (exits_current) {
        close_all_process_fds(g_proc);
        if (g_proc) g_proc->used = 0;
        prime_reply_return_signal();
        exit_trap_target_no_wait(current_principal);
        (void)satisfy_pending_waiters_for_child(target_pid);
        struct ipc_message msg = wait_ipc();
        return msg;
    }

    close_all_process_fds(target);
    target->used = 0;
    prime_reply_return_signal();
    (void)satisfy_pending_waiters_for_child(target_pid);
    return reply(0, 0);
}

static struct ipc_message handle_thread_signal(u64 tgid, u64 tid, u64 sig, const struct trap_request *req) {
    if (sig >= 65) return reply(errno_inval(), 0);
    struct linux_process_state *target = process_state_for_tid(tid);
    if (!target) return reply(errno_noent(), 0);
    if (tgid != 0 && target->pid != tgid) return reply(errno_noent(), 0);
    trace_signal_target("thread_signal.target", target, sig);
    if (sig == 0) return reply(0, 0);
    if (target->sig_handler[sig] == 1) return reply(0, 0);
    if (target->sig_handler[sig] != 0) {
        queue_process_signal_code(target, sig, tgid != 0 ? SI_TKILL : SI_USER);
        prime_reply_return_signal();
        return reply(0, 0);
    }
    if (!linux_signal_default_terminates(sig)) return reply(0, 0);
    return terminate_linux_process_by_signal(req, target, sig);
}

static struct ipc_message handle_tkill(const struct trap_request *req) {
    return handle_thread_signal(0, req->args[0], req->args[1], req);
}

static struct ipc_message handle_tgkill(const struct trap_request *req) {
    return handle_thread_signal(req->args[0], req->args[1], req->args[2], req);
}

static struct ipc_message handle_wait4(const struct trap_request *req) {
    const i64 pid = (i64)req->args[0];
    const u64 status_va = req->args[1];
    const u64 options = req->args[2];
    const u64 rusage_va = req->args[3];
    const u64 supported_options = (u64)WNOHANG | (u64)WUNTRACED | (u64)WCONTINUED;
    if ((options & ~supported_options) != 0) return reply(errno_inval(), 0);
    u64 child = 0;
    int fault = 0;
    if (reap_exited_child_for_current(g_proc, pid, status_va, rusage_va, &child, &fault)) {
        profile_trace_event_u64("wait4.reap child", child);
        return reply(child, 0);
    }
    if (fault) return reply(errno_fault(), 0);
    if ((options & WNOHANG) != 0) return reply(0, 0);
    int has_matching_child = 0;
    for (u64 i = 0; i < LINUX_CHILD_MAX; i++) {
        if (g_proc->child_used[i] && wait_pid_matches_child(pid, g_proc->child_slot[i])) has_matching_child = 1;
    }
    if (!has_matching_child) return reply(errno_child(), 0);
    if (g_proc->wait_pending) {
        (void)detach_reply_token();
        return wait_ipc();
    }
    profile_trace_event_u64("wait4.pending pid", (u64)pid);
    g_proc->wait_pending = 1;
    g_proc->wait_pid = pid;
    g_proc->wait_status_va = status_va;
    g_proc->wait_rusage_va = rusage_va;
    const u64 detach_status = detach_reply_token();
    if (detach_status != SYSCALL_OK) {
        g_proc->wait_pending = 0;
        g_proc->wait_pid = 0;
        g_proc->wait_status_va = 0;
        g_proc->wait_rusage_va = 0;
        return reply(errno_again(), 0);
    }
    for (u64 i = 0; i < LINUX_CHILD_MAX; i++) {
        if (!g_proc->child_used[i] || !wait_pid_matches_child(pid, g_proc->child_slot[i])) continue;
        if (satisfy_pending_waiters_for_child(g_proc->child_slot[i])) return wait_ipc_timeout(1);
    }
    return wait_ipc();
}
