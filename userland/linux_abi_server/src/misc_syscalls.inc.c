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
    struct linux_timespec ts;
    ts.tv_sec = 1;
    ts.tv_nsec = 0;
    return copy_to_target(req->args[1], &ts, sizeof(ts)) == sizeof(ts) ? reply(0, 0) : reply(errno_fault(), 0);
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
    termios->c_lflag = 0000001 | 0000002 | 0000010 | 0000020 | 0000100; /* ISIG | ICANON | ECHO | ECHOE | ECHOK */
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
    termios->c_ispeed = 15; /* B38400 */
    termios->c_ospeed = 15; /* B38400 */
}

static struct ipc_message handle_ioctl(const struct trap_request *req) {
    const u64 fd = req->args[0];
    const u64 request = req->args[1];
    const u64 argp = req->args[2];
    if (!fd_valid(fd)) return reply(errno_badf(), 0);
    if (!fd_is_stdio_tty(fd)) return reply(errno_notty(), 0);

    if (request == TCGETS) {
        struct linux_termios_kernel termios;
        fill_default_termios(&termios);
        return copy_to_target(argp, &termios, sizeof(termios)) == sizeof(termios) ? reply(0, 0) : reply(errno_fault(), 0);
    }
    if (request == TIOCGWINSZ) {
        struct linux_winsize ws;
        ws.ws_row = 40;
        ws.ws_col = 120;
        ws.ws_xpixel = 0;
        ws.ws_ypixel = 0;
        return copy_to_target(argp, &ws, sizeof(ws)) == sizeof(ws) ? reply(0, 0) : reply(errno_fault(), 0);
    }
    if (request == TIOCGPGRP) {
        const u32 pgrp = (u32)(g_proc && g_proc->pid != 0 ? g_proc->pid : 1);
        return copy_to_target(argp, &pgrp, sizeof(pgrp)) == sizeof(pgrp) ? reply(0, 0) : reply(errno_fault(), 0);
    }
    if (request == TCSETS || request == TCSETSW || request == TCSETSF || request == TIOCSPGRP || request == TIOCSWINSZ) return reply(0, 0);
    return reply(errno_inval(), 0);
}
