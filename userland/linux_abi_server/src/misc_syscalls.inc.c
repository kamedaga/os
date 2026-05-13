static struct ipc_message handle_getcwd(const struct trap_request *req) {
    const u64 dst = req->args[0]; const u64 size = req->args[1]; const u64 needed = (u64)g_cwd_len + 1;
    if (dst == 0 || size < needed) return reply(errno_range(), 0);
    if (copy_to_target(dst, g_cwd, needed) != needed) return reply(errno_fault(), 0);
    return reply(dst, 0);
}

static struct ipc_message handle_chdir(const struct trap_request *req) {
    char path[256]; char resolved[FS_MAX_PATH_BYTES + 1];
    if (!copy_cstr_from_target(req->args[0], path, sizeof(path))) return reply(errno_fault(), 0);
    if (path[0] == 0) return reply(errno_noent(), 0);
    if (!resolve_path_at(AT_FDCWD_U64, path, resolved)) return reply(errno_nametoolong(), 0);
    struct fs_stat_record rec; u64 token = 0, size = 0; u8 kind = 0;
    if (!vfs_lookup_stat(resolved, &token, &rec, &size, &kind)) return reply(errno_noent(), 0);
    if (kind != FS_OBJECT_DIRECTORY && kind != FS_OBJECT_MOUNT) return reply(errno_notdir(), 0);
    g_cwd_len = (u16)cstr_len(resolved);
    for (u16 i = 0; i <= g_cwd_len; i++) g_cwd[i] = resolved[i];
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

static struct ipc_message handle_readlink(const struct trap_request *req) {
    char path[256]; char resolved[FS_MAX_PATH_BYTES + 1];
    const u64 dst = req->args[1]; const u64 size = req->args[2];
    if (!copy_cstr_from_target(req->args[0], path, sizeof(path))) return reply(errno_fault(), 0);
    if (path[0] == 0) return reply(errno_noent(), 0);
    if (!resolve_path_at(AT_FDCWD_U64, path, resolved)) return reply(errno_nametoolong(), 0);
    if (cstr_eq(resolved, "/proc/self/exe")) {
        if (g_exec_path_len == 0) return reply(errno_noent(), 0);
        const u64 n = min_u64(g_exec_path_len, size);
        if (copy_to_target(dst, g_exec_path, n) != n) return reply(errno_fault(), 0);
        return reply(n, 0);
    }
    return reply(errno_noent(), 0);
}

struct linux_timespec { i64 tv_sec; i64 tv_nsec; };
struct linux_timeval { i64 tv_sec; i64 tv_usec; };
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
    attr->columns = 120;
    attr->rows = 40;
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
            ws.ws_row = 40;
            ws.ws_col = 120;
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
    if (request == TIOCSPGRP || request == TIOCSWINSZ) return reply(0, 0);
    return reply(errno_inval(), 0);
}
