static u64 cstr_len(const char *s) { u64 n = 0; while (s[n] != 0) n++; return n; }
void *memcpy(void *dst, const void *src, u64 len) {
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    for (u64 i = 0; i < len; i++) d[i] = s[i];
    return dst;
}
void *memset(void *dst, int value, u64 len) {
    u8 *d = (u8 *)dst;
    for (u64 i = 0; i < len; i++) d[i] = (u8)value;
    return dst;
}
static void user_log_len(const char *message, u64 len) { u64 ret; __asm__ volatile("int $0x80" : "=a"(ret) : "a"((u64)SYSCALL_LOG), "D"((u64)message), "S"(len) : "rcx", "rdx", "r8", "r9", "r10", "r11", "memory"); (void)ret; }
static void user_log(const char *message) { user_log_len(message, cstr_len(message)); }
static void user_log_dec_value(u64 value) {
    char buf[32];
    u64 pos = 0;
    if (value == 0) {
        buf[pos++] = '0';
    } else {
        char rev[32];
        u64 n = 0;
        while (value != 0 && n < sizeof(rev)) {
            rev[n++] = (char)('0' + (value % 10));
            value /= 10;
        }
        while (n != 0) buf[pos++] = rev[--n];
    }
    user_log_len(buf, pos);
}
static void user_log_dec_line(const char *label, u64 value) {
    user_log(label);
    user_log_dec_value(value);
    user_log("\n");
}
static void user_log_hex_inline(u64 value) { static const char hex[] = "0123456789ABCDEF"; char buf[32]; u64 pos = 0; buf[pos++] = '0'; buf[pos++] = 'x'; int started = 0; for (int shift = 60; shift >= 0; shift -= 4) { unsigned nibble = (unsigned)((value >> (u64)shift) & 0xFULL); if (nibble != 0 || started || shift == 0) { buf[pos++] = hex[nibble]; started = 1; } } user_log_len(buf, pos); }
static void user_log_hex_value(u64 value) { static const char hex[] = "0123456789ABCDEF"; char buf[32]; u64 pos = 0; buf[pos++] = '0'; buf[pos++] = 'x'; int started = 0; for (int shift = 60; shift >= 0; shift -= 4) { unsigned nibble = (unsigned)((value >> (u64)shift) & 0xFULL); if (nibble != 0 || started || shift == 0) { buf[pos++] = hex[nibble]; started = 1; } } buf[pos++] = '\n'; user_log_len(buf, pos); }
static void clear_page(u64 va) { volatile u64 *p = (volatile u64 *)va; for (u64 i = 0; i < 512; i++) p[i] = 0; }
static u64 min_u64(u64 a, u64 b) { return a < b ? a : b; }
static u64 align_up(u64 value, u64 align) { return (value + align - 1) & ~(align - 1); }

static u64 syscall0(u64 nr) { u64 ret; __asm__ volatile("int $0x80" : "=a"(ret) : "a"(nr) : "rcx", "rdx", "r8", "r9", "r10", "r11", "memory"); return ret; }
static u64 syscall1(u64 nr, u64 a0) { u64 ret; __asm__ volatile("int $0x80" : "=a"(ret) : "a"(nr), "D"(a0) : "rcx", "rdx", "r8", "r9", "r10", "r11", "memory"); return ret; }
static u64 syscall2(u64 nr, u64 a0, u64 a1) { u64 ret; __asm__ volatile("int $0x80" : "=a"(ret) : "a"(nr), "D"(a0), "S"(a1) : "rcx", "rdx", "r8", "r9", "r10", "r11", "memory"); return ret; }
static u64 syscall3(u64 nr, u64 a0, u64 a1, u64 a2) { u64 ret; __asm__ volatile("int $0x80" : "=a"(ret) : "a"(nr), "D"(a0), "S"(a1), "d"(a2) : "rcx", "r8", "r9", "r10", "r11", "memory"); return ret; }
static u64 syscall4(u64 nr, u64 a0, u64 a1, u64 a2, u64 a3) { u64 ret; __asm__ volatile("int $0x80" : "=a"(ret) : "a"(nr), "D"(a0), "S"(a1), "d"(a2), "c"(a3) : "r8", "r9", "r10", "r11", "memory"); return ret; }
static u64 syscall4_r10(u64 nr, u64 a0, u64 a1, u64 a2, u64 a3) { register u64 r10 __asm__("r10") = a3; u64 ret; __asm__ volatile("int $0x80" : "=a"(ret), "+r"(r10) : "a"(nr), "D"(a0), "S"(a1), "d"(a2) : "rcx", "r8", "r9", "r11", "memory"); return ret; }
static u64 detach_reply_token(void);
static void process_exit(u64 code) { (void)syscall2(SYSCALL_PROCESS_EXIT, code, 0); for (;;) __asm__ volatile("pause"); }

static int profile_trace_enabled(void) {
    if (g_profile_trace_verbose) return 1;
    return g_proc != 0 && g_proc->profile_verbose_enabled != 0;
}

static int profile_detail_enabled(void) {
    if (profile_trace_enabled()) return 1;
    return g_proc != 0 && g_proc->profile_detail_enabled != 0;
}

static void profile_trace_prefix(const char *event) {
    user_log("LinuxAbiServer.trace tick=");
    user_log_dec_value(syscall0(SYSCALL_GET_TICK_COUNT));
    user_log(" event=");
    user_log(event);
    if (g_proc != 0) {
        user_log(" pid=");
        user_log_dec_value(g_proc->pid);
        user_log(" principal=");
        user_log_dec_value(g_proc->principal);
    }
}

static void profile_trace_event_u64(const char *event, u64 value) {
    if (!profile_trace_enabled()) return;
    profile_trace_prefix(event);
    user_log(" value=");
    user_log_dec_value(value);
    user_log("\n");
}

static void profile_trace_syscall_span(u64 nr, u64 principal, u64 start_tick, u64 end_tick) {
    if (!profile_trace_enabled()) return;
    user_log("LinuxAbiServer.trace tick=");
    user_log_dec_value(end_tick);
    user_log(" event=syscall.done nr=");
    user_log_dec_value(nr);
    user_log(" principal=");
    user_log_dec_value(principal);
    user_log(" dt=");
    user_log_dec_value(end_tick - start_tick);
    user_log("\n");
}

static void wait_without_consuming_ipc(void) {
    static u64 preserve_wait_poll_counter = 0;
    for (u64 i = 0; i < 1024; i++) __asm__ volatile("pause");
    preserve_wait_poll_counter++;
    if ((preserve_wait_poll_counter & 0x3u) != 0) return;
    (void)syscall2(SYSCALL_WAIT_EVENT, WAIT_EVENT_FLAG_PRESERVE_IPC_QUEUE, 1);
}

static void wait_without_consuming_ipc_no_switch(void) {
    const u64 start = syscall0(SYSCALL_GET_TICK_COUNT);
    for (;;) {
        for (u64 i = 0; i < 256; i++) __asm__ volatile("pause");
        if (syscall0(SYSCALL_GET_TICK_COUNT) != start) return;
    }
}

static u64 linux_signal_bit(u64 signo) {
    return (signo == 0 || signo >= 65) ? 0 : (1ULL << (signo - 1ULL));
}

static u64 linux_unblockable_signal_mask(void) {
    return linux_signal_bit(SIGKILL) | linux_signal_bit(SIGSTOP);
}

static int linux_signal_default_ignored_inline(u64 signo) {
    return signo == SIGCHLD || signo == SIGURG || signo == SIGWINCH;
}

static void queue_process_signal(struct linux_process_state *proc, u64 signo) {
    if (!proc || signo == 0 || signo >= 65) return;
    if (proc->sig_handler[signo] == 1) return;
    if (proc->sig_handler[signo] == 0 && linux_signal_default_ignored_inline(signo)) return;
    proc->pending_signals |= linux_signal_bit(signo);
}

static void queue_timer_interrupt_signal(struct linux_process_state *proc, u64 signo) {
    if (!proc || signo == 0 || signo >= 65) return;
    proc->timer_interrupt_signals |= linux_signal_bit(signo);
}

static u64 dequeue_pending_signal_matching(struct linux_process_state *proc, u64 set_word) {
    if (!proc) return 0;
    const u64 pending = proc->pending_signals & set_word;
    if (pending == 0) return 0;
    for (u64 signo = 1; signo < 65; signo++) {
        const u64 bit = linux_signal_bit(signo);
        if ((pending & bit) == 0) continue;
        proc->pending_signals &= ~bit;
        return signo;
    }
    return 0;
}

static void clear_process_timers(struct linux_process_state *proc) {
    if (!proc) return;
    proc->pending_signals = 0;
    proc->timer_interrupt_signals = 0;
    proc->itimer_real_expiry_tick = 0;
    proc->itimer_real_interval_ticks = 0;
    for (u64 i = 0; i < LINUX_POSIX_TIMER_MAX; i++) {
        proc->timers[i].used = 0;
        proc->timers[i].timer_id = (i32)(i + 1);
        proc->timers[i].clock_id = 0;
        proc->timers[i].signo = 0;
        proc->timers[i].notify = SIGEV_NONE;
        proc->timers[i].value = 0;
        proc->timers[i].expiry_tick = 0;
        proc->timers[i].interval_ticks = 0;
        proc->timers[i].overrun = 0;
    }
}

static u64 timer_remaining_ticks(u64 expiry_tick) {
    if (expiry_tick == 0) return 0;
    const u64 now = syscall0(SYSCALL_GET_TICK_COUNT);
    return expiry_tick > now ? expiry_tick - now : 0;
}

static u64 process_itimer_real_remaining_ticks(struct linux_process_state *proc) {
    if (!proc) return 0;
    return timer_remaining_ticks(proc->itimer_real_expiry_tick);
}

static void process_timers_update(struct linux_process_state *proc) {
    if (!proc) return;
    const u64 now = syscall0(SYSCALL_GET_TICK_COUNT);
    if (proc->itimer_real_expiry_tick != 0 && now >= proc->itimer_real_expiry_tick) {
        if (proc->itimer_real_interval_ticks != 0) {
            const u64 elapsed = now - proc->itimer_real_expiry_tick;
            const u64 periods = elapsed / proc->itimer_real_interval_ticks + 1ULL;
            proc->itimer_real_expiry_tick += periods * proc->itimer_real_interval_ticks;
        } else {
            proc->itimer_real_expiry_tick = 0;
        }
        queue_timer_interrupt_signal(proc, SIGALRM);
        queue_process_signal(proc, SIGALRM);
    }
    for (u64 i = 0; i < LINUX_POSIX_TIMER_MAX; i++) {
        struct linux_posix_timer_state *timer = &proc->timers[i];
        if (!timer->used || timer->expiry_tick == 0 || now < timer->expiry_tick) continue;
        u64 overruns = 0;
        if (timer->interval_ticks != 0) {
            const u64 elapsed = now - timer->expiry_tick;
            const u64 periods = elapsed / timer->interval_ticks + 1ULL;
            timer->expiry_tick += periods * timer->interval_ticks;
            overruns = periods - 1ULL;
        } else {
            timer->expiry_tick = 0;
        }
        timer->overrun = overruns;
        if (timer->notify == SIGEV_SIGNAL || timer->notify == SIGEV_THREAD_ID) {
            const u64 signo = timer->signo == 0 ? SIGALRM : timer->signo;
            queue_timer_interrupt_signal(proc, signo);
            queue_process_signal(proc, signo);
        }
    }
}

static int process_signal_interrupt_pending(struct linux_process_state *proc) {
    process_timers_update(proc);
    if (!proc) return 0;
    return (proc->pending_signals & ~proc->blocked_signals) != 0;
}

static int process_next_signal_timer_timeout(struct linux_process_state *proc, u64 *ticks_out) {
    *ticks_out = 0;
    if (!proc) return 0;
    process_timers_update(proc);
    if (process_signal_interrupt_pending(proc)) return 1;
    u64 best = 0;
    if (proc->itimer_real_expiry_tick != 0) best = process_itimer_real_remaining_ticks(proc);
    for (u64 i = 0; i < LINUX_POSIX_TIMER_MAX; i++) {
        struct linux_posix_timer_state *timer = &proc->timers[i];
        if (!timer->used || timer->expiry_tick == 0) continue;
        if (timer->notify != SIGEV_SIGNAL && timer->notify != SIGEV_THREAD_ID) continue;
        const u64 remaining = timer_remaining_ticks(timer->expiry_tick);
        if (best == 0 || remaining < best) best = remaining;
    }
    if (best == 0) return 0;
    *ticks_out = best;
    return 1;
}

static int process_itimer_real_expired(struct linux_process_state *proc) {
    return process_signal_interrupt_pending(proc);
}

static void fill_user_context_from_request(struct abi_trap_user_context *ctx, const struct trap_request *req, u64 result_rax) {
    u8 *p = (u8 *)ctx;
    for (u64 i = 0; i < sizeof(*ctx); i++) p[i] = 0;
    ctx->rip = req->rip;
    ctx->rsp = req->rsp;
    ctx->rflags = req->rflags != 0 ? req->rflags : 0x202ULL;
    ctx->rax = result_rax;
    ctx->rbx = req->rbx;
    ctx->rcx = req->rcx;
    ctx->rdx = req->rdx;
    ctx->rsi = req->rsi;
    ctx->rdi = req->rdi;
    ctx->rbp = req->rbp;
    ctx->r8 = req->r8;
    ctx->r9 = req->r9;
    ctx->r10 = req->r10;
    ctx->r11 = req->r11;
    ctx->r12 = req->r12;
    ctx->r13 = req->r13;
    ctx->r14 = req->r14;
    ctx->r15 = req->r15;
    ctx->fs_base = req->fs_base;
}

static u64 reply_trap_target_context(u64 principal, const struct abi_trap_user_context *ctx) {
    return syscall4_r10(SYSCALL_REPLY_ABI_TRAP_TARGET_CONTEXT, principal, (u64)ctx, sizeof(*ctx), 0);
}

static int copy_to_current_reply_target(u64 target_va, const void *src, u64 len) {
    return syscall3(SYSCALL_COPY_TO_ABI_TRAP_REPLY_TARGET, target_va, (u64)src, len) == len;
}

static u64 take_deliverable_signal(struct linux_process_state *proc) {
    process_timers_update(proc);
    if (!proc) return 0;
    const u64 deliverable = proc->pending_signals & ~proc->blocked_signals;
    if (deliverable == 0) return 0;
    for (u64 signo = 1; signo < 65; signo++) {
        const u64 bit = linux_signal_bit(signo);
        if ((deliverable & bit) == 0) continue;
        if (proc->sig_handler[signo] == 1) {
            proc->pending_signals &= ~bit;
            continue;
        }
        return signo;
    }
    return 0;
}

static int try_reply_signal_frame(u64 result, u64 flags) {
    if (flags != 0 || !g_proc || !g_abi_ctx || !g_abi_ctx->request) return 0;
    const struct trap_request *req = g_abi_ctx->request;
    if (req->kind != TRAP_KIND_ABI_SYSCALL) return 0;
    if (req->nr == LINUX_SYS_RT_SIGRETURN) return 0;
    const u64 signo = take_deliverable_signal(g_proc);
    if (signo == 0) return 0;
    const u64 handler = g_proc->sig_handler[signo];
    const u64 restorer = g_proc->sig_restorer[signo];
    if (handler == 0) {
        g_proc->pending_signals &= ~linux_signal_bit(signo);
        return 0;
    }
    if (restorer == 0) {
        g_proc->pending_signals &= ~linux_signal_bit(signo);
        return 0;
    }

    struct linux_signal_frame_body body;
    u8 *body_bytes = (u8 *)&body;
    for (u64 i = 0; i < sizeof(body); i++) body_bytes[i] = 0;
    body.magic = LINUX_SIGNAL_FRAME_MAGIC;
    body.signo = signo;
    body.info.si_signo = (i32)signo;
    body.info.si_code = SI_TIMER;
    fill_user_context_from_request(&body.saved_context, req, result);

    const u64 body_va = (req->rsp - sizeof(body)) & ~15ULL;
    const u64 return_slot_va = body_va - 8ULL;
    if (!copy_to_current_reply_target(return_slot_va, &restorer, sizeof(restorer))) return 0;
    if (!copy_to_current_reply_target(body_va, &body, sizeof(body))) return 0;

    struct abi_trap_user_context handler_context;
    fill_user_context_from_request(&handler_context, req, result);
    handler_context.rip = handler;
    handler_context.rsp = return_slot_va;
    handler_context.rax = 0;
    handler_context.rdi = signo;
    handler_context.rsi = body_va + OFFSETOF(struct linux_signal_frame_body, info);
    handler_context.rdx = body_va;

    g_proc->pending_signals &= ~linux_signal_bit(signo);
    const u64 target = abi_reply_target_principal();
    const u64 status = reply_trap_target_context(target, &handler_context);
    if (status != SYSCALL_OK) {
        return 0;
    }
    abi_set_reply_target_principal(0);
    (void)detach_reply_token();
    return 1;
}

static struct ipc_message wait_ipc(void) {
    register u64 rax __asm__("rax") = SYSCALL_WAIT_EVENT; register u64 rdi __asm__("rdi") = 0; register u64 rsi __asm__("rsi") = 0; register u64 rdx __asm__("rdx"); register u64 r8 __asm__("r8");
    __asm__ volatile("int $0x80" : "+r"(rax), "+r"(rdi), "+r"(rsi), "=r"(rdx), "=r"(r8) : : "rcx", "r9", "r10", "r11", "memory");
    struct ipc_message msg = { rax, rdi, rsi, rdx, r8 }; return msg;
}

static struct ipc_message wait_ipc_timeout(u64 timeout_ticks) {
    register u64 rax __asm__("rax") = SYSCALL_WAIT_EVENT; register u64 rdi __asm__("rdi") = 0; register u64 rsi __asm__("rsi") = timeout_ticks; register u64 rdx __asm__("rdx"); register u64 r8 __asm__("r8");
    __asm__ volatile("int $0x80" : "+r"(rax), "+r"(rdi), "+r"(rsi), "=r"(rdx), "=r"(r8) : : "rcx", "r9", "r10", "r11", "memory");
    struct ipc_message msg = { rax, rdi, rsi, rdx, r8 }; return msg;
}

static struct ipc_message reply_current_token(u64 result, u64 flags) {
    register u64 rax __asm__("rax") = SYSCALL_IPC_CALL_REPLY_RECV; register u64 rdi __asm__("rdi") = result; register u64 rsi __asm__("rsi") = 0; register u64 rdx __asm__("rdx") = IPC_CALL_FLAG_SIGNAL_ONLY; register u64 r8 __asm__("r8") = flags; register u64 r9 __asm__("r9") = 0; register u64 r10 __asm__("r10") = 0;
    __asm__ volatile("int $0x80" : "+r"(rax), "+r"(rdi), "+r"(rsi), "+r"(rdx), "+r"(r8), "+r"(r9), "+r"(r10) : : "rcx", "r11", "memory");
    struct ipc_message msg = { rax, rdi, rsi, rdx, r8 }; return msg;
}

static void trace_errno_reply(u64 result, u64 flags) {
    if (result != (u64)(i64)-22 || !profile_trace_enabled() || g_abi_ctx == 0 || g_abi_ctx->request == 0) return;
    const struct trap_request *req = g_abi_ctx->request;
    user_log("LinuxAbiServer.trace errno=EINVAL nr=");
    user_log_dec_value(req->nr);
    user_log(" principal=");
    user_log_dec_value(req->caller_principal);
    user_log(" flags=");
    user_log_hex_inline(flags);
    user_log(" a0=");
    user_log_hex_inline(req->args[0]);
    user_log(" a1=");
    user_log_hex_inline(req->args[1]);
    user_log(" a2=");
    user_log_hex_inline(req->args[2]);
    user_log(" a3=");
    user_log_hex_inline(req->args[3]);
    user_log(" a4=");
    user_log_hex_inline(req->args[4]);
    user_log(" a5=");
    user_log_hex_inline(req->args[5]);
    user_log("\n");
}

static struct ipc_message reply(u64 result, u64 flags) {
    trace_errno_reply(result, flags);
    if (try_reply_signal_frame(result, flags)) return wait_ipc_timeout(1);
    const u64 explicit_target = abi_reply_target_principal();
    if (explicit_target != 0) {
        const u64 target = explicit_target;
        abi_set_reply_target_principal(0);
        const u64 status = syscall3(SYSCALL_REPLY_ABI_TRAP_TARGET, target, result, flags);
        if (status == SYSCALL_OK) (void)syscall0(SYSCALL_DETACH_ABI_TRAP_REPLY_TOKEN);
        else return reply_current_token(result, flags);
        return wait_ipc_timeout(1);
    }
    return reply_current_token(result, flags);
}

static u64 errno_noent(void) { return (u64)(i64)-2; }
static u64 errno_perm(void) { return (u64)(i64)-1; }
static u64 errno_intr(void) { return (u64)(i64)-4; }
static u64 errno_io(void) { return (u64)(i64)-5; }
static u64 errno_badf(void) { return (u64)(i64)-9; }
static u64 errno_again(void) { return (u64)(i64)-11; }
static u64 errno_nomem(void) { return (u64)(i64)-12; }
static u64 errno_acces(void) { return (u64)(i64)-13; }
static u64 errno_fault(void) { return (u64)(i64)-14; }
static u64 errno_busy(void) { return (u64)(i64)-16; }
static u64 errno_exist(void) { return (u64)(i64)-17; }
static u64 errno_child(void) { return (u64)(i64)-10; }
static u64 errno_notdir(void) { return (u64)(i64)-20; }
static u64 errno_inval(void) { return (u64)(i64)-22; }
static u64 errno_notty(void) { return (u64)(i64)-25; }
static u64 errno_spipe(void) { return (u64)(i64)-29; }
static u64 errno_pipe(void) { return (u64)(i64)-32; }
static u64 errno_nosys(void) { return (u64)(i64)-38; }
static u64 errno_nametoolong(void) { return (u64)(i64)-36; }
static u64 errno_nospc(void) { return (u64)(i64)-28; }
static u64 errno_fbig(void) { return (u64)(i64)-27; }
static u64 errno_range(void) { return (u64)(i64)-34; }
static u64 errno_destaddrreq(void) { return (u64)(i64)-89; }
static u64 errno_msgsize(void) { return (u64)(i64)-90; }
static u64 errno_protonosupport(void) { return (u64)(i64)-93; }
static u64 errno_socktnosupport(void) { return (u64)(i64)-94; }
static u64 errno_opnotsupp(void) { return (u64)(i64)-95; }
static u64 errno_afnosupport(void) { return (u64)(i64)-97; }
static u64 errno_addrinuse(void) { return (u64)(i64)-98; }
static u64 errno_netunreach(void) { return (u64)(i64)-101; }
static u64 errno_isconn(void) { return (u64)(i64)-106; }
static u64 errno_notconn(void) { return (u64)(i64)-107; }
static u64 errno_timedout(void) { return (u64)(i64)-110; }
static u64 errno_already(void) { return (u64)(i64)-114; }
static u64 errno_inprogress(void) { return (u64)(i64)-115; }

static u64 map_reply_target_pages(u64 target_va, u64 page_count, u64 prot_bits) { return syscall3(SYSCALL_MAP_ABI_TRAP_REPLY_TARGET_PAGES, target_va, page_count, prot_bits); }
static u64 reserve_reply_target_pages(u64 target_va, u64 page_count, u64 prot_bits) { return syscall3(SYSCALL_RESERVE_ABI_TRAP_REPLY_TARGET_PAGES, target_va, page_count, prot_bits); }
static u64 protect_reply_target_pages(u64 target_va, u64 page_count, u64 prot_bits) { return syscall3(SYSCALL_PROTECT_ABI_TRAP_REPLY_TARGET_PAGES, target_va, page_count, prot_bits); }
static u64 unmap_reply_target_pages(u64 target_va, u64 page_count) { return syscall2(SYSCALL_UNMAP_ABI_TRAP_REPLY_TARGET_PAGES, target_va, page_count); }
static u64 map_current_pages_to_reply_target(u64 source_va, u64 target_va, u64 page_count, u64 prot_bits) { return syscall4(SYSCALL_MAP_CURRENT_PAGES_TO_ABI_TRAP_REPLY_TARGET, source_va, target_va, page_count, prot_bits); }
static u64 cow_reply_target_page(u64 target_va, u64 prot_bits) { return syscall2(SYSCALL_COW_ABI_TRAP_REPLY_TARGET_PAGE, target_va, prot_bits); }
static u64 map_vm_object_to_reply_target(u64 vm_token, u64 target_va, u64 prot_bits) { return syscall3(SYSCALL_MAP_VM_OBJECT_TO_ABI_TRAP_REPLY_TARGET, vm_token, target_va, prot_bits); }
static u64 copy_from_target(u64 target_va, void *dst, u64 len) { return syscall3(SYSCALL_COPY_FROM_ABI_TRAP_REPLY_TARGET, (u64)dst, target_va, len); }
static u64 copy_to_target(u64 target_va, const void *src, u64 len) { return syscall3(SYSCALL_COPY_TO_ABI_TRAP_REPLY_TARGET, target_va, (u64)src, len); }
static u64 copy_to_target_bulk(u64 target_va, const void *src, u64 len) { return syscall3(SYSCALL_COPY_TO_ABI_TRAP_REPLY_TARGET_BULK, target_va, (u64)src, len); }
static u64 copy_to_trap_target(u64 principal, u64 target_va, const void *src, u64 len) { return syscall4_r10(SYSCALL_COPY_TO_ABI_TRAP_TARGET, principal, target_va, (u64)src, len); }
static u64 reply_trap_target(u64 principal, u64 result, u64 flags) { return syscall3(SYSCALL_REPLY_ABI_TRAP_TARGET, principal, result, flags); }
static u64 start_trap_target(u64 principal) { return syscall1(SYSCALL_START_ABI_TRAP_TARGET, principal); }
static u64 set_trap_target_request_page(u64 principal, u64 request_page_va) { return syscall2(SYSCALL_SET_ABI_TRAP_TARGET_REQUEST_PAGE, principal, request_page_va); }
static u64 set_target_fs_base(u64 fs_base) { return syscall1(SYSCALL_SET_ABI_TRAP_REPLY_TARGET_FS_BASE, fs_base); }
static u64 clone_reply_target(u64 child_stack, u64 tls) { return syscall2(SYSCALL_CLONE_ABI_TRAP_REPLY_TARGET, child_stack, tls); }
static u64 detach_reply_token(void) { return syscall0(SYSCALL_DETACH_ABI_TRAP_REPLY_TOKEN); }
static u64 share_reply_target_pages_to_trap_target(u64 principal, u64 target_va, u64 page_count, u64 prot_bits) { return syscall4_r10(SYSCALL_SHARE_ABI_TRAP_REPLY_TARGET_PAGES_TO_TARGET, principal, target_va, page_count, prot_bits); }
static u64 unmap_trap_target_pages(u64 principal, u64 target_va, u64 page_count) { return syscall3(SYSCALL_UNMAP_ABI_TRAP_TARGET_PAGES, principal, target_va, page_count); }
static u64 alloc_map_pages(u64 target_va, u64 page_count, u64 flags) { return syscall4(SYSCALL_ALLOC_MAP_PAGES, target_va, page_count, flags, 0); }
static u64 map_page_anywhere(u64 paddr, u64 flags) { return syscall2(SYSCALL_MAP_PAGE_ANYWHERE, paddr, flags); }
static u64 alloc_map_pages_anywhere(u64 page_count, u64 flags, u64 out_paddr_list_addr) { return syscall3(SYSCALL_ALLOC_MAP_PAGES_ANYWHERE, page_count, flags, out_paddr_list_addr); }
static int is_ipc_buffer_token(u64 token) { return (token & ~IPC_BUFFER_TOKEN_MASK) == IPC_BUFFER_TOKEN_TAG && (token & IPC_BUFFER_TOKEN_MASK) != 0; }
static u64 create_ipc_buffer_from_page(u64 paddr, u64 rights_bits, u64 role) { return syscall3(SYSCALL_CREATE_IPC_BUFFER_FROM_PAGE, paddr, rights_bits, role); }
static u64 grant_ipc_buffer_on_endpoint(u64 token, u64 endpoint_id, u64 rights_bits) { return syscall3(SYSCALL_GRANT_IPC_BUFFER_ON_ENDPOINT, token, endpoint_id, rights_bits); }
static u64 share_ipc_buffer_on_endpoint(u64 token, u64 endpoint_id, u64 rights_bits) { return syscall3(SYSCALL_SHARE_IPC_BUFFER_ON_ENDPOINT, token, endpoint_id, rights_bits); }
static int install_self_wake_endpoint(void) { return syscall3(SYSCALL_INSTALL_ENDPOINT, 0, LINUX_ABI_SELF_WAKE_ENDPOINT_ID, syscall0(SYSCALL_GET_PROCESS_SLOT)) == SYSCALL_OK; }
static void prime_reply_return_signal(void) { (void)syscall2(SYSCALL_SIGNAL_ENDPOINT, LINUX_ABI_SELF_WAKE_ENDPOINT_ID, 0); }

static void exit_trap_target_no_wait(u64 principal) {
    abi_set_reply_target_principal(0);
    const u64 status = reply_trap_target(principal, 0, TRAP_RESPONSE_FLAG_EXIT);
    if (status != SYSCALL_OK) {
        user_log("LinuxAbiServer: explicit exit reply failed=");
        user_log_hex_value(status);
    }
}

static struct ipc_message exit_trap_target_and_wait(u64 principal) {
    exit_trap_target_no_wait(principal);
    return wait_ipc();
}

static u64 trap_request_page_for_principal(u64 principal) {
    if (principal >= LINUX_ABI_REQUEST_PAGE_COUNT) return 0;
    return LINUX_ABI_REQUEST_PAGES_VA + principal * PAGE_BYTES;
}

static int is_known_trap_request_page(u64 request_va) {
    if (request_va == trap_request_page_va) return 1;
    if (request_va < LINUX_ABI_REQUEST_PAGES_VA) return 0;
    const u64 offset = request_va - LINUX_ABI_REQUEST_PAGES_VA;
    return (offset & (PAGE_BYTES - 1)) == 0 && offset / PAGE_BYTES < LINUX_ABI_REQUEST_PAGE_COUNT;
}

static int ensure_all_child_trap_request_pages(void) {
    for (u64 principal = 0; principal < LINUX_ABI_REQUEST_PAGE_COUNT; principal++) {
        if (g_request_page_mapped[principal]) continue;
        const u64 request_va = trap_request_page_for_principal(principal);
        const u64 status = alloc_map_pages(request_va, 1, 0x3);
        if (status != SYSCALL_OK) {
            user_log("LinuxAbiServer: request page table map failed principal=");
            user_log_hex_value(principal);
            user_log("LinuxAbiServer: request page table map status=");
            user_log_hex_value(status);
            return 0;
        }
        g_request_page_mapped[principal] = 1;
        clear_page(request_va);
    }
    return 1;
}

static int ensure_child_trap_request_page(u64 principal, u64 *request_va_out) {
    const u64 request_va = trap_request_page_for_principal(principal);
    if (request_va == 0) {
        user_log("LinuxAbiServer: request page principal out of range=");
        user_log_hex_value(principal);
        return 0;
    }
    if (!g_request_page_mapped[principal]) {
        const u64 status = alloc_map_pages(request_va, 1, 0x3);
        if (status != SYSCALL_OK) {
            user_log("LinuxAbiServer: request page map failed principal=");
            user_log_hex_value(principal);
            user_log("LinuxAbiServer: request page map status=");
            user_log_hex_value(status);
            return 0;
        }
        g_request_page_mapped[principal] = 1;
    }
    clear_page(request_va);
    const u64 set_status = set_trap_target_request_page(principal, request_va);
    if (set_status != SYSCALL_OK) {
        user_log("LinuxAbiServer: request page set failed principal=");
        user_log_hex_value(principal);
        user_log("LinuxAbiServer: request page set va=");
        user_log_hex_value(request_va);
        user_log("LinuxAbiServer: request page set status=");
        user_log_hex_value(set_status);
        return 0;
    }
    *request_va_out = request_va;
    return 1;
}

static int copy_cstr_from_target(u64 target_va, char *dst, u64 cap) {
    if (target_va == 0 || cap == 0) return 0;
    u64 copied = 0;
    while (copied + 1 < cap) {
        u64 chunk = min_u64(cap - 1 - copied, 64);
        const u64 page_left = PAGE_BYTES - ((target_va + copied) & (PAGE_BYTES - 1));
        chunk = min_u64(chunk, page_left);
        const u64 got = copy_from_target(target_va + copied, dst + copied, chunk);
        if (got == 0) return 0;
        for (u64 i = 0; i < got; i++) {
            if (dst[copied + i] == 0) return 1;
        }
        if (got != chunk) return 0;
        copied += got;
    }
    dst[cap - 1] = 0;
    return 0;
}

static int find_service(u64 kind, struct service_entry *out) {
    volatile struct service_registry_page *page = (volatile struct service_registry_page *)SERVICE_REGISTRY_SHADOW_VA;
    if (page->magic != SERVICE_REGISTRY_MAGIC || page->version != SERVICE_REGISTRY_VERSION) return 0;
    for (u64 i = 0; i < page->entry_count && i < SERVICE_REGISTRY_MAX_ENTRIES; i++) {
        if (page->entries[i].kind != kind) continue;
        out->kind = page->entries[i].kind; out->process_slot = page->entries[i].process_slot; out->endpoint_id = page->entries[i].endpoint_id; out->flags = page->entries[i].flags;
        return out->endpoint_id != 0 && out->process_slot != 0;
    }
    return 0;
}

static u64 make_nonce(u64 request_token, u64 response_token, u64 endpoint_id, u64 process_slot) {
    return request_token ^ ((response_token << 17) | (response_token >> 47)) ^ (endpoint_id << 1) ^ (process_slot << 33) ^ 0x4653434f4e4e4543ULL;
}
static int wait_fs_response_at(u64 response_va, u64 expected_seq, u16 expected_op) {
    volatile struct fs_response_header *response = (volatile struct fs_response_header *)response_va;
    g_prof.vfs_wait_calls++;
    for (u64 i = 0; i < 8192; i++) {
        if (response->response_seq == expected_seq) {
            g_prof.vfs_wait_loops += i;
            if (i > 8) g_prof.vfs_wait_slow++;
            return response->magic == FS_RESPONSE_MAGIC && response->version == FS_PROTOCOL_VERSION && response->op == expected_op;
        }
        wait_without_consuming_ipc();
    }
    g_prof.vfs_wait_loops += 8192;
    g_prof.vfs_wait_timeouts++;
    return 0;
}

static int wait_vfs_response(u64 expected_seq, u16 expected_op) {
    return wait_fs_response_at(g_vfs.response_map.addr, expected_seq, expected_op);
}

static void profile_count_syscall(u64 nr) {
    g_prof.syscall_total++;
    if (nr <= LINUX_SYSCALL_PROFILE_COUNT) g_prof.syscall_counts[nr]++;
}

static void profile_record_syscall_ticks(u64 nr, u64 ticks) {
    if (nr > LINUX_SYSCALL_PROFILE_COUNT) return;
    g_prof.syscall_ticks[nr] += ticks;
    if (ticks > g_prof.syscall_max_ticks[nr]) g_prof.syscall_max_ticks[nr] = ticks;
}

static void profile_print_syscall(const char *name, u64 nr) {
    if (nr > LINUX_SYSCALL_PROFILE_COUNT || g_prof.syscall_counts[nr] == 0) return;
    user_log("LinuxAbiServer.perf.syscall ");
    user_log(name);
    user_log(" count=");
    user_log_dec_value(g_prof.syscall_counts[nr]);
    user_log(" ticks=");
    user_log_dec_value(g_prof.syscall_ticks[nr]);
    user_log(" max=");
    user_log_dec_value(g_prof.syscall_max_ticks[nr]);
    user_log("\n");
}

static void profile_print_fs_op(const char *name, u64 op) {
    if (op >= FS_PROFILE_OP_COUNT || g_prof.vfs_op_counts[op] == 0) return;
    user_log("LinuxAbiServer.perf.vfs.op ");
    user_log(name);
    user_log("=");
    user_log_dec_value(g_prof.vfs_op_counts[op]);
    user_log("\n");
}

static void profile_print_net_op(const char *name, u64 op) {
    if (op >= NET_PROFILE_OP_COUNT || g_prof.net_op_counts[op] == 0) return;
    user_log("LinuxAbiServer.perf.net.op ");
    user_log(name);
    user_log("=");
    user_log_dec_value(g_prof.net_op_counts[op]);
    user_log("\n");
}

static void profile_print_net_wait_op(const char *name, u64 op) {
    if (op >= NET_PROFILE_OP_COUNT || g_prof.net_wait_op_calls[op] == 0) return;
    user_log("LinuxAbiServer.perf.net.wait_op ");
    user_log(name);
    user_log(" calls=");
    user_log_dec_value(g_prof.net_wait_op_calls[op]);
    user_log(" loops=");
    user_log_dec_value(g_prof.net_wait_op_loops[op]);
    user_log(" slow=");
    user_log_dec_value(g_prof.net_wait_op_slow[op]);
    user_log(" timeouts=");
    user_log_dec_value(g_prof.net_wait_op_timeouts[op]);
    user_log("\n");
}

static void profile_print_net_wait_context(const char *name, u64 context) {
    if (context >= NET_WAIT_CONTEXT_COUNT || g_prof.net_wait_context_calls[context] == 0) return;
    user_log("LinuxAbiServer.perf.net.wait_context ");
    user_log(name);
    user_log(" calls=");
    user_log_dec_value(g_prof.net_wait_context_calls[context]);
    user_log(" loops=");
    user_log_dec_value(g_prof.net_wait_context_loops[context]);
    user_log(" slow=");
    user_log_dec_value(g_prof.net_wait_context_slow[context]);
    user_log(" timeouts=");
    user_log_dec_value(g_prof.net_wait_context_timeouts[context]);
    user_log("\n");
}

static void profile_print_vm_bucket(const char *prefix, const char *name, u64 index, const u64 *calls, const u64 *pages) {
    if (calls[index] == 0) return;
    user_log(prefix);
    user_log(name);
    user_log(" calls=");
    user_log_dec_value(calls[index]);
    user_log(" pages=");
    user_log_dec_value(pages[index]);
    user_log("\n");
}

static void profile_print_byte_bucket(const char *prefix, const char *name, u64 index, const u64 *calls, const u64 *bytes) {
    if (calls[index] == 0) return;
    user_log(prefix);
    user_log(name);
    user_log(" calls=");
    user_log_dec_value(calls[index]);
    user_log(" bytes=");
    user_log_dec_value(bytes[index]);
    user_log("\n");
}

static void profile_clear(void) {
    u8 *p = (u8 *)&g_prof;
    for (u64 i = 0; i < sizeof(g_prof); i++) p[i] = 0;
}

static void profile_report_and_reset(void) {
    user_log("LinuxAbiServer.perf.begin exec=");
    user_log(g_exec_path);
    user_log("\n");
    if (!profile_detail_enabled()) {
        user_log_dec_line("LinuxAbiServer.perf.summary.syscalls=", g_prof.syscall_total);
        user_log_dec_line("LinuxAbiServer.perf.summary.vfs_requests=", g_prof.vfs_requests);
        user_log_dec_line("LinuxAbiServer.perf.summary.fs_read_bytes=", g_prof.fs_read_bytes);
        user_log_dec_line("LinuxAbiServer.perf.summary.path_misses=", g_prof.path_cache_misses);
        user_log("LinuxAbiServer.perf.end\n");
        profile_clear();
        return;
    }
    user_log_dec_line("LinuxAbiServer.perf.syscalls.total=", g_prof.syscall_total);
    profile_print_syscall("read", LINUX_SYS_READ);
    profile_print_syscall("write", LINUX_SYS_WRITE);
    profile_print_syscall("readv", LINUX_SYS_READV);
    profile_print_syscall("writev", LINUX_SYS_WRITEV);
    profile_print_syscall("open", LINUX_SYS_OPEN);
    profile_print_syscall("openat", LINUX_SYS_OPENAT);
    profile_print_syscall("close", LINUX_SYS_CLOSE);
    profile_print_syscall("stat", LINUX_SYS_STAT);
    profile_print_syscall("fstat", LINUX_SYS_FSTAT);
    profile_print_syscall("newfstatat", LINUX_SYS_NEWFSTATAT);
    profile_print_syscall("access", LINUX_SYS_ACCESS);
    profile_print_syscall("mmap", LINUX_SYS_MMAP);
    profile_print_syscall("mprotect", LINUX_SYS_MPROTECT);
    profile_print_syscall("munmap", LINUX_SYS_MUNMAP);
    profile_print_syscall("mremap", LINUX_SYS_MREMAP);
    profile_print_syscall("brk", LINUX_SYS_BRK);
    profile_print_syscall("poll", LINUX_SYS_POLL);
    profile_print_syscall("select", LINUX_SYS_SELECT);
    profile_print_syscall("ppoll", LINUX_SYS_PPOLL);
    profile_print_syscall("pselect6", LINUX_SYS_PSELECT6);
    profile_print_syscall("socket", LINUX_SYS_SOCKET);
    profile_print_syscall("connect", LINUX_SYS_CONNECT);
    profile_print_syscall("sendto", LINUX_SYS_SENDTO);
    profile_print_syscall("recvfrom", LINUX_SYS_RECVFROM);
    profile_print_syscall("sendmsg", LINUX_SYS_SENDMSG);
    profile_print_syscall("recvmsg", LINUX_SYS_RECVMSG);
    profile_print_syscall("getsockopt", LINUX_SYS_GETSOCKOPT);
    profile_print_syscall("setsockopt", LINUX_SYS_SETSOCKOPT);
    profile_print_syscall("time", LINUX_SYS_TIME);
    profile_print_syscall("gettimeofday", LINUX_SYS_GETTIMEOFDAY);
    profile_print_syscall("getrandom", LINUX_SYS_GETRANDOM);
    profile_print_syscall("clock_gettime", LINUX_SYS_CLOCK_GETTIME);
    profile_print_syscall("nanosleep", LINUX_SYS_NANOSLEEP);
    profile_print_syscall("clock_nanosleep", LINUX_SYS_CLOCK_NANOSLEEP);
    profile_print_syscall("setitimer", LINUX_SYS_SETITIMER);
    profile_print_syscall("rt_sigtimedwait", LINUX_SYS_RT_SIGTIMEDWAIT);
    profile_print_syscall("timer_create", LINUX_SYS_TIMER_CREATE);
    profile_print_syscall("timer_settime", LINUX_SYS_TIMER_SETTIME);
    profile_print_syscall("timer_gettime", LINUX_SYS_TIMER_GETTIME);
    profile_print_syscall("fcntl", LINUX_SYS_FCNTL);
    profile_print_syscall("ioctl", LINUX_SYS_IOCTL);

    user_log_dec_line("LinuxAbiServer.perf.vfs.requests=", g_prof.vfs_requests);
    profile_print_fs_op("lookup", FS_OP_LOOKUP);
    profile_print_fs_op("open", FS_OP_OPEN);
    profile_print_fs_op("read", FS_OP_READ);
    profile_print_fs_op("read_bulk", FS_OP_READ_BULK);
    profile_print_fs_op("readdir", FS_OP_READDIR);
    profile_print_fs_op("stat", FS_OP_STAT);
    profile_print_fs_op("close", FS_OP_CLOSE);
    profile_print_fs_op("create", FS_OP_CREATE);
    profile_print_fs_op("write", FS_OP_WRITE);
    profile_print_fs_op("write_bulk", FS_OP_WRITE_BULK);
    profile_print_fs_op("unlink", FS_OP_UNLINK);
    profile_print_fs_op("rename", FS_OP_RENAME);
    user_log_dec_line("LinuxAbiServer.perf.vfs.read_request_bytes=", g_prof.vfs_read_request_bytes);
    user_log_dec_line("LinuxAbiServer.perf.vfs.write_request_bytes=", g_prof.vfs_write_request_bytes);
    user_log_dec_line("LinuxAbiServer.perf.vfs.wait_calls=", g_prof.vfs_wait_calls);
    user_log_dec_line("LinuxAbiServer.perf.vfs.wait_loops=", g_prof.vfs_wait_loops);
    user_log_dec_line("LinuxAbiServer.perf.vfs.wait_slow=", g_prof.vfs_wait_slow);
    user_log_dec_line("LinuxAbiServer.perf.vfs.wait_timeouts=", g_prof.vfs_wait_timeouts);
    user_log_dec_line("LinuxAbiServer.perf.vfs.bulk_cap_pages=", g_prof.vfs_bulk_cap_pages);
    user_log_dec_line("LinuxAbiServer.perf.vfs.bulk_cap_ticks=", g_prof.vfs_bulk_cap_ticks);
    user_log_dec_line("LinuxAbiServer.perf.vfs.bulk_request_ticks=", g_prof.vfs_bulk_request_ticks);
    user_log_dec_line("LinuxAbiServer.perf.vfs.bulk_copy_ticks=", g_prof.vfs_bulk_copy_ticks);
    user_log_dec_line("LinuxAbiServer.perf.vfs.bulk_direct_pages=", g_prof.vfs_bulk_direct_pages);
    user_log_dec_line("LinuxAbiServer.perf.vfs.bulk_direct_ticks=", g_prof.vfs_bulk_direct_ticks);
    user_log_dec_line("LinuxAbiServer.perf.vfs.bulk_direct_attempts=", g_prof.vfs_bulk_direct_attempts);
    user_log_dec_line("LinuxAbiServer.perf.vfs.bulk_direct_fallback_pages=", g_prof.vfs_bulk_direct_fallback_pages);
    user_log_dec_line("LinuxAbiServer.perf.vfs.bulk_direct_paddr_fail=", g_prof.vfs_bulk_direct_paddr_fail);
    user_log_dec_line("LinuxAbiServer.perf.vfs.bulk_direct_signal_fail=", g_prof.vfs_bulk_direct_signal_fail);
    user_log_dec_line("LinuxAbiServer.perf.vfs.bulk_direct_wait_fail=", g_prof.vfs_bulk_direct_wait_fail);
    user_log_dec_line("LinuxAbiServer.perf.vfs.bulk_direct_status_fail=", g_prof.vfs_bulk_direct_status_fail);
    user_log_dec_line("LinuxAbiServer.perf.vfs.bulk_direct_bytes_fail=", g_prof.vfs_bulk_direct_bytes_fail);

    user_log_dec_line("LinuxAbiServer.perf.fs.read_bytes=", g_prof.fs_read_bytes);
    user_log_dec_line("LinuxAbiServer.perf.fs.read_cmd_bytes=", g_prof.fs_read_cmd_bytes);
    user_log_dec_line("LinuxAbiServer.perf.fs.read_lib_bytes=", g_prof.fs_read_lib_bytes);
    user_log_dec_line("LinuxAbiServer.perf.fs.read_tmp_bytes=", g_prof.fs_read_tmp_bytes);
    user_log_dec_line("LinuxAbiServer.perf.fs.read_proc_bytes=", g_prof.fs_read_proc_bytes);
    user_log_dec_line("LinuxAbiServer.perf.fs.write_bytes=", g_prof.fs_write_bytes);
    user_log_dec_line("LinuxAbiServer.perf.cache.file_hits=", g_prof.file_cache_hits);
    user_log_dec_line("LinuxAbiServer.perf.cache.file_misses=", g_prof.file_cache_misses);
    user_log_dec_line("LinuxAbiServer.perf.cache.file_fill_bytes=", g_prof.file_cache_fill_bytes);
    user_log_dec_line("LinuxAbiServer.perf.cache.file_map_pages=", g_prof.file_cache_map_pages);
    user_log_dec_line("LinuxAbiServer.perf.cache.file_cow_faults=", g_prof.file_cache_cow_faults);
    user_log_dec_line("LinuxAbiServer.perf.cache.file_cow_considered=", g_prof.file_cache_cow_considered);
    user_log_dec_line("LinuxAbiServer.perf.cache.file_cow_candidates=", g_prof.file_cache_cow_candidates);
    user_log_dec_line("LinuxAbiServer.perf.cache.file_cow_skip_peers=", g_prof.file_cache_cow_skip_peers);
    user_log_dec_line("LinuxAbiServer.perf.cache.file_cow_skip_no_path=", g_prof.file_cache_cow_skip_no_path);
    user_log_dec_line("LinuxAbiServer.perf.cache.file_cow_skip_uncacheable=", g_prof.file_cache_cow_skip_uncacheable);
    user_log_dec_line("LinuxAbiServer.perf.cache.file_cow_fallbacks=", g_prof.file_cache_cow_fallbacks);
    user_log_dec_line("LinuxAbiServer.perf.cache.file_vm_object_considered=", g_prof.file_vm_object_mmap_considered);
    user_log_dec_line("LinuxAbiServer.perf.cache.file_vm_object_candidates=", g_prof.file_vm_object_mmap_candidates);
    user_log_dec_line("LinuxAbiServer.perf.cache.file_vm_object_mapped=", g_prof.file_vm_object_mmap_mapped);
    user_log_dec_line("LinuxAbiServer.perf.cache.file_vm_object_fallbacks=", g_prof.file_vm_object_mmap_fallbacks);
    user_log_dec_line("LinuxAbiServer.perf.cache.file_vm_object_pages=", g_prof.file_vm_object_mmap_pages);
    user_log_dec_line("LinuxAbiServer.perf.cache.file_vm_object_tail_pages=", g_prof.file_vm_object_mmap_tail_pages);
    user_log_dec_line("LinuxAbiServer.perf.cache.file_vm_object_install_fail=", g_prof.file_vm_object_mmap_install_fail);
    user_log_dec_line("LinuxAbiServer.perf.cache.file_vm_object_map_fail=", g_prof.file_vm_object_mmap_map_fail);
    user_log_dec_line("LinuxAbiServer.perf.cache.file_fill_fail_no_path=", g_prof.file_cache_fill_fail_no_path);
    user_log_dec_line("LinuxAbiServer.perf.cache.file_fill_fail_uncacheable=", g_prof.file_cache_fill_fail_uncacheable);
    user_log_dec_line("LinuxAbiServer.perf.cache.file_fill_fail_size=", g_prof.file_cache_fill_fail_size);
    user_log_dec_line("LinuxAbiServer.perf.cache.file_fill_fail_slot=", g_prof.file_cache_fill_fail_slot);
    user_log_dec_line("LinuxAbiServer.perf.cache.file_fill_fail_alloc=", g_prof.file_cache_fill_fail_alloc);
    user_log_dec_line("LinuxAbiServer.perf.cache.file_fill_fail_read=", g_prof.file_cache_fill_fail_read);
    user_log_dec_line("LinuxAbiServer.perf.cache.file_map_fail_unaligned=", g_prof.file_cache_map_fail_unaligned);
    user_log_dec_line("LinuxAbiServer.perf.cache.file_map_fail_fill=", g_prof.file_cache_map_fail_fill);
    user_log_dec_line("LinuxAbiServer.perf.cache.file_map_fail_range=", g_prof.file_cache_map_fail_range);
    user_log_dec_line("LinuxAbiServer.perf.cache.file_map_fail_syscall=", g_prof.file_cache_map_fail_syscall);
    user_log_dec_line("LinuxAbiServer.perf.cache.path_hits=", g_prof.path_cache_hits);
    user_log_dec_line("LinuxAbiServer.perf.cache.path_misses=", g_prof.path_cache_misses);
    user_log_dec_line("LinuxAbiServer.perf.cache.open_hits=", g_prof.open_cache_hits);
    user_log_dec_line("LinuxAbiServer.perf.cache.open_misses=", g_prof.open_cache_misses);

    user_log_dec_line("LinuxAbiServer.perf.vm.mmap_calls=", g_prof.mmap_calls);
    user_log_dec_line("LinuxAbiServer.perf.vm.mmap_pages=", g_prof.mmap_pages);
    user_log_dec_line("LinuxAbiServer.perf.vm.mmap_file_calls=", g_prof.mmap_file_calls);
    user_log_dec_line("LinuxAbiServer.perf.vm.mmap_file_pages=", g_prof.mmap_file_pages);
    user_log_dec_line("LinuxAbiServer.perf.vm.mmap_file_bytes=", g_prof.mmap_file_bytes);
    profile_print_vm_bucket("LinuxAbiServer.perf.vm.mmap_bucket ", "1", 0, g_prof.mmap_bucket_calls, g_prof.mmap_bucket_pages);
    profile_print_vm_bucket("LinuxAbiServer.perf.vm.mmap_bucket ", "2_4", 1, g_prof.mmap_bucket_calls, g_prof.mmap_bucket_pages);
    profile_print_vm_bucket("LinuxAbiServer.perf.vm.mmap_bucket ", "5_16", 2, g_prof.mmap_bucket_calls, g_prof.mmap_bucket_pages);
    profile_print_vm_bucket("LinuxAbiServer.perf.vm.mmap_bucket ", "17_64", 3, g_prof.mmap_bucket_calls, g_prof.mmap_bucket_pages);
    profile_print_vm_bucket("LinuxAbiServer.perf.vm.mmap_bucket ", "65_plus", 4, g_prof.mmap_bucket_calls, g_prof.mmap_bucket_pages);
    user_log_dec_line("LinuxAbiServer.perf.vm.munmap_calls=", g_prof.munmap_calls);
    user_log_dec_line("LinuxAbiServer.perf.vm.munmap_pages=", g_prof.munmap_pages);
    profile_print_vm_bucket("LinuxAbiServer.perf.vm.munmap_bucket ", "1", 0, g_prof.munmap_bucket_calls, g_prof.munmap_bucket_pages);
    profile_print_vm_bucket("LinuxAbiServer.perf.vm.munmap_bucket ", "2_4", 1, g_prof.munmap_bucket_calls, g_prof.munmap_bucket_pages);
    profile_print_vm_bucket("LinuxAbiServer.perf.vm.munmap_bucket ", "5_16", 2, g_prof.munmap_bucket_calls, g_prof.munmap_bucket_pages);
    profile_print_vm_bucket("LinuxAbiServer.perf.vm.munmap_bucket ", "17_64", 3, g_prof.munmap_bucket_calls, g_prof.munmap_bucket_pages);
    profile_print_vm_bucket("LinuxAbiServer.perf.vm.munmap_bucket ", "65_plus", 4, g_prof.munmap_bucket_calls, g_prof.munmap_bucket_pages);
    user_log_dec_line("LinuxAbiServer.perf.vm.mprotect_calls=", g_prof.mprotect_calls);
    user_log_dec_line("LinuxAbiServer.perf.vm.mprotect_pages=", g_prof.mprotect_pages);
    user_log_dec_line("LinuxAbiServer.perf.vm.brk_calls=", g_prof.brk_calls);

    user_log_dec_line("LinuxAbiServer.perf.net.requests=", g_prof.net_requests);
    profile_print_net_op("connect", NET_OP_CONNECT);
    profile_print_net_op("bind", NET_OP_BIND);
    profile_print_net_op("send_to", NET_OP_SEND_TO);
    profile_print_net_op("recv_from", NET_OP_RECV_FROM);
    profile_print_net_op("close", NET_OP_CLOSE);
    profile_print_net_op("poll", NET_OP_POLL);
    profile_print_net_op("tcp_connect", NET_OP_TCP_CONNECT);
    profile_print_net_op("tcp_write", NET_OP_TCP_WRITE);
    profile_print_net_op("tcp_read", NET_OP_TCP_READ);
    profile_print_net_op("tcp_read_bulk", NET_OP_TCP_READ_BULK);
    user_log_dec_line("LinuxAbiServer.perf.net.tx_payload_bytes=", g_prof.net_payload_tx_bytes);
    user_log_dec_line("LinuxAbiServer.perf.net.rx_payload_bytes=", g_prof.net_payload_rx_bytes);
    user_log_dec_line("LinuxAbiServer.perf.net.wait_calls=", g_prof.net_wait_calls);
    user_log_dec_line("LinuxAbiServer.perf.net.wait_loops=", g_prof.net_wait_loops);
    user_log_dec_line("LinuxAbiServer.perf.net.wait_slow=", g_prof.net_wait_slow);
    user_log_dec_line("LinuxAbiServer.perf.net.wait_timeouts=", g_prof.net_wait_timeouts);
    profile_print_net_wait_op("connect", NET_OP_CONNECT);
    profile_print_net_wait_op("bind", NET_OP_BIND);
    profile_print_net_wait_op("send_to", NET_OP_SEND_TO);
    profile_print_net_wait_op("recv_from", NET_OP_RECV_FROM);
    profile_print_net_wait_op("close", NET_OP_CLOSE);
    profile_print_net_wait_op("poll", NET_OP_POLL);
    profile_print_net_wait_op("tcp_connect", NET_OP_TCP_CONNECT);
    profile_print_net_wait_op("tcp_write", NET_OP_TCP_WRITE);
    profile_print_net_wait_op("tcp_read", NET_OP_TCP_READ);
    profile_print_net_wait_op("tcp_read_bulk", NET_OP_TCP_READ_BULK);
    profile_print_net_wait_context("poll_prefetch_read", NET_WAIT_CONTEXT_POLL_PREFETCH_READ);
    profile_print_net_wait_context("recvmsg_blocking_read", NET_WAIT_CONTEXT_RECVMSG_BLOCKING_READ);
    profile_print_net_wait_context("recvmsg_nowait_read", NET_WAIT_CONTEXT_RECVMSG_NOWAIT_READ);
    profile_print_net_wait_context("read_inline_blocking", NET_WAIT_CONTEXT_READ_INLINE_BLOCKING);
    profile_print_net_wait_context("read_inline_nowait", NET_WAIT_CONTEXT_READ_INLINE_NOWAIT);
    profile_print_net_wait_context("read_bulk_blocking", NET_WAIT_CONTEXT_READ_BULK_BLOCKING);
    profile_print_net_wait_context("read_bulk_nowait", NET_WAIT_CONTEXT_READ_BULK_NOWAIT);
    user_log_dec_line("LinuxAbiServer.perf.net.tcp_connect_attempts=", g_prof.net_tcp_connect_attempts);
    user_log_dec_line("LinuxAbiServer.perf.net.tcp_connect_poll_loops=", g_prof.net_tcp_connect_poll_loops);
    user_log_dec_line("LinuxAbiServer.perf.net.tcp_prefetch_attempts=", g_prof.net_tcp_prefetch_attempts);
    user_log_dec_line("LinuxAbiServer.perf.net.tcp_prefetch_ready_hits=", g_prof.net_tcp_prefetch_ready_hits);
    user_log_dec_line("LinuxAbiServer.perf.net.tcp_prefetch_bytes=", g_prof.net_tcp_prefetch_bytes);
    user_log_dec_line("LinuxAbiServer.perf.net.tcp_prefetch_consumed=", g_prof.net_tcp_prefetch_consumed);
    user_log_dec_line("LinuxAbiServer.perf.net.tcp_prefetch_eof=", g_prof.net_tcp_prefetch_eof);
    profile_print_byte_bucket("LinuxAbiServer.perf.net.tcp_read_request_bucket ", "0", 0, g_prof.net_tcp_read_request_bucket_calls, g_prof.net_tcp_read_request_bucket_bytes);
    profile_print_byte_bucket("LinuxAbiServer.perf.net.tcp_read_request_bucket ", "1_512", 1, g_prof.net_tcp_read_request_bucket_calls, g_prof.net_tcp_read_request_bucket_bytes);
    profile_print_byte_bucket("LinuxAbiServer.perf.net.tcp_read_request_bucket ", "513_1500", 2, g_prof.net_tcp_read_request_bucket_calls, g_prof.net_tcp_read_request_bucket_bytes);
    profile_print_byte_bucket("LinuxAbiServer.perf.net.tcp_read_request_bucket ", "1501_4096", 3, g_prof.net_tcp_read_request_bucket_calls, g_prof.net_tcp_read_request_bucket_bytes);
    profile_print_byte_bucket("LinuxAbiServer.perf.net.tcp_read_request_bucket ", "4097_16384", 4, g_prof.net_tcp_read_request_bucket_calls, g_prof.net_tcp_read_request_bucket_bytes);
    profile_print_byte_bucket("LinuxAbiServer.perf.net.tcp_read_request_bucket ", "16385_plus", 5, g_prof.net_tcp_read_request_bucket_calls, g_prof.net_tcp_read_request_bucket_bytes);
    profile_print_byte_bucket("LinuxAbiServer.perf.net.tcp_read_return_bucket ", "0", 0, g_prof.net_tcp_read_return_bucket_calls, g_prof.net_tcp_read_return_bucket_bytes);
    profile_print_byte_bucket("LinuxAbiServer.perf.net.tcp_read_return_bucket ", "1_512", 1, g_prof.net_tcp_read_return_bucket_calls, g_prof.net_tcp_read_return_bucket_bytes);
    profile_print_byte_bucket("LinuxAbiServer.perf.net.tcp_read_return_bucket ", "513_1500", 2, g_prof.net_tcp_read_return_bucket_calls, g_prof.net_tcp_read_return_bucket_bytes);
    profile_print_byte_bucket("LinuxAbiServer.perf.net.tcp_read_return_bucket ", "1501_4096", 3, g_prof.net_tcp_read_return_bucket_calls, g_prof.net_tcp_read_return_bucket_bytes);
    profile_print_byte_bucket("LinuxAbiServer.perf.net.tcp_read_return_bucket ", "4097_16384", 4, g_prof.net_tcp_read_return_bucket_calls, g_prof.net_tcp_read_return_bucket_bytes);
    profile_print_byte_bucket("LinuxAbiServer.perf.net.tcp_read_return_bucket ", "16385_plus", 5, g_prof.net_tcp_read_return_bucket_calls, g_prof.net_tcp_read_return_bucket_bytes);
    user_log_dec_line("LinuxAbiServer.perf.net.tcp_read_return_zero_calls=", g_prof.net_tcp_read_return_zero_calls);
    user_log_dec_line("LinuxAbiServer.perf.net.tcp_read_return_prefetch_calls=", g_prof.net_tcp_read_return_prefetch_calls);
    user_log_dec_line("LinuxAbiServer.perf.net.tcp_read_return_prefetch_bytes=", g_prof.net_tcp_read_return_prefetch_bytes);
    user_log_dec_line("LinuxAbiServer.perf.net.tcp_read_return_direct_calls=", g_prof.net_tcp_read_return_direct_calls);
    user_log_dec_line("LinuxAbiServer.perf.net.tcp_read_return_direct_bytes=", g_prof.net_tcp_read_return_direct_bytes);
    user_log_dec_line("LinuxAbiServer.perf.net.tcp_read_return_bulk_calls=", g_prof.net_tcp_read_return_bulk_calls);
    user_log_dec_line("LinuxAbiServer.perf.net.tcp_read_return_bulk_bytes=", g_prof.net_tcp_read_return_bulk_bytes);
    user_log_dec_line("LinuxAbiServer.perf.net.tcp_bulk_cap_pages=", g_prof.net_tcp_bulk_cap_pages);
    user_log_dec_line("LinuxAbiServer.perf.net.tcp_bulk_cap_ticks=", g_prof.net_tcp_bulk_cap_ticks);
    user_log_dec_line("LinuxAbiServer.perf.net.tcp_bulk_copy_ticks=", g_prof.net_tcp_bulk_copy_ticks);
    user_log_dec_line("LinuxAbiServer.perf.poll.calls=", g_prof.poll_calls);
    user_log_dec_line("LinuxAbiServer.perf.poll.wait_loops=", g_prof.poll_wait_loops);
    user_log_dec_line("LinuxAbiServer.perf.select.calls=", g_prof.select_calls);
    user_log_dec_line("LinuxAbiServer.perf.select.wait_loops=", g_prof.select_wait_loops);
    user_log_dec_line("LinuxAbiServer.perf.getrandom.calls=", g_prof.getrandom_calls);
    user_log_dec_line("LinuxAbiServer.perf.getrandom.bytes=", g_prof.getrandom_bytes);
    user_log("LinuxAbiServer.perf.end\n");
    profile_clear();
}
