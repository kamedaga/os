typedef unsigned long long u64;
typedef long long i64;

enum {
    SYSCALL_ALLOC_MAP_PAGES = 0xC,
    SYSCALL_LOG = 0x9,
    SYSCALL_WAIT_EVENT = 0x17,
    SYSCALL_IPC_CALL_REPLY_RECV = 0x40,
    SYSCALL_MAP_ABI_TRAP_REPLY_TARGET_PAGES = 0x4C,
    SYSCALL_OK = 0,
    IPC_CALL_FLAG_SIGNAL_ONLY = 0x2,

    PAGE_BYTES = 4096,
    TRAP_MAGIC = 0x3149424150415254ULL,
    TRAP_VERSION = 1,

    LINUX_SYS_WRITE = 1,
    LINUX_SYS_MMAP = 9,
    LINUX_SYS_EXIT = 60,
    LINUX_SYS_EXIT_GROUP = 231,

    TRAP_RESPONSE_FLAG_EXIT = 1,
};

struct ipc_message {
    u64 status;
    u64 request_va;
    u64 reserved0;
    u64 reserved1;
    u64 reserved2;
};

struct trap_request {
    u64 magic;
    unsigned version;
    unsigned kind;
    unsigned flavor;
    unsigned reserved0;
    u64 caller_principal;
    u64 thread_id;
    u64 rip;
    u64 rsp;
    u64 fault_addr;
    u64 error_code;
    u64 nr;
    u64 args[6];
};

static u64 mmap_next_va = 0x31000000ULL;
static u64 trap_request_page_va = 0;

static u64 cstr_len(const char *s) {
    u64 n = 0;
    while (s[n] != 0) n++;
    return n;
}

static void user_log_len(const char *message, u64 len) {
    u64 ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"((u64)SYSCALL_LOG),
          "D"((u64)message),
          "S"(len)
        : "rcx", "rdx", "r8", "r9", "r10", "r11", "memory");
    (void)ret;
}

static void user_log(const char *message) {
    user_log_len(message, cstr_len(message));
}

static void user_log_hex_value(u64 value) {
    static const char hex[] = "0123456789ABCDEF";
    char buf[32];
    u64 pos = 0;
    if (pos + 2 < sizeof(buf)) {
        buf[pos++] = '0';
        buf[pos++] = 'x';
    }
    int started = 0;
    for (int shift = 60; shift >= 0; shift -= 4) {
        unsigned nibble = (unsigned)((value >> (u64)shift) & 0xFULL);
        if (nibble != 0 || started || shift == 0) {
            if (pos + 1 < sizeof(buf)) buf[pos++] = hex[nibble];
            started = 1;
        }
    }
    if (pos + 1 < sizeof(buf)) buf[pos++] = '\n';
    user_log_len(buf, pos);
}

static void log_syscall_nr(u64 nr) {
    user_log("LinuxAbiServer: syscall\n");
    user_log_hex_value(nr);
}

static struct ipc_message wait_ipc(void) {
    register u64 rax __asm__("rax") = SYSCALL_WAIT_EVENT;
    register u64 rdi __asm__("rdi") = 0;
    register u64 rsi __asm__("rsi") = 0;
    register u64 rdx __asm__("rdx");
    register u64 r8 __asm__("r8");

    __asm__ volatile(
        "int $0x80"
        : "+r"(rax), "+r"(rdi), "+r"(rsi), "=r"(rdx), "=r"(r8)
        :
        : "rcx", "r9", "r10", "r11", "memory");

    struct ipc_message msg;
    msg.status = rax;
    msg.request_va = rdi;
    msg.reserved0 = rsi;
    msg.reserved1 = rdx;
    msg.reserved2 = r8;
    return msg;
}

static struct ipc_message reply(u64 result, u64 flags) {
    register u64 rax __asm__("rax") = SYSCALL_IPC_CALL_REPLY_RECV;
    register u64 rdi __asm__("rdi") = result;
    register u64 rsi __asm__("rsi") = 0;
    register u64 rdx __asm__("rdx") = IPC_CALL_FLAG_SIGNAL_ONLY;
    register u64 r8 __asm__("r8") = flags;
    register u64 r9 __asm__("r9") = 0;
    register u64 r10 __asm__("r10") = 0;

    __asm__ volatile(
        "int $0x80"
        : "+r"(rax), "+r"(rdi), "+r"(rsi), "+r"(rdx), "+r"(r8), "+r"(r9), "+r"(r10)
        :
        : "rcx", "r11", "memory");

    struct ipc_message msg;
    msg.status = rax;
    msg.request_va = rdi;
    msg.reserved0 = rsi;
    msg.reserved1 = rdx;
    msg.reserved2 = r8;
    return msg;
}

static u64 map_reply_target_pages(u64 target_va, u64 page_count, u64 prot_bits) {
    u64 ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"((u64)SYSCALL_MAP_ABI_TRAP_REPLY_TARGET_PAGES),
          "D"(target_va),
          "S"(page_count),
          "d"(prot_bits)
        : "rcx", "r8", "r9", "r10", "r11", "memory");
    return ret;
}

static u64 alloc_map_pages(u64 target_va, u64 page_count, u64 flags) {
    u64 ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"((u64)SYSCALL_ALLOC_MAP_PAGES),
          "D"(target_va),
          "S"(page_count),
          "d"(flags),
          "c"(0)
        : "r8", "r9", "r10", "r11", "memory");
    return ret;
}

static u64 errno_inval(void) {
    return (u64)(i64)-22;
}

static u64 errno_badf(void) {
    return (u64)(i64)-9;
}

static u64 errno_acces(void) {
    return (u64)(i64)-13;
}

static u64 errno_nosys(void) {
    return (u64)(i64)-38;
}

static u64 errno_nomem(void) {
    return (u64)(i64)-12;
}

static u64 page_up(u64 value) {
    return (value + PAGE_BYTES - 1) & ~(u64)(PAGE_BYTES - 1);
}

static struct ipc_message handle_mmap(const struct trap_request *req) {
    enum {
        PROT_READ = 0x1,
        PROT_WRITE = 0x2,
        PROT_EXEC = 0x4,

        MAP_SHARED = 0x01,
        MAP_PRIVATE = 0x02,
        MAP_SHARED_VALIDATE = 0x03,
        MAP_TYPE = 0x0F,
        MAP_FIXED = 0x10,
        MAP_ANONYMOUS = 0x20,
        MAP_FIXED_NOREPLACE = 0x100000,
    };
    const u64 requested_va = req->args[0];
    const u64 len = req->args[1];
    const u64 requested_prot = req->args[2] & (PROT_READ | PROT_WRITE | PROT_EXEC);
    const u64 flags = req->args[3];
    const u64 fd = req->args[4];
    const u64 offset = req->args[5];
    const u64 map_type = flags & MAP_TYPE;
    u64 prot = requested_prot;

    if (len == 0) {
        user_log("LinuxAbiServer: mmap invalid zero len\n");
        return reply(errno_inval(), 0);
    }

    if (map_type != MAP_PRIVATE && map_type != MAP_SHARED && map_type != MAP_SHARED_VALIDATE) {
        user_log("LinuxAbiServer: mmap invalid map type\n");
        user_log_hex_value(flags);
        return reply(errno_inval(), 0);
    }

    if ((offset & (PAGE_BYTES - 1)) != 0) {
        user_log("LinuxAbiServer: mmap unaligned offset\n");
        user_log_hex_value(offset);
        return reply(errno_inval(), 0);
    }

    if ((flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) != 0 && (requested_va == 0 || (requested_va & (PAGE_BYTES - 1)) != 0)) {
        user_log("LinuxAbiServer: mmap bad fixed va\n");
        user_log_hex_value(requested_va);
        return reply(errno_inval(), 0);
    }

    if ((flags & MAP_ANONYMOUS) == 0) {
        user_log("LinuxAbiServer: file-backed mmap unsupported\n");
        user_log_hex_value(flags);
        user_log_hex_value(fd);
        if (fd <= 2) return reply(errno_acces(), 0);
        return reply(errno_badf(), 0);
    }

    if (prot == 0) {
        user_log("LinuxAbiServer: mmap PROT_NONE unsupported\n");
        return reply(errno_inval(), 0);
    }

    if ((prot & PROT_WRITE) != 0) prot |= PROT_READ;
    if ((prot & PROT_WRITE) != 0 && (prot & PROT_EXEC) != 0) {
        user_log("LinuxAbiServer: mmap W+X unsupported\n");
        return reply(errno_inval(), 0);
    }

    const u64 size = page_up(len);
    const u64 page_count = size / PAGE_BYTES;
    const u64 target_va = ((flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) != 0) ? requested_va : mmap_next_va;
    const u64 map_status = map_reply_target_pages(target_va, page_count, prot);
    if (map_status == SYSCALL_OK) {
        if ((flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) == 0) mmap_next_va += size;
        user_log("LinuxAbiServer: mmap ok va\n");
        user_log_hex_value(target_va);
        return reply(target_va, 0);
    } else {
        user_log("LinuxAbiServer: mmap failed pages\n");
        user_log_hex_value(page_count);
        user_log("LinuxAbiServer: mmap status\n");
        user_log_hex_value(map_status);
        return reply(errno_nomem(), 0);
    }
}

static struct ipc_message handle_write(const struct trap_request *req) {
    const u64 fd = req->args[0];
    const u64 len = req->args[2];
    if (fd == 1 || fd == 2) {
        user_log("LinuxAbiServer: write len\n");
        user_log_hex_value(len);
        return reply(len, 0);
    } else {
        user_log("LinuxAbiServer: write bad fd\n");
        user_log_hex_value(fd);
        return reply(errno_inval(), 0);
    }
}

void linux_abi_main(void) {
    const struct ipc_message cfg = wait_ipc();
    if (cfg.status != SYSCALL_OK || cfg.request_va == 0) {
        user_log("LinuxAbiServer: config IPC invalid\n");
        for (;;) __asm__ volatile("pause");
    }
    trap_request_page_va = cfg.request_va;
    const u64 request_page_status = alloc_map_pages(trap_request_page_va, 1, 0x1);
    if (request_page_status != SYSCALL_OK) {
        user_log("LinuxAbiServer: request page map failed\n");
        user_log_hex_value(request_page_status);
        for (;;) __asm__ volatile("pause");
    }
    user_log("LinuxAbiServer: started\n");
    struct ipc_message msg = reply(0, 0);
    for (;;) {
        if (msg.status != SYSCALL_OK) {
            msg = wait_ipc();
            continue;
        }
        if (msg.request_va != trap_request_page_va) {
            user_log("LinuxAbiServer: bad request va\n");
            user_log_hex_value(msg.request_va);
            msg = reply(errno_inval(), 0);
            continue;
        }

        const struct trap_request *req = (const struct trap_request *)trap_request_page_va;
        if (req->magic != TRAP_MAGIC || req->version != TRAP_VERSION) {
            user_log("LinuxAbiServer: bad request header\n");
            msg = reply(errno_inval(), 0);
            continue;
        }

        log_syscall_nr(req->nr);
        switch (req->nr) {
        case LINUX_SYS_MMAP:
            msg = handle_mmap(req);
            break;
        case LINUX_SYS_WRITE:
            msg = handle_write(req);
            break;
        case LINUX_SYS_EXIT:
        case LINUX_SYS_EXIT_GROUP:
            user_log("LinuxAbiServer: exit\n");
            msg = reply(0, TRAP_RESPONSE_FLAG_EXIT);
            break;
        default:
            user_log("LinuxAbiServer: unhandled syscall\n");
            user_log_hex_value(req->nr);
            msg = reply(errno_nosys(), 0);
            break;
        }
    }
}
