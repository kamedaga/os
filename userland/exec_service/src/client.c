#include "exec_service_client.h"

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;

enum {
    SYSCALL_OK = 0,
    SYSCALL_ALLOC_PAGE = 0x1,
    SYSCALL_WAIT_EVENT = 0x17,
    SYSCALL_GET_PROCESS_SLOT = 0x2E,
    SYSCALL_MAP_PAGE_ANYWHERE = 0x5C,
    SYSCALL_CREATE_IPC_BUFFER_FROM_PAGE = 0x5E,
    SYSCALL_GRANT_IPC_BUFFER_ON_ENDPOINT = 0x5F,
    SYSCALL_SHARE_IPC_BUFFER_ON_ENDPOINT = 0x60,
    PAGE_BYTES = 4096,
    IPC_BUFFER_TOKEN_TAG = 0xA000000000000000ULL,
    IPC_BUFFER_TOKEN_MASK = 0x0FFFFFFFFFFFFFFFULL,
    IPC_BUFFER_RIGHT_READ = 0x1,
    IPC_BUFFER_RIGHT_WRITE = 0x2,
    IPC_BUFFER_RIGHT_MAP = 0x4,
    IPC_BUFFER_RIGHT_GRANT = 0x8,
    IPC_BUFFER_ROLE_REQUEST = 1,
    IPC_BUFFER_ROLE_RESPONSE = 2,
};

static u64 syscall0(u64 n) {
    u64 ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(n) : "rcx", "rdx", "r8", "r9", "r10", "r11", "memory");
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

static u64 cstr_len(const char *s, u64 cap) {
    u64 i = 0;
    if (!s) return 0;
    while (i < cap && s[i] != 0) i++;
    return i;
}

static void clear_page(u64 va) {
    u8 *p = (u8 *)va;
    for (u64 i = 0; i < PAGE_BYTES; i++) p[i] = 0;
}

static int append_arg(struct exec_service_request *request, u16 *cursor, const char *value, u16 *offset_out, u16 *bytes_out) {
    const u64 len = cstr_len(value, EXEC_SERVICE_MAX_ARG_DATA_BYTES);
    if (len == 0 || len > 0xffff) return 0;
    if ((u64)*cursor + len > EXEC_SERVICE_MAX_ARG_DATA_BYTES) return 0;
    *offset_out = *cursor;
    *bytes_out = (u16)len;
    for (u64 i = 0; i < len; i++) request->arg_data[(u64)*cursor + i] = (u8)value[i];
    *cursor = (u16)((u64)*cursor + len);
    return 1;
}

static int is_ipc_buffer_token(u64 token) {
    return (token & ~IPC_BUFFER_TOKEN_MASK) == IPC_BUFFER_TOKEN_TAG && (token & IPC_BUFFER_TOKEN_MASK) != 0;
}

static int fill_request(struct exec_service_request *request, const struct exec_service_spawn_options *options, u64 response_token) {
    if (!request || !options || !options->path || !options->argv || options->argv_count == 0) return 0;
    if (options->argv_count > EXEC_SERVICE_MAX_ARGV || options->envp_count > EXEC_SERVICE_MAX_ENVP) return 0;

    clear_page((u64)request);
    request->magic = EXEC_SERVICE_ABI_MAGIC;
    request->version = EXEC_SERVICE_ABI_VERSION;
    request->op = EXEC_SERVICE_OP_SPAWN_LINUX;
    request->response_token = response_token;
    request->client_process_slot = syscall0(SYSCALL_GET_PROCESS_SLOT);

    u16 cursor = 0;
    if (!append_arg(request, &cursor, options->path, &request->argv_offsets[0], &request->path_bytes)) return 0;

    request->argv_count = (u16)options->argv_count;
    for (u64 i = 0; i < options->argv_count; i++) {
        if (!append_arg(request, &cursor, options->argv[i], &request->argv_offsets[i], &request->argv_bytes[i])) return 0;
    }

    request->envp_count = (u16)options->envp_count;
    for (u64 i = 0; i < options->envp_count; i++) {
        if (!options->envp || !append_arg(request, &cursor, options->envp[i], &request->envp_offsets[i], &request->envp_bytes[i])) return 0;
    }

    request->arg_data_bytes = cursor;
    return 1;
}

int exec_service_spawn_linux(const struct exec_service_spawn_options *options,
                             struct exec_service_spawn_result *result) {
    if (!options || !result) return 0;

    result->status = EXEC_SERVICE_STATUS_INVALID;
    result->linux_abi_process_slot = 0;
    result->exec_process_slot = 0;

    const u64 request_paddr = syscall0(SYSCALL_ALLOC_PAGE);
    const u64 response_paddr = syscall0(SYSCALL_ALLOC_PAGE);
    if (request_paddr < PAGE_BYTES || response_paddr < PAGE_BYTES) return 0;
    const u64 request_va = syscall2(SYSCALL_MAP_PAGE_ANYWHERE, request_paddr, 1);
    const u64 response_va = syscall2(SYSCALL_MAP_PAGE_ANYWHERE, response_paddr, 1);
    if (request_va < PAGE_BYTES || response_va < PAGE_BYTES) return 0;
    const u64 owner_rights = IPC_BUFFER_RIGHT_READ | IPC_BUFFER_RIGHT_WRITE | IPC_BUFFER_RIGHT_MAP | IPC_BUFFER_RIGHT_GRANT;
    const u64 request_token = syscall3(SYSCALL_CREATE_IPC_BUFFER_FROM_PAGE, request_paddr, owner_rights, IPC_BUFFER_ROLE_REQUEST);
    const u64 response_token = syscall3(SYSCALL_CREATE_IPC_BUFFER_FROM_PAGE, response_paddr, owner_rights, IPC_BUFFER_ROLE_RESPONSE);
    if (!is_ipc_buffer_token(request_token) || !is_ipc_buffer_token(response_token)) return 0;
    const u64 remote_response_token = syscall3(
        SYSCALL_GRANT_IPC_BUFFER_ON_ENDPOINT,
        response_token,
        EXEC_SERVICE_ENDPOINT_ID,
        IPC_BUFFER_RIGHT_READ | IPC_BUFFER_RIGHT_WRITE | IPC_BUFFER_RIGHT_MAP
    );
    if (!is_ipc_buffer_token(remote_response_token)) return 0;

    struct exec_service_request *request = (struct exec_service_request *)request_va;
    volatile struct exec_service_response *response = (volatile struct exec_service_response *)response_va;
    clear_page(response_va);
    if (!fill_request(request, options, remote_response_token)) return 0;

    u64 attempts = 0;
    const u64 wait_ticks = options->wait_ticks ? options->wait_ticks : 20000;
    while (1) {
        if (syscall3(
                SYSCALL_SHARE_IPC_BUFFER_ON_ENDPOINT,
                request_token,
                EXEC_SERVICE_ENDPOINT_ID,
                IPC_BUFFER_RIGHT_READ | IPC_BUFFER_RIGHT_MAP
            ) == SYSCALL_OK) {
            break;
        }
        if (attempts++ >= wait_ticks) return 0;
        (void)syscall2(SYSCALL_WAIT_EVENT, 0, 1);
    }

    attempts = 0;
    while (1) {
        if (response->magic == EXEC_SERVICE_ABI_MAGIC &&
            response->version == EXEC_SERVICE_ABI_VERSION &&
            response->op == EXEC_SERVICE_OP_SPAWN_LINUX) {
            result->status = response->status;
            result->linux_abi_process_slot = response->linux_abi_process_slot;
            result->exec_process_slot = response->exec_process_slot;
            return result->status == EXEC_SERVICE_STATUS_OK;
        }
        if (attempts++ >= wait_ticks * 512ULL) return 0;
        (void)syscall2(SYSCALL_WAIT_EVENT, 0, 1);
    }
}
