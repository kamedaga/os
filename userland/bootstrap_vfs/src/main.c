#include "fs_protocol.h"

typedef unsigned long long u64;
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;
typedef int i32;

enum {
    SYSCALL_ALLOC_PAGE = 0x1,
    SYSCALL_MAP_PAGE = 0x2,
    SYSCALL_LOG = 0x9,
    SYSCALL_ALLOC_MAP_PAGES = 0xC,
    SYSCALL_WAIT_EVENT = 0x17,
    SYSCALL_SPAWN_EXEC = 0x1D,
    SYSCALL_INSTALL_VM_OBJECT = 0x1E,
    SYSCALL_INSTALL_EXEC_IMAGE = 0x20,
    SYSCALL_GRANT_CAP_ON_ENDPOINT = 0x24,
    SYSCALL_INSTALL_ENDPOINT = 0x26,
    SYSCALL_SHARE_CAP = 0x2B,
    SYSCALL_SIGNAL_ENDPOINT = 0x2C,
    SYSCALL_GET_PROCESS_SLOT = 0x2E,
    SYSCALL_OK = 0,
    SYSCALL_ERR_ENDPOINT = 9,
    PAGE_RIGHT_CPU_READ = 0x1,
    PAGE_RIGHT_CPU_WRITE = 0x2,
    BOOTSTRAP_VFS_CONFIG_VA = 0x3C002000,
    REQUEST_VA = 0x28000000,
    RESPONSE_VA = 0x28001000,
    ROOT_SEED2_IMAGE_VA = 0x28100000,
    REPLY_ENDPOINT_ID = 0xEA,
    VM_OBJECT_TOKEN_TAG = 1ULL << 62,
    EXEC_IMAGE_TOKEN_TAG = (1ULL << 62) | (1ULL << 61),
    SPAWN_RESULT_TAG = 1ULL << 63,
    SPAWN_RESULT_PROCESS_MASK = 0xFFFFFFFFULL,
    MAX_ROOT_SEED2_PAGES = 256,
};

struct backend_session {
    u8 active;
    u8 reserved0[7];
    u64 endpoint_id;
    u64 process_slot;
    u64 request_paddr;
    u64 response_paddr;
    u64 session_nonce;
    u64 root_token;
    u64 next_seq;
};

static struct backend_session g_fat;
static u64 g_image_page_paddrs[MAX_ROOT_SEED2_PAGES];

static u64 cstr_len(const char *s) {
    u64 n = 0;
    while (s[n] != 0) n++;
    return n;
}

static void user_log_len(const char *message, u64 len) {
    u64 ret;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"((u64)SYSCALL_LOG), "D"((u64)message), "S"(len)
        : "rcx", "rdx", "r8", "r9", "r10", "r11", "memory");
    (void)ret;
}

static void user_log(const char *message) {
    user_log_len(message, cstr_len(message));
}

static u64 syscall0(u64 nr) {
    u64 ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(nr) : "rcx", "rdx", "r8", "r9", "r10", "r11", "memory");
    return ret;
}

static u64 syscall2(u64 nr, u64 a0, u64 a1) {
    u64 ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(nr), "D"(a0), "S"(a1) : "rcx", "rdx", "r8", "r9", "r10", "r11", "memory");
    return ret;
}

static u64 syscall3(u64 nr, u64 a0, u64 a1, u64 a2) {
    u64 ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(nr), "D"(a0), "S"(a1), "d"(a2) : "rcx", "r8", "r9", "r10", "r11", "memory");
    return ret;
}

static u64 syscall4(u64 nr, u64 a0, u64 a1, u64 a2, u64 a3) {
    u64 ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(nr), "D"(a0), "S"(a1), "d"(a2), "c"(a3) : "r8", "r9", "r10", "r11", "memory");
    return ret;
}

static u64 wait_event_poll(void) {
    return syscall2(SYSCALL_WAIT_EVENT, 0, 1);
}

static u64 wait_event(void) {
    return syscall2(SYSCALL_WAIT_EVENT, 1, 1);
}

static void clear_page(u64 va) {
    volatile u64 *p = (volatile u64 *)va;
    for (u64 i = 0; i < 512; i++) p[i] = 0;
}

static int install_fat_endpoint(void) {
    if (g_fat.endpoint_id == 0 || g_fat.process_slot == 0) return 0;
    return syscall3(SYSCALL_INSTALL_ENDPOINT, 0, g_fat.endpoint_id, g_fat.process_slot) == SYSCALL_OK;
}

static int grant_response_page(void) {
    u64 ret = syscall3(SYSCALL_GRANT_CAP_ON_ENDPOINT, g_fat.response_paddr, g_fat.endpoint_id, PAGE_RIGHT_CPU_READ | PAGE_RIGHT_CPU_WRITE);
    if (ret == SYSCALL_OK) return 1;
    if (ret == SYSCALL_ERR_ENDPOINT && install_fat_endpoint()) {
        ret = syscall3(SYSCALL_GRANT_CAP_ON_ENDPOINT, g_fat.response_paddr, g_fat.endpoint_id, PAGE_RIGHT_CPU_READ | PAGE_RIGHT_CPU_WRITE);
    }
    return ret == SYSCALL_OK;
}

static int share_request_page(void) {
    u64 ret = syscall2(SYSCALL_SHARE_CAP, g_fat.request_paddr, g_fat.endpoint_id);
    if (ret == SYSCALL_OK) return 1;
    if (ret == SYSCALL_ERR_ENDPOINT && install_fat_endpoint()) ret = syscall2(SYSCALL_SHARE_CAP, g_fat.request_paddr, g_fat.endpoint_id);
    return ret == SYSCALL_OK;
}

static int signal_fat(void) {
    u64 ret = syscall2(SYSCALL_SIGNAL_ENDPOINT, g_fat.endpoint_id, 0);
    if (ret == SYSCALL_OK) return 1;
    if (ret == SYSCALL_ERR_ENDPOINT && install_fat_endpoint()) ret = syscall2(SYSCALL_SIGNAL_ENDPOINT, g_fat.endpoint_id, 0);
    return ret == SYSCALL_OK;
}

static int wait_response(u64 expected_seq, u16 expected_op) {
    volatile struct fs_response_header *response = (volatile struct fs_response_header *)RESPONSE_VA;
    for (u64 i = 0; i < 4096; i++) {
        if (response->response_seq == expected_seq) {
            return response->magic == FS_RESPONSE_MAGIC && response->version == FS_PROTOCOL_VERSION && response->op == expected_op;
        }
        (void)wait_event_poll();
    }
    return 0;
}

static u64 make_nonce(u64 request_paddr, u64 response_paddr, u64 endpoint_id, u64 process_slot) {
    u64 nonce = request_paddr ^ ((response_paddr << 17) | (response_paddr >> 47)) ^ ((endpoint_id << 7) | (endpoint_id >> 57)) ^ process_slot ^ 0x5eed2002b0075ULL;
    return nonce == 0 ? 1 : nonce;
}

static int connect_fat(u64 endpoint_id, u64 process_slot) {
    g_fat.endpoint_id = endpoint_id;
    g_fat.process_slot = process_slot;
    if (endpoint_id == 0 || process_slot == 0) return 0;
    g_fat.request_paddr = syscall0(SYSCALL_ALLOC_PAGE);
    g_fat.response_paddr = syscall0(SYSCALL_ALLOC_PAGE);
    if (g_fat.request_paddr < 0x1000 || g_fat.response_paddr < 0x1000) return 0;
    if (syscall3(SYSCALL_MAP_PAGE, REQUEST_VA, g_fat.request_paddr, 1) != SYSCALL_OK) return 0;
    if (syscall3(SYSCALL_MAP_PAGE, RESPONSE_VA, g_fat.response_paddr, 1) != SYSCALL_OK) return 0;
    if (!grant_response_page()) return 0;

    clear_page(REQUEST_VA);
    clear_page(RESPONSE_VA);
    const u64 self_slot = syscall0(SYSCALL_GET_PROCESS_SLOT);
    g_fat.session_nonce = make_nonce(g_fat.request_paddr, g_fat.response_paddr, endpoint_id, self_slot);

    volatile struct fs_request_header *request = (volatile struct fs_request_header *)REQUEST_VA;
    request->magic = FS_REQUEST_MAGIC;
    request->version = FS_PROTOCOL_VERSION;
    request->op = FS_OP_CONNECT;
    request->arg0 = g_fat.response_paddr;
    request->arg1 = self_slot;
    request->session_nonce = g_fat.session_nonce;
    __asm__ volatile("" ::: "memory");
    request->request_seq = 1;

    if (!share_request_page()) return 0;
    if (!wait_response(1, FS_OP_CONNECT)) return 0;
    volatile struct fs_response_header *response = (volatile struct fs_response_header *)RESPONSE_VA;
    if (response->status != FS_STATUS_OK || response->result_token == 0) return 0;
    g_fat.root_token = response->result_token;
    g_fat.next_seq = 2;
    g_fat.active = 1;
    return 1;
}

static int fs_request(u16 op, u64 token, u64 offset, u32 length, const char *path) {
    const u16 path_len = path ? (u16)cstr_len(path) : 0;
    const u64 seq = g_fat.next_seq++;
    clear_page(REQUEST_VA);
    clear_page(RESPONSE_VA);
    volatile struct fs_request_header *request = (volatile struct fs_request_header *)REQUEST_VA;
    request->magic = FS_REQUEST_MAGIC;
    request->version = FS_PROTOCOL_VERSION;
    request->op = op;
    request->object_token = token;
    request->offset = offset;
    request->length = length;
    request->flags = 0;
    request->path_bytes = path_len;
    request->inline_bytes = 0;
    request->arg0 = 0;
    request->arg1 = 0;
    request->session_nonce = g_fat.session_nonce;
    volatile u8 *payload = (volatile u8 *)(REQUEST_VA + FS_REQUEST_HEADER_BYTES);
    for (u16 i = 0; i < path_len; i++) payload[i] = (u8)path[i];
    __asm__ volatile("" ::: "memory");
    request->request_seq = seq;
    if (!signal_fat()) return 0;
    return wait_response(seq, op);
}

static int alloc_image_pages(u64 file_bytes) {
    const u64 pages = (file_bytes + 4095) / 4096;
    if (pages == 0 || pages > MAX_ROOT_SEED2_PAGES) return 0;
    for (u64 i = 0; i < pages; i++) {
        const u64 va = ROOT_SEED2_IMAGE_VA + i * 4096;
        g_image_page_paddrs[i] = 0;
        if (syscall4(SYSCALL_ALLOC_MAP_PAGES, va, 1, 1, (u64)&g_image_page_paddrs[i]) != SYSCALL_OK) return 0;
        clear_page(va);
    }
    return 1;
}

static int load_root_seed2(u64 *exec_token_out) {
    if (!fs_request(FS_OP_LOOKUP, g_fat.root_token, 0, 0, "/sbin/seed2.elf")) return 0;
    volatile struct fs_response_header *response = (volatile struct fs_response_header *)RESPONSE_VA;
    if (response->status != FS_STATUS_OK || response->result_token == 0) return 0;
    const u64 file_token = response->result_token;

    if (!fs_request(FS_OP_OPEN_EXEC, file_token, 0, 0, 0)) return 0;
    response = (volatile struct fs_response_header *)RESPONSE_VA;
    if (response->status != FS_STATUS_OK || response->result_token == 0 || response->file_bytes == 0) return 0;
    const u64 open_token = response->result_token;
    const u64 file_bytes = response->file_bytes;
    if (!alloc_image_pages(file_bytes)) return 0;

    u64 offset = 0;
    while (offset < file_bytes) {
        u32 request_len = FS_RESPONSE_PAYLOAD_BYTES;
        if ((u64)request_len > file_bytes - offset) request_len = (u32)(file_bytes - offset);
        if (!fs_request(FS_OP_READ, open_token, offset, request_len, 0)) return 0;
        response = (volatile struct fs_response_header *)RESPONSE_VA;
        if (response->status != FS_STATUS_OK || response->inline_bytes == 0) return 0;
        volatile u8 *dst = (volatile u8 *)(ROOT_SEED2_IMAGE_VA + offset);
        volatile u8 *src = (volatile u8 *)(RESPONSE_VA + FS_RESPONSE_HEADER_BYTES);
        for (u16 i = 0; i < response->inline_bytes; i++) dst[i] = src[i];
        offset += response->inline_bytes;
    }

    const u64 vm_token = syscall3(SYSCALL_INSTALL_VM_OBJECT, ROOT_SEED2_IMAGE_VA, file_bytes, 1);
    if ((vm_token & VM_OBJECT_TOKEN_TAG) != VM_OBJECT_TOKEN_TAG) return 0;
    const u64 exec_token = syscall2(SYSCALL_INSTALL_EXEC_IMAGE, vm_token, 1);
    if ((exec_token & EXEC_IMAGE_TOKEN_TAG) != EXEC_IMAGE_TOKEN_TAG) return 0;
    *exec_token_out = exec_token;
    return 1;
}

static u64 decode_spawn_process_slot(u64 value) {
    if ((value & SPAWN_RESULT_TAG) == 0) return 0;
    return value & SPAWN_RESULT_PROCESS_MASK;
}

static void spawn_root_seed2(void) {
    u64 exec_token = 0;
    if (!load_root_seed2(&exec_token)) {
        user_log("[bootstrap_vfs] root seed2 load failed\n");
        return;
    }
    user_log("[bootstrap_vfs] root seed2 exec ready\n");
    const u64 spawned = syscall4(SYSCALL_SPAWN_EXEC, exec_token, 0, 0, 0);
    const u64 child_slot = decode_spawn_process_slot(spawned);
    if (child_slot == 0) {
        user_log("[bootstrap_vfs] root seed2 spawn failed\n");
        return;
    }
    user_log("[bootstrap_vfs] root seed2 spawned from rootfs\n");
}

void bootstrap_vfs_main(void) {
    user_log("[bootstrap_vfs] started\n");
    volatile u64 *config = (volatile u64 *)BOOTSTRAP_VFS_CONFIG_VA;
    const u64 fat_endpoint_id = config[3];
    const u64 fat_process_slot = config[4];
    if (connect_fat(fat_endpoint_id, fat_process_slot)) {
        user_log("[bootstrap_vfs] fat connect ok\n");
        spawn_root_seed2();
    } else {
        user_log("[bootstrap_vfs] fat connect failed\n");
    }
    config[2] = 1;

    for (;;) {
        (void)wait_event();
    }
}