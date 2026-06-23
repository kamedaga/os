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
    SYSCALL_GET_TICK_COUNT = 0x2D,
    SYSCALL_GRANT_CAP_ON_ENDPOINT = 0x24,
    SYSCALL_INSTALL_ENDPOINT = 0x26,
    SYSCALL_ACCEPT_CAP_TRANSFER = 0x2A,
    SYSCALL_SHARE_CAP = 0x2B,
    SYSCALL_SIGNAL_ENDPOINT = 0x2C,
    SYSCALL_GET_PROCESS_SLOT = 0x2E,
    SYSCALL_PUBLISH_SERVICE_ENDPOINT = 0x33,
    SYSCALL_MAP_PAGE_ANYWHERE = 0x5C,
    SYSCALL_CREATE_IPC_BUFFER_FROM_PAGE = 0x5E,
    SYSCALL_GRANT_IPC_BUFFER_ON_ENDPOINT = 0x5F,
    SYSCALL_SHARE_IPC_BUFFER_ON_ENDPOINT = 0x60,
    SYSCALL_ACCEPT_IPC_BUFFER_TRANSFER = 0x61,
    SYSCALL_MAP_IPC_BUFFER_ANYWHERE = 0x62,

    PAGE_BYTES = 4096,
    PAGE_RIGHT_CPU_READ = 0x1,
    PAGE_RIGHT_CPU_WRITE = 0x2,
    IPC_BUFFER_TOKEN_TAG = 0xA000000000000000ULL,
    IPC_BUFFER_TOKEN_MASK = 0x0FFFFFFFFFFFFFFFULL,
    IPC_BUFFER_RIGHT_READ = 0x1,
    IPC_BUFFER_RIGHT_WRITE = 0x2,
    IPC_BUFFER_RIGHT_MAP = 0x4,
    IPC_BUFFER_RIGHT_GRANT = 0x8,
    IPC_BUFFER_ROLE_REQUEST = 1,
    IPC_BUFFER_ROLE_RESPONSE = 2,
    SERVICE_REGISTRY_SHADOW_VA = 0x3C2C0000,
    SERVICE_REGISTRY_MAGIC = 0x53525643,
    SERVICE_REGISTRY_VERSION = 1,
    SERVICE_REGISTRY_MAX_ENTRIES = 12,
    SERVICE_KIND_CONSOLE = 10,

    TTY_SERVICE_ENDPOINT_ID = 0x8A,
    TTY_REPLY_ENDPOINT_ID = 0xEF,

    CONSOLE_REQUEST_MAGIC = 0x514E4F43,
    CONSOLE_RESPONSE_MAGIC = 0x524E4F43,
    CONSOLE_PROTOCOL_VERSION = 1,
    CONSOLE_OP_CONNECT = 1,
    CONSOLE_OP_READ = 2,
    CONSOLE_OP_WRITE = 3,
    CONSOLE_OP_GET_ATTR = 4,
    CONSOLE_OP_SET_ATTR = 5,
    CONSOLE_OP_GET_SIGNAL = 6,
    CONSOLE_OP_POLL = 7,
    CONSOLE_REQUEST_FLAG_NONBLOCK = 1 << 0,
    CONSOLE_REQUEST_FLAG_TIMEOUT = 1 << 1,
    CONSOLE_STATUS_OK = 0,
    CONSOLE_STATUS_AGAIN = 1,
    CONSOLE_STATUS_INVALID = 2,
    CONSOLE_STATUS_IO_ERROR = 3,
    CONSOLE_STATUS_INTERRUPTED = 5,
    CONSOLE_REQUEST_HEADER_BYTES = 64,
    CONSOLE_RESPONSE_HEADER_BYTES = 64,
    CONSOLE_REQUEST_PAYLOAD_BYTES = PAGE_BYTES - CONSOLE_REQUEST_HEADER_BYTES,
    CONSOLE_RESPONSE_PAYLOAD_BYTES = PAGE_BYTES - CONSOLE_RESPONSE_HEADER_BYTES,
    TTY_INPUT_RING_BYTES = 32768,
    TTY_PUMP_IDLE_BUDGET = 32,
    TTY_PUMP_READ_BUDGET = 64,
    TTY_FLUSH_IDLE_BUDGET = 1,
    TTY_FLUSH_ACTIVE_BUDGET = 32,
    TTY_RAW_READ_CHUNK_BYTES = 64,
    TTY_BACKEND_TX_HIGH_WATER_BYTES = 786432,
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
    u64 request_token;
    u64 response_token;
    u64 request_va;
    u64 response_va;
    u64 session_nonce;
    u64 next_seq;
};

struct tty_client_session {
    int active;
    u64 request_paddr;
    u64 response_paddr;
    u64 request_token;
    u64 response_token;
    u64 request_va;
    u64 response_va;
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

static u64 map_page_anywhere(u64 paddr, u64 writable) {
    return syscall2(SYSCALL_MAP_PAGE_ANYWHERE, paddr, writable);
}

static int is_ipc_buffer_token(u64 token) {
    return (token & ~IPC_BUFFER_TOKEN_MASK) == IPC_BUFFER_TOKEN_TAG && (token & IPC_BUFFER_TOKEN_MASK) != 0;
}

static u64 create_ipc_buffer_from_page(u64 paddr, u64 rights, u64 role) {
    return syscall3(SYSCALL_CREATE_IPC_BUFFER_FROM_PAGE, paddr, rights, role);
}

static u64 grant_ipc_buffer_on_endpoint(u64 token, u64 endpoint_id, u64 rights) {
    return syscall3(SYSCALL_GRANT_IPC_BUFFER_ON_ENDPOINT, token, endpoint_id, rights);
}

static u64 share_ipc_buffer_on_endpoint(u64 token, u64 endpoint_id, u64 rights) {
    return syscall3(SYSCALL_SHARE_IPC_BUFFER_ON_ENDPOINT, token, endpoint_id, rights);
}

static u64 accept_ipc_buffer_transfer(u64 transfer_id) {
    return syscall1(SYSCALL_ACCEPT_IPC_BUFFER_TRANSFER, transfer_id);
}

static u64 map_ipc_buffer_anywhere(u64 token, u64 writable) {
    return syscall2(SYSCALL_MAP_IPC_BUFFER_ANYWHERE, token, writable);
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
        tty_core_pump_input(TTY_PUMP_IDLE_BUDGET);
        try_complete_pending_read();
        backend_flush_output_budget(TTY_FLUSH_IDLE_BUDGET);
        handle_client_request();
        if (backend_output_pending() || g_client.active) {
            (void)syscall2(SYSCALL_WAIT_EVENT, 0, 1);
        } else {
            const u64 received = syscall2(SYSCALL_WAIT_EVENT, 1, 1);
            if (received >= 0x1000) handle_client_connect_transfer(received);
        }
        tty_core_pump_input(TTY_PUMP_READ_BUDGET);
        try_complete_pending_read();
        backend_flush_output_budget(TTY_FLUSH_ACTIVE_BUDGET);
        handle_client_request();
    }
}
