#include "pacha/ipc.h"

typedef unsigned long long u64;

enum {
    SMOKE_SYSCALL_LOG = 0x9,
    SMOKE_SYSCALL_CREATE_SUSPENDED_PROCESS = 0x41,
    SMOKE_SYSCALL_ALLOC_MAP_PAGES_TO_PROCESS = 0x43,
    SMOKE_SYSCALL_SET_PROCESS_INITIAL_CONTEXT = 0x44,
    SMOKE_SYSCALL_START_PROCESS = 0x45,
    SMOKE_SYSCALL_ABORT_PROCESS = 0x46,
    SMOKE_SYSCALL_COPY_TO_PROCESS = 0x47,
    SMOKE_SYSCALL_TRANSFER_FD_TO_PROCESS = 0x6C,
    SMOKE_SYSCALL_OK = 0,
    SMOKE_PROCESS_BUILDER_TOKEN_TAG = 1ULL << 60,
    SMOKE_SPAWN_RESULT_TAG = 1ULL << 63,
    USER_STACK_TOP = 0x3C000000,
    USER_STACK_PAGES = 16,
    USER_STACK_BOTTOM_VA = USER_STACK_TOP - USER_STACK_PAGES * 4096,
    USER_ENTRY_RSP = USER_STACK_TOP - 8,
    CHILD_CONTROL_FD = 16,
    ELF_PT_LOAD = 1,
    ELF_PF_X = 1,
    ELF_PF_W = 2,
    ELF_PF_R = 4,
};

static u64 cstr_len(const char *s) {
    u64 n = 0;
    while (s[n] != 0) n++;
    return n;
}

static u64 smoke_syscall0(u64 nr) {
    u64 ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(nr)
        : "rcx", "rdx", "r8", "r9", "r10", "r11", "memory");
    return ret;
}

static u64 smoke_syscall1(u64 nr, u64 a0) {
    u64 ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(nr), "D"(a0)
        : "rcx", "rdx", "r8", "r9", "r10", "r11", "memory");
    return ret;
}

static u64 smoke_syscall2(u64 nr, u64 a0, u64 a1) {
    u64 ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(nr), "D"(a0), "S"(a1)
        : "rcx", "rdx", "r8", "r9", "r10", "r11", "memory");
    return ret;
}

static u64 smoke_syscall4(u64 nr, u64 a0, u64 a1, u64 a2, u64 a3) {
    u64 ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(nr), "D"(a0), "S"(a1), "d"(a2), "c"(a3)
        : "r8", "r9", "r10", "r11", "memory");
    return ret;
}

static u64 smoke_syscall5(u64 nr, u64 a0, u64 a1, u64 a2, u64 a3, u64 a4) {
    u64 ret;
    __asm__ volatile(
        "mov %[a4], %%r8\n\t"
        "int $0x80"
        : "=a"(ret)
        : "a"(nr), "D"(a0), "S"(a1), "d"(a2), "c"(a3), [a4] "r"(a4)
        : "r8", "r9", "r10", "r11", "memory");
    return ret;
}

static u64 smoke_syscall6(u64 nr, u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
    u64 ret;
    __asm__ volatile(
        "mov %[a4], %%r8\n\t"
        "mov %[a5], %%r9\n\t"
        "int $0x80"
        : "=a"(ret)
        : "a"(nr), "D"(a0), "S"(a1), "d"(a2), "c"(a3), [a4] "r"(a4), [a5] "r"(a5)
        : "r8", "r9", "r10", "r11", "memory");
    return ret;
}

static int smoke_builder_token_valid(u64 token) {
    return (token & SMOKE_PROCESS_BUILDER_TOKEN_TAG) == SMOKE_PROCESS_BUILDER_TOKEN_TAG;
}

static u64 smoke_builder_create_suspended(void) {
    return smoke_syscall0(SMOKE_SYSCALL_CREATE_SUSPENDED_PROCESS);
}

static int smoke_builder_alloc_map_pages(u64 token, u64 dest_va, u64 page_count, u64 prot) {
    return smoke_syscall5(SMOKE_SYSCALL_ALLOC_MAP_PAGES_TO_PROCESS, token, dest_va, page_count, prot, 0) == SMOKE_SYSCALL_OK;
}

static int smoke_builder_copy_to_process(u64 token, u64 dest_va, u64 src_va, u64 byte_len) {
    return smoke_syscall4(SMOKE_SYSCALL_COPY_TO_PROCESS, token, dest_va, src_va, byte_len) == SMOKE_SYSCALL_OK;
}

static int smoke_builder_set_initial_context(u64 token, u64 rip, u64 rsp) {
    return smoke_syscall4(SMOKE_SYSCALL_SET_PROCESS_INITIAL_CONTEXT, token, rip, rsp, 0) == SMOKE_SYSCALL_OK;
}

static int smoke_builder_start(u64 token) {
    return (smoke_syscall1(SMOKE_SYSCALL_START_PROCESS, token) & SMOKE_SPAWN_RESULT_TAG) != 0;
}

static void smoke_builder_abort(u64 token) {
    if (smoke_builder_token_valid(token)) (void)smoke_syscall1(SMOKE_SYSCALL_ABORT_PROCESS, token);
}

static int smoke_builder_transfer_fd_to_process(u64 token, int source_fd, int min_fd, u64 rights, u64 flags) {
    const u64 result = smoke_syscall6(
        SMOKE_SYSCALL_TRANSFER_FD_TO_PROCESS,
        token,
        (u64)(unsigned)source_fd,
        (u64)(unsigned)min_fd,
        rights,
        flags,
        0
    );
    return result >= 16 ? (int)result : -(int)result;
}

static void log_text(const char *s) {
    (void)smoke_syscall2(SMOKE_SYSCALL_LOG, (u64)s, cstr_len(s));
}

static void log_hex(const char *prefix, u64 value) {
    static const char digits[] = "0123456789abcdef";
    char buf[96];
    u64 n = 0;
    while (prefix[n] != 0 && n + 19 < sizeof(buf)) {
        buf[n] = prefix[n];
        n++;
    }
    buf[n++] = '0';
    buf[n++] = 'x';
    for (int i = 15; i >= 0; i--) {
        buf[n++] = digits[(value >> ((u64)i * 4)) & 0xf];
    }
    buf[n++] = '\n';
    (void)smoke_syscall2(SMOKE_SYSCALL_LOG, (u64)buf, n);
}

static void log_hex_inline(u64 value) {
    static const char digits[] = "0123456789abcdef";
    char buf[18];
    u64 n = 0;
    buf[n++] = '0';
    buf[n++] = 'x';
    for (int i = 15; i >= 0; i--) {
        buf[n++] = digits[(value >> ((u64)i * 4)) & 0xf];
    }
    (void)smoke_syscall2(SMOKE_SYSCALL_LOG, (u64)buf, n);
}

static int expect_u64(const char *label, u64 got, u64 expected) {
    if (got == expected) return 1;
    log_text(label);
    log_hex(" got=", got);
    log_hex(" expected=", expected);
    return 0;
}

static int expect_ok(const char *label, int status) {
    if (status == 0) return 1;
    log_text(label);
    log_hex(" status=", (u64)(long long)status);
    return 0;
}

static int fast_echo_handler(void *ctx, const struct pacha_ipc_fast_entry *request, struct pacha_ipc_fast_entry *response) {
    (void)ctx;
    pacha_ipc_fast_entry_init(response, request->op + 1, request->offset + 0x1000, request->len * 2, request->flags + 1);
    response->status = 0x1234;
    return 0;
}

struct elf64_ehdr {
    unsigned char e_ident[16];
    unsigned short e_type;
    unsigned short e_machine;
    unsigned int e_version;
    u64 e_entry;
    u64 e_phoff;
    u64 e_shoff;
    unsigned int e_flags;
    unsigned short e_ehsize;
    unsigned short e_phentsize;
    unsigned short e_phnum;
    unsigned short e_shentsize;
    unsigned short e_shnum;
    unsigned short e_shstrndx;
};

struct elf64_phdr {
    unsigned int p_type;
    unsigned int p_flags;
    u64 p_offset;
    u64 p_vaddr;
    u64 p_paddr;
    u64 p_filesz;
    u64 p_memsz;
    u64 p_align;
};

extern void _start(void);

static u64 page_down(u64 value) {
    return value & ~0xfffull;
}

static u64 page_up(u64 value) {
    return (value + 0xfffull) & ~0xfffull;
}

static u64 prot_bits_from_elf_flags(unsigned int flags) {
    u64 prot = 0;
    if ((flags & ELF_PF_R) != 0) prot |= PACHA_PROT_READ;
    if ((flags & ELF_PF_W) != 0) prot |= PACHA_PROT_WRITE;
    if ((flags & ELF_PF_X) != 0) prot |= 1ull << 2;
    if ((prot & PACHA_PROT_READ) == 0) prot |= PACHA_PROT_READ;
    return prot;
}

static int child_copy_pages(u64 token, u64 dest_va, u64 src_va, u64 page_count, u64 prot) {
    if (!smoke_builder_alloc_map_pages(token, dest_va, page_count, prot)) return 0;
    return smoke_builder_copy_to_process(token, dest_va, src_va, page_count * 4096);
}

static int child_alloc_pages(u64 token, u64 dest_va, u64 page_count, u64 prot) {
    return smoke_builder_alloc_map_pages(token, dest_va, page_count, prot);
}

static int load_self_into_child(u64 token) {
    const u64 self_entry = (u64)(void *)&_start;
    const struct elf64_ehdr *eh = 0;
    u64 base = page_down(self_entry);
    for (u64 scan = 0; scan < 16; scan++) {
        const struct elf64_ehdr *candidate = (const struct elf64_ehdr *)(base - scan * 4096);
        if (candidate->e_ident[0] == 0x7f &&
            candidate->e_ident[1] == 'E' &&
            candidate->e_ident[2] == 'L' &&
            candidate->e_ident[3] == 'F')
        {
            eh = candidate;
            base = (u64)(void *)candidate;
            break;
        }
    }
    if (!eh || eh->e_phentsize != sizeof(struct elf64_phdr)) return 0;

    const struct elf64_phdr *ph = (const struct elf64_phdr *)(base + eh->e_phoff);
    for (unsigned short i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != ELF_PT_LOAD || ph[i].p_memsz == 0) continue;
        const u64 seg_start = page_down(base + ph[i].p_vaddr);
        const u64 seg_end = page_up(base + ph[i].p_vaddr + ph[i].p_memsz);
        const u64 page_count = (seg_end - seg_start) / 4096;
        if (!child_copy_pages(token, seg_start, seg_start, page_count, prot_bits_from_elf_flags(ph[i].p_flags))) return 0;
    }

    return child_alloc_pages(token, USER_STACK_BOTTOM_VA, USER_STACK_PAGES, PACHA_PROT_READ | PACHA_PROT_WRITE);
}

static int start_child_process(u64 token, u64 rip) {
    if (!smoke_builder_set_initial_context(token, rip, USER_ENTRY_RSP)) return 0;
    return smoke_builder_start(token);
}

static void child_process_main(void) {
    struct pacha_ipc_fast_channel server_fast = {0};
    int accept_status = -1;
    for (u64 spin = 0; spin < 10000000; spin++) {
        accept_status = pacha_ipc_fast_channel_accept(&server_fast, CHILD_CONTROL_FD, PACHA_IPC_FAST_F_PREFER_PKEY, 1);
        if (accept_status == 0) break;
        __asm__ volatile("pause");
    }
    if (accept_status != 0) {
        log_text("[fd_ipc_child] accept failed\n");
        log_hex("[fd_ipc_child] accept status=", (u64)(long long)accept_status);
        for (;;) __asm__ volatile("pause");
    }
    log_text("[fd_ipc_child] backend=");
    log_text(pacha_ipc_fast_backend_name(server_fast.backend));
    log_text(" reason=");
    log_text(pacha_ipc_fast_fallback_reason_name(server_fast.fallback_reason));
    log_text("\n");
    if (pacha_ipc_fast_serve_once(&server_fast, fast_echo_handler, 0) != 0) {
        log_text("[fd_ipc_child] serve failed\n");
    }
    for (;;) __asm__ volatile("pause");
}

static int run_separate_process_fast_smoke(u64 endpoint_rights) {
    struct pacha_ipc_channel_pair fast_pair = {0, 0};
    if (!expect_ok("[fd_ipc_boot_smoke] child channel create failed\n", pacha_ipc_channel_create(&fast_pair, endpoint_rights, 0))) return 0;

    const u64 token = smoke_builder_create_suspended();
    if (!smoke_builder_token_valid(token)) {
        log_hex("[fd_ipc_boot_smoke] create child failed=", token);
        return 0;
    }
    if (!load_self_into_child(token)) {
        log_text("[fd_ipc_boot_smoke] load child failed\n");
        smoke_builder_abort(token);
        return 0;
    }
    const int child_control_fd = smoke_builder_transfer_fd_to_process(
        token,
        fast_pair.b,
        CHILD_CONTROL_FD,
        PACHA_FD_RIGHT_SEND | PACHA_FD_RIGHT_RECV | PACHA_FD_RIGHT_CLOSE,
        0
    );
    if (child_control_fd != CHILD_CONTROL_FD) {
        log_hex("[fd_ipc_boot_smoke] child fd transfer failed=", (u64)(long long)child_control_fd);
        smoke_builder_abort(token);
        return 0;
    }

    struct pacha_ipc_fast_channel client_fast = {0};
    if (!expect_ok("[fd_ipc_boot_smoke] child fast offer failed\n", pacha_ipc_fast_channel_offer(&client_fast, fast_pair.a, PACHA_IPC_FAST_F_PREFER_PKEY, 1))) return 0;
    log_text("[fd_ipc_boot_smoke] child fast client=");
    log_text(pacha_ipc_fast_backend_name(client_fast.backend));
    log_text(" reason=");
    log_text(pacha_ipc_fast_fallback_reason_name(client_fast.fallback_reason));
    log_text("\n");

    if (!start_child_process(token, (u64)(void *)&child_process_main)) {
        log_text("[fd_ipc_boot_smoke] child start failed\n");
        smoke_builder_abort(token);
        return 0;
    }

    struct pacha_ipc_fast_entry fast_request = {0};
    pacha_ipc_fast_entry_init(&fast_request, 0x6161, 0x3000, 64, 7);
    if (!expect_ok("[fd_ipc_boot_smoke] child fast send failed\n", pacha_ipc_fast_send(&client_fast, &fast_request))) return 0;

    struct pacha_ipc_fast_entry fast_response_recv = {0};
    int recv_status = -1;
    for (u64 spin = 0; spin < 10000000; spin++) {
        recv_status = pacha_ipc_fast_recv(&client_fast, &fast_response_recv);
        if (recv_status == 0) break;
        __asm__ volatile("pause");
    }
    if (!expect_ok("[fd_ipc_boot_smoke] child fast recv failed\n", recv_status)) return 0;
    if (!expect_u64("[fd_ipc_boot_smoke] child fast response op mismatch\n", fast_response_recv.op, 0x6162)) return 0;
    if (!expect_u64("[fd_ipc_boot_smoke] child fast response len mismatch\n", fast_response_recv.len, 128)) return 0;
    if (!expect_u64("[fd_ipc_boot_smoke] child fast response status mismatch\n", fast_response_recv.status, 0x1234)) return 0;
    return 1;
}

static int run_normal_backend_smoke(u64 endpoint_rights) {
    struct pacha_ipc_channel_pair pair = {0, 0};
    if (!expect_ok("[fd_ipc_boot_smoke] normal backend channel create failed\n", pacha_ipc_channel_create(&pair, endpoint_rights, 0))) return 0;
    struct pacha_ipc_fast_channel client = {0};
    struct pacha_ipc_fast_channel server = {0};
    if (!expect_ok("[fd_ipc_boot_smoke] normal client init failed\n", pacha_ipc_fast_channel_init_normal(&client, pair.a))) return 0;
    if (!expect_ok("[fd_ipc_boot_smoke] normal server init failed\n", pacha_ipc_fast_channel_init_normal(&server, pair.b))) return 0;
    struct pacha_ipc_fast_entry request = {0};
    pacha_ipc_fast_entry_init(&request, 0x7171, 0x4000, 32, 3);
    if (!expect_ok("[fd_ipc_boot_smoke] normal send failed\n", pacha_ipc_fast_send(&client, &request))) return 0;
    if (!expect_ok("[fd_ipc_boot_smoke] normal serve failed\n", pacha_ipc_fast_serve_once(&server, fast_echo_handler, 0))) return 0;
    struct pacha_ipc_fast_entry response = {0};
    if (!expect_ok("[fd_ipc_boot_smoke] normal recv failed\n", pacha_ipc_fast_recv(&client, &response))) return 0;
    if (!expect_u64("[fd_ipc_boot_smoke] normal response op mismatch\n", response.op, 0x7172)) return 0;
    if (!expect_u64("[fd_ipc_boot_smoke] normal response len mismatch\n", response.len, 64)) return 0;
    return 1;
}

void fd_ipc_boot_smoke_main(void) {
    log_text("[fd_ipc_boot_smoke] start\n");

    const u64 endpoint_rights =
        PACHA_FD_RIGHT_SEND | PACHA_FD_RIGHT_RECV | PACHA_FD_RIGHT_CALL | PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_CLOSE;
    const int endpoint = pacha_ipc_endpoint_create(endpoint_rights, 0);
    if (endpoint < 16) {
        log_hex("[fd_ipc_boot_smoke] endpoint_create failed=", (u64)(long long)endpoint);
        return;
    }

    struct pacha_ipc_msg send_msg = {
        .word0 = 11,
        .word1 = 22,
        .word2 = 33,
        .word3 = 44,
    };
    if (!expect_ok("[fd_ipc_boot_smoke] endpoint send failed\n", pacha_ipc_send(endpoint, &send_msg))) return;

    struct pacha_ipc_msg recv_msg = {
        .fd_capacity = 0,
    };
    if (!expect_ok("[fd_ipc_boot_smoke] endpoint recv failed\n", pacha_ipc_recv(endpoint, &recv_msg))) return;
    if (!expect_u64("[fd_ipc_boot_smoke] endpoint word0 mismatch\n", recv_msg.word0, 11)) return;
    if (!expect_u64("[fd_ipc_boot_smoke] endpoint word3 mismatch\n", recv_msg.word3, 44)) return;

    struct pacha_ipc_channel_pair pair = {0, 0};
    if (!expect_ok("[fd_ipc_boot_smoke] channel create failed\n", pacha_ipc_channel_create(&pair, endpoint_rights, 0))) return;
    send_msg.word0 = 55;
    send_msg.word3 = 88;
    if (!expect_ok("[fd_ipc_boot_smoke] channel send failed\n", pacha_ipc_send(pair.a, &send_msg))) return;
    recv_msg = (struct pacha_ipc_msg){0};
    if (!expect_ok("[fd_ipc_boot_smoke] channel recv failed\n", pacha_ipc_recv(pair.b, &recv_msg))) return;
    if (!expect_u64("[fd_ipc_boot_smoke] channel word0 mismatch\n", recv_msg.word0, 55)) return;
    if (!expect_u64("[fd_ipc_boot_smoke] channel word3 mismatch\n", recv_msg.word3, 88)) return;

    send_msg = (struct pacha_ipc_msg){
        .word0 = 101,
        .word1 = 202,
    };
    const int client_reply = pacha_ipc_call(endpoint, &send_msg);
    if (client_reply < 16) {
        log_hex("[fd_ipc_boot_smoke] call failed=", (u64)(long long)client_reply);
        return;
    }

    struct pacha_ipc_fd recv_fds[1] = {0};
    recv_msg = (struct pacha_ipc_msg){
        .fds = recv_fds,
        .fd_capacity = 1,
    };
    if (!expect_ok("[fd_ipc_boot_smoke] call recv failed\n", pacha_ipc_recv(endpoint, &recv_msg))) return;
    if (!expect_u64("[fd_ipc_boot_smoke] call word0 mismatch\n", recv_msg.word0, 101)) return;
    if (!expect_u64("[fd_ipc_boot_smoke] call fd_count mismatch\n", recv_msg.fd_count, 1)) return;

    const int server_reply = (int)recv_fds[0].fd;
    send_msg = (struct pacha_ipc_msg){
        .word0 = 303,
        .word1 = 404,
    };
    if (!expect_ok("[fd_ipc_boot_smoke] reply failed\n", pacha_ipc_reply(server_reply, &send_msg))) return;

    recv_msg = (struct pacha_ipc_msg){0};
    if (!expect_ok("[fd_ipc_boot_smoke] reply recv failed\n", pacha_ipc_recv(client_reply, &recv_msg))) return;
    if (!expect_u64("[fd_ipc_boot_smoke] reply word0 mismatch\n", recv_msg.word0, 303)) return;
    if (!expect_u64("[fd_ipc_boot_smoke] reply word1 mismatch\n", recv_msg.word1, 404)) return;

    struct pacha_ipc_fast_channel fast = {0};
    if (!expect_ok("[fd_ipc_boot_smoke] fast init failed\n", pacha_ipc_fast_channel_init_local(&fast, endpoint, PACHA_IPC_FAST_F_PREFER_PKEY, 1))) return;
    log_text("[fd_ipc_boot_smoke] fast backend=");
    log_text(pacha_ipc_fast_backend_name(fast.backend));
    log_text(" reason=");
    log_text(pacha_ipc_fast_fallback_reason_name(fast.fallback_reason));
    log_text(" pku=");
    log_hex_inline((u64)(unsigned)pacha_ipc_pkey_supported());
    log_text(" ospke=");
    log_hex_inline((u64)(unsigned)pacha_ipc_pkey_enabled());
    log_text("\n");

    struct pacha_ipc_channel_pair fast_pair = {0, 0};
    if (!expect_ok("[fd_ipc_boot_smoke] fast setup channel create failed\n", pacha_ipc_channel_create(&fast_pair, endpoint_rights, 0))) return;
    struct pacha_ipc_fast_channel client_fast = {0};
    struct pacha_ipc_fast_channel server_fast = {0};
    if (!expect_ok("[fd_ipc_boot_smoke] fast offer failed\n", pacha_ipc_fast_channel_offer(&client_fast, fast_pair.a, PACHA_IPC_FAST_F_PREFER_PKEY, 1))) return;
    if (!expect_ok("[fd_ipc_boot_smoke] fast accept failed\n", pacha_ipc_fast_channel_accept(&server_fast, fast_pair.b, PACHA_IPC_FAST_F_PREFER_PKEY, 1))) return;
    if (!expect_u64("[fd_ipc_boot_smoke] fast client not ready\n", (u64)(unsigned)pacha_ipc_fast_channel_ready(&client_fast), 1)) return;
    if (!expect_u64("[fd_ipc_boot_smoke] fast server not ready\n", (u64)(unsigned)pacha_ipc_fast_channel_ready(&server_fast), 1)) return;
    log_text("[fd_ipc_boot_smoke] fast setup client=");
    log_text(pacha_ipc_fast_backend_name(client_fast.backend));
    log_text(" server=");
    log_text(pacha_ipc_fast_backend_name(server_fast.backend));
    log_text(" reason=");
    log_text(pacha_ipc_fast_fallback_reason_name(server_fast.fallback_reason));
    log_text("\n");

    struct pacha_ipc_fast_entry fast_request = {0};
    pacha_ipc_fast_entry_init(&fast_request, 0x5151, 0x2000, 128, 9);
    if (!expect_ok("[fd_ipc_boot_smoke] fast request send failed\n", pacha_ipc_fast_send(&client_fast, &fast_request))) return;
    if (!expect_ok("[fd_ipc_boot_smoke] fast serve_once failed\n", pacha_ipc_fast_serve_once(&server_fast, fast_echo_handler, 0))) return;
    struct pacha_ipc_fast_entry fast_response_recv = {0};
    if (!expect_ok("[fd_ipc_boot_smoke] fast response recv failed\n", pacha_ipc_fast_recv(&client_fast, &fast_response_recv))) return;
    if (!expect_u64("[fd_ipc_boot_smoke] fast response op mismatch\n", fast_response_recv.op, 0x5152)) return;
    if (!expect_u64("[fd_ipc_boot_smoke] fast response len mismatch\n", fast_response_recv.len, 256)) return;
    if (!expect_u64("[fd_ipc_boot_smoke] fast response status mismatch\n", fast_response_recv.status, 0x1234)) return;

    if (!run_separate_process_fast_smoke(endpoint_rights)) return;
    if (!run_normal_backend_smoke(endpoint_rights)) return;

    log_text("[fd_ipc_boot_smoke] OK\n");
}
