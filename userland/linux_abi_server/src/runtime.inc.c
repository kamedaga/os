static u64 cstr_len(const char *s) { u64 n = 0; while (s[n] != 0) n++; return n; }
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
static void process_exit(u64 code) { (void)syscall2(SYSCALL_PROCESS_EXIT, code, 0); for (;;) __asm__ volatile("pause"); }

static void wait_without_consuming_ipc(void) {
    const u64 start = syscall0(SYSCALL_GET_TICK_COUNT);
    for (;;) {
        for (u64 i = 0; i < 256; i++) __asm__ volatile("pause");
        if (syscall0(SYSCALL_GET_TICK_COUNT) != start) return;
    }
}

static struct ipc_message wait_ipc(void) {
    register u64 rax __asm__("rax") = SYSCALL_WAIT_EVENT; register u64 rdi __asm__("rdi") = 0; register u64 rsi __asm__("rsi") = 0; register u64 rdx __asm__("rdx"); register u64 r8 __asm__("r8");
    __asm__ volatile("int $0x80" : "+r"(rax), "+r"(rdi), "+r"(rsi), "=r"(rdx), "=r"(r8) : : "rcx", "r9", "r10", "r11", "memory");
    struct ipc_message msg = { rax, rdi, rsi, rdx, r8 }; return msg;
}

static struct ipc_message reply(u64 result, u64 flags) {
    const u64 explicit_target = abi_reply_target_principal();
    if (explicit_target != 0) {
        const u64 target = explicit_target;
        abi_set_reply_target_principal(0);
        const u64 status = syscall3(SYSCALL_REPLY_ABI_TRAP_TARGET, target, result, flags);
        (void)syscall0(SYSCALL_DETACH_ABI_TRAP_REPLY_TOKEN);
        if (status != SYSCALL_OK) {
            user_log("LinuxAbiServer: explicit reply failed=");
            user_log_hex_value(status);
        }
        return wait_ipc();
    }
    register u64 rax __asm__("rax") = SYSCALL_IPC_CALL_REPLY_RECV; register u64 rdi __asm__("rdi") = result; register u64 rsi __asm__("rsi") = 0; register u64 rdx __asm__("rdx") = IPC_CALL_FLAG_SIGNAL_ONLY; register u64 r8 __asm__("r8") = flags; register u64 r9 __asm__("r9") = 0; register u64 r10 __asm__("r10") = 0;
    __asm__ volatile("int $0x80" : "+r"(rax), "+r"(rdi), "+r"(rsi), "+r"(rdx), "+r"(r8), "+r"(r9), "+r"(r10) : : "rcx", "r11", "memory");
    struct ipc_message msg = { rax, rdi, rsi, rdx, r8 }; return msg;
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
static u64 protect_reply_target_pages(u64 target_va, u64 page_count, u64 prot_bits) { return syscall3(SYSCALL_PROTECT_ABI_TRAP_REPLY_TARGET_PAGES, target_va, page_count, prot_bits); }
static u64 unmap_reply_target_pages(u64 target_va, u64 page_count) { return syscall2(SYSCALL_UNMAP_ABI_TRAP_REPLY_TARGET_PAGES, target_va, page_count); }
static u64 copy_from_target(u64 target_va, void *dst, u64 len) { return syscall3(SYSCALL_COPY_FROM_ABI_TRAP_REPLY_TARGET, (u64)dst, target_va, len); }
static u64 copy_to_target(u64 target_va, const void *src, u64 len) { return syscall3(SYSCALL_COPY_TO_ABI_TRAP_REPLY_TARGET, target_va, (u64)src, len); }
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
static u64 alloc_map_pages_with_paddrs(u64 target_va, u64 page_count, u64 writable, u64 out_paddr_list_va) { return syscall4(SYSCALL_ALLOC_MAP_PAGES, target_va, page_count, writable, out_paddr_list_va); }
static int install_self_wake_endpoint(void) { return syscall3(SYSCALL_INSTALL_ENDPOINT, 0, LINUX_ABI_SELF_WAKE_ENDPOINT_ID, syscall0(SYSCALL_GET_PROCESS_SLOT)) == SYSCALL_OK; }
static void prime_reply_return_signal(void) { (void)syscall2(SYSCALL_SIGNAL_ENDPOINT, LINUX_ABI_SELF_WAKE_ENDPOINT_ID, 0); }

static void exit_trap_target_no_wait(u64 principal) {
    abi_set_reply_target_principal(0);
    const u64 status = reply_trap_target(principal, 0, TRAP_RESPONSE_FLAG_EXIT);
    (void)detach_reply_token();
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

static u64 make_nonce(u64 request_paddr, u64 response_paddr, u64 endpoint_id, u64 process_slot) {
    return request_paddr ^ ((response_paddr << 17) | (response_paddr >> 47)) ^ (endpoint_id << 1) ^ (process_slot << 33) ^ 0x4653434f4e4e4543ULL;
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
    return wait_fs_response_at(VFS_RESPONSE_VA, expected_seq, expected_op);
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

static void profile_clear(void) {
    u8 *p = (u8 *)&g_prof;
    for (u64 i = 0; i < sizeof(g_prof); i++) p[i] = 0;
}

static void profile_report_and_reset(void) {
    user_log("LinuxAbiServer.perf.begin exec=");
    user_log(g_exec_path);
    user_log("\n");
    user_log_dec_line("LinuxAbiServer.perf.syscalls.total=", g_prof.syscall_total);
    profile_print_syscall("read", LINUX_SYS_READ);
    profile_print_syscall("write", LINUX_SYS_WRITE);
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
    profile_print_syscall("fcntl", LINUX_SYS_FCNTL);
    profile_print_syscall("ioctl", LINUX_SYS_IOCTL);

    user_log_dec_line("LinuxAbiServer.perf.vfs.requests=", g_prof.vfs_requests);
    profile_print_fs_op("lookup", FS_OP_LOOKUP);
    profile_print_fs_op("open", FS_OP_OPEN);
    profile_print_fs_op("read", FS_OP_READ);
    profile_print_fs_op("readdir", FS_OP_READDIR);
    profile_print_fs_op("stat", FS_OP_STAT);
    profile_print_fs_op("close", FS_OP_CLOSE);
    profile_print_fs_op("create", FS_OP_CREATE);
    profile_print_fs_op("write", FS_OP_WRITE);
    profile_print_fs_op("unlink", FS_OP_UNLINK);
    profile_print_fs_op("rename", FS_OP_RENAME);
    user_log_dec_line("LinuxAbiServer.perf.vfs.read_request_bytes=", g_prof.vfs_read_request_bytes);
    user_log_dec_line("LinuxAbiServer.perf.vfs.write_request_bytes=", g_prof.vfs_write_request_bytes);
    user_log_dec_line("LinuxAbiServer.perf.vfs.wait_calls=", g_prof.vfs_wait_calls);
    user_log_dec_line("LinuxAbiServer.perf.vfs.wait_loops=", g_prof.vfs_wait_loops);
    user_log_dec_line("LinuxAbiServer.perf.vfs.wait_slow=", g_prof.vfs_wait_slow);
    user_log_dec_line("LinuxAbiServer.perf.vfs.wait_timeouts=", g_prof.vfs_wait_timeouts);

    user_log_dec_line("LinuxAbiServer.perf.fs.read_bytes=", g_prof.fs_read_bytes);
    user_log_dec_line("LinuxAbiServer.perf.fs.read_cmd_bytes=", g_prof.fs_read_cmd_bytes);
    user_log_dec_line("LinuxAbiServer.perf.fs.read_lib_bytes=", g_prof.fs_read_lib_bytes);
    user_log_dec_line("LinuxAbiServer.perf.fs.read_tmp_bytes=", g_prof.fs_read_tmp_bytes);
    user_log_dec_line("LinuxAbiServer.perf.fs.read_proc_bytes=", g_prof.fs_read_proc_bytes);
    user_log_dec_line("LinuxAbiServer.perf.fs.write_bytes=", g_prof.fs_write_bytes);
    user_log_dec_line("LinuxAbiServer.perf.cache.file_hits=", g_prof.file_cache_hits);
    user_log_dec_line("LinuxAbiServer.perf.cache.file_misses=", g_prof.file_cache_misses);
    user_log_dec_line("LinuxAbiServer.perf.cache.file_fill_bytes=", g_prof.file_cache_fill_bytes);
    user_log_dec_line("LinuxAbiServer.perf.cache.path_hits=", g_prof.path_cache_hits);
    user_log_dec_line("LinuxAbiServer.perf.cache.path_misses=", g_prof.path_cache_misses);
    user_log_dec_line("LinuxAbiServer.perf.cache.open_hits=", g_prof.open_cache_hits);
    user_log_dec_line("LinuxAbiServer.perf.cache.open_misses=", g_prof.open_cache_misses);

    user_log_dec_line("LinuxAbiServer.perf.vm.mmap_calls=", g_prof.mmap_calls);
    user_log_dec_line("LinuxAbiServer.perf.vm.mmap_pages=", g_prof.mmap_pages);
    user_log_dec_line("LinuxAbiServer.perf.vm.mmap_file_calls=", g_prof.mmap_file_calls);
    user_log_dec_line("LinuxAbiServer.perf.vm.mmap_file_pages=", g_prof.mmap_file_pages);
    user_log_dec_line("LinuxAbiServer.perf.vm.mmap_file_bytes=", g_prof.mmap_file_bytes);
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
    user_log_dec_line("LinuxAbiServer.perf.net.tx_payload_bytes=", g_prof.net_payload_tx_bytes);
    user_log_dec_line("LinuxAbiServer.perf.net.rx_payload_bytes=", g_prof.net_payload_rx_bytes);
    user_log_dec_line("LinuxAbiServer.perf.net.wait_calls=", g_prof.net_wait_calls);
    user_log_dec_line("LinuxAbiServer.perf.net.wait_loops=", g_prof.net_wait_loops);
    user_log_dec_line("LinuxAbiServer.perf.net.wait_slow=", g_prof.net_wait_slow);
    user_log_dec_line("LinuxAbiServer.perf.net.wait_timeouts=", g_prof.net_wait_timeouts);
    user_log_dec_line("LinuxAbiServer.perf.net.tcp_connect_attempts=", g_prof.net_tcp_connect_attempts);
    user_log_dec_line("LinuxAbiServer.perf.net.tcp_connect_poll_loops=", g_prof.net_tcp_connect_poll_loops);
    user_log_dec_line("LinuxAbiServer.perf.poll.calls=", g_prof.poll_calls);
    user_log_dec_line("LinuxAbiServer.perf.poll.wait_loops=", g_prof.poll_wait_loops);
    user_log_dec_line("LinuxAbiServer.perf.select.calls=", g_prof.select_calls);
    user_log_dec_line("LinuxAbiServer.perf.select.wait_loops=", g_prof.select_wait_loops);
    user_log_dec_line("LinuxAbiServer.perf.getrandom.calls=", g_prof.getrandom_calls);
    user_log_dec_line("LinuxAbiServer.perf.getrandom.bytes=", g_prof.getrandom_bytes);
    user_log("LinuxAbiServer.perf.end\n");
    profile_clear();
}
