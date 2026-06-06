static struct ipc_message handle_getcwd(const struct trap_request *req) {
    const u64 dst = req->args[0]; const u64 size = req->args[1]; const u64 needed = (u64)g_cwd_len + 1;
    if (dst == 0 || size < needed) return reply(errno_range(), 0);
    if (copy_to_target(dst, g_cwd, needed) != needed) return reply(errno_fault(), 0);
    return reply(dst, 0);
}

static struct ipc_message handle_chdir(const struct trap_request *req) {
    char path[256]; char virtual_path[FS_MAX_PATH_BYTES + 1]; char resolved[FS_MAX_PATH_BYTES + 1];
    if (!copy_cstr_from_target(req->args[0], path, sizeof(path))) return reply(errno_fault(), 0);
    if (path[0] == 0) return reply(errno_noent(), 0);
    if (!resolve_virtual_path_at(AT_FDCWD_U64, path, virtual_path)) return reply(errno_nametoolong(), 0);
    if (!map_virtual_path_to_host(virtual_path, resolved)) return reply(errno_nametoolong(), 0);
    struct fs_stat_record rec; u64 token = 0, size = 0; u8 kind = 0;
    if (!vfs_lookup_stat(resolved, &token, &rec, &size, &kind)) return reply(errno_noent(), 0);
    if (kind != FS_OBJECT_DIRECTORY && kind != FS_OBJECT_MOUNT) return reply(errno_notdir(), 0);
    g_cwd_len = (u16)cstr_len(virtual_path);
    for (u16 i = 0; i <= g_cwd_len; i++) g_cwd[i] = virtual_path[i];
    return reply(0, 0);
}

static struct ipc_message handle_fchdir(const struct trap_request *req) {
    const u64 fd = req->args[0];
    if (!fd_valid(fd)) return reply(errno_badf(), 0);
    if (g_fds[fd].kind != FD_DIR || g_fds[fd].path_len == 0) return reply(errno_notdir(), 0);
    char virtual_path[FS_MAX_PATH_BYTES + 1];
    if (!host_path_to_virtual(g_fds[fd].path, virtual_path)) return reply(errno_noent(), 0);
    g_cwd_len = (u16)cstr_len(virtual_path);
    for (u16 i = 0; i <= g_cwd_len; i++) g_cwd[i] = virtual_path[i];
    return reply(0, 0);
}

static struct ipc_message handle_chroot(const struct trap_request *req) {
    char path[256];
    char resolved[FS_MAX_PATH_BYTES + 1];
    if (!copy_cstr_from_target(req->args[0], path, sizeof(path))) return reply(errno_fault(), 0);
    if (path[0] == 0) return reply(errno_noent(), 0);
    if (!resolve_path_at(AT_FDCWD_U64, path, resolved)) return reply(errno_nametoolong(), 0);
    struct fs_stat_record rec;
    u64 token = 0, size = 0;
    u8 kind = 0;
    if (!vfs_lookup_stat(resolved, &token, &rec, &size, &kind)) return reply(errno_noent(), 0);
    if (kind != FS_OBJECT_DIRECTORY && kind != FS_OBJECT_MOUNT) return reply(errno_notdir(), 0);
    g_proc->root_len = (u16)cstr_len(resolved);
    for (u16 i = 0; i <= g_proc->root_len; i++) g_proc->root_path[i] = resolved[i];
    g_cwd[0] = '/';
    g_cwd[1] = 0;
    g_cwd_len = 1;
    return reply(0, 0);
}

static int cstr_eq(const char *a, const char *b) {
    u64 i = 0;
    while (a[i] != 0 && b[i] != 0) {
        if (a[i] != b[i]) return 0;
        i++;
    }
    return a[i] == 0 && b[i] == 0;
}

static int vfs_readlink_path(const char *path, char *target, u64 target_capacity, u64 *target_len_out) {
    *target_len_out = 0;
    if (target_capacity == 0) return 0;
    struct fs_stat_record rec;
    u64 token = 0;
    u64 size = 0;
    u8 kind = FS_OBJECT_NONE;
    if (!vfs_lookup_stat(path, &token, &rec, &size, &kind)) return 0;
    if (kind != FS_OBJECT_SYMLINK) return 0;
    u32 request_len = (u32)target_capacity;
    if (request_len > FS_RESPONSE_PAYLOAD_BYTES) request_len = FS_RESPONSE_PAYLOAD_BYTES;
    if (!vfs_request(FS_OP_READ, token, 0, request_len, 0)) return 0;
    volatile struct fs_response_header *response = (volatile struct fs_response_header *)vfs_response_addr();
    if (response->status != FS_STATUS_OK || response->inline_bytes > request_len) return 0;
    volatile u8 *payload = (volatile u8 *)vfs_response_payload();
    for (u16 i = 0; i < response->inline_bytes; i++) target[i] = (char)payload[i];
    *target_len_out = response->inline_bytes;
    (void)rec;
    (void)size;
    return 1;
}

static struct ipc_message handle_readlinkat(const struct trap_request *req, int old_readlink) {
    char path[256]; char resolved[FS_MAX_PATH_BYTES + 1];
    const u64 dirfd = old_readlink ? AT_FDCWD_U64 : req->args[0];
    const u64 path_ptr = old_readlink ? req->args[0] : req->args[1];
    const u64 dst = old_readlink ? req->args[1] : req->args[2];
    const u64 size = old_readlink ? req->args[2] : req->args[3];
    if (!copy_cstr_from_target(path_ptr, path, sizeof(path))) return reply(errno_fault(), 0);
    if (path[0] == 0) return reply(errno_noent(), 0);
    if (!resolve_path_at(dirfd, path, resolved)) return reply(errno_nametoolong(), 0);
    if (cstr_eq(resolved, "/proc/self/exe")) {
        if (g_exec_path_len == 0) return reply(errno_noent(), 0);
        const u64 n = min_u64(g_exec_path_len, size);
        if (copy_to_target(dst, g_exec_path, n) != n) return reply(errno_fault(), 0);
        return reply(n, 0);
    }
    char target[FS_MAX_PATH_BYTES + 1];
    u64 target_len = 0;
    if (vfs_readlink_path(resolved, target, sizeof(target) - 1, &target_len)) {
        const u64 n = min_u64(target_len, size);
        if (copy_to_target(dst, target, n) != n) return reply(errno_fault(), 0);
        return reply(n, 0);
    }
    struct fs_stat_record rec;
    u64 token = 0;
    u64 object_size = 0;
    u8 kind = FS_OBJECT_NONE;
    if (vfs_lookup_stat(resolved, &token, &rec, &object_size, &kind)) {
        (void)token;
        (void)rec;
        (void)object_size;
        (void)kind;
        return reply(errno_inval(), 0);
    }
    return reply(errno_noent(), 0);
}

static struct ipc_message handle_readlink(const struct trap_request *req) {
    return handle_readlinkat(req, 1);
}

struct linux_timespec { i64 tv_sec; i64 tv_nsec; };
struct linux_timeval { i64 tv_sec; i64 tv_usec; };
struct linux_itimerval { struct linux_timeval it_interval; struct linux_timeval it_value; };
struct linux_itimerspec { struct linux_timespec it_interval; struct linux_timespec it_value; };
struct linux_sigevent {
    u64 sigev_value;
    i32 sigev_signo;
    i32 sigev_notify;
    u8 reserved[48];
};
struct linux_timezone { int tz_minuteswest; int tz_dsttime; };
struct linux_sysinfo {
    i64 uptime;
    u64 loads[3];
    u64 totalram;
    u64 freeram;
    u64 sharedram;
    u64 bufferram;
    u64 totalswap;
    u64 freeswap;
    u16 procs;
    u16 pad;
    u64 totalhigh;
    u64 freehigh;
    u32 mem_unit;
};
enum {
    LINUX_CLOCK_REALTIME = 0,
    LINUX_CLOCK_MONOTONIC = 1,
    LINUX_CLOCK_MONOTONIC_RAW = 4,
    LINUX_CLOCK_BOOTTIME = 7,
    LINUX_TIMER_ABSTIME = 1,
    LINUX_ABI_MONOTONIC_NS_PER_TICK = 1000000,
    CAPABILITYOS_CERT_TIME_UNIX = 1778025600
};

static i64 realtime_unix_seconds(void) {
    const u64 rtc = syscall0(SYSCALL_GET_RTC_UNIX_TIME);
    if (rtc != 0) return (i64)rtc;
    return CAPABILITYOS_CERT_TIME_UNIX;
}

static void monotonic_timespec(struct linux_timespec *ts) {
    const u64 ticks = syscall0(SYSCALL_GET_TICK_COUNT);
    const u64 ns = ticks * (u64)LINUX_ABI_MONOTONIC_NS_PER_TICK;
    ts->tv_sec = (i64)(ns / 1000000000ULL);
    ts->tv_nsec = (i64)(ns % 1000000000ULL);
}

static int linux_timespec_valid(const struct linux_timespec *ts) {
    return ts->tv_sec >= 0 && ts->tv_nsec >= 0 && ts->tv_nsec < 1000000000LL;
}

static u64 g_timer_invalid_diag_count;

static void timer_invalid_diag(const char *name, u64 a, u64 b, u64 c, u64 d) {
    if (g_timer_invalid_diag_count++ >= 32) return;
    user_log("LinuxAbiServer.timer invalid ");
    user_log(name);
    user_log(" a=");
    user_log_hex_value(a);
    user_log(" b=");
    user_log_hex_value(b);
    user_log(" c=");
    user_log_hex_value(c);
    user_log(" d=");
    user_log_hex_value(d);
}

static u64 linux_timespec_to_ticks_ceil(const struct linux_timespec *ts) {
    const u64 sec = (u64)ts->tv_sec;
    const u64 nsec = (u64)ts->tv_nsec;
    return sec * 1000ULL + (nsec + (LINUX_ABI_MONOTONIC_NS_PER_TICK - 1ULL)) / LINUX_ABI_MONOTONIC_NS_PER_TICK;
}

static struct linux_timespec ticks_to_linux_timespec(u64 ticks) {
    struct linux_timespec ts;
    ts.tv_sec = (i64)(ticks / 1000ULL);
    ts.tv_nsec = (i64)((ticks % 1000ULL) * LINUX_ABI_MONOTONIC_NS_PER_TICK);
    return ts;
}

static i64 linux_timespec_compare(const struct linux_timespec *a, const struct linux_timespec *b) {
    if (a->tv_sec != b->tv_sec) return a->tv_sec < b->tv_sec ? -1 : 1;
    if (a->tv_nsec != b->tv_nsec) return a->tv_nsec < b->tv_nsec ? -1 : 1;
    return 0;
}

static struct linux_timespec linux_timespec_sub(const struct linux_timespec *a, const struct linux_timespec *b) {
    struct linux_timespec out;
    out.tv_sec = a->tv_sec - b->tv_sec;
    out.tv_nsec = a->tv_nsec - b->tv_nsec;
    if (out.tv_nsec < 0) {
        out.tv_sec--;
        out.tv_nsec += 1000000000LL;
    }
    if (out.tv_sec < 0) {
        out.tv_sec = 0;
        out.tv_nsec = 0;
    }
    return out;
}
struct linux_utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};

static void copy_cstr_field(char *dst, u64 cap, const char *src) {
    u64 i = 0;
    for (; i + 1 < cap && src[i] != 0; i++) dst[i] = src[i];
    dst[i] = 0;
}

static struct ipc_message handle_uname(const struct trap_request *req) {
    struct linux_utsname uts;
    u8 *p = (u8 *)&uts; for (u64 i = 0; i < sizeof(uts); i++) p[i] = 0;
    copy_cstr_field(uts.sysname, sizeof(uts.sysname), "Linux");
    copy_cstr_field(uts.nodename, sizeof(uts.nodename), "capabilityos");
    copy_cstr_field(uts.release, sizeof(uts.release), "6.0.0-capabilityos");
    copy_cstr_field(uts.version, sizeof(uts.version), "CapabilityOS Linux ABI");
    copy_cstr_field(uts.machine, sizeof(uts.machine), "x86_64");
    copy_cstr_field(uts.domainname, sizeof(uts.domainname), "localdomain");
    return copy_to_target(req->args[0], &uts, sizeof(uts)) == sizeof(uts) ? reply(0, 0) : reply(errno_fault(), 0);
}

static struct ipc_message handle_clock_gettime(const struct trap_request *req) {
    const u64 clock_id = req->args[0];
    struct linux_timespec ts;
    if (clock_id == LINUX_CLOCK_REALTIME) {
        ts.tv_sec = realtime_unix_seconds();
        ts.tv_nsec = 0;
    } else if (clock_id == LINUX_CLOCK_MONOTONIC ||
        clock_id == LINUX_CLOCK_MONOTONIC_RAW ||
        clock_id == LINUX_CLOCK_BOOTTIME)
    {
        monotonic_timespec(&ts);
    } else {
        monotonic_timespec(&ts);
    }
    return copy_to_target(req->args[1], &ts, sizeof(ts)) == sizeof(ts) ? reply(0, 0) : reply(errno_fault(), 0);
}

static struct ipc_message handle_clock_getres(const struct trap_request *req) {
    const u64 dst = req->args[1];
    if (dst == 0) return reply(0, 0);
    struct linux_timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = LINUX_ABI_MONOTONIC_NS_PER_TICK;
    return copy_to_target(dst, &ts, sizeof(ts)) == sizeof(ts) ? reply(0, 0) : reply(errno_fault(), 0);
}

static int sleep_ticks_interruptible(u64 ticks, u64 *remaining_ticks_out) {
    if (remaining_ticks_out) *remaining_ticks_out = 0;
    if (ticks == 0) return 0;
    const u64 start_tick = syscall0(SYSCALL_GET_TICK_COUNT);
    for (;;) {
        const u64 now = syscall0(SYSCALL_GET_TICK_COUNT);
        const u64 elapsed = now - start_tick;
        if (elapsed >= ticks) return 0;
        if (process_signal_interrupt_pending(g_proc)) {
            if (remaining_ticks_out) *remaining_ticks_out = ticks - elapsed;
            return 1;
        }
        const u64 remaining = ticks - elapsed;
        (void)syscall2(SYSCALL_WAIT_EVENT, WAIT_EVENT_FLAG_PRESERVE_IPC_QUEUE, remaining > 1 ? 1 : remaining);
    }
}

static struct ipc_message handle_pause_syscall(const struct trap_request *req) {
    (void)req;
    if (!process_signal_interrupt_pending(g_proc)) {
        (void)syscall2(SYSCALL_WAIT_EVENT, WAIT_EVENT_FLAG_PRESERVE_IPC_QUEUE, 1);
    }
    return reply(errno_intr(), 0);
}

static struct ipc_message handle_rt_sigsuspend(const struct trap_request *req) {
    const u64 set_va = req->args[0];
    const u64 sigset_size = req->args[1];
    if (sigset_size != sizeof(u64)) return reply(errno_inval(), 0);

    u64 new_mask = 0;
    if (set_va != 0 && copy_from_target(set_va, &new_mask, sizeof(new_mask)) != sizeof(new_mask)) {
        return reply(errno_fault(), 0);
    }

    const u64 old_mask = g_proc->blocked_signals;
    g_proc->blocked_signals = new_mask & ~linux_unblockable_signal_mask();
    struct ipc_message msg = handle_pause_syscall(req);
    g_proc->blocked_signals = old_mask;
    return msg;
}

static struct ipc_message handle_nanosleep(const struct trap_request *req) {
    struct linux_timespec ts;
    if (req->args[0] == 0) return reply(errno_fault(), 0);
    if (copy_from_target(req->args[0], &ts, sizeof(ts)) != sizeof(ts)) return reply(errno_fault(), 0);
    if (!linux_timespec_valid(&ts)) {
        timer_invalid_diag("nanosleep", req->args[0], (u64)ts.tv_sec, (u64)ts.tv_nsec, 0);
        return reply(errno_inval(), 0);
    }

    u64 ticks = linux_timespec_to_ticks_ceil(&ts);
    u64 remaining_ticks = 0;
    if (sleep_ticks_interruptible(ticks, &remaining_ticks)) {
        if (req->args[1] != 0) {
            const struct linux_timespec rem = ticks_to_linux_timespec(remaining_ticks);
            if (copy_to_target(req->args[1], &rem, sizeof(rem)) != sizeof(rem)) return reply(errno_fault(), 0);
        }
        return reply(errno_intr(), 0);
    }
    if (req->args[1] != 0) {
        struct linux_timespec rem;
        rem.tv_sec = 0;
        rem.tv_nsec = 0;
        if (copy_to_target(req->args[1], &rem, sizeof(rem)) != sizeof(rem)) return reply(errno_fault(), 0);
    }

    return reply(0, 0);
}

static struct ipc_message handle_clock_nanosleep(const struct trap_request *req) {
    const u64 clock_id = req->args[0];
    const u64 flags = req->args[1];
    const u64 request_va = req->args[2];
    const u64 remain_va = req->args[3];
    if (clock_id != LINUX_CLOCK_REALTIME &&
        clock_id != LINUX_CLOCK_MONOTONIC &&
        clock_id != LINUX_CLOCK_MONOTONIC_RAW &&
        clock_id != LINUX_CLOCK_BOOTTIME)
    {
        timer_invalid_diag("clock-nanosleep-clock", clock_id, flags, request_va, remain_va);
        return reply(errno_inval(), 0);
    }
    if ((flags & ~(u64)LINUX_TIMER_ABSTIME) != 0) {
        timer_invalid_diag("clock-nanosleep-flags", clock_id, flags, request_va, remain_va);
        return reply(errno_inval(), 0);
    }
    if (request_va == 0) return reply(errno_fault(), 0);

    struct linux_timespec requested;
    if (copy_from_target(request_va, &requested, sizeof(requested)) != sizeof(requested)) return reply(errno_fault(), 0);
    if (!linux_timespec_valid(&requested)) {
        timer_invalid_diag("clock-nanosleep-time", clock_id, (u64)requested.tv_sec, (u64)requested.tv_nsec, flags);
        return reply(errno_inval(), 0);
    }

    struct linux_timespec duration = requested;
    if ((flags & LINUX_TIMER_ABSTIME) != 0) {
        struct linux_timespec now;
        if (clock_id == LINUX_CLOCK_REALTIME) {
            now.tv_sec = realtime_unix_seconds();
            now.tv_nsec = 0;
        } else {
            monotonic_timespec(&now);
        }
        if (linux_timespec_compare(&requested, &now) <= 0) return reply(0, 0);
        duration = linux_timespec_sub(&requested, &now);
    } else if (remain_va != 0) {
        struct linux_timespec rem;
        rem.tv_sec = 0;
        rem.tv_nsec = 0;
        if (copy_to_target(remain_va, &rem, sizeof(rem)) != sizeof(rem)) return reply(errno_fault(), 0);
    }

    const u64 ticks = linux_timespec_to_ticks_ceil(&duration);
    u64 remaining_ticks = 0;
    if (sleep_ticks_interruptible(ticks, &remaining_ticks)) {
        if ((flags & LINUX_TIMER_ABSTIME) == 0 && remain_va != 0) {
            const struct linux_timespec rem = ticks_to_linux_timespec(remaining_ticks);
            if (copy_to_target(remain_va, &rem, sizeof(rem)) != sizeof(rem)) return reply(errno_fault(), 0);
        }
        return reply(errno_intr(), 0);
    }
    return reply(0, 0);
}

static struct ipc_message handle_setitimer(const struct trap_request *req) {
    const u64 which = req->args[0];
    const u64 new_value_va = req->args[1];
    const u64 old_value_va = req->args[2];
    if (which > 2) {
        timer_invalid_diag("setitimer-which", which, new_value_va, old_value_va, 0);
        return reply(errno_inval(), 0);
    }
    if (which != 0) return reply(errno_nosys(), 0);
    if (old_value_va != 0) {
        struct linux_itimerval old_value;
        u8 *p = (u8 *)&old_value;
        for (u64 i = 0; i < sizeof(old_value); i++) p[i] = 0;
        const u64 remaining = process_itimer_real_remaining_ticks(g_proc);
        old_value.it_value.tv_sec = (i64)(remaining / 1000ULL);
        old_value.it_value.tv_usec = (i64)((remaining % 1000ULL) * 1000ULL);
        old_value.it_interval.tv_sec = (i64)(g_proc->itimer_real_interval_ticks / 1000ULL);
        old_value.it_interval.tv_usec = (i64)((g_proc->itimer_real_interval_ticks % 1000ULL) * 1000ULL);
        if (copy_to_target(old_value_va, &old_value, sizeof(old_value)) != sizeof(old_value)) return reply(errno_fault(), 0);
    }
    if (new_value_va != 0) {
        struct linux_itimerval new_value;
        if (copy_from_target(new_value_va, &new_value, sizeof(new_value)) != sizeof(new_value)) return reply(errno_fault(), 0);
        if (new_value.it_interval.tv_sec < 0 || new_value.it_value.tv_sec < 0 ||
            new_value.it_interval.tv_usec < 0 || new_value.it_interval.tv_usec >= 1000000LL ||
            new_value.it_value.tv_usec < 0 || new_value.it_value.tv_usec >= 1000000LL)
        {
            timer_invalid_diag("setitimer-time",
                (u64)new_value.it_interval.tv_sec,
                (u64)new_value.it_interval.tv_usec,
                (u64)new_value.it_value.tv_sec,
                (u64)new_value.it_value.tv_usec);
            return reply(errno_inval(), 0);
        }
        const u64 value_ticks = (u64)new_value.it_value.tv_sec * 1000ULL + ((u64)new_value.it_value.tv_usec + 999ULL) / 1000ULL;
        const u64 interval_ticks = (u64)new_value.it_interval.tv_sec * 1000ULL + ((u64)new_value.it_interval.tv_usec + 999ULL) / 1000ULL;
        g_proc->itimer_real_interval_ticks = interval_ticks;
        g_proc->itimer_real_expiry_tick = value_ticks == 0 ? 0 : syscall0(SYSCALL_GET_TICK_COUNT) + value_ticks;
        if (value_ticks == 0) {
            g_proc->pending_signals &= ~linux_signal_bit(SIGALRM);
            g_proc->timer_interrupt_signals &= ~linux_signal_bit(SIGALRM);
        }
    }
    return reply(0, 0);
}

static int itimerspec_valid(const struct linux_itimerspec *its) {
    if (its->it_interval.tv_sec < 0 || its->it_value.tv_sec < 0) return 0;
    if (its->it_interval.tv_nsec < 0 || its->it_interval.tv_nsec >= 1000000000LL) return 0;
    if (its->it_value.tv_nsec < 0 || its->it_value.tv_nsec >= 1000000000LL) return 0;
    return 1;
}

static struct linux_posix_timer_state *posix_timer_for_id(struct linux_process_state *proc, i32 timer_id) {
    if (!proc || timer_id <= 0) return 0;
    for (u64 i = 0; i < LINUX_POSIX_TIMER_MAX; i++) {
        if (proc->timers[i].used && proc->timers[i].timer_id == timer_id) return &proc->timers[i];
    }
    return 0;
}

static void fill_itimerspec_from_timer(const struct linux_posix_timer_state *timer, struct linux_itimerspec *out) {
    u8 *p = (u8 *)out;
    for (u64 i = 0; i < sizeof(*out); i++) p[i] = 0;
    if (!timer || !timer->used) return;
    const struct linux_timespec value = ticks_to_linux_timespec(timer_remaining_ticks(timer->expiry_tick));
    const struct linux_timespec interval = ticks_to_linux_timespec(timer->interval_ticks);
    out->it_value.tv_sec = value.tv_sec;
    out->it_value.tv_nsec = value.tv_nsec;
    out->it_interval.tv_sec = interval.tv_sec;
    out->it_interval.tv_nsec = interval.tv_nsec;
}

static u64 timer_start_tick_for_clock(u64 clock_id, const struct linux_timespec *value, u64 flags) {
    const u64 value_ticks = linux_timespec_to_ticks_ceil(value);
    if (value_ticks == 0) return 0;
    const u64 now_tick = syscall0(SYSCALL_GET_TICK_COUNT);
    if ((flags & LINUX_TIMER_ABSTIME) == 0) return now_tick + value_ticks;
    if (clock_id == LINUX_CLOCK_REALTIME) {
        const i64 now_sec = realtime_unix_seconds();
        if (value->tv_sec < now_sec || (value->tv_sec == now_sec && value->tv_nsec == 0)) return now_tick;
        const u64 sec_delta = (u64)(value->tv_sec - now_sec);
        const u64 nsec = (u64)value->tv_nsec;
        return now_tick + sec_delta * 1000ULL + (nsec + (LINUX_ABI_MONOTONIC_NS_PER_TICK - 1ULL)) / LINUX_ABI_MONOTONIC_NS_PER_TICK;
    }
    struct linux_timespec now;
    monotonic_timespec(&now);
    if (linux_timespec_compare(value, &now) <= 0) return now_tick;
    const struct linux_timespec delta = linux_timespec_sub(value, &now);
    return now_tick + linux_timespec_to_ticks_ceil(&delta);
}

static struct ipc_message handle_timer_create(const struct trap_request *req) {
    const u64 clock_id = req->args[0];
    const u64 sigevent_va = req->args[1];
    const u64 timerid_va = req->args[2];
    if (clock_id != LINUX_CLOCK_REALTIME &&
        clock_id != LINUX_CLOCK_MONOTONIC &&
        clock_id != LINUX_CLOCK_MONOTONIC_RAW &&
        clock_id != LINUX_CLOCK_BOOTTIME)
    {
        return reply(errno_inval(), 0);
    }
    if (timerid_va == 0) return reply(errno_fault(), 0);
    struct linux_sigevent sev;
    sev.sigev_value = 0;
    sev.sigev_signo = SIGALRM;
    sev.sigev_notify = SIGEV_SIGNAL;
    if (sigevent_va != 0) {
        if (copy_from_target(sigevent_va, &sev, sizeof(sev)) != sizeof(sev)) return reply(errno_fault(), 0);
        if (sev.sigev_notify != SIGEV_SIGNAL && sev.sigev_notify != SIGEV_NONE && sev.sigev_notify != SIGEV_THREAD_ID) {
            return reply(errno_nosys(), 0);
        }
        if ((sev.sigev_notify == SIGEV_SIGNAL || sev.sigev_notify == SIGEV_THREAD_ID) &&
            (sev.sigev_signo <= 0 || sev.sigev_signo >= 65))
        {
            return reply(errno_inval(), 0);
        }
    }
    struct linux_posix_timer_state *slot = 0;
    for (u64 i = 0; i < LINUX_POSIX_TIMER_MAX; i++) {
        if (g_proc->timers[i].used) continue;
        slot = &g_proc->timers[i];
        break;
    }
    if (!slot) return reply(errno_again(), 0);
    slot->used = 1;
    slot->clock_id = (u8)clock_id;
    slot->notify = (u8)sev.sigev_notify;
    slot->signo = (u8)(sev.sigev_signo == 0 ? SIGALRM : sev.sigev_signo);
    slot->value = sev.sigev_value;
    slot->expiry_tick = 0;
    slot->interval_ticks = 0;
    slot->overrun = 0;
    const i32 timerid = slot->timer_id;
    return copy_to_target(timerid_va, &timerid, sizeof(timerid)) == sizeof(timerid) ? reply(0, 0) : reply(errno_fault(), 0);
}

static struct ipc_message handle_timer_settime(const struct trap_request *req) {
    const i32 timer_id = (i32)req->args[0];
    const u64 flags = req->args[1];
    const u64 new_value_va = req->args[2];
    const u64 old_value_va = req->args[3];
    if ((flags & ~(u64)LINUX_TIMER_ABSTIME) != 0) return reply(errno_inval(), 0);
    struct linux_posix_timer_state *timer = posix_timer_for_id(g_proc, timer_id);
    if (!timer) return reply(errno_inval(), 0);
    process_timers_update(g_proc);
    if (old_value_va != 0) {
        struct linux_itimerspec old_value;
        fill_itimerspec_from_timer(timer, &old_value);
        if (copy_to_target(old_value_va, &old_value, sizeof(old_value)) != sizeof(old_value)) return reply(errno_fault(), 0);
    }
    if (new_value_va == 0) return reply(errno_fault(), 0);
    struct linux_itimerspec new_value;
    if (copy_from_target(new_value_va, &new_value, sizeof(new_value)) != sizeof(new_value)) return reply(errno_fault(), 0);
    if (!itimerspec_valid(&new_value)) return reply(errno_inval(), 0);
    timer->interval_ticks = linux_timespec_to_ticks_ceil(&new_value.it_interval);
    timer->expiry_tick = timer_start_tick_for_clock(timer->clock_id, &new_value.it_value, flags);
    timer->overrun = 0;
    if (timer->expiry_tick == 0 && timer->signo != 0) {
        g_proc->pending_signals &= ~linux_signal_bit(timer->signo);
        g_proc->timer_interrupt_signals &= ~linux_signal_bit(timer->signo);
    }
    return reply(0, 0);
}

static struct ipc_message handle_timer_gettime(const struct trap_request *req) {
    const i32 timer_id = (i32)req->args[0];
    const u64 dst = req->args[1];
    if (dst == 0) return reply(errno_fault(), 0);
    struct linux_posix_timer_state *timer = posix_timer_for_id(g_proc, timer_id);
    if (!timer) return reply(errno_inval(), 0);
    process_timers_update(g_proc);
    struct linux_itimerspec value;
    fill_itimerspec_from_timer(timer, &value);
    return copy_to_target(dst, &value, sizeof(value)) == sizeof(value) ? reply(0, 0) : reply(errno_fault(), 0);
}

static struct ipc_message handle_timer_delete(const struct trap_request *req) {
    const i32 timer_id = (i32)req->args[0];
    struct linux_posix_timer_state *timer = posix_timer_for_id(g_proc, timer_id);
    if (!timer) return reply(errno_inval(), 0);
    if (timer->signo != 0) {
        g_proc->pending_signals &= ~linux_signal_bit(timer->signo);
        g_proc->timer_interrupt_signals &= ~linux_signal_bit(timer->signo);
    }
    timer->used = 0;
    timer->expiry_tick = 0;
    timer->interval_ticks = 0;
    timer->overrun = 0;
    timer->value = 0;
    timer->signo = 0;
    timer->notify = SIGEV_NONE;
    return reply(0, 0);
}

static struct ipc_message handle_gettimeofday(const struct trap_request *req) {
    if (req->args[0] != 0) {
        struct linux_timeval tv;
        tv.tv_sec = realtime_unix_seconds();
        tv.tv_usec = 0;
        if (copy_to_target(req->args[0], &tv, sizeof(tv)) != sizeof(tv)) return reply(errno_fault(), 0);
    }
    if (req->args[1] != 0) {
        struct linux_timezone tz;
        tz.tz_minuteswest = 0;
        tz.tz_dsttime = 0;
        if (copy_to_target(req->args[1], &tz, sizeof(tz)) != sizeof(tz)) return reply(errno_fault(), 0);
    }
    return reply(0, 0);
}

static struct ipc_message handle_sysinfo(const struct trap_request *req) {
    struct linux_sysinfo info;
    u8 *p = (u8 *)&info; for (u64 i = 0; i < sizeof(info); i++) p[i] = 0;
    const u64 ticks = syscall0(SYSCALL_GET_TICK_COUNT);
    info.uptime = (i64)(ticks / 1000ULL);
    info.totalram = 512ULL * 1024ULL * 1024ULL;
    info.freeram = 256ULL * 1024ULL * 1024ULL;
    info.mem_unit = 1;
    u16 procs = 0;
    for (u64 i = 0; i < LINUX_PROCESS_MAX; i++) if (g_processes[i].used) procs++;
    info.procs = procs;
    return copy_to_target(req->args[0], &info, sizeof(info)) == sizeof(info) ? reply(0, 0) : reply(errno_fault(), 0);
}

static struct ipc_message handle_time_syscall(const struct trap_request *req) {
    const i64 now = realtime_unix_seconds();
    if (req->args[0] != 0 && copy_to_target(req->args[0], &now, sizeof(now)) != sizeof(now)) return reply(errno_fault(), 0);
    return reply((u64)now, 0);
}

static struct ipc_message handle_membarrier(const struct trap_request *req) {
    const u64 cmd = req->args[0];
    const u64 flags = req->args[1];
    if (flags != 0) return reply(errno_inval(), 0);
    const u64 supported = (u64)MEMBARRIER_CMD_GLOBAL |
        MEMBARRIER_CMD_PRIVATE_EXPEDITED |
        MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED;
    if (cmd == MEMBARRIER_CMD_QUERY) return reply(supported, 0);
    if ((cmd & ~supported) != 0) return reply(errno_inval(), 0);
    return reply(0, 0);
}

static struct ipc_message handle_sched_getaffinity(const struct trap_request *req) {
    const u64 cpu_set_size = req->args[1];
    const u64 mask_va = req->args[2];
    if (cpu_set_size == 0 || mask_va == 0) return reply(errno_inval(), 0);

    u8 mask[128];
    const u64 n = min_u64(cpu_set_size, sizeof(mask));
    for (u64 i = 0; i < n; i++) mask[i] = 0;
    mask[0] = 1;
    return copy_to_target(mask_va, mask, n) == n ? reply(0, 0) : reply(errno_fault(), 0);
}

static int cpu_has_rdrand(void) {
    u32 eax = 1, ebx = 0, ecx = 0, edx = 0;
    __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
    (void)ebx;
    (void)edx;
    return (ecx & (1u << 30)) != 0;
}

static int rdrand64(u64 *out) {
    unsigned char ok = 0;
    u64 value = 0;
    for (u32 attempt = 0; attempt < 16; attempt++) {
        __asm__ volatile("rdrand %0; setc %1" : "=r"(value), "=qm"(ok));
        if (ok) {
            *out = value;
            return 1;
        }
    }
    return 0;
}

static struct ipc_message handle_getrandom(const struct trap_request *req) {
    enum { GRND_NONBLOCK = 0x0001, GRND_RANDOM = 0x0002 };
    const u64 dst = req->args[0];
    const u64 len = req->args[1];
    const u64 flags = req->args[2];
    g_prof.getrandom_calls++;
    if (dst == 0 && len != 0) return reply(errno_fault(), 0);
    if ((flags & ~(u64)(GRND_NONBLOCK | GRND_RANDOM)) != 0) return reply(errno_inval(), 0);
    if (len == 0) return reply(0, 0);
    if (!cpu_has_rdrand()) return reply(errno_again(), 0);

    u64 copied = 0;
    while (copied < len) {
        u64 value = 0;
        if (!rdrand64(&value)) return copied != 0 ? reply(copied, 0) : reply(errno_again(), 0);
        const u64 chunk = min_u64(len - copied, sizeof(value));
        if (copy_to_target(dst + copied, &value, chunk) != chunk) return copied != 0 ? reply(copied, 0) : reply(errno_fault(), 0);
        copied += chunk;
    }
    g_prof.getrandom_bytes += copied;
    return reply(copied, 0);
}

struct linux_termios_kernel {
    u32 c_iflag;
    u32 c_oflag;
    u32 c_cflag;
    u32 c_lflag;
    u8 c_line;
    u8 c_cc[19];
    u32 c_ispeed;
    u32 c_ospeed;
};

struct linux_winsize {
    u16 ws_row;
    u16 ws_col;
    u16 ws_xpixel;
    u16 ws_ypixel;
};

static int fd_is_stdio_tty(u64 fd) {
    return fd_is_tty_like(fd);
}

static void fill_default_termios(struct linux_termios_kernel *termios) {
    u8 *p = (u8 *)termios; for (u64 i = 0; i < sizeof(*termios); i++) p[i] = 0;
    termios->c_iflag = 0000400; /* ICRNL */
    termios->c_oflag = 0000001 | 0000004; /* OPOST | ONLCR */
    termios->c_cflag = 0000060 | 0000400 | 0000200; /* CS8 | CREAD | HUPCL */
    termios->c_lflag = 0000001 | 0000002 | 0000010 | 0000020 | 0000040 | 0100000; /* ISIG | ICANON | ECHO | ECHOE | ECHOK | IEXTEN */
    termios->c_cc[0] = 3;   /* VINTR */
    termios->c_cc[1] = 28;  /* VQUIT */
    termios->c_cc[2] = 127; /* VERASE */
    termios->c_cc[3] = 21;  /* VKILL */
    termios->c_cc[4] = 4;   /* VEOF */
    termios->c_cc[5] = 0;   /* VTIME */
    termios->c_cc[6] = 1;   /* VMIN */
    termios->c_cc[8] = 17;  /* VSTART */
    termios->c_cc[9] = 19;  /* VSTOP */
    termios->c_cc[10] = 26; /* VSUSP */
    termios->c_cc[12] = 18; /* VREPRINT */
    termios->c_cc[14] = 23; /* VWERASE */
    termios->c_ispeed = 15; /* B38400 */
    termios->c_ospeed = 15; /* B38400 */
}

static void tty_attr_to_linux_termios(const struct tty_attr_payload *attr, struct linux_termios_kernel *termios) {
    fill_default_termios(termios);
    termios->c_iflag = 0;
    if (attr->iflag & TTY_IFLAG_INLCR) termios->c_iflag |= 0000100;
    if (attr->iflag & TTY_IFLAG_IGNCR) termios->c_iflag |= 0000200;
    if (attr->iflag & TTY_IFLAG_ICRNL) termios->c_iflag |= 0000400;
    termios->c_oflag = 0;
    if (attr->oflag & TTY_OFLAG_OPOST) termios->c_oflag |= 0000001;
    if (attr->oflag & TTY_OFLAG_ONLCR) termios->c_oflag |= 0000004;
    termios->c_lflag = 0;
    if (attr->lflag & TTY_LFLAG_ISIG) termios->c_lflag |= 0000001;
    if (attr->lflag & TTY_LFLAG_ICANON) termios->c_lflag |= 0000002;
    if (attr->lflag & TTY_LFLAG_ECHO) termios->c_lflag |= 0000010;
    if (attr->lflag & TTY_LFLAG_ECHOE) termios->c_lflag |= 0000020;
    if (attr->lflag & TTY_LFLAG_ECHOK) termios->c_lflag |= 0000040;
    if (attr->lflag & TTY_LFLAG_ECHONL) termios->c_lflag |= 0000100;
    if (attr->lflag & TTY_LFLAG_IEXTEN) termios->c_lflag |= 0100000;
    for (u64 i = 0; i < sizeof(termios->c_cc) && i < sizeof(attr->cc); i++) termios->c_cc[i] = attr->cc[i];
}

static void linux_termios_to_tty_attr(const struct linux_termios_kernel *termios, struct tty_attr_payload *attr) {
    attr->version = TTY_ATTR_VERSION;
    attr->iflag = 0;
    attr->oflag = 0;
    attr->lflag = 0;
    if (termios->c_iflag & 0000100) attr->iflag |= TTY_IFLAG_INLCR;
    if (termios->c_iflag & 0000200) attr->iflag |= TTY_IFLAG_IGNCR;
    if (termios->c_iflag & 0000400) attr->iflag |= TTY_IFLAG_ICRNL;
    if (termios->c_oflag & 0000001) attr->oflag |= TTY_OFLAG_OPOST;
    if (termios->c_oflag & 0000004) attr->oflag |= TTY_OFLAG_ONLCR;
    if (termios->c_lflag & 0000001) attr->lflag |= TTY_LFLAG_ISIG;
    if (termios->c_lflag & 0000002) attr->lflag |= TTY_LFLAG_ICANON;
    if (termios->c_lflag & 0000010) attr->lflag |= TTY_LFLAG_ECHO;
    if (termios->c_lflag & 0000020) attr->lflag |= TTY_LFLAG_ECHOE;
    if (termios->c_lflag & 0000040) attr->lflag |= TTY_LFLAG_ECHOK;
    if (termios->c_lflag & 0000100) attr->lflag |= TTY_LFLAG_ECHONL;
    if (termios->c_lflag & 0100000) attr->lflag |= TTY_LFLAG_IEXTEN;
    for (u64 i = 0; i < sizeof(attr->cc); i++) attr->cc[i] = i < sizeof(termios->c_cc) ? termios->c_cc[i] : 0;
    attr->columns = 80;
    attr->rows = 24;
    attr->reserved0 = 0;

    struct tty_attr_payload current;
    if (console_get_tty_attr(&current)) {
        attr->columns = current.columns;
        attr->rows = current.rows;
    }
}

static struct ipc_message handle_ioctl(const struct trap_request *req) {
    const u64 fd = req->args[0];
    const u64 request = req->args[1];
    const u64 argp = req->args[2];
    if (!fd_valid(fd)) return reply(errno_badf(), 0);
    if (!fd_is_stdio_tty(fd)) return reply(errno_notty(), 0);

    if (request == TCGETS) {
        struct linux_termios_kernel termios;
        struct tty_attr_payload attr;
        if (console_get_tty_attr(&attr)) {
            tty_attr_to_linux_termios(&attr, &termios);
        } else {
            fill_default_termios(&termios);
        }
        return copy_to_target(argp, &termios, sizeof(termios)) == sizeof(termios) ? reply(0, 0) : reply(errno_fault(), 0);
    }
    if (request == TIOCGWINSZ) {
        struct linux_winsize ws;
        struct tty_attr_payload attr;
        if (console_get_tty_attr(&attr)) {
            ws.ws_row = attr.rows;
            ws.ws_col = attr.columns;
        } else {
            ws.ws_row = 24;
            ws.ws_col = 80;
        }
        ws.ws_xpixel = 0;
        ws.ws_ypixel = 0;
        return copy_to_target(argp, &ws, sizeof(ws)) == sizeof(ws) ? reply(0, 0) : reply(errno_fault(), 0);
    }
    if (request == TIOCGPGRP) {
        const u32 pgrp = (u32)(g_proc && g_proc->pid != 0 ? g_proc->pid : 1);
        return copy_to_target(argp, &pgrp, sizeof(pgrp)) == sizeof(pgrp) ? reply(0, 0) : reply(errno_fault(), 0);
    }
    if (request == TCSETS || request == TCSETSW || request == TCSETSF) {
        struct linux_termios_kernel termios;
        if (copy_from_target(argp, &termios, sizeof(termios)) != sizeof(termios)) return reply(errno_fault(), 0);
        struct tty_attr_payload attr;
        linux_termios_to_tty_attr(&termios, &attr);
        return reply(console_set_tty_attr(&attr) ? 0 : errno_io(), 0);
    }
    if (request == TIOCSWINSZ) {
        struct linux_winsize ws;
        if (copy_from_target(argp, &ws, sizeof(ws)) != sizeof(ws)) return reply(errno_fault(), 0);
        struct tty_attr_payload attr;
        if (!console_get_tty_attr(&attr)) {
            attr.version = TTY_ATTR_VERSION;
            attr.iflag = TTY_IFLAG_ICRNL;
            attr.oflag = TTY_OFLAG_OPOST | TTY_OFLAG_ONLCR;
            attr.lflag = TTY_LFLAG_ISIG | TTY_LFLAG_ICANON | TTY_LFLAG_ECHO | TTY_LFLAG_ECHOE | TTY_LFLAG_ECHOK | TTY_LFLAG_IEXTEN;
            for (u64 i = 0; i < sizeof(attr.cc); i++) attr.cc[i] = 0;
            attr.cc[0] = 3;
            attr.cc[1] = 28;
            attr.cc[2] = 127;
            attr.cc[3] = 21;
            attr.cc[4] = 4;
            attr.cc[5] = 0;
            attr.cc[6] = 1;
            attr.columns = 80;
            attr.rows = 24;
            attr.reserved0 = 0;
        }
        if (ws.ws_col != 0) attr.columns = ws.ws_col;
        if (ws.ws_row != 0) attr.rows = ws.ws_row;
        return reply(console_set_tty_attr(&attr) ? 0 : errno_io(), 0);
    }
    if (request == TIOCSPGRP) return reply(0, 0);
    return reply(errno_inval(), 0);
}
