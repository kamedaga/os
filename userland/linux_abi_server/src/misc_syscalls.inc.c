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
