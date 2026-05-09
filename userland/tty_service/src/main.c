typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef int i32;

enum {
    SYSCALL_OK = 0,
    SYSCALL_ALLOC_PAGE = 0x1,
    SYSCALL_MAP_PAGE = 0x2,
    SYSCALL_LOG = 0x9,
    SYSCALL_WAIT_EVENT = 0x17,
    SYSCALL_GRANT_CAP_ON_ENDPOINT = 0x24,
    SYSCALL_INSTALL_ENDPOINT = 0x26,
    SYSCALL_ACCEPT_CAP_TRANSFER = 0x2A,
    SYSCALL_SHARE_CAP = 0x2B,
    SYSCALL_SIGNAL_ENDPOINT = 0x2C,
    SYSCALL_GET_PROCESS_SLOT = 0x2E,
    SYSCALL_PUBLISH_SERVICE_ENDPOINT = 0x33,

    PAGE_BYTES = 4096,
    PAGE_RIGHT_CPU_READ = 0x1,
    PAGE_RIGHT_CPU_WRITE = 0x2,
    SERVICE_REGISTRY_SHADOW_VA = 0x3C2C0000,
    SERVICE_REGISTRY_MAGIC = 0x53525643,
    SERVICE_REGISTRY_VERSION = 1,
    SERVICE_REGISTRY_MAX_ENTRIES = 12,
    SERVICE_KIND_CONSOLE = 10,

    TTY_SERVICE_ENDPOINT_ID = 0x8A,
    TTY_REPLY_ENDPOINT_ID = 0xEF,
    TTY_CLIENT_REQUEST_VA = 0x2B030000,
    TTY_CLIENT_RESPONSE_VA = 0x2B031000,
    TTY_CONSOLE_REQUEST_VA = 0x2B032000,
    TTY_CONSOLE_RESPONSE_VA = 0x2B033000,

    CONSOLE_REQUEST_MAGIC = 0x514E4F43,
    CONSOLE_RESPONSE_MAGIC = 0x524E4F43,
    CONSOLE_PROTOCOL_VERSION = 1,
    CONSOLE_OP_CONNECT = 1,
    CONSOLE_OP_READ = 2,
    CONSOLE_OP_WRITE = 3,
    CONSOLE_OP_GET_ATTR = 4,
    CONSOLE_OP_SET_ATTR = 5,
    CONSOLE_OP_GET_SIGNAL = 6,
    CONSOLE_REQUEST_FLAG_NONBLOCK = 1 << 0,
    CONSOLE_STATUS_OK = 0,
    CONSOLE_STATUS_AGAIN = 1,
    CONSOLE_STATUS_INVALID = 2,
    CONSOLE_STATUS_IO_ERROR = 3,
    CONSOLE_STATUS_INTERRUPTED = 5,
    CONSOLE_REQUEST_HEADER_BYTES = 64,
    CONSOLE_RESPONSE_HEADER_BYTES = 64,
    CONSOLE_REQUEST_PAYLOAD_BYTES = PAGE_BYTES - CONSOLE_REQUEST_HEADER_BYTES,
    CONSOLE_RESPONSE_PAYLOAD_BYTES = PAGE_BYTES - CONSOLE_RESPONSE_HEADER_BYTES,
    TTY_INPUT_RING_BYTES = 4096,
};

struct service_entry { u64 kind; u64 process_slot; u64 endpoint_id; u64 flags; };
struct service_registry_page { u64 magic; u64 version; u64 entry_count; u64 reserved0; struct service_entry entries[SERVICE_REGISTRY_MAX_ENTRIES]; };

struct console_request_header {
    u32 magic; u16 version; u16 op; u64 request_seq; u64 session_nonce; u32 length; u32 flags;
    u64 arg0; u64 arg1; u64 arg2; u64 reserved0;
};

struct console_response_header {
    u32 magic; u16 version; u16 op; u64 response_seq; i32 status; u32 result_flags;
    u32 inline_bytes; u32 reserved0; u64 arg0; u64 arg1; u64 reserved1; u64 reserved2;
};

struct console_client {
    int active;
    u64 endpoint_id;
    u64 process_slot;
    u64 request_paddr;
    u64 response_paddr;
    u64 session_nonce;
    u64 next_seq;
};

struct tty_client_session {
    int active;
    u64 request_paddr;
    u64 response_paddr;
    u64 session_nonce;
    u64 reply_endpoint_id;
    u64 last_completed_seq;
};

static struct console_client g_console;
static struct tty_client_session g_client;

static u64 syscall0(u64 n) {
    u64 ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(n) : "rcx", "rdx", "r8", "r9", "r10", "r11", "memory");
    return ret;
}

static u64 syscall1(u64 n, u64 a0) {
    u64 ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(n), "D"(a0) : "rcx", "rdx", "r8", "r9", "r10", "r11", "memory");
    return ret;
}

static u64 syscall2(u64 n, u64 a0, u64 a1) {
    u64 ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(n), "D"(a0), "S"(a1) : "rcx", "rdx", "r8", "r9", "r10", "r11", "memory");
    return ret;
}

static u64 syscall3(u64 n, u64 a0, u64 a1, u64 a2) {
    u64 ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(n), "D"(a0), "S"(a1), "d"(a2) : "rcx", "r8", "r9", "r10", "r11", "memory");
    return ret;
}

static u64 cstr_len(const char *s) {
    u64 n = 0;
    while (s[n] != 0) n++;
    return n;
}

static void user_log(const char *s) {
    (void)syscall2(SYSCALL_LOG, (u64)s, cstr_len(s));
}

static void clear_page(u64 va) {
    volatile u64 *p = (volatile u64 *)va;
    for (u64 i = 0; i < 512; i++) p[i] = 0;
}

static u64 min_u64(u64 a, u64 b) {
    return a < b ? a : b;
}

#include "backend.inc.c"
#include "bsd_line/tty_queue.h"
#include "bsd_line/tty_inq.c"
#include "bsd_line/tty_ttydisc.c"
#include "tty_core.inc.c"
#include "client_protocol.inc.c"

void tty_service_main(void) {
    user_log("TtyService: started\n");
    while (!backend_connect_console()) (void)syscall2(SYSCALL_WAIT_EVENT, 0, 1);
    tty_core_init_defaults();

    const u64 self_slot = syscall0(SYSCALL_GET_PROCESS_SLOT);
    if (syscall3(SYSCALL_INSTALL_ENDPOINT, 0, TTY_SERVICE_ENDPOINT_ID, self_slot) != SYSCALL_OK ||
        syscall2(SYSCALL_PUBLISH_SERVICE_ENDPOINT, TTY_SERVICE_ENDPOINT_ID, self_slot) != SYSCALL_OK) {
        user_log("TtyService: publish failed\n");
        for (;;) (void)syscall2(SYSCALL_WAIT_EVENT, 0, 1);
    }
    user_log("TtyService: endpoint ready\n");

    for (;;) {
        tty_core_pump_input(4);
        const u64 received = syscall2(SYSCALL_WAIT_EVENT, 1, 1);
        if (received >= 0x1000) handle_client_connect_transfer(received);
        handle_client_request();
        tty_core_pump_input(4);
    }
}
