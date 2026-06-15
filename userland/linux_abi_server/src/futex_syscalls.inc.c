static u64 futex_owner_pid_for_current(void) {
    if (g_proc && g_proc->pid != 0) return g_proc->pid;
    return 0;
}

static int futex_diag_enabled(void) {
    for (u64 i = 0; i < LINUX_PROCESS_MAX; i++) {
        if (g_processes[i].used && g_processes[i].profile_progress_enabled) return 1;
    }
    return 0;
}

static u64 g_futex_diag_wait_logs;
static u64 g_futex_diag_wake_logs;

static void clear_futex_waiter(struct futex_waiter *waiter) {
    *waiter = (struct futex_waiter){0};
}

static void remove_futex_waiters_for_principal(u64 principal) {
    if (principal == 0) return;
    for (u64 i = 0; i < FUTEX_WAITER_MAX; i++) {
        if (!g_futex_waiters[i].used || g_futex_waiters[i].principal != principal) continue;
        clear_futex_waiter(&g_futex_waiters[i]);
    }
}

static int remove_futex_waiter_exact(u64 principal, u64 owner_pid, u64 uaddr) {
    if (principal == 0 || owner_pid == 0 || uaddr == 0) return 0;
    for (u64 i = 0; i < FUTEX_WAITER_MAX; i++) {
        if (!g_futex_waiters[i].used) continue;
        if (g_futex_waiters[i].principal != principal) continue;
        if (g_futex_waiters[i].owner_pid != owner_pid || g_futex_waiters[i].uaddr != uaddr) continue;
        clear_futex_waiter(&g_futex_waiters[i]);
        return 1;
    }
    return 0;
}

static u64 wake_futex_waiters(u64 owner_pid, u64 uaddr, u64 max_wake) {
    if (owner_pid == 0 || uaddr == 0 || max_wake == 0) return 0;
    u64 woke = 0;
    for (u64 i = 0; i < FUTEX_WAITER_MAX && woke < max_wake; i++) {
        if (!g_futex_waiters[i].used) continue;
        if (g_futex_waiters[i].owner_pid != owner_pid || g_futex_waiters[i].uaddr != uaddr) continue;
        const u64 principal = g_futex_waiters[i].principal;
        if (reply_trap_target(principal, 0, 0) == SYSCALL_OK) {
            clear_futex_waiter(&g_futex_waiters[i]);
            woke++;
        } else {
            clear_futex_waiter(&g_futex_waiters[i]);
        }
    }
    if (profile_trace_enabled()) {
        profile_trace_prefix("futex.wake");
        user_log(" owner=");
        user_log_dec_value(owner_pid);
        user_log(" uaddr=");
        user_log_hex_inline(uaddr);
        user_log(" max=");
        user_log_dec_value(max_wake);
        user_log(" woke=");
        user_log_dec_value(woke);
        user_log("\n");
    }
    if (futex_diag_enabled() && g_futex_diag_wake_logs < 256) {
        g_futex_diag_wake_logs++;
        user_log("LinuxAbiServer.futex.wake owner=");
        user_log_dec_value(owner_pid);
        user_log(" uaddr=");
        user_log_hex_inline(uaddr);
        user_log(" max=");
        user_log_dec_value(max_wake);
        user_log(" woke=");
        user_log_dec_value(woke);
        user_log("\n");
    }
    return woke;
}

static int interrupt_futex_waiter_for_principal(u64 principal) {
    if (principal == 0) return 0;
    for (u64 i = 0; i < FUTEX_WAITER_MAX; i++) {
        if (!g_futex_waiters[i].used || g_futex_waiters[i].principal != principal) continue;
        clear_futex_waiter(&g_futex_waiters[i]);
        return reply_trap_target(principal, errno_intr(), 0) == SYSCALL_OK;
    }
    return 0;
}

static int futex_waiters_next_timeout(u64 *ticks_out) {
    const u64 now = syscall0(SYSCALL_GET_TICK_COUNT);
    u64 best = 0;
    for (u64 i = 0; i < FUTEX_WAITER_MAX; i++) {
        const struct futex_waiter *waiter = &g_futex_waiters[i];
        if (!waiter->used || waiter->deadline_tick == 0) continue;
        const u64 remaining = waiter->deadline_tick > now ? waiter->deadline_tick - now : 1;
        if (best == 0 || remaining < best) best = remaining;
    }
    if (best == 0) return 0;
    *ticks_out = best;
    return 1;
}

static void process_futex_waiters_update(void) {
    const u64 now = syscall0(SYSCALL_GET_TICK_COUNT);
    u64 mismatch_repairs = 0;
    for (u64 i = 0; i < FUTEX_WAITER_MAX; i++) {
        struct futex_waiter *waiter = &g_futex_waiters[i];
        if (!waiter->used) continue;
        u32 current = 0;
        if (copy_from_trap_target(waiter->principal, waiter->uaddr, &current, sizeof(current)) != sizeof(current)) {
            const u64 principal = waiter->principal;
            clear_futex_waiter(waiter);
            (void)reply_trap_target(principal, errno_fault(), 0);
            continue;
        }
        if (current != waiter->expected) {
            const u64 principal = waiter->principal;
            clear_futex_waiter(waiter);
            (void)reply_trap_target(principal, 0, 0);
            mismatch_repairs++;
            if (mismatch_repairs >= 16) break;
            continue;
        }
        if (waiter->deadline_tick == 0 || now < waiter->deadline_tick) continue;
        const u64 principal = waiter->principal;
        clear_futex_waiter(waiter);
        (void)reply_trap_target(principal, errno_timedout(), 0);
    }
}

static int futex_timeout_ticks_from_target(u64 timeout_va, u64 *ticks_out) {
    i64 ts[2];
    if (copy_from_target(timeout_va, ts, sizeof(ts)) != sizeof(ts)) return 0;
    if (ts[0] < 0 || ts[1] < 0 || ts[1] >= 1000000000LL) {
        *ticks_out = (u64)-1;
        return 1;
    }
    const u64 sec = (u64)ts[0];
    const u64 nsec = (u64)ts[1];
    if (sec > ((u64)-1) / 1000ULL) {
        *ticks_out = (u64)-1;
        return 1;
    }
    *ticks_out = sec * 1000ULL + (nsec + 999999ULL) / 1000000ULL;
    return 1;
}

static struct ipc_message handle_futex(const struct trap_request *req) {
    const u64 uaddr = req->args[0];
    const u64 op = req->args[1];
    const u64 val = req->args[2];
    const u64 timeout = req->args[3];
    const u64 cmd = op & FUTEX_CMD_MASK;
    const u64 owner_pid = futex_owner_pid_for_current();

    if ((op & ~(u64)(FUTEX_CMD_MASK | FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME)) != 0) return reply(errno_inval(), 0);
    if (uaddr == 0 || (uaddr & 3) != 0 || owner_pid == 0) return reply(errno_inval(), 0);

    if (cmd == FUTEX_WAKE) {
        return reply(wake_futex_waiters(owner_pid, uaddr, val), 0);
    }

    if (cmd != FUTEX_WAIT) return reply(errno_nosys(), 0);
    u64 timeout_ticks = 0;
    if (timeout != 0) {
        if (!futex_timeout_ticks_from_target(timeout, &timeout_ticks)) return reply(errno_fault(), 0);
        if (timeout_ticks == (u64)-1) return reply(errno_inval(), 0);
    }

    u32 current = 0;
    if (copy_from_target(uaddr, &current, sizeof(current)) != sizeof(current)) return reply(errno_fault(), 0);
    if (current != (u32)val) {
        if (profile_trace_enabled()) {
            profile_trace_prefix("futex.wait.again");
            user_log(" owner=");
            user_log_dec_value(owner_pid);
            user_log(" uaddr=");
            user_log_hex_inline(uaddr);
            user_log(" current=");
            user_log_hex_inline(current);
            user_log(" val=");
            user_log_hex_inline(val);
            user_log("\n");
        }
        return reply(errno_again(), 0);
    }

    remove_futex_waiters_for_principal(req->caller_principal);
    for (u64 i = 0; i < FUTEX_WAITER_MAX; i++) {
        if (g_futex_waiters[i].used) continue;
        g_futex_waiters[i].used = 1;
        g_futex_waiters[i].principal = req->caller_principal;
        g_futex_waiters[i].owner_pid = owner_pid;
        g_futex_waiters[i].uaddr = uaddr;
        g_futex_waiters[i].expected = (u32)val;
        g_futex_waiters[i].deadline_tick = timeout == 0 ? 0 : syscall0(SYSCALL_GET_TICK_COUNT) + timeout_ticks;
        if (copy_from_target(uaddr, &current, sizeof(current)) != sizeof(current)) {
            clear_futex_waiter(&g_futex_waiters[i]);
            return reply(errno_fault(), 0);
        }
        if (current != (u32)val) {
            clear_futex_waiter(&g_futex_waiters[i]);
            return reply(errno_again(), 0);
        }
        if (profile_trace_enabled()) {
            profile_trace_prefix("futex.wait.block");
            user_log(" owner=");
            user_log_dec_value(owner_pid);
            user_log(" waiter=");
            user_log_dec_value(req->caller_principal);
            user_log(" uaddr=");
            user_log_hex_inline(uaddr);
            user_log(" val=");
            user_log_hex_inline(val);
            user_log("\n");
        }
        if (futex_diag_enabled() && g_futex_diag_wait_logs < 256) {
            g_futex_diag_wait_logs++;
            user_log("LinuxAbiServer.futex.wait owner=");
            user_log_dec_value(owner_pid);
            user_log(" waiter=");
            user_log_dec_value(req->caller_principal);
            user_log(" uaddr=");
            user_log_hex_inline(uaddr);
            user_log(" val=");
            user_log_hex_inline(val);
            user_log(" timeout_ticks=");
            user_log_dec_value(timeout == 0 ? 0 : timeout_ticks);
            user_log("\n");
        }
        detach_reply_token();
        if (timeout == 0) return wait_linux_abi_event();
        if (timeout_ticks == 0) {
            if (remove_futex_waiter_exact(req->caller_principal, owner_pid, uaddr)) {
                (void)reply_trap_target(req->caller_principal, errno_timedout(), 0);
            }
            return wait_ipc_timeout(1);
        }
        return wait_linux_abi_event();
    }
    return reply(errno_again(), 0);
}
