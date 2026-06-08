static u64 futex_owner_pid_for_current(void) {
    if (g_proc && g_proc->pid != 0) return g_proc->pid;
    return 0;
}

static void remove_futex_waiters_for_principal(u64 principal) {
    if (principal == 0) return;
    for (u64 i = 0; i < FUTEX_WAITER_MAX; i++) {
        if (!g_futex_waiters[i].used || g_futex_waiters[i].principal != principal) continue;
        g_futex_waiters[i].used = 0;
        g_futex_waiters[i].principal = 0;
        g_futex_waiters[i].owner_pid = 0;
        g_futex_waiters[i].uaddr = 0;
    }
}

static u64 wake_futex_waiters(u64 owner_pid, u64 uaddr, u64 max_wake) {
    if (owner_pid == 0 || uaddr == 0 || max_wake == 0) return 0;
    u64 woke = 0;
    for (u64 i = 0; i < FUTEX_WAITER_MAX && woke < max_wake; i++) {
        if (!g_futex_waiters[i].used) continue;
        if (g_futex_waiters[i].owner_pid != owner_pid || g_futex_waiters[i].uaddr != uaddr) continue;
        const u64 principal = g_futex_waiters[i].principal;
        g_futex_waiters[i].used = 0;
        g_futex_waiters[i].principal = 0;
        g_futex_waiters[i].owner_pid = 0;
        g_futex_waiters[i].uaddr = 0;
        if (reply_trap_target(principal, 0, 0) == SYSCALL_OK) woke++;
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
    return woke;
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
    if (timeout != 0) {
        if (profile_trace_enabled()) {
            profile_trace_prefix("futex.wait.timeout_stub");
            user_log(" owner=");
            user_log_dec_value(owner_pid);
            user_log(" uaddr=");
            user_log_hex_inline(uaddr);
            user_log(" val=");
            user_log_hex_inline(val);
            user_log(" timeout=");
            user_log_hex_inline(timeout);
            user_log("\n");
        }
        return reply(errno_timedout(), 0);
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
        detach_reply_token();
        return wait_ipc();
    }
    return reply(errno_again(), 0);
}
