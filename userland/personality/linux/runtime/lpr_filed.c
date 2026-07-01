#include "lpr_filed.h"
#include "support/string.h"
#include "support/syscall.h"
#include <filed/ipc_protocol.h>
#include <pacha/ipc.h>
#include <pachaos/abi.h>
#include <personality/linux_lpr.h>
#include <stddef.h>
#include <stdint.h>

__attribute__((visibility("hidden")))
void *memset(void *dst, int c, size_t n)
{
    return lpr_memset(dst, c, n);
}

#define LPR_FD_TABLE_SIZE 128u
#define LPR_LINUX_AT_FDCWD (-100)
#define LPR_LINUX_AT_SYMLINK_NOFOLLOW 0x100ull
#define LPR_LINUX_AT_EMPTY_PATH 0x1000ull
#define LPR_LINUX_AT_REMOVEDIR 0x200ull
#define LPR_LINUX_O_ACCMODE 00000003ull
#define LPR_LINUX_O_RDONLY 00000000ull
#define LPR_LINUX_O_WRONLY 00000001ull
#define LPR_LINUX_O_RDWR 00000002ull
#define LPR_LINUX_O_CREAT 00000100ull
#define LPR_LINUX_O_EXCL 00000200ull
#define LPR_LINUX_O_TRUNC 00001000ull
#define LPR_LINUX_O_APPEND 00002000ull
#define LPR_LINUX_O_NONBLOCK 00004000ull
#define LPR_LINUX_O_DIRECTORY 00200000ull
#define LPR_LINUX_O_NOFOLLOW 00400000ull
#define LPR_LINUX_O_CLOEXEC 02000000ull
#define LPR_LINUX_F_DUPFD 0ull
#define LPR_LINUX_F_GETFD 1ull
#define LPR_LINUX_F_SETFD 2ull
#define LPR_LINUX_F_GETFL 3ull
#define LPR_LINUX_F_SETFL 4ull
#define LPR_LINUX_F_DUPFD_CLOEXEC 1030ull
#define LPR_LINUX_FD_CLOEXEC 1ull
#define LPR_LINUX_TCGETS 0x5401ull
#define LPR_LINUX_TIOCGWINSZ 0x5413ull
#define LPR_LINUX_UTIME_NOW 1073741823ll
#define LPR_LINUX_UTIME_OMIT 1073741822ll

#define LPR_LINUX_S_IFMT 0170000ull
#define LPR_LINUX_S_IFIFO 0010000ull
#define LPR_LINUX_S_IFCHR 0020000ull
#define LPR_LINUX_S_IFDIR 0040000ull
#define LPR_LINUX_S_IFBLK 0060000ull
#define LPR_LINUX_S_IFREG 0100000ull
#define LPR_LINUX_S_IFLNK 0120000ull
#define LPR_LINUX_S_IFSOCK 0140000ull

#define LPR_LINUX_DT_UNKNOWN 0u
#define LPR_LINUX_DT_FIFO 1u
#define LPR_LINUX_DT_CHR 2u
#define LPR_LINUX_DT_DIR 4u
#define LPR_LINUX_DT_BLK 6u
#define LPR_LINUX_DT_REG 8u
#define LPR_LINUX_DT_LNK 10u
#define LPR_LINUX_DT_SOCK 12u

typedef struct lpr_filed_fd {
    uint8_t active;
    uint8_t reserved0;
    uint16_t reserved1;
    uint32_t flags;
    uint64_t handle;
} lpr_filed_fd_t;

typedef struct lpr_linux_stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t __pad0;
    uint64_t st_rdev;
    int64_t st_size;
    int64_t st_blksize;
    int64_t st_blocks;
    int64_t st_atime_sec;
    int64_t st_atime_nsec;
    int64_t st_mtime_sec;
    int64_t st_mtime_nsec;
    int64_t st_ctime_sec;
    int64_t st_ctime_nsec;
    int64_t __unused[3];
} lpr_linux_stat_t;

typedef struct lpr_linux_iovec {
    uint64_t base;
    uint64_t len;
} lpr_linux_iovec_t;

typedef struct lpr_linux_timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
} lpr_linux_timespec_t;

static lpr_filed_fd_t lpr_fds[LPR_FD_TABLE_SIZE];
static uint64_t lpr_request_id = 0x4c505246494c4501ull;
static int lpr_filed_endpoint_checked;
static int lpr_wire_page_fd = -1;
static void *lpr_wire_page;
static int lpr_wire_page_busy;
static int lpr_session_fd = -1;
static int lpr_session_page_fd = -1;
static void *lpr_session_page;
static int lpr_session_checked;
static int lpr_session_payload_busy;

static void *lpr_session_payload_slot(uint64_t slot)
{
    if (lpr_session_page == 0 || slot >= FILED_WIRE_FAST_PAYLOAD_SLOT_COUNT) {
        return 0;
    }
    return (void *)((uintptr_t)lpr_session_page +
        FILED_WIRE_FAST_PAYLOAD_OFFSET +
        slot * FILED_WIRE_PAGE_BYTES);
}

static void lpr_zero_bytes(void *ptr, uint64_t len)
{
    unsigned char *p = (unsigned char *)ptr;
    while (len != 0) {
        *p++ = 0;
        len--;
    }
}

static int64_t lpr_pacha_status_to_errno(int64_t status)
{
    if (status == 0) {
        return 0;
    }
    int negative = 0;
    if (status < 0) {
        negative = 1;
        status = -status;
    }
    if (status > PACHAOS_SYSCALL_ERR_EMPTY) {
        return negative ? -status : status;
    }
    switch (status) {
    case PACHAOS_SYSCALL_ERR_INVALID:
        return -LPR_LINUX_EINVAL;
    case PACHAOS_SYSCALL_ERR_ALLOC:
    case PACHAOS_SYSCALL_ERR_MAP:
        return -LPR_LINUX_ENOMEM;
    case PACHAOS_SYSCALL_ERR_NOT_READY:
    case PACHAOS_SYSCALL_ERR_EMPTY:
        return -LPR_LINUX_EAGAIN;
    default:
        return -LPR_LINUX_EINVAL;
    }
}

static int lpr_fd_is_filed(uint64_t fd)
{
    return fd < LPR_FD_TABLE_SIZE && lpr_fds[fd].active != 0;
}

int lpr_linux_filed_fd_active(uint64_t fd)
{
    return lpr_fd_is_filed(fd);
}

static int lpr_fd_alloc(uint64_t handle, uint64_t flags)
{
    for (uint64_t fd = 3; fd < LPR_FD_TABLE_SIZE; fd += 1) {
        if (fd == LPR_FILED_ENDPOINT_FD) {
            continue;
        }
        if (lpr_fds[fd].active == 0) {
            lpr_fds[fd].active = 1;
            lpr_fds[fd].flags = (uint32_t)flags;
            lpr_fds[fd].handle = handle;
            return (int)fd;
        }
    }
    return -LPR_LINUX_ENOMEM;
}

static int64_t lpr_filed_endpoint_ready(void)
{
    if (lpr_filed_endpoint_checked > 0) {
        return 0;
    }
    if (lpr_filed_endpoint_checked < 0) {
        return -LPR_LINUX_ENOSYS;
    }
    struct pacha_fd_info info;
    lpr_memset(&info, 0, sizeof(info));
    const int64_t status = lpr_pacha_syscall2(
        PACHAOS_SYSCALL_FD_GET_INFO,
        LPR_FILED_ENDPOINT_FD,
        (uint64_t)(uintptr_t)&info);
    if (status != 0 ||
        (info.kind != PACHA_FD_KIND_ENDPOINT && info.kind != PACHA_FD_KIND_CHANNEL))
    {
        lpr_filed_endpoint_checked = -1;
        return -LPR_LINUX_ENOSYS;
    }
    lpr_filed_endpoint_checked = 1;
    return 0;
}

static int64_t lpr_filed_session_connect(void)
{
    if (lpr_session_fd >= 16 &&
        lpr_session_page_fd >= 16 &&
        lpr_session_page != 0)
    {
        return 0;
    }
    if (lpr_session_checked < 0) {
        return -LPR_LINUX_ENOSYS;
    }
    const int64_t ready = lpr_filed_endpoint_ready();
    if (ready != 0) {
        lpr_session_checked = -1;
        return ready;
    }

    const uint64_t channel_rights =
        PACHA_FD_RIGHT_INSPECT |
        PACHA_FD_RIGHT_WAIT |
        PACHA_FD_RIGHT_POLL |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_SEND |
        PACHA_FD_RIGHT_RECV |
        PACHA_FD_RIGHT_TRANSFER;
    const uint64_t page_rights =
        PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE;
    uint64_t pair[2] = {0, 0};
    int64_t status = lpr_pacha_syscall3(
        PACHAOS_SYSCALL_IPC_CHANNEL_CREATE,
        (uint64_t)(uintptr_t)pair,
        channel_rights,
        PACHA_FD_FLAG_CLOEXEC);
    if (status != 0 || pair[0] < 16 || pair[1] < 16) {
        if (pair[0] >= 16) {
            (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, pair[0]);
        }
        if (pair[1] >= 16) {
            (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, pair[1]);
        }
        lpr_session_checked = -1;
        return status != 0 ? lpr_pacha_status_to_errno(status) : -LPR_LINUX_EINVAL;
    }

    const int64_t page_fd = lpr_pacha_syscall3(
        PACHAOS_SYSCALL_VMO_CREATE,
        FILED_WIRE_SESSION_PAGE_BYTES,
        page_rights,
        0);
    if (page_fd < 16) {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, pair[0]);
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, pair[1]);
        lpr_session_checked = -1;
        return lpr_pacha_status_to_errno(page_fd);
    }

    const int64_t mapped = lpr_pacha_syscall6(
        PACHAOS_SYSCALL_MMAP,
        (uint64_t)(uint32_t)page_fd,
        0,
        FILED_WIRE_SESSION_PAGE_BYTES,
        PACHAOS_PROT_READ | PACHAOS_PROT_WRITE,
        PACHAOS_MMAP_SHARED,
        0);
    if (mapped < 4096) {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)page_fd);
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, pair[0]);
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, pair[1]);
        lpr_session_checked = -1;
        return lpr_pacha_status_to_errno(mapped);
    }

    void *page = (void *)(uintptr_t)mapped;
    lpr_zero_bytes(page, FILED_WIRE_SESSION_PAGE_BYTES);
    filed_wire_fast_header_t *header = (filed_wire_fast_header_t *)page;
    header->magic = FILED_WIRE_FAST_MAGIC;
    header->version = FILED_WIRE_FAST_VERSION;
    header->request_capacity = FILED_WIRE_FAST_REQUEST_CAPACITY;
    header->completion_capacity = FILED_WIRE_FAST_COMPLETION_CAPACITY;
    header->payload_slot_count = FILED_WIRE_FAST_PAYLOAD_SLOT_COUNT;
    header->payload_slot_size = FILED_WIRE_PAGE_BYTES;
    header->payload_offset = FILED_WIRE_FAST_PAYLOAD_OFFSET;
    header->generation_offset = FILED_WIRE_FAST_GENERATION_OFFSET;
    header->generation_capacity = FILED_WIRE_FAST_GENERATION_CAPACITY;

    struct pacha_ipc_fd fds[2];
    struct pacha_ipc_msg request;
    struct pacha_ipc_msg reply;
    lpr_zero_bytes(fds, sizeof(fds));
    lpr_zero_bytes(&request, sizeof(request));
    lpr_zero_bytes(&reply, sizeof(reply));
    fds[0].fd = pair[1];
    fds[0].rights =
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_WAIT |
        PACHA_FD_RIGHT_POLL |
        PACHA_FD_RIGHT_SEND |
        PACHA_FD_RIGHT_RECV;
    fds[1].fd = (uint64_t)(uint32_t)page_fd;
    fds[1].rights = page_rights;
    request.word0 = FILED_WIRE_REQUEST_MAGIC;
    request.word1 = FILED_WIRE_OP_CONNECT;
    request.word3 = ++lpr_request_id;
    request.fds = fds;
    request.fd_count = 2;
    const int64_t reply_fd = lpr_pacha_syscall2(
        PACHAOS_SYSCALL_IPC_CALL,
        LPR_FILED_ENDPOINT_FD,
        (uint64_t)(uintptr_t)&request);
    (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, pair[1]);
    if (reply_fd < 16) {
        (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP, (uint64_t)(uintptr_t)page, FILED_WIRE_SESSION_PAGE_BYTES);
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)page_fd);
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, pair[0]);
        lpr_session_checked = -1;
        return lpr_pacha_status_to_errno(reply_fd);
    }
    status = lpr_pacha_syscall4(
        PACHAOS_SYSCALL_IPC_RECV_WAIT,
        (uint64_t)(uint32_t)reply_fd,
        (uint64_t)(uintptr_t)&reply,
        UINT64_MAX,
        0);
    (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)reply_fd);
    if (status != 0 ||
        reply.word0 != FILED_WIRE_REPLY_MAGIC ||
        reply.word1 != 0 ||
        reply.word3 != request.word3)
    {
        (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP, (uint64_t)(uintptr_t)page, FILED_WIRE_SESSION_PAGE_BYTES);
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)page_fd);
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, pair[0]);
        lpr_session_checked = -1;
        return status != 0 ? lpr_pacha_status_to_errno(status) : -LPR_LINUX_EIO;
    }

    lpr_session_fd = (int)(uint32_t)pair[0];
    lpr_session_page_fd = (int)(uint32_t)page_fd;
    lpr_session_page = page;
    lpr_session_checked = 1;
    return 0;
}

static int64_t lpr_filed_fast_call(uint64_t op, uint64_t word2, uint64_t *out_result)
{
    if (lpr_filed_session_connect() != 0 ||
        lpr_session_fd < 16 ||
        lpr_session_page == 0)
    {
        return -LPR_LINUX_ENOSYS;
    }
    filed_wire_fast_header_t *header = (filed_wire_fast_header_t *)lpr_session_page;
    if (header->magic != FILED_WIRE_FAST_MAGIC ||
        header->version != FILED_WIRE_FAST_VERSION ||
        header->request_capacity != FILED_WIRE_FAST_REQUEST_CAPACITY ||
        header->completion_capacity != FILED_WIRE_FAST_COMPLETION_CAPACITY ||
        header->payload_offset != FILED_WIRE_FAST_PAYLOAD_OFFSET ||
        header->generation_offset != FILED_WIRE_FAST_GENERATION_OFFSET ||
        header->generation_capacity != FILED_WIRE_FAST_GENERATION_CAPACITY)
    {
        return -LPR_LINUX_EIO;
    }
    if (header->request_tail - header->request_head >= header->request_capacity) {
        return -LPR_LINUX_EAGAIN;
    }

    filed_wire_fast_request_t *requests =
        (filed_wire_fast_request_t *)((uintptr_t)lpr_session_page + sizeof(*header));
    filed_wire_fast_completion_t *completions =
        (filed_wire_fast_completion_t *)((uintptr_t)requests +
            sizeof(*requests) * FILED_WIRE_FAST_REQUEST_CAPACITY);
    const uint64_t request_id = ++lpr_request_id;
    const uint64_t tail = header->request_tail;
    filed_wire_fast_request_t *fast_request =
        &requests[tail % header->request_capacity];
    lpr_zero_bytes(fast_request, sizeof(*fast_request));
    fast_request->request_id = request_id;
    fast_request->opcode = op;
    fast_request->word2 = word2;
    fast_request->payload_slot = 0;
    fast_request->payload_length = FILED_WIRE_PAGE_BYTES;
    __sync_synchronize();
    header->request_tail = tail + 1u;

    struct pacha_ipc_msg request;
    struct pacha_ipc_msg reply;
    lpr_zero_bytes(&request, sizeof(request));
    lpr_zero_bytes(&reply, sizeof(reply));
    request.word0 = FILED_WIRE_REQUEST_MAGIC;
    request.word1 = FILED_WIRE_OP_FAST_DOORBELL;
    request.word2 = ++header->doorbell_seq;
    request.word3 = request_id;
    int64_t status = lpr_pacha_syscall2(
        PACHAOS_SYSCALL_IPC_SEND,
        (uint64_t)(uint32_t)lpr_session_fd,
        (uint64_t)(uintptr_t)&request);
    if (status != 0) {
        return lpr_pacha_status_to_errno(status);
    }
    status = lpr_pacha_syscall4(
        PACHAOS_SYSCALL_IPC_RECV_WAIT,
        (uint64_t)(uint32_t)lpr_session_fd,
        (uint64_t)(uintptr_t)&reply,
        UINT64_MAX,
        0);
    if (status != 0) {
        return lpr_pacha_status_to_errno(status);
    }
    if (reply.word0 != FILED_WIRE_REPLY_MAGIC ||
        reply.word1 != 0 ||
        reply.word3 != request_id)
    {
        return -LPR_LINUX_EIO;
    }
    if (header->completion_head == header->completion_tail) {
        return -LPR_LINUX_EIO;
    }
    __sync_synchronize();
    filed_wire_fast_completion_t *completion =
        &completions[header->completion_head % header->completion_capacity];
    if (completion->request_id != request_id) {
        return -LPR_LINUX_EIO;
    }
    header->completion_head++;
    if (completion->status < 0) {
        return completion->status;
    }
    if (out_result != 0) {
        *out_result = completion->result;
    }
    return 0;
}

static int lpr_create_wire_page(void **out_page)
{
    if (out_page == 0) {
        return -LPR_LINUX_EINVAL;
    }
    if (lpr_filed_session_connect() == 0 &&
        lpr_session_page_fd >= 16 &&
        lpr_session_page != 0 &&
        !lpr_session_payload_busy)
    {
        void *payload = lpr_session_payload_slot(0);
        if (payload != 0) {
            lpr_session_payload_busy = 1;
            *out_page = payload;
            return lpr_session_page_fd;
        }
    }
    if (lpr_wire_page_fd >= 16 && lpr_wire_page != 0 && !lpr_wire_page_busy) {
        lpr_wire_page_busy = 1;
        *out_page = lpr_wire_page;
        return lpr_wire_page_fd;
    }
    const uint64_t rights =
        PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE;
    const int64_t fd = lpr_pacha_syscall3(
        PACHAOS_SYSCALL_VMO_CREATE,
        FILED_WIRE_PAGE_BYTES,
        rights,
        0);
    if (fd < 16) {
        return (int)lpr_pacha_status_to_errno(fd);
    }
    const int64_t mapped = lpr_pacha_syscall6(
        PACHAOS_SYSCALL_MMAP,
        (uint64_t)(uint32_t)fd,
        0,
        FILED_WIRE_PAGE_BYTES,
        PACHAOS_PROT_READ | PACHAOS_PROT_WRITE,
        PACHAOS_MMAP_SHARED,
        0);
    if (mapped < 4096) {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)fd);
        return (int)lpr_pacha_status_to_errno(mapped);
    }
    *out_page = (void *)(uintptr_t)mapped;
    if (!lpr_wire_page_busy && lpr_wire_page_fd < 16 && lpr_wire_page == 0) {
        lpr_wire_page_fd = (int)fd;
        lpr_wire_page = *out_page;
        lpr_wire_page_busy = 1;
    }
    return (int)fd;
}

static void lpr_destroy_wire_page(int fd, void *page)
{
    if (fd == lpr_session_page_fd && page == lpr_session_payload_slot(0)) {
        lpr_session_payload_busy = 0;
        return;
    }
    if (fd == lpr_wire_page_fd && page == lpr_wire_page) {
        lpr_wire_page_busy = 0;
        return;
    }
    if (page != 0) {
        (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP, (uint64_t)(uintptr_t)page, FILED_WIRE_PAGE_BYTES);
    }
    if (fd >= 16) {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)fd);
    }
}

static int64_t lpr_filed_call(uint64_t op, int page_fd, uint64_t word2, uint64_t *out_result)
{
    if (page_fd == lpr_session_page_fd && lpr_session_page != 0) {
        return lpr_filed_fast_call(op, word2, out_result);
    } else if (page_fd < 16 &&
        lpr_session_page != 0 &&
        !lpr_session_payload_busy)
    {
        const int64_t fast_status = lpr_filed_fast_call(op, word2, out_result);
        if (fast_status != -LPR_LINUX_ENOSYS && fast_status != -LPR_LINUX_EAGAIN) {
            return fast_status;
        }
    }

    const int64_t ready = lpr_filed_endpoint_ready();
    if (ready != 0) {
        return ready;
    }

    struct pacha_ipc_fd fd_item;
    lpr_memset(&fd_item, 0, sizeof(fd_item));
    if (page_fd >= 16) {
        fd_item.fd = (uint64_t)(uint32_t)page_fd;
        fd_item.rights =
            PACHA_FD_RIGHT_CLOSE |
            PACHA_FD_RIGHT_MAP_READ |
            PACHA_FD_RIGHT_MAP_WRITE;
    }

    const uint64_t request_id = ++lpr_request_id;
    const struct pacha_ipc_msg request = {
        .word0 = FILED_WIRE_REQUEST_MAGIC,
        .word1 = op,
        .word2 = word2,
        .word3 = request_id,
        .fds = page_fd >= 16 ? &fd_item : 0,
        .fd_count = page_fd >= 16 ? 1u : 0u,
    };
    const int64_t reply_fd = lpr_pacha_syscall2(
        PACHAOS_SYSCALL_IPC_CALL,
        LPR_FILED_ENDPOINT_FD,
        (uint64_t)(uintptr_t)&request);
    if (reply_fd < 16) {
        return lpr_pacha_status_to_errno(reply_fd);
    }

    struct pacha_ipc_msg reply;
    lpr_memset(&reply, 0, sizeof(reply));
    const int64_t recv_status = lpr_pacha_syscall4(
        PACHAOS_SYSCALL_IPC_RECV_WAIT,
        (uint64_t)(uint32_t)reply_fd,
        (uint64_t)(uintptr_t)&reply,
        UINT64_MAX,
        0);
    (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)reply_fd);
    if (recv_status != 0) {
        return lpr_pacha_status_to_errno(recv_status);
    }
    if (reply.word0 != FILED_WIRE_REPLY_MAGIC || reply.word3 != request_id) {
        return -LPR_LINUX_EIO;
    }
    if ((int64_t)reply.word1 < 0) {
        return (int64_t)reply.word1;
    }
    if (out_result != 0) {
        *out_result = reply.word2;
    }
    return 0;
}

static uint64_t lpr_open_rights(uint64_t flags)
{
    uint64_t rights = FILED_WIRE_RIGHT_STAT;
    const uint64_t accmode = flags & LPR_LINUX_O_ACCMODE;
    if (accmode != LPR_LINUX_O_WRONLY) {
        rights |= FILED_WIRE_RIGHT_READ | FILED_WIRE_RIGHT_GETDENTS;
    }
    if (accmode == LPR_LINUX_O_WRONLY || accmode == LPR_LINUX_O_RDWR) {
        rights |= FILED_WIRE_RIGHT_WRITE;
    }
    if ((flags & LPR_LINUX_O_DIRECTORY) != 0) {
        rights |=
            FILED_WIRE_RIGHT_LOOKUP |
            FILED_WIRE_RIGHT_GETDENTS |
            FILED_WIRE_RIGHT_CREATE |
            FILED_WIRE_RIGHT_REMOVE |
            FILED_WIRE_RIGHT_RENAME;
    }
    if ((flags & LPR_LINUX_O_CREAT) != 0) {
        rights |= FILED_WIRE_RIGHT_CREATE | FILED_WIRE_RIGHT_WRITE;
    }
    return rights;
}

static uint64_t lpr_open_flags(uint64_t flags)
{
    uint64_t out = 0;
    if ((flags & LPR_LINUX_O_CREAT) != 0) {
        out |= FILED_WIRE_OPEN_CREATE;
    }
    if ((flags & LPR_LINUX_O_EXCL) != 0) {
        out |= FILED_WIRE_OPEN_EXCLUSIVE;
    }
    if ((flags & LPR_LINUX_O_TRUNC) != 0) {
        out |= FILED_WIRE_OPEN_TRUNCATE;
    }
    if ((flags & LPR_LINUX_O_DIRECTORY) != 0) {
        out |= FILED_WIRE_OPEN_DIRECTORY;
    }
    if ((flags & LPR_LINUX_O_NOFOLLOW) != 0) {
        out |= FILED_WIRE_OPEN_NOFOLLOW;
    }
    if ((flags & LPR_LINUX_O_CLOEXEC) != 0) {
        out |= FILED_WIRE_OPEN_CLOEXEC;
    }
    if ((flags & LPR_LINUX_O_APPEND) != 0) {
        out |= FILED_WIRE_OPEN_APPEND;
    }
    if ((flags & LPR_LINUX_O_NONBLOCK) != 0) {
        out |= FILED_WIRE_OPEN_NONBLOCK;
    }
    return out;
}

static int64_t lpr_copy_path(char out[FILED_WIRE_NAME_BYTES], const char *path)
{
    if (path == 0) {
        return -LPR_LINUX_EFAULT;
    }
    const size_t len = lpr_strnlen(path, FILED_WIRE_NAME_BYTES);
    if (len == FILED_WIRE_NAME_BYTES) {
        return -LPR_LINUX_ENAMETOOLONG;
    }
    lpr_memset(out, 0, FILED_WIRE_NAME_BYTES);
    lpr_memcpy(out, path, len + 1u);
    return 0;
}

static int64_t lpr_dir_handle_for(uint64_t dirfd, const char *path, uint64_t *out)
{
    if (out == 0 || path == 0) {
        return -LPR_LINUX_EFAULT;
    }
    if (path[0] == '/' || (int64_t)dirfd == LPR_LINUX_AT_FDCWD) {
        *out = 0;
        return 0;
    }
    if (!lpr_fd_is_filed(dirfd)) {
        return -LPR_LINUX_EBADF;
    }
    *out = lpr_fds[dirfd].handle;
    return 0;
}

static int64_t lpr_filed_close_handle(uint64_t handle)
{
    uint64_t ignored = 0;
    return lpr_filed_call(FILED_WIRE_OP_CLOSE, -1, handle, &ignored);
}

int64_t lpr_linux_fsync(uint64_t fd)
{
    if (!lpr_fd_is_filed(fd)) {
        return -LPR_LINUX_EINVAL;
    }
    uint64_t ignored = 0;
    return lpr_filed_call(FILED_WIRE_OP_FSYNC, -1, lpr_fds[fd].handle, &ignored);
}

int64_t lpr_linux_mkdirat(uint64_t dirfd, uint64_t path_raw, uint64_t mode)
{
    void *page = 0;
    const char *path = (const char *)(uintptr_t)path_raw;
    uint64_t dir_handle = 0;
    int64_t status = lpr_dir_handle_for(dirfd, path, &dir_handle);
    if (status != 0) {
        return status;
    }
    const int page_fd = lpr_create_wire_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }
    filed_wire_mkdir_t *mkdir_req = (filed_wire_mkdir_t *)page;
    lpr_memset(mkdir_req, 0, sizeof(*mkdir_req));
    mkdir_req->dir_handle = dir_handle;
    mkdir_req->mode = mode;
    status = lpr_copy_path(mkdir_req->name, path);
    uint64_t ignored = 0;
    if (status == 0) {
        status = lpr_filed_call(FILED_WIRE_OP_MKDIR, page_fd, 0, &ignored);
    }
    lpr_destroy_wire_page(page_fd, page);
    return status;
}

int64_t lpr_linux_unlinkat(uint64_t dirfd, uint64_t path_raw, uint64_t flags)
{
    const uint64_t known_flags = LPR_LINUX_AT_REMOVEDIR;
    if ((flags & ~known_flags) != 0) {
        return -LPR_LINUX_EINVAL;
    }
    void *page = 0;
    const char *path = (const char *)(uintptr_t)path_raw;
    uint64_t dir_handle = 0;
    int64_t status = lpr_dir_handle_for(dirfd, path, &dir_handle);
    if (status != 0) {
        return status;
    }
    const int page_fd = lpr_create_wire_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }
    uint64_t op = FILED_WIRE_OP_UNLINK;
    if ((flags & LPR_LINUX_AT_REMOVEDIR) != 0) {
        filed_wire_rmdir_t *rmdir_req = (filed_wire_rmdir_t *)page;
        lpr_memset(rmdir_req, 0, sizeof(*rmdir_req));
        rmdir_req->dir_handle = dir_handle;
        status = lpr_copy_path(rmdir_req->name, path);
        op = FILED_WIRE_OP_RMDIR;
    } else {
        filed_wire_unlink_t *unlink_req = (filed_wire_unlink_t *)page;
        lpr_memset(unlink_req, 0, sizeof(*unlink_req));
        unlink_req->dir_handle = dir_handle;
        status = lpr_copy_path(unlink_req->name, path);
    }
    uint64_t ignored = 0;
    if (status == 0) {
        status = lpr_filed_call(op, page_fd, 0, &ignored);
    }
    lpr_destroy_wire_page(page_fd, page);
    return status;
}

int64_t lpr_linux_renameat(uint64_t old_dirfd, uint64_t old_path_raw, uint64_t new_dirfd, uint64_t new_path_raw)
{
    void *page = 0;
    const char *old_path = (const char *)(uintptr_t)old_path_raw;
    const char *new_path = (const char *)(uintptr_t)new_path_raw;
    uint64_t old_dir_handle = 0;
    uint64_t new_dir_handle = 0;
    int64_t status = lpr_dir_handle_for(old_dirfd, old_path, &old_dir_handle);
    if (status != 0) {
        return status;
    }
    status = lpr_dir_handle_for(new_dirfd, new_path, &new_dir_handle);
    if (status != 0) {
        return status;
    }
    const int page_fd = lpr_create_wire_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }
    filed_wire_rename_t *rename_req = (filed_wire_rename_t *)page;
    lpr_memset(rename_req, 0, sizeof(*rename_req));
    rename_req->old_dir_handle = old_dir_handle;
    rename_req->new_dir_handle = new_dir_handle;
    status = lpr_copy_path(rename_req->old_name, old_path);
    if (status == 0) {
        status = lpr_copy_path(rename_req->new_name, new_path);
    }
    uint64_t ignored = 0;
    if (status == 0) {
        status = lpr_filed_call(FILED_WIRE_OP_RENAME, page_fd, 0, &ignored);
    }
    lpr_destroy_wire_page(page_fd, page);
    return status;
}

int64_t lpr_linux_openat(uint64_t dirfd, uint64_t path_raw, uint64_t flags, uint64_t mode)
{
    (void)mode;
    void *page = 0;
    const char *path = (const char *)(uintptr_t)path_raw;
    uint64_t dir_handle = 0;
    int64_t status = lpr_dir_handle_for(dirfd, path, &dir_handle);
    if (status != 0) {
        return status;
    }
    status = lpr_filed_endpoint_ready();
    if (status != 0) {
        return status;
    }
    const int page_fd = lpr_create_wire_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }

    filed_wire_openat_t *open_req = (filed_wire_openat_t *)page;
    lpr_memset(open_req, 0, sizeof(*open_req));
    open_req->dir_handle = dir_handle;
    open_req->rights = lpr_open_rights(flags);
    open_req->open_flags = lpr_open_flags(flags);
    status = lpr_copy_path(open_req->name, path);
    uint64_t handle = 0;
    if (status == 0) {
        status = lpr_filed_call(FILED_WIRE_OP_OPENAT, page_fd, 0, &handle);
    }
    lpr_destroy_wire_page(page_fd, page);
    if (status != 0) {
        return status;
    }
    const int fd = lpr_fd_alloc(handle, flags);
    if (fd < 0) {
        (void)lpr_filed_close_handle(handle);
        return fd;
    }
    return fd;
}

static int64_t lpr_filed_io(uint64_t op, uint64_t fd, uint64_t buf, uint64_t count, uint64_t offset)
{
    if (!lpr_fd_is_filed(fd)) {
        return -LPR_LINUX_EBADF;
    }
    if (buf == 0 && count != 0) {
        return -LPR_LINUX_EFAULT;
    }
    void *page = 0;
    const int page_fd = lpr_create_wire_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }
    filed_wire_io_t *io = (filed_wire_io_t *)page;
    lpr_memset(io, 0, sizeof(*io));
    io->handle = lpr_fds[fd].handle;
    io->offset = offset;
    io->length = count > FILED_WIRE_IO_BYTES ? FILED_WIRE_IO_BYTES : count;
    if (op == FILED_WIRE_OP_WRITE && io->length != 0) {
        lpr_memcpy(io->data, (const void *)(uintptr_t)buf, (size_t)io->length);
    }
    uint64_t result = 0;
    const int64_t status = lpr_filed_call(op, page_fd, 0, &result);
    if (status == 0 && op != FILED_WIRE_OP_WRITE && result != 0) {
        lpr_memcpy((void *)(uintptr_t)buf, io->data, (size_t)result);
    }
    lpr_destroy_wire_page(page_fd, page);
    return status == 0 ? (int64_t)result : status;
}

int64_t lpr_linux_read(uint64_t fd, uint64_t buf, uint64_t count)
{
    if (lpr_fd_is_filed(fd)) {
        return lpr_filed_io(FILED_WIRE_OP_READ, fd, buf, count, 0);
    }
    return lpr_pacha_syscall3(PACHAOS_SYSCALL_FD_READ, fd, buf, count);
}

int64_t lpr_linux_readv(uint64_t fd, uint64_t iov_raw, uint64_t iov_count)
{
    if (iov_raw == 0 && iov_count != 0) {
        return -LPR_LINUX_EFAULT;
    }
    if (!lpr_fd_is_filed(fd)) {
        return lpr_pacha_syscall3(PACHAOS_SYSCALL_FD_READV, fd, iov_raw, iov_count);
    }
    const lpr_linux_iovec_t *iov = (const lpr_linux_iovec_t *)(uintptr_t)iov_raw;
    int64_t total = 0;
    for (uint64_t i = 0; i < iov_count; i += 1) {
        if (iov[i].len == 0) {
            continue;
        }
        const int64_t n = lpr_linux_read(fd, iov[i].base, iov[i].len);
        if (n < 0) {
            return total != 0 ? total : n;
        }
        total += n;
        if ((uint64_t)n < iov[i].len) {
            break;
        }
    }
    return total;
}

int64_t lpr_linux_pread64(uint64_t fd, uint64_t buf, uint64_t count, uint64_t offset)
{
    return lpr_filed_io(FILED_WIRE_OP_PREAD, fd, buf, count, offset);
}

int64_t lpr_linux_write(uint64_t fd, uint64_t buf, uint64_t count)
{
    if (lpr_fd_is_filed(fd)) {
        return lpr_filed_io(FILED_WIRE_OP_WRITE, fd, buf, count, 0);
    }
    if (fd == 1 || fd == 2) {
        return (int64_t)count;
    }
    return lpr_pacha_syscall3(PACHAOS_SYSCALL_FD_WRITE, fd, buf, count);
}

int64_t lpr_linux_writev(uint64_t fd, uint64_t iov_raw, uint64_t iov_count)
{
    if (iov_raw == 0 && iov_count != 0) {
        return -LPR_LINUX_EFAULT;
    }
    if (!lpr_fd_is_filed(fd)) {
        if (fd == 1 || fd == 2) {
            const lpr_linux_iovec_t *iov = (const lpr_linux_iovec_t *)(uintptr_t)iov_raw;
            uint64_t total = 0;
            for (uint64_t i = 0; i < iov_count; i += 1) {
                if (total > UINT64_MAX - iov[i].len) {
                    return -LPR_LINUX_EINVAL;
                }
                total += iov[i].len;
            }
            return (int64_t)total;
        }
        return lpr_pacha_syscall3(PACHAOS_SYSCALL_FD_WRITEV, fd, iov_raw, iov_count);
    }
    const lpr_linux_iovec_t *iov = (const lpr_linux_iovec_t *)(uintptr_t)iov_raw;
    int64_t total = 0;
    for (uint64_t i = 0; i < iov_count; i += 1) {
        if (iov[i].len == 0) {
            continue;
        }
        const int64_t n = lpr_linux_write(fd, iov[i].base, iov[i].len);
        if (n < 0) {
            return total != 0 ? total : n;
        }
        total += n;
        if ((uint64_t)n < iov[i].len) {
            break;
        }
    }
    return total;
}

int64_t lpr_linux_close(uint64_t fd)
{
    if (lpr_fd_is_filed(fd)) {
        const uint64_t handle = lpr_fds[fd].handle;
        lpr_memset(&lpr_fds[fd], 0, sizeof(lpr_fds[fd]));
        return lpr_filed_close_handle(handle);
    }
    if (fd >= 3 && fd < LPR_FD_TABLE_SIZE) {
        return -LPR_LINUX_EBADF;
    }
    return lpr_pacha_status_to_errno(lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, fd));
}

int64_t lpr_linux_lseek(uint64_t fd, uint64_t offset, uint64_t whence)
{
    if (!lpr_fd_is_filed(fd)) {
        return -LPR_LINUX_ESPIPE;
    }
    void *page = 0;
    const int page_fd = lpr_create_wire_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }
    filed_wire_seek_t *seek = (filed_wire_seek_t *)page;
    lpr_memset(seek, 0, sizeof(*seek));
    seek->handle = lpr_fds[fd].handle;
    seek->offset = (int64_t)offset;
    seek->whence = whence;
    uint64_t result = 0;
    const int64_t status = lpr_filed_call(FILED_WIRE_OP_SEEK, page_fd, 0, &result);
    lpr_destroy_wire_page(page_fd, page);
    return status == 0 ? (int64_t)result : status;
}

int64_t lpr_linux_fcntl(uint64_t fd, uint64_t cmd, uint64_t arg)
{
    if (!lpr_fd_is_filed(fd)) {
        switch (cmd) {
        case LPR_LINUX_F_GETFD:
            return 0;
        case LPR_LINUX_F_SETFD:
            return 0;
        case LPR_LINUX_F_GETFL:
            return 0;
        case LPR_LINUX_F_SETFL:
            return 0;
        default:
            return -LPR_LINUX_EINVAL;
        }
    }
    switch (cmd) {
    case LPR_LINUX_F_GETFD:
        return (lpr_fds[fd].flags & LPR_LINUX_O_CLOEXEC) != 0 ? LPR_LINUX_FD_CLOEXEC : 0;
    case LPR_LINUX_F_SETFD:
        if ((arg & LPR_LINUX_FD_CLOEXEC) != 0) {
            lpr_fds[fd].flags |= LPR_LINUX_O_CLOEXEC;
        } else {
            lpr_fds[fd].flags &= ~LPR_LINUX_O_CLOEXEC;
        }
        return 0;
    case LPR_LINUX_F_GETFL:
        return lpr_fds[fd].flags;
    case LPR_LINUX_F_SETFL:
        lpr_fds[fd].flags = (uint32_t)arg;
        return 0;
    case LPR_LINUX_F_DUPFD:
    case LPR_LINUX_F_DUPFD_CLOEXEC:
        return -LPR_LINUX_ENOTSUP;
    default:
        return -LPR_LINUX_EINVAL;
    }
}

int64_t lpr_linux_ioctl(uint64_t fd, uint64_t request, uint64_t arg)
{
    (void)fd;
    (void)arg;
    switch (request) {
    case LPR_LINUX_TCGETS:
    case LPR_LINUX_TIOCGWINSZ:
        return -LPR_LINUX_ENOTTY;
    default:
        return -LPR_LINUX_ENOTTY;
    }
}

static uint8_t lpr_dtype_from_mode(uint64_t mode)
{
    switch (mode & LPR_LINUX_S_IFMT) {
    case LPR_LINUX_S_IFIFO:
        return LPR_LINUX_DT_FIFO;
    case LPR_LINUX_S_IFCHR:
        return LPR_LINUX_DT_CHR;
    case LPR_LINUX_S_IFDIR:
        return LPR_LINUX_DT_DIR;
    case LPR_LINUX_S_IFBLK:
        return LPR_LINUX_DT_BLK;
    case LPR_LINUX_S_IFREG:
        return LPR_LINUX_DT_REG;
    case LPR_LINUX_S_IFLNK:
        return LPR_LINUX_DT_LNK;
    case LPR_LINUX_S_IFSOCK:
        return LPR_LINUX_DT_SOCK;
    default:
        return LPR_LINUX_DT_UNKNOWN;
    }
}

static void lpr_write_linux_stat(void *statbuf, const filed_wire_statx_t *wire)
{
    lpr_linux_stat_t *st = (lpr_linux_stat_t *)statbuf;
    lpr_memset(st, 0, sizeof(*st));
    st->st_dev = 1;
    st->st_ino = wire->handle != 0 ? wire->handle : 1;
    st->st_nlink = wire->nlink != 0 ? wire->nlink : 1;
    st->st_mode = (uint32_t)wire->mode;
    st->st_size = (int64_t)wire->size;
    st->st_blksize = 4096;
    st->st_blocks = (int64_t)wire->blocks;
    st->st_atime_sec = wire->atime_sec;
    st->st_atime_nsec = wire->atime_nsec;
    st->st_mtime_sec = wire->mtime_sec;
    st->st_mtime_nsec = wire->mtime_nsec;
    st->st_ctime_sec = wire->ctime_sec;
    st->st_ctime_nsec = wire->ctime_nsec;
}

int64_t lpr_linux_fstat(uint64_t fd, uint64_t statbuf)
{
    if (statbuf == 0) {
        return -LPR_LINUX_EFAULT;
    }
    if (!lpr_fd_is_filed(fd)) {
        return lpr_pacha_status_to_errno(lpr_pacha_syscall2(PACHAOS_SYSCALL_FD_STAT, fd, statbuf));
    }
    void *page = 0;
    const int page_fd = lpr_create_wire_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }
    filed_wire_statx_t *stat = (filed_wire_statx_t *)page;
    lpr_memset(stat, 0, sizeof(*stat));
    stat->handle = lpr_fds[fd].handle;
    uint64_t ignored = 0;
    const int64_t status = lpr_filed_call(FILED_WIRE_OP_STAT, page_fd, 0, &ignored);
    if (status == 0) {
        lpr_write_linux_stat((void *)(uintptr_t)statbuf, stat);
    }
    lpr_destroy_wire_page(page_fd, page);
    return status;
}

int64_t lpr_linux_newfstatat(uint64_t dirfd, uint64_t path_raw, uint64_t statbuf, uint64_t flags)
{
    if (statbuf == 0) {
        return -LPR_LINUX_EFAULT;
    }
    const char *path = (const char *)(uintptr_t)path_raw;
    if ((flags & LPR_LINUX_AT_EMPTY_PATH) != 0 && path != 0 && path[0] == 0) {
        return lpr_linux_fstat(dirfd, statbuf);
    }
    const int64_t fd = lpr_linux_openat(dirfd, path_raw, LPR_LINUX_O_RDONLY, 0);
    if (fd < 0) {
        return fd;
    }
    const int64_t status = lpr_linux_fstat((uint64_t)fd, statbuf);
    (void)lpr_linux_close((uint64_t)fd);
    return status;
}

int64_t lpr_linux_access(uint64_t path, uint64_t mode)
{
    (void)mode;
    const int64_t fd = lpr_linux_openat((uint64_t)(int64_t)LPR_LINUX_AT_FDCWD, path, LPR_LINUX_O_RDONLY, 0);
    if (fd < 0) {
        return fd;
    }
    (void)lpr_linux_close((uint64_t)fd);
    return 0;
}

static int64_t lpr_linux_open_metadata(uint64_t dirfd, uint64_t path_raw)
{
    int64_t fd = lpr_linux_openat(dirfd, path_raw, LPR_LINUX_O_RDWR, 0);
    if (fd == -LPR_LINUX_EISDIR) {
        fd = lpr_linux_openat(dirfd, path_raw, LPR_LINUX_O_RDWR | LPR_LINUX_O_DIRECTORY, 0);
    }
    return fd;
}

int64_t lpr_linux_fchmod(uint64_t fd, uint64_t mode)
{
    if (!lpr_fd_is_filed(fd)) {
        return -LPR_LINUX_EBADF;
    }
    void *page = 0;
    const int page_fd = lpr_create_wire_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }
    filed_wire_chmod_t *chmod_req = (filed_wire_chmod_t *)page;
    lpr_memset(chmod_req, 0, sizeof(*chmod_req));
    chmod_req->handle = lpr_fds[fd].handle;
    chmod_req->mode = mode & 07777ull;
    uint64_t ignored = 0;
    const int64_t status = lpr_filed_call(FILED_WIRE_OP_CHMOD, page_fd, 0, &ignored);
    lpr_destroy_wire_page(page_fd, page);
    return status;
}

int64_t lpr_linux_fchmodat(uint64_t dirfd, uint64_t path_raw, uint64_t mode, uint64_t flags)
{
    const uint64_t known_flags = LPR_LINUX_AT_SYMLINK_NOFOLLOW | LPR_LINUX_AT_EMPTY_PATH;
    if ((flags & ~known_flags) != 0) {
        return -LPR_LINUX_EINVAL;
    }
    const char *path = (const char *)(uintptr_t)path_raw;
    if ((flags & LPR_LINUX_AT_EMPTY_PATH) != 0 && path != 0 && path[0] == 0) {
        return lpr_linux_fchmod(dirfd, mode);
    }
    if (path == 0) {
        return -LPR_LINUX_EFAULT;
    }
    const int64_t fd = lpr_linux_open_metadata(dirfd, path_raw);
    if (fd < 0) {
        return fd;
    }
    const int64_t status = lpr_linux_fchmod((uint64_t)fd, mode);
    (void)lpr_linux_close((uint64_t)fd);
    return status;
}

static int64_t lpr_linux_now(lpr_linux_timespec_t *out)
{
    if (out == 0) {
        return -LPR_LINUX_EFAULT;
    }
    lpr_memset(out, 0, sizeof(*out));
    const int64_t status = lpr_pacha_syscall2(
        PACHAOS_SYSCALL_CLOCK_GETTIME,
        0,
        (uint64_t)(uintptr_t)out);
    return lpr_pacha_status_to_errno(status);
}

static int64_t lpr_linux_resolve_utime(
    const lpr_linux_timespec_t *input,
    const lpr_linux_timespec_t *now,
    uint64_t wire_bit,
    uint64_t *mask,
    int64_t *out_sec,
    int64_t *out_nsec)
{
    if (mask == 0 || out_sec == 0 || out_nsec == 0) {
        return -LPR_LINUX_EFAULT;
    }
    if (input == 0) {
        if (now == 0) {
            return -LPR_LINUX_EFAULT;
        }
        *mask |= wire_bit;
        *out_sec = now->tv_sec;
        *out_nsec = now->tv_nsec;
        return 0;
    }
    if (input->tv_nsec == LPR_LINUX_UTIME_OMIT) {
        return 0;
    }
    *mask |= wire_bit;
    if (input->tv_nsec == LPR_LINUX_UTIME_NOW) {
        if (now == 0) {
            return -LPR_LINUX_EFAULT;
        }
        *out_sec = now->tv_sec;
        *out_nsec = now->tv_nsec;
        return 0;
    }
    if (input->tv_nsec < 0 || input->tv_nsec >= 1000000000ll) {
        return -LPR_LINUX_EINVAL;
    }
    *out_sec = input->tv_sec;
    *out_nsec = input->tv_nsec;
    return 0;
}

static int64_t lpr_filed_utimens_handle(uint64_t handle, uint64_t times_raw)
{
    const lpr_linux_timespec_t *times = (const lpr_linux_timespec_t *)(uintptr_t)times_raw;
    lpr_linux_timespec_t now;
    uint64_t mask = 0;
    int64_t status = lpr_linux_now(&now);
    if (status != 0) {
        return status;
    }

    void *page = 0;
    const int page_fd = lpr_create_wire_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }
    filed_wire_utimens_t *utimens = (filed_wire_utimens_t *)page;
    lpr_memset(utimens, 0, sizeof(*utimens));
    utimens->handle = handle;

    status = lpr_linux_resolve_utime(
        times_raw == 0 ? 0 : &times[0],
        &now,
        FILED_WIRE_UTIMENS_ATIME,
        &mask,
        &utimens->atime_sec,
        &utimens->atime_nsec);
    if (status == 0) {
        status = lpr_linux_resolve_utime(
            times_raw == 0 ? 0 : &times[1],
            &now,
            FILED_WIRE_UTIMENS_MTIME,
            &mask,
            &utimens->mtime_sec,
            &utimens->mtime_nsec);
    }
    uint64_t ignored = 0;
    if (status == 0 && mask != 0) {
        utimens->mask = mask;
        status = lpr_filed_call(FILED_WIRE_OP_UTIMENS, page_fd, 0, &ignored);
    }
    lpr_destroy_wire_page(page_fd, page);
    return status;
}

int64_t lpr_linux_utimensat(uint64_t dirfd, uint64_t path_raw, uint64_t times, uint64_t flags)
{
    const uint64_t known_flags = LPR_LINUX_AT_SYMLINK_NOFOLLOW | LPR_LINUX_AT_EMPTY_PATH;
    if ((flags & ~known_flags) != 0) {
        return -LPR_LINUX_EINVAL;
    }
    const char *path = (const char *)(uintptr_t)path_raw;
    if ((flags & LPR_LINUX_AT_EMPTY_PATH) != 0 && path != 0 && path[0] == 0) {
        if (!lpr_fd_is_filed(dirfd)) {
            return -LPR_LINUX_EBADF;
        }
        return lpr_filed_utimens_handle(lpr_fds[dirfd].handle, times);
    }
    if (path == 0) {
        return -LPR_LINUX_EFAULT;
    }
    const int64_t fd = lpr_linux_openat(dirfd, path_raw, LPR_LINUX_O_RDWR, 0);
    if (fd < 0) {
        return fd;
    }
    const int64_t status = lpr_filed_utimens_handle(lpr_fds[(uint64_t)fd].handle, times);
    (void)lpr_linux_close((uint64_t)fd);
    return status;
}

int64_t lpr_linux_readlink(uint64_t path, uint64_t buf, uint64_t bufsiz)
{
    if (buf == 0 && bufsiz != 0) {
        return -LPR_LINUX_EFAULT;
    }
    lpr_linux_stat_t st;
    const int64_t status = lpr_linux_newfstatat(
        (uint64_t)(int64_t)LPR_LINUX_AT_FDCWD,
        path,
        (uint64_t)(uintptr_t)&st,
        0);
    if (status != 0) {
        return status;
    }
    if (((uint64_t)st.st_mode & LPR_LINUX_S_IFMT) != LPR_LINUX_S_IFLNK) {
        return -LPR_LINUX_EINVAL;
    }
    return -LPR_LINUX_ENOTSUP;
}

static uint16_t lpr_dirent_reclen(uint64_t name_len)
{
    const uint64_t raw = 19u + name_len + 1u;
    return (uint16_t)((raw + 7u) & ~7ull);
}

int64_t lpr_linux_getdents64(uint64_t fd, uint64_t buf, uint64_t count)
{
    if (!lpr_fd_is_filed(fd)) {
        return -LPR_LINUX_EBADF;
    }
    if (buf == 0 && count != 0) {
        return -LPR_LINUX_EFAULT;
    }
    void *page = 0;
    const int page_fd = lpr_create_wire_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }
    filed_wire_getdents_t *gd = (filed_wire_getdents_t *)page;
    lpr_memset(gd, 0, sizeof(*gd));
    gd->dir_handle = lpr_fds[fd].handle;
    gd->capacity = FILED_WIRE_DIRENT_CAPACITY;
    uint64_t ignored = 0;
    int64_t status = lpr_filed_call(FILED_WIRE_OP_GETDENTS, page_fd, 0, &ignored);
    if (status != 0) {
        lpr_destroy_wire_page(page_fd, page);
        return status;
    }

    uint8_t *out = (uint8_t *)(uintptr_t)buf;
    uint64_t written = 0;
    for (uint64_t i = 0; i < gd->count && i < FILED_WIRE_DIRENT_CAPACITY; i += 1) {
        const filed_wire_dirent_t *entry = &gd->entries[i];
        const uint64_t name_len = entry->name_len < FILED_WIRE_DIRENT_NAME_BYTES ?
            entry->name_len :
            FILED_WIRE_DIRENT_NAME_BYTES - 1u;
        const uint16_t reclen = lpr_dirent_reclen(name_len);
        if (written + reclen > count) {
            break;
        }
        lpr_memset(out + written, 0, reclen);
        *(uint64_t *)(void *)(out + written + 0u) = entry->handle != 0 ? entry->handle : (i + 1u);
        *(int64_t *)(void *)(out + written + 8u) = (int64_t)(i + 1u);
        *(uint16_t *)(void *)(out + written + 16u) = reclen;
        *(uint8_t *)(void *)(out + written + 18u) = lpr_dtype_from_mode(entry->kind);
        lpr_memcpy(out + written + 19u, entry->name, (size_t)name_len);
        out[written + 19u + name_len] = 0;
        written += reclen;
    }
    lpr_destroy_wire_page(page_fd, page);
    return (int64_t)written;
}
