static u64 cstr_len(const char *s) { u64 n = 0; while (s[n] != 0) n++; return n; }
static void user_log_len(const char *message, u64 len) { u64 ret; __asm__ volatile("int $0x80" : "=a"(ret) : "a"((u64)SYSCALL_LOG), "D"((u64)message), "S"(len) : "rcx", "rdx", "r8", "r9", "r10", "r11", "memory"); (void)ret; }
static void user_log(const char *message) { user_log_len(message, cstr_len(message)); }
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

static struct ipc_message wait_ipc(void) {
    register u64 rax __asm__("rax") = SYSCALL_WAIT_EVENT; register u64 rdi __asm__("rdi") = 0; register u64 rsi __asm__("rsi") = 0; register u64 rdx __asm__("rdx"); register u64 r8 __asm__("r8");
    __asm__ volatile("int $0x80" : "+r"(rax), "+r"(rdi), "+r"(rsi), "=r"(rdx), "=r"(r8) : : "rcx", "r9", "r10", "r11", "memory");
    struct ipc_message msg = { rax, rdi, rsi, rdx, r8 }; return msg;
}
static struct ipc_message reply(u64 result, u64 flags) {
    register u64 rax __asm__("rax") = SYSCALL_IPC_CALL_REPLY_RECV; register u64 rdi __asm__("rdi") = result; register u64 rsi __asm__("rsi") = 0; register u64 rdx __asm__("rdx") = IPC_CALL_FLAG_SIGNAL_ONLY; register u64 r8 __asm__("r8") = flags; register u64 r9 __asm__("r9") = 0; register u64 r10 __asm__("r10") = 0;
    __asm__ volatile("int $0x80" : "+r"(rax), "+r"(rdi), "+r"(rsi), "+r"(rdx), "+r"(r8), "+r"(r9), "+r"(r10) : : "rcx", "r11", "memory");
    struct ipc_message msg = { rax, rdi, rsi, rdx, r8 }; return msg;
}

static u64 errno_noent(void) { return (u64)(i64)-2; }
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
static u64 errno_spipe(void) { return (u64)(i64)-29; }
static u64 errno_pipe(void) { return (u64)(i64)-32; }
static u64 errno_nosys(void) { return (u64)(i64)-38; }
static u64 errno_nametoolong(void) { return (u64)(i64)-36; }
static u64 errno_range(void) { return (u64)(i64)-34; }
static u64 errno_timedout(void) { return (u64)(i64)-110; }

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
static void detach_reply_token(void) { (void)syscall0(SYSCALL_DETACH_ABI_TRAP_REPLY_TOKEN); }
static u64 alloc_map_pages(u64 target_va, u64 page_count, u64 flags) { return syscall4(SYSCALL_ALLOC_MAP_PAGES, target_va, page_count, flags, 0); }
static int install_self_wake_endpoint(void) { return syscall3(SYSCALL_INSTALL_ENDPOINT, 0, LINUX_ABI_SELF_WAKE_ENDPOINT_ID, syscall0(SYSCALL_GET_PROCESS_SLOT)) == SYSCALL_OK; }
static void prime_reply_return_signal(void) { (void)syscall2(SYSCALL_SIGNAL_ENDPOINT, LINUX_ABI_SELF_WAKE_ENDPOINT_ID, 0); }

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
static int install_vfs_endpoint(void) { return syscall3(SYSCALL_INSTALL_ENDPOINT, 0, g_vfs.endpoint_id, g_vfs.process_slot) == SYSCALL_OK; }
static int grant_vfs_response_page(void) { u64 ret = syscall3(SYSCALL_GRANT_CAP_ON_ENDPOINT, g_vfs.response_paddr, g_vfs.endpoint_id, PAGE_RIGHT_CPU_READ | PAGE_RIGHT_CPU_WRITE); if (ret == SYSCALL_OK) return 1; if (ret == SYSCALL_ERR_ENDPOINT && install_vfs_endpoint()) ret = syscall3(SYSCALL_GRANT_CAP_ON_ENDPOINT, g_vfs.response_paddr, g_vfs.endpoint_id, PAGE_RIGHT_CPU_READ | PAGE_RIGHT_CPU_WRITE); return ret == SYSCALL_OK; }
static int share_vfs_request_page(void) { u64 ret = syscall2(SYSCALL_SHARE_CAP, g_vfs.request_paddr, g_vfs.endpoint_id); if (ret == SYSCALL_OK) return 1; if (ret == SYSCALL_ERR_ENDPOINT && install_vfs_endpoint()) ret = syscall2(SYSCALL_SHARE_CAP, g_vfs.request_paddr, g_vfs.endpoint_id); return ret == SYSCALL_OK; }
static int signal_vfs(void) { u64 ret = syscall2(SYSCALL_SIGNAL_ENDPOINT, g_vfs.endpoint_id, 0); if (ret == SYSCALL_OK) return 1; if (ret == SYSCALL_ERR_ENDPOINT && install_vfs_endpoint()) ret = syscall2(SYSCALL_SIGNAL_ENDPOINT, g_vfs.endpoint_id, 0); return ret == SYSCALL_OK; }
static int wait_vfs_response(u64 expected_seq, u16 expected_op) { volatile struct fs_response_header *response = (volatile struct fs_response_header *)VFS_RESPONSE_VA; for (u64 i = 0; i < 8192; i++) { if (response->response_seq == expected_seq) return response->magic == FS_RESPONSE_MAGIC && response->version == FS_PROTOCOL_VERSION && response->op == expected_op; (void)syscall2(SYSCALL_WAIT_EVENT, 0, 1); } return 0; }
