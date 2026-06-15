static int fd_clone_into(u64 dst, u64 src, u32 descriptor_flags) {
    if (!fd_valid(src) || dst >= LINUX_FD_MAX) return 0;
    copy_fd_entry(&g_fds[dst], &g_fds[src]);
    g_fds[dst].fd_flags = (u32)((g_fds[dst].fd_flags & ~((u32)FD_INTERNAL_CLOEXEC)) | descriptor_flags);
    if (fd_entry_is_pipe(&g_fds[dst])) g_prof.pipe_dup_refs++;
    pipe_ref_fd(&g_fds[dst]);
    socket_ref_fd(&g_fds[dst]);
    sync_fd_to_thread_group(dst);
    return 1;
}

static int alloc_fd_at_least(u64 min_fd) {
    if (min_fd >= LINUX_FD_MAX) return -1;
    for (u64 i = min_fd; i < LINUX_FD_MAX; i++) if (g_fds[i].kind == FD_UNUSED) return (int)i;
    return -1;
}

enum { EFD_SEMAPHORE = 1 };

static struct ipc_message handle_eventfd2(const struct trap_request *req) {
    const u64 initval = req->args[0] & 0xffffffffULL;
    const u64 flags = req->args[1];
    if ((flags & ~(u64)(EFD_SEMAPHORE | O_CLOEXEC | O_NONBLOCK)) != 0) return reply(errno_inval(), 0);
    const int fd = alloc_fd();
    if (fd < 0) return reply(errno_busy(), 0);
    g_fds[(u64)fd].kind = FD_EVENTFD;
    g_fds[(u64)fd].token = initval;
    g_fds[(u64)fd].offset = 0;
    g_fds[(u64)fd].size = 0;
    g_fds[(u64)fd].fd_flags = (u32)((flags & O_NONBLOCK) | ((flags & O_CLOEXEC) != 0 ? FD_INTERNAL_CLOEXEC : 0));
    g_fds[(u64)fd].mode_bits = (flags & EFD_SEMAPHORE) != 0 ? EFD_SEMAPHORE : 0;
    g_fds[(u64)fd].object_kind = FS_OBJECT_FILE;
    g_fds[(u64)fd].path_len = 0;
    g_fds[(u64)fd].path[0] = 0;
    sync_fd_to_thread_group((u64)fd);
    return reply((u64)fd, 0);
}

static u64 eventfd_read_to_target(u64 fd, u64 dst, u64 len, int *fault) {
    *fault = 0;
    if (!fd_valid(fd) || g_fds[fd].kind != FD_EVENTFD) return errno_badf();
    if (len < sizeof(u64)) return errno_inval();
    if (g_fds[fd].token == 0) return errno_again();
    u64 value = g_fds[fd].token;
    if ((g_fds[fd].mode_bits & EFD_SEMAPHORE) != 0) {
        value = 1;
        g_fds[fd].token--;
    } else {
        g_fds[fd].token = 0;
    }
    if (copy_to_target(dst, &value, sizeof(value)) != sizeof(value)) {
        *fault = 1;
        return 0;
    }
    sync_fd_to_thread_group(fd);
    return sizeof(value);
}

static u64 eventfd_write_from_target(u64 fd, u64 src, u64 len, int *fault) {
    *fault = 0;
    if (!fd_valid(fd) || g_fds[fd].kind != FD_EVENTFD) return errno_badf();
    if (len < sizeof(u64)) return errno_inval();
    u64 value = 0;
    if (copy_from_target(src, &value, sizeof(value)) != sizeof(value)) {
        *fault = 1;
        return 0;
    }
    if (value == ~0ULL) return errno_inval();
    if (~0ULL - g_fds[fd].token <= value) return errno_again();
    g_fds[fd].token += value;
    sync_fd_to_thread_group(fd);
    return sizeof(value);
}

static struct ipc_message handle_pipe2(const struct trap_request *req, int has_flags) {
    const u64 pipefd_va = req->args[0];
    const u64 flags = has_flags ? req->args[1] : 0;
    if ((flags & ~(u64)(O_CLOEXEC | O_NONBLOCK)) != 0) return reply(errno_inval(), 0);
    g_prof.pipe_create_calls++;
    const int pipe_slot = alloc_pipe_slot();
    if (pipe_slot < 0) { g_prof.pipe_create_busy++; return reply(errno_busy(), 0); }
    const int read_fd = alloc_fd_at_least(0);
    if (read_fd < 0) { g_prof.pipe_create_busy++; return reply(errno_busy(), 0); }
    g_fds[(u64)read_fd].kind = FD_PIPE_READ;
    const int write_fd = alloc_fd_at_least(0);
    if (write_fd < 0) { g_fds[(u64)read_fd].kind = FD_UNUSED; g_prof.pipe_create_busy++; return reply(errno_busy(), 0); }

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
    g_fds[(u64)read_fd].fd_flags = (u32)((flags & O_NONBLOCK) | ((flags & O_CLOEXEC) != 0 ? FD_INTERNAL_CLOEXEC : 0));
    g_fds[(u64)read_fd].mode_bits = FS_FILE_MODE;
    g_fds[(u64)read_fd].object_kind = FS_OBJECT_FILE;
    g_fds[(u64)read_fd].pipe_id = (u8)pipe_slot;
    g_fds[(u64)read_fd].path_len = 0;
    g_fds[(u64)read_fd].path[0] = 0;

    g_fds[(u64)write_fd].kind = FD_PIPE_WRITE;
    g_fds[(u64)write_fd].token = 0;
    g_fds[(u64)write_fd].offset = 0;
    g_fds[(u64)write_fd].size = 0;
    g_fds[(u64)write_fd].fd_flags = (u32)((flags & O_NONBLOCK) | ((flags & O_CLOEXEC) != 0 ? FD_INTERNAL_CLOEXEC : 0));
    g_fds[(u64)write_fd].mode_bits = FS_FILE_MODE;
    g_fds[(u64)write_fd].object_kind = FS_OBJECT_FILE;
    g_fds[(u64)write_fd].pipe_id = (u8)pipe_slot;
    g_fds[(u64)write_fd].path_len = 0;
    g_fds[(u64)write_fd].path[0] = 0;
    sync_fd_to_thread_group((u64)read_fd);
    sync_fd_to_thread_group((u64)write_fd);

    const u32 read_fd32 = (u32)read_fd;
    const u32 write_fd32 = (u32)write_fd;
    if (copy_to_target(pipefd_va, &read_fd32, sizeof(read_fd32)) != sizeof(read_fd32) ||
        copy_to_target(pipefd_va + sizeof(read_fd32), &write_fd32, sizeof(write_fd32)) != sizeof(write_fd32)) {
        g_fds[(u64)read_fd].kind = FD_UNUSED;
        g_fds[(u64)write_fd].kind = FD_UNUSED;
        g_pipes[(u64)pipe_slot].used = 0;
        g_prof.pipe_create_faults++;
        return reply(errno_fault(), 0);
    }
    return reply(0, 0);
}

static struct ipc_message handle_epoll_create1(const struct trap_request *req) {
    const u64 flags = req->args[0];
    if ((flags & ~(u64)O_CLOEXEC) != 0) return reply(errno_inval(), 0);
    const int fd = alloc_fd();
    if (fd < 0) return reply(errno_busy(), 0);
    g_fds[(u64)fd].kind = FD_EPOLL;
    g_fds[(u64)fd].token = 0;
    g_fds[(u64)fd].offset = 0;
    g_fds[(u64)fd].size = 0;
    g_fds[(u64)fd].fd_flags = (flags & O_CLOEXEC) != 0 ? FD_INTERNAL_CLOEXEC : 0;
    g_fds[(u64)fd].mode_bits = FS_FILE_MODE;
    g_fds[(u64)fd].object_kind = FS_OBJECT_FILE;
    g_fds[(u64)fd].path_len = 0;
    g_fds[(u64)fd].path[0] = 0;
    for (u64 i = 0; i < EPOLL_WATCH_MAX; i++) g_fds[(u64)fd].epoll_watches[i] = (struct epoll_watch){0};
    sync_fd_to_thread_group((u64)fd);
    return reply((u64)fd, 0);
}

enum { EPOLL_CTL_ADD = 1, EPOLL_CTL_DEL = 2, EPOLL_CTL_MOD = 3 };

static int read_epoll_event(u64 event_va, u32 *events_out, u64 *data_out) {
    u32 events = 0;
    u64 data = 0;
    if (event_va == 0) return 0;
    if (copy_from_target(event_va, &events, sizeof(events)) != sizeof(events)) return 0;
    if (copy_from_target(event_va + 4, &data, sizeof(data)) != sizeof(data)) return 0;
    *events_out = events;
    *data_out = data;
    return 1;
}

static int write_epoll_event(u64 event_va, u32 events, u64 data) {
    return copy_to_target(event_va, &events, sizeof(events)) == sizeof(events) &&
        copy_to_target(event_va + 4, &data, sizeof(data)) == sizeof(data);
}

static int write_epoll_event_to_principal(u64 principal, u64 event_va, u32 events, u64 data) {
    return copy_to_trap_target(principal, event_va, &events, sizeof(events)) == sizeof(events) &&
        copy_to_trap_target(principal, event_va + 4, &data, sizeof(data)) == sizeof(data);
}

static int fd_epoll_supported(u64 fd) {
    if (!fd_valid(fd)) return 0;
    switch (g_fds[fd].kind) {
    case FD_PIPE_READ:
    case FD_PIPE_WRITE:
    case FD_TTY:
    case FD_SOCKET:
    case FD_EVENTFD:
        return 1;
    default:
        return 0;
    }
}

static void trace_epoll_ctl(u64 epfd, u64 op, u64 fd, u32 events, u64 data, u64 result) {
    if (!profile_trace_enabled()) return;
    profile_trace_prefix("epoll.ctl");
    user_log(" epfd=");
    user_log_dec_value(epfd);
    user_log(" op=");
    user_log_dec_value(op);
    user_log(" fd=");
    user_log_dec_value(fd);
    user_log(" kind=");
    user_log_dec_value(fd_valid(fd) ? (u64)g_fds[fd].kind : 0);
    user_log(" events=");
    user_log_hex_inline(events);
    user_log(" data=");
    user_log_hex_inline(data);
    user_log(" result=");
    user_log_hex_inline(result);
    user_log("\n");
}

static void trace_epoll_wait_ready(u64 epfd, u64 fd, u32 events, u16 revents, u64 data) {
    if (!profile_trace_enabled()) return;
    profile_trace_prefix("epoll.ready");
    user_log(" epfd=");
    user_log_dec_value(epfd);
    user_log(" fd=");
    user_log_dec_value(fd);
    user_log(" kind=");
    user_log_dec_value(fd_valid(fd) ? (u64)g_fds[fd].kind : 0);
    user_log(" events=");
    user_log_hex_inline(events);
    user_log(" revents=");
    user_log_hex_inline(revents);
    user_log(" data=");
    user_log_hex_inline(data);
    user_log("\n");
}

static void trace_epoll_wait_block(u64 epfd, i32 timeout_ms, u64 wait_limit) {
    if (!profile_trace_enabled()) return;
    profile_trace_prefix("epoll.block");
    user_log(" epfd=");
    user_log_dec_value(epfd);
    user_log(" timeout_ms=");
    user_log_dec_value((u64)(u32)timeout_ms);
    user_log(" wait_limit=");
    user_log_dec_value(wait_limit);
    user_log("\n");
}

enum { LINUX_EPOLL_WAITER_MAX = 64 };

struct linux_epoll_waiter {
    u8 used;
    u8 wait_forever;
    u64 principal;
    u64 epfd;
    u64 events_va;
    u64 deadline_tick;
};

static struct linux_epoll_waiter g_epoll_waiters[LINUX_EPOLL_WAITER_MAX];

static u64 epoll_scan_ready_current(u64 epfd, u64 events_va, int count_pipe_wait, int *ready_out) {
    *ready_out = 0;
    for (u64 i = 0; i < EPOLL_WATCH_MAX; i++) {
        struct epoll_watch *watch = &g_fds[epfd].epoll_watches[i];
        if (watch->fd_plus_one == 0) continue;
        const u64 fd = watch->fd_plus_one - 1;
        if (!fd_valid(fd)) {
            *watch = (struct epoll_watch){0};
            sync_fd_to_thread_group(epfd);
            continue;
        }
        const int watched_pipe = fd_entry_is_pipe(&g_fds[fd]);
        if (count_pipe_wait && watched_pipe) g_prof.pipe_epoll_wait_calls++;
        const u16 revents = fd_poll_revents(fd, (u16)watch->events);
        if (revents == 0) continue;
        if (watched_pipe) g_prof.pipe_epoll_ready++;
        trace_epoll_wait_ready(epfd, fd, watch->events, revents, watch->data);
        if (!write_epoll_event(events_va, revents, watch->data)) return errno_fault();
        *ready_out = 1;
        return 1;
    }
    return 0;
}

static void trace_epoll_wait_badf_for_proc(u64 epfd, const struct linux_process_state *proc);

static u64 epoll_scan_ready_for_principal(u64 principal, u64 epfd, u64 events_va, int *ready_out) {
    *ready_out = 0;
    struct linux_process_state *proc = process_state_for(principal);
    if (!proc || epfd >= LINUX_FD_MAX || proc->fds[epfd].kind != FD_EPOLL) {
        trace_epoll_wait_badf_for_proc(epfd, proc);
        return errno_badf();
    }
    struct linux_abi_context waiter_ctx;
    struct linux_abi_context *saved_ctx = abi_current_context();
    abi_context_enter(&waiter_ctx, proc, 0, principal);
    for (u64 i = 0; i < EPOLL_WATCH_MAX; i++) {
        struct epoll_watch *watch = &g_fds[epfd].epoll_watches[i];
        if (watch->fd_plus_one == 0) continue;
        const u64 fd = watch->fd_plus_one - 1;
        if (!fd_valid(fd)) {
            *watch = (struct epoll_watch){0};
            sync_fd_to_thread_group(epfd);
            continue;
        }
        const int watched_pipe = fd_entry_is_pipe(&g_fds[fd]);
        const u16 revents = fd_poll_revents(fd, (u16)watch->events);
        if (revents == 0) continue;
        if (watched_pipe) g_prof.pipe_epoll_ready++;
        trace_epoll_wait_ready(epfd, fd, watch->events, revents, watch->data);
        if (!write_epoll_event_to_principal(principal, events_va, revents, watch->data)) {
            g_abi_ctx = saved_ctx;
            return errno_fault();
        }
        *ready_out = 1;
        g_abi_ctx = saved_ctx;
        return 1;
    }
    g_abi_ctx = saved_ctx;
    return 0;
}

static void remove_epoll_waiters_for_principal(u64 principal) {
    if (principal == 0) return;
    for (u64 i = 0; i < LINUX_EPOLL_WAITER_MAX; i++) {
        if (!g_epoll_waiters[i].used || g_epoll_waiters[i].principal != principal) continue;
        g_epoll_waiters[i] = (struct linux_epoll_waiter){0};
    }
}

static int interrupt_epoll_waiter_for_principal(u64 principal) {
    if (principal == 0) return 0;
    for (u64 i = 0; i < LINUX_EPOLL_WAITER_MAX; i++) {
        if (!g_epoll_waiters[i].used || g_epoll_waiters[i].principal != principal) continue;
        g_epoll_waiters[i] = (struct linux_epoll_waiter){0};
        return reply_trap_target(principal, errno_intr(), 0) == SYSCALL_OK;
    }
    return 0;
}

static void remove_fd_from_current_epoll_sets(u64 fd) {
    if (!g_proc || fd >= LINUX_FD_MAX) return;
    for (u64 epfd = 0; epfd < LINUX_FD_MAX; epfd++) {
        if (epfd == fd || g_fds[epfd].kind != FD_EPOLL) continue;
        int changed = 0;
        for (u64 i = 0; i < EPOLL_WATCH_MAX; i++) {
            struct epoll_watch *watch = &g_fds[epfd].epoll_watches[i];
            if (watch->fd_plus_one != fd + 1) continue;
            *watch = (struct epoll_watch){0};
            changed = 1;
        }
        if (changed) sync_fd_to_thread_group(epfd);
    }
}

static void trace_epoll_wait_badf_for_proc(u64 epfd, const struct linux_process_state *proc) {
    if (profile_trace_enabled()) profile_trace_prefix("epoll.wait.badf");
    else user_log("LinuxAbiServer: epoll.wait.badf");
    user_log(" epfd=");
    user_log_dec_value(epfd);
    user_log(" kind=");
    user_log_dec_value((proc && epfd < LINUX_FD_MAX) ? (u64)proc->fds[epfd].kind : 0);
    if (proc) {
        user_log(" pid=");
        user_log_dec_value(proc->pid);
        user_log(" tid=");
        user_log_dec_value(proc->tid);
        user_log(" principal=");
        user_log_dec_value(proc->principal);
    }
    user_log("\n");
}

static void trace_epoll_wait_badf(u64 epfd) {
    trace_epoll_wait_badf_for_proc(epfd, g_proc);
}

static int epoll_waiters_next_timeout(u64 *ticks_out) {
    const u64 now = syscall0(SYSCALL_GET_TICK_COUNT);
    u64 best = 0;
    for (u64 i = 0; i < LINUX_EPOLL_WAITER_MAX; i++) {
        const struct linux_epoll_waiter *waiter = &g_epoll_waiters[i];
        if (!waiter->used || waiter->wait_forever) continue;
        const u64 remaining = waiter->deadline_tick > now ? waiter->deadline_tick - now : 1;
        if (best == 0 || remaining < best) best = remaining;
    }
    if (best == 0) return 0;
    *ticks_out = best;
    return 1;
}

static void process_epoll_waiters_update(void) {
    const u64 now = syscall0(SYSCALL_GET_TICK_COUNT);
    for (u64 i = 0; i < LINUX_EPOLL_WAITER_MAX; i++) {
        struct linux_epoll_waiter *waiter = &g_epoll_waiters[i];
        if (!waiter->used) continue;
        struct linux_process_state *proc = process_state_for(waiter->principal);
        u64 result = 0;
        int ready = 0;
        if (!proc) {
            *waiter = (struct linux_epoll_waiter){0};
            continue;
        }
        if (process_signal_interrupt_pending(proc)) {
            result = errno_intr();
            ready = 1;
        } else {
            int event_ready = 0;
            result = epoll_scan_ready_for_principal(waiter->principal, waiter->epfd, waiter->events_va, &event_ready);
            if (event_ready || (i64)result < 0) ready = 1;
            else if (!waiter->wait_forever && now >= waiter->deadline_tick) {
                result = 0;
                ready = 1;
            }
        }
        if (!ready) continue;
        const u64 principal = waiter->principal;
        *waiter = (struct linux_epoll_waiter){0};
        (void)reply_trap_target(principal, result, 0);
    }
}

static struct ipc_message register_epoll_waiter_or_wait(u64 principal, u64 epfd, u64 events_va, u64 wait_limit) {
    const u64 now = syscall0(SYSCALL_GET_TICK_COUNT);
    for (u64 i = 0; i < LINUX_EPOLL_WAITER_MAX; i++) {
        struct linux_epoll_waiter *waiter = &g_epoll_waiters[i];
        if (waiter->used) continue;
        waiter->used = 1;
        waiter->wait_forever = wait_limit == LINUX_ABI_WAIT_FOREVER ? 1 : 0;
        waiter->principal = principal;
        waiter->epfd = epfd;
        waiter->events_va = events_va;
        waiter->deadline_tick = waiter->wait_forever ? 0 : now + wait_limit;
        detach_reply_token();
        return wait_ipc_timeout(1);
    }
    return reply(errno_nomem(), 0);
}

static struct ipc_message handle_epoll_ctl(const struct trap_request *req) {
    const u64 epfd = req->args[0];
    const u64 op = req->args[1];
    const u64 fd = req->args[2];
    const u64 event_va = req->args[3];
    if (!fd_valid(epfd) || g_fds[epfd].kind != FD_EPOLL) {
        trace_epoll_ctl(epfd, op, fd, 0, 0, errno_badf());
        return reply(errno_badf(), 0);
    }
    if (op != EPOLL_CTL_DEL && !fd_valid(fd)) {
        trace_epoll_ctl(epfd, op, fd, 0, 0, errno_badf());
        return reply(errno_badf(), 0);
    }
    struct fd_entry *ep = &g_fds[epfd];
    i64 found = -1;
    i64 free_slot = -1;
    for (u64 i = 0; i < EPOLL_WATCH_MAX; i++) {
        if (ep->epoll_watches[i].fd_plus_one == fd + 1) found = (i64)i;
        if (free_slot < 0 && ep->epoll_watches[i].fd_plus_one == 0) free_slot = (i64)i;
    }
    if (op == EPOLL_CTL_DEL) {
        if (found < 0) {
            trace_epoll_ctl(epfd, op, fd, 0, 0, errno_noent());
            return reply(errno_noent(), 0);
        }
        ep->epoll_watches[found] = (struct epoll_watch){0};
        sync_fd_to_thread_group(epfd);
        trace_epoll_ctl(epfd, op, fd, 0, 0, 0);
        return reply(0, 0);
    }
    if (op != EPOLL_CTL_ADD && op != EPOLL_CTL_MOD) {
        trace_epoll_ctl(epfd, op, fd, 0, 0, errno_inval());
        return reply(errno_inval(), 0);
    }
    if (!fd_epoll_supported(fd)) {
        trace_epoll_ctl(epfd, op, fd, 0, 0, errno_perm());
        return reply(errno_perm(), 0);
    }
    u32 events = 0;
    u64 data = 0;
    if (!read_epoll_event(event_va, &events, &data)) {
        trace_epoll_ctl(epfd, op, fd, 0, 0, errno_fault());
        return reply(errno_fault(), 0);
    }
    i64 slot = found;
    if (op == EPOLL_CTL_ADD) {
        if (found >= 0) {
            trace_epoll_ctl(epfd, op, fd, events, data, errno_exist());
            return reply(errno_exist(), 0);
        }
        if (free_slot < 0) {
            trace_epoll_ctl(epfd, op, fd, events, data, errno_busy());
            return reply(errno_busy(), 0);
        }
        slot = free_slot;
    } else if (found < 0) {
        trace_epoll_ctl(epfd, op, fd, events, data, errno_noent());
        return reply(errno_noent(), 0);
    }
    ep->epoll_watches[slot].fd_plus_one = fd + 1;
    ep->epoll_watches[slot].events = events;
    ep->epoll_watches[slot].data = data;
    sync_fd_to_thread_group(epfd);
    trace_epoll_ctl(epfd, op, fd, events, data, 0);
    return reply(0, 0);
}

static struct ipc_message handle_epoll_wait(const struct trap_request *req) {
    const u64 epfd = req->args[0];
    const u64 events_va = req->args[1];
    const u64 maxevents = req->args[2];
    const i32 timeout_ms = (i32)(u32)req->args[3];
    if (!fd_valid(epfd) || g_fds[epfd].kind != FD_EPOLL) {
        trace_epoll_wait_badf(epfd);
        return reply(errno_badf(), 0);
    }
    if (events_va == 0 || maxevents == 0) return reply(errno_inval(), 0);

    u64 wait_limit = 0;
    int immediate = 0;
    if (timeout_ms == 0) {
        immediate = 1;
    } else if (timeout_ms < 0) {
        wait_limit = LINUX_ABI_WAIT_FOREVER;
    } else {
        wait_limit = poll_timeout_ms_to_ticks((u64)(u32)timeout_ms);
    }

    int event_ready = 0;
    const u64 result = epoll_scan_ready_current(epfd, events_va, 1, &event_ready);
    if (event_ready || (i64)result < 0) return reply(result, 0);
    if (immediate) return reply(0, 0);
    if (process_signal_interrupt_pending(g_proc)) return reply(errno_intr(), 0);
    trace_epoll_wait_block(epfd, timeout_ms, wait_limit);
    return register_epoll_waiter_or_wait(req->caller_principal, epfd, events_va, wait_limit);
}

static struct ipc_message handle_dup(const struct trap_request *req) {
    const u64 oldfd = req->args[0];
    const int newfd = alloc_fd_at_least(0);
    if (newfd < 0) return reply(errno_busy(), 0);
    if (!fd_clone_into((u64)newfd, oldfd, 0)) return reply(errno_badf(), 0);
    return reply((u64)newfd, 0);
}

static struct ipc_message handle_dup2_like(const struct trap_request *req, int dup3) {
    const u64 oldfd = req->args[0]; const u64 newfd = req->args[1];
    const u64 flags = dup3 ? req->args[2] : 0;
    if (newfd >= LINUX_FD_MAX) return reply(errno_badf(), 0);
    if (!fd_valid(oldfd)) return reply(errno_badf(), 0);
    if ((flags & ~(u64)O_CLOEXEC) != 0) return reply(errno_inval(), 0);
    if (dup3 && oldfd == newfd) return reply(errno_inval(), 0);
    if (oldfd == newfd) return reply(newfd, 0);
    if (fd_is_pipe(newfd)) close_pipe_fd(newfd);
    if (fd_valid(newfd) && g_fds[newfd].kind == FD_SOCKET) close_socket_entry(&g_fds[newfd]);
    if (!fd_clone_into(newfd, oldfd, (flags & O_CLOEXEC) != 0 ? FD_INTERNAL_CLOEXEC : 0)) return reply(errno_badf(), 0);
    return reply(newfd, 0);
}

static struct ipc_message handle_fcntl(const struct trap_request *req) {
    const u64 fd = req->args[0]; const u64 cmd = req->args[1]; const u64 arg = req->args[2];
    if (!fd_valid(fd)) return reply(errno_badf(), 0);
    if (cmd == F_DUPFD || cmd == F_DUPFD_CLOEXEC) {
        const int newfd = alloc_fd_at_least(arg);
        if (newfd < 0) return reply(errno_busy(), 0);
        if (!fd_clone_into((u64)newfd, fd, cmd == F_DUPFD_CLOEXEC ? FD_INTERNAL_CLOEXEC : 0)) return reply(errno_badf(), 0);
        return reply((u64)newfd, 0);
    }
    if (cmd == F_GETFD) return reply((g_fds[fd].fd_flags & FD_INTERNAL_CLOEXEC) != 0 ? FD_CLOEXEC : 0, 0);
    if (cmd == F_SETFD) {
        if ((arg & FD_CLOEXEC) != 0) g_fds[fd].fd_flags |= FD_INTERNAL_CLOEXEC;
        else g_fds[fd].fd_flags &= ~((u32)FD_INTERNAL_CLOEXEC);
        sync_fd_to_thread_group(fd);
        return reply(0, 0);
    }
    if (cmd == F_GETFL) {
        const u64 access = g_fds[fd].kind == FD_PIPE_WRITE ? O_WRONLY : (g_fds[fd].kind == FD_SOCKET ? O_RDWR : (g_fds[fd].fd_flags & O_ACCMODE));
        return reply(access | (g_fds[fd].fd_flags & (O_NONBLOCK | O_APPEND)), 0);
    }
    if (cmd == F_SETFL) {
        g_fds[fd].fd_flags = (u32)((g_fds[fd].fd_flags & (FD_INTERNAL_CLOEXEC | O_ACCMODE)) | (arg & (O_NONBLOCK | O_APPEND)));
        sync_fd_to_thread_group(fd);
        return reply(0, 0);
    }
    if (cmd == F_GETLK) {
        if (arg == 0) return reply(errno_fault(), 0);
        const u16 unlocked = F_UNLCK;
        return copy_to_target(arg, &unlocked, sizeof(unlocked)) == sizeof(unlocked) ? reply(0, 0) : reply(errno_fault(), 0);
    }
    if (cmd == F_SETLK || cmd == F_SETLKW) return reply(0, 0);
    return reply(errno_inval(), 0);
}

static struct ipc_message handle_flock(const struct trap_request *req) {
    const u64 fd = req->args[0];
    const u64 op = req->args[1];
    if (!fd_valid(fd)) return reply(errno_badf(), 0);
    const u64 lock_op = op & ~(u64)LOCK_NB;
    if (lock_op != LOCK_SH && lock_op != LOCK_EX && lock_op != LOCK_UN) return reply(errno_inval(), 0);
    return reply(0, 0);
}
