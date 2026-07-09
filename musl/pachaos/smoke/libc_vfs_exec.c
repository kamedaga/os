#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#ifdef __pachaos__
#define smoke_getdents getdents
#else
extern ssize_t getdents64(int fd, void *buffer, size_t length);
#define smoke_getdents getdents64
#endif

static unsigned long long now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec;
}

static unsigned long long read_tsc(void)
{
#if defined(__x86_64__) || defined(__i386__)
    uint32_t lo = 0;
    uint32_t hi = 0;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((unsigned long long)hi << 32) | (unsigned long long)lo;
#else
    return 0;
#endif
}

static unsigned long long tsc_calibration_cycles;
static unsigned long long tsc_calibration_ns;

static void calibrate_tsc_timebase(void)
{
    const unsigned long long min_calibration_ns = 20000000ull;
    unsigned long long start_ns = now_ns();
    unsigned long long start_cycles = read_tsc();
    unsigned long long end_ns = start_ns;
    unsigned long long end_cycles = start_cycles;

    if (start_ns == 0 || start_cycles == 0) {
        return;
    }

    do {
        end_ns = now_ns();
        end_cycles = read_tsc();
    } while (end_ns >= start_ns && end_ns - start_ns < min_calibration_ns);

    if (end_ns > start_ns && end_cycles > start_cycles) {
        tsc_calibration_ns = end_ns - start_ns;
        tsc_calibration_cycles = end_cycles - start_cycles;
    }
}

static unsigned long long cycles_to_ns(unsigned long long cycles)
{
    if (cycles == 0) return 0;
    if (tsc_calibration_cycles == 0 || tsc_calibration_ns == 0) return 0;
    long double scaled = (long double)cycles * (long double)tsc_calibration_ns;
    scaled /= (long double)tsc_calibration_cycles;
    if (scaled < 0.0L) return 0;
    return (unsigned long long)scaled;
}

static void metric(const char *name, unsigned long long start_ns, unsigned long long end_ns)
{
    printf("[libc-vfs-exec-smoke] metric op=%s ns=%llu\n", name, end_ns - start_ns);
    fflush(stdout);
}

static void metric_avg(
    const char *name,
    unsigned long long start_ns,
    unsigned long long end_ns,
    unsigned long long iterations)
{
    unsigned long long total = end_ns - start_ns;
    unsigned long long avg = iterations > 0 ? total / iterations : 0;
    printf(
        "[libc-vfs-exec-smoke] metric op=%s iterations=%llu total_ns=%llu avg_ns=%llu\n",
        name,
        iterations,
        total,
        avg);
    fflush(stdout);
}

static void metric_total_avg(
    const char *name,
    unsigned long long total_ns,
    unsigned long long iterations)
{
    unsigned long long avg = iterations > 0 ? total_ns / iterations : 0;
    printf(
        "[libc-vfs-exec-smoke] metric op=%s iterations=%llu total_ns=%llu avg_ns=%llu\n",
        name,
        iterations,
        total_ns,
        avg);
    fflush(stdout);
}

static void metric_total_avg_from_cycles(
    const char *name,
    unsigned long long total_cycles,
    unsigned long long iterations)
{
    unsigned long long total_ns = cycles_to_ns(total_cycles);
    unsigned long long avg = iterations > 0 ? total_ns / iterations : 0;
    printf(
        "[libc-vfs-exec-smoke] metric op=%s iterations=%llu total_ns=%llu avg_ns=%llu total_cycles=%llu avg_cycles=%llu\n",
        name,
        iterations,
        total_ns,
        avg,
        total_cycles,
        iterations > 0 ? total_cycles / iterations : 0);
    fflush(stdout);
}

static int bench_done(unsigned long long start_ns, unsigned long long now, unsigned long long iterations)
{
    const unsigned long long min_total_ns = 10000000ull;
    const unsigned long long max_iterations = 1048576ull;
    return (iterations != 0 && now - start_ns >= min_total_ns) || iterations >= max_iterations;
}

static int fail(const char *what)
{
    fprintf(stderr, "[libc-vfs-exec-smoke] %s failed errno=%d\n", what, errno);
    return 1;
}

#ifndef __pachaos__
static int errno_is_unsupported(int value)
{
    return value == ENOSYS || value == ENOTSUP || value == EOPNOTSUPP;
}
#endif

#ifdef __pachaos__
static void metric_total_avg_cycles(
    const char *name,
    unsigned long long total_cycles,
    unsigned long long iterations)
{
    unsigned long long avg = iterations > 0 ? total_cycles / iterations : 0;
    printf(
        "[libc-vfs-exec-smoke] metric op=%s iterations=%llu total_cycles=%llu avg_cycles=%llu\n",
        name,
        iterations,
        total_cycles,
        avg);
    fflush(stdout);
}

enum {
    PACHAOS_SYSCALL_FD_CLOSE = 28,
    PACHAOS_SYSCALL_FD_WAIT_MANY = 38,
    PACHAOS_SYSCALL_VMO_CREATE = 46,
    PACHAOS_SYSCALL_MMAP = 48,
    PACHAOS_SYSCALL_MUNMAP = 49,
    PACHAOS_SYSCALL_IPC_CHANNEL_CREATE = 54,
    PACHAOS_SYSCALL_IPC_SEND = 55,
    PACHAOS_SYSCALL_IPC_RECV = 56,
    PACHAOS_SYSCALL_IPC_CALL = 57,
    PACHAOS_SYSCALL_IPC_RECV_WAIT = 59,

    PACHAOS_FD_FLAG_CLOEXEC = 1,

    PACHAOS_POLL_READABLE = 1,
    PACHAOS_POLL_WRITABLE = 2,

    PACHAOS_PROT_READ = 1,
    PACHAOS_PROT_WRITE = 2,
    PACHAOS_MMAP_SHARED = 8,

    PACHAOS_FD_RIGHT_INSPECT = 1ull << 0,
    PACHAOS_FD_RIGHT_TRANSFER = 1ull << 2,
    PACHAOS_FD_RIGHT_WAIT = 1ull << 3,
    PACHAOS_FD_RIGHT_POLL = 1ull << 4,
    PACHAOS_FD_RIGHT_CLOSE = 1ull << 6,
    PACHAOS_FD_RIGHT_SEND = 1ull << 7,
    PACHAOS_FD_RIGHT_RECV = 1ull << 8,
    PACHAOS_FD_RIGHT_MAP_READ = 1ull << 13,
    PACHAOS_FD_RIGHT_MAP_WRITE = 1ull << 14,

    PACHAOS_FILED_ENDPOINT_FD = 240,
    PACHAOS_FILED_PAGE_BYTES = 8192,
    PACHAOS_FILED_SESSION_PAGE_BYTES = 40960,
    PACHAOS_FILED_FAST_VERSION = 1,
    PACHAOS_FILED_FAST_REQUEST_CAPACITY = 8,
    PACHAOS_FILED_FAST_COMPLETION_CAPACITY = 8,
    PACHAOS_FILED_FAST_PAYLOAD_SLOT_COUNT = 4,
    PACHAOS_FILED_FAST_PAYLOAD_OFFSET = 4096,
    PACHAOS_FILED_FAST_GENERATION_OFFSET =
        PACHAOS_FILED_FAST_PAYLOAD_OFFSET +
        PACHAOS_FILED_FAST_PAYLOAD_SLOT_COUNT * PACHAOS_FILED_PAGE_BYTES,
    PACHAOS_FILED_FAST_GENERATION_CAPACITY = 64,
    PACHAOS_FILED_REQUEST_MAGIC = 0x31465152444c4946ull,
    PACHAOS_FILED_REPLY_MAGIC = 0x31595052444c4946ull,
    PACHAOS_FILED_FAST_MAGIC = 0x31545341464c4446ull,
    PACHAOS_FILED_OP_HELLO = 1,
    PACHAOS_FILED_OP_OPENAT = 4,
    PACHAOS_FILED_OP_STAT = 5,
    PACHAOS_FILED_OP_PREAD = 6,
    PACHAOS_FILED_OP_GETDENTS = 7,
    PACHAOS_FILED_OP_CLOSE = 8,
    PACHAOS_FILED_OP_PWRITE = 14,
    PACHAOS_FILED_OP_TRUNCATE = 17,
    PACHAOS_FILED_OP_UNLINK = 18,
    PACHAOS_FILED_OP_RENAME = 19,
    PACHAOS_FILED_OP_MKDIR = 20,
    PACHAOS_FILED_OP_RMDIR = 21,
    PACHAOS_FILED_OP_CONNECT = 25,
    PACHAOS_FILED_OP_PING = 26,
    PACHAOS_FILED_OP_FAST_DOORBELL = 27,
    PACHAOS_FILED_OP_VALIDATE_OPEN_CACHE = 28,

    PACHAOS_FILED_NAME_BYTES = 96,
    PACHAOS_FILED_IO_BYTES = 7680,
    PACHAOS_FILED_RIGHT_LOOKUP = 1u << 0,
    PACHAOS_FILED_RIGHT_READ = 1u << 1,
    PACHAOS_FILED_RIGHT_WRITE = 1u << 2,
    PACHAOS_FILED_RIGHT_STAT = 1u << 4,
    PACHAOS_FILED_RIGHT_GETDENTS = 1u << 5,
    PACHAOS_FILED_RIGHT_CREATE = 1u << 6,
    PACHAOS_FILED_RIGHT_REMOVE = 1u << 7,
    PACHAOS_FILED_RIGHT_RENAME = 1u << 8,
    PACHAOS_FILED_OPEN_CREATE = 1u << 0,
    PACHAOS_FILED_OPEN_EXCLUSIVE = 1u << 1,
    PACHAOS_FILED_OPEN_TRUNCATE = 1u << 2,
    PACHAOS_FILED_OPEN_DIRECTORY = 1u << 3,
    PACHAOS_FILED_OPEN_CLOEXEC = 1u << 5,
};

struct pachaos_raw_pollfd {
    long fd;
    long events;
    long revents;
};

struct pachaos_raw_ipc_fd {
    uint64_t fd;
    uint64_t rights;
    uint64_t flags;
    uint64_t transfer_flags;
};

struct pachaos_raw_ipc_msg {
    uint64_t word0;
    uint64_t word1;
    uint64_t word2;
    uint64_t word3;
    struct pachaos_raw_ipc_fd *fds;
    uint64_t fd_count;
    uint64_t fd_capacity;
    uint64_t flags;
};

struct pachaos_filed_openat {
    uint64_t dir_handle;
    uint64_t rights;
    uint64_t open_flags;
    uint64_t object_generation;
    uint64_t dir_generation;
    uint64_t reserved0;
    char name[PACHAOS_FILED_NAME_BYTES];
};

struct pachaos_filed_io {
    uint64_t handle;
    uint64_t offset;
    uint64_t length;
    uint8_t data[PACHAOS_FILED_IO_BYTES];
};

struct pachaos_filed_statx {
    uint64_t handle;
    uint64_t mode;
    uint64_t size;
    uint64_t blocks;
    uint64_t nlink;
    uint64_t kind;
};

struct pachaos_filed_truncate {
    uint64_t handle;
    uint64_t size;
    uint64_t reserved0;
    uint64_t reserved1;
};

struct pachaos_filed_unlink {
    uint64_t dir_handle;
    uint64_t reserved0;
    char name[PACHAOS_FILED_NAME_BYTES];
};

struct pachaos_filed_mkdir {
    uint64_t dir_handle;
    uint64_t mode;
    char name[PACHAOS_FILED_NAME_BYTES];
};

struct pachaos_filed_rmdir {
    uint64_t dir_handle;
    uint64_t reserved0;
    char name[PACHAOS_FILED_NAME_BYTES];
};

struct pachaos_filed_rename {
    uint64_t old_dir_handle;
    uint64_t new_dir_handle;
    char old_name[PACHAOS_FILED_NAME_BYTES];
    char new_name[PACHAOS_FILED_NAME_BYTES];
};

struct pachaos_filed_fast_header {
    uint64_t magic;
    uint64_t version;
    uint64_t flags;
    uint64_t request_capacity;
    uint64_t completion_capacity;
    uint64_t payload_slot_count;
    uint64_t payload_slot_size;
    uint64_t payload_offset;
    uint64_t request_head;
    uint64_t request_tail;
    uint64_t completion_head;
    uint64_t completion_tail;
    uint64_t doorbell_seq;
    uint64_t completion_seq;
    uint64_t generation_offset;
    uint64_t generation_capacity;
};

struct pachaos_filed_fast_request {
    uint64_t request_id;
    uint64_t opcode;
    uint64_t flags;
    uint64_t handle;
    uint64_t word2;
    uint64_t offset;
    uint64_t length;
    uint64_t payload_slot;
    uint64_t payload_length;
    uint64_t timeout_ns;
};

struct pachaos_filed_fast_completion {
    uint64_t request_id;
    int64_t status;
    uint64_t result;
    uint64_t bytes;
    uint64_t flags;
};

struct pachaos_filed_call_breakdown {
    uint64_t enqueue_ns;
    uint64_t send_ns;
    uint64_t recv_ns;
    uint64_t completion_ns;
    uint64_t enqueue_cycles;
    uint64_t send_cycles;
    uint64_t recv_cycles;
    uint64_t completion_cycles;
};

static long pachaos_raw1(long n, long a1)
{
    unsigned long ret;
    __asm__ __volatile__("syscall" : "=a"(ret) : "a"(n), "D"(a1) : "rcx", "r11", "memory");
    return (long)ret;
}

static long pachaos_raw2(long n, long a1, long a2)
{
    unsigned long ret;
    __asm__ __volatile__("syscall" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2) : "rcx", "r11", "memory");
    return (long)ret;
}

static long pachaos_raw3(long n, long a1, long a2, long a3)
{
    unsigned long ret;
    __asm__ __volatile__("syscall" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2), "d"(a3) : "rcx", "r11", "memory");
    return (long)ret;
}

static long pachaos_raw4(long n, long a1, long a2, long a3, long a4)
{
    unsigned long ret;
    register long r10 __asm__("r10") = a4;
    __asm__ __volatile__(
        "syscall"
        : "=a"(ret)
        : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10)
        : "rcx", "r11", "memory");
    return (long)ret;
}

static long pachaos_raw6(long n, long a1, long a2, long a3, long a4, long a5, long a6)
{
    unsigned long ret;
    register long r10 __asm__("r10") = a4;
    register long r8 __asm__("r8") = a5;
    register long r9 __asm__("r9") = a6;
    __asm__ __volatile__(
        "syscall"
        : "=a"(ret)
        : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory");
    return (long)ret;
}

static void close_raw_fd(uint64_t fd)
{
    if (fd >= 16) {
        (void)pachaos_raw1(PACHAOS_SYSCALL_FD_CLOSE, (long)fd);
    }
}

static void unmap_raw(uint64_t addr, uint64_t len)
{
    if (addr >= 4096 && len != 0) {
        (void)pachaos_raw2(PACHAOS_SYSCALL_MUNMAP, (long)addr, (long)len);
    }
}

static int ipc_bench_fail(const char *what, long status)
{
    fprintf(stderr, "[libc-vfs-exec-smoke] %s failed status=%ld\n", what, status);
    return 1;
}

static long ipc_send_wait_timeout(uint64_t fd, const struct pachaos_raw_ipc_msg *msg, uint64_t timeout_ticks)
{
    for (;;) {
        long status = pachaos_raw2(PACHAOS_SYSCALL_IPC_SEND, (long)fd, (long)msg);
        if (status == 0) return 0;
        if (status != 2 && status != -2 && status != 5 && status != -5) return status;
        struct pachaos_raw_pollfd wait_fd = {
            .fd = (long)fd,
            .events = PACHAOS_POLL_WRITABLE,
            .revents = 0,
        };
        (void)pachaos_raw4(PACHAOS_SYSCALL_FD_WAIT_MANY, (long)&wait_fd, 1, (long)timeout_ticks, 0);
    }
}

static long ipc_recv_wait_timeout(uint64_t fd, struct pachaos_raw_ipc_msg *msg, uint64_t timeout_ticks)
{
    (void)timeout_ticks;
    for (;;) {
        long status = pachaos_raw4(PACHAOS_SYSCALL_IPC_RECV_WAIT, (long)fd, (long)msg, (long)UINT64_MAX, 0);
        if (status == 0) return 0;
        if (status != 2 && status != -2 && status != 5 && status != -5) return status;
    }
}

static long ipc_send_wait(uint64_t fd, const struct pachaos_raw_ipc_msg *msg)
{
    return ipc_send_wait_timeout(fd, msg, 1);
}

static long ipc_recv_wait(uint64_t fd, struct pachaos_raw_ipc_msg *msg)
{
    return ipc_recv_wait_timeout(fd, msg, 1);
}

static long filed_endpoint_hello_once(uint64_t request_id)
{
    struct pachaos_raw_ipc_msg request;
    struct pachaos_raw_ipc_msg reply;
    memset(&request, 0, sizeof(request));
    memset(&reply, 0, sizeof(reply));
    request.word0 = PACHAOS_FILED_REQUEST_MAGIC;
    request.word1 = PACHAOS_FILED_OP_HELLO;
    request.word3 = request_id;

    long reply_fd = pachaos_raw2(PACHAOS_SYSCALL_IPC_CALL, PACHAOS_FILED_ENDPOINT_FD, (long)&request);
    if (reply_fd < 16) return -9;
    long status = ipc_recv_wait((uint64_t)reply_fd, &reply);
    close_raw_fd((uint64_t)reply_fd);
    if (status != 0) return status;
    if (reply.word0 != PACHAOS_FILED_REPLY_MAGIC ||
        reply.word1 != 0 ||
        reply.word2 != 1 ||
        reply.word3 != request_id)
    {
        return -71;
    }
    return 0;
}

static long filed_session_connect_for_bench(uint64_t *out_fd, uint64_t *out_page_addr, uint64_t *out_page_fd)
{
    if (out_fd == NULL || out_page_addr == NULL || out_page_fd == NULL) return -22;
    *out_fd = 0;
    *out_page_addr = 0;
    *out_page_fd = 0;

    const uint64_t channel_rights =
        PACHAOS_FD_RIGHT_INSPECT |
        PACHAOS_FD_RIGHT_WAIT |
        PACHAOS_FD_RIGHT_POLL |
        PACHAOS_FD_RIGHT_CLOSE |
        PACHAOS_FD_RIGHT_SEND |
        PACHAOS_FD_RIGHT_RECV |
        PACHAOS_FD_RIGHT_TRANSFER;
    const uint64_t page_rights =
        PACHAOS_FD_RIGHT_TRANSFER |
        PACHAOS_FD_RIGHT_CLOSE |
        PACHAOS_FD_RIGHT_MAP_READ |
        PACHAOS_FD_RIGHT_MAP_WRITE;

    uint64_t pair[2] = {0, 0};
    long status = pachaos_raw3(
        PACHAOS_SYSCALL_IPC_CHANNEL_CREATE,
        (long)pair,
        (long)channel_rights,
        PACHAOS_FD_FLAG_CLOEXEC);
    if (status != 0 || pair[0] < 16 || pair[1] < 16) {
        close_raw_fd(pair[0]);
        close_raw_fd(pair[1]);
        return status != 0 ? status : -22;
    }

    long page_fd = pachaos_raw3(
        PACHAOS_SYSCALL_VMO_CREATE,
        PACHAOS_FILED_SESSION_PAGE_BYTES,
        (long)page_rights,
        0);
    if (page_fd < 16) {
        close_raw_fd(pair[0]);
        close_raw_fd(pair[1]);
        return -23;
    }
    long page_addr = pachaos_raw6(
        PACHAOS_SYSCALL_MMAP,
        page_fd,
        0,
        PACHAOS_FILED_SESSION_PAGE_BYTES,
        PACHAOS_PROT_READ | PACHAOS_PROT_WRITE,
        PACHAOS_MMAP_SHARED,
        0);
    if (page_addr < 4096) {
        close_raw_fd((uint64_t)page_fd);
        close_raw_fd(pair[0]);
        close_raw_fd(pair[1]);
        return -14;
    }

    memset((void *)(uintptr_t)page_addr, 0, PACHAOS_FILED_SESSION_PAGE_BYTES);
    struct pachaos_filed_fast_header *header = (struct pachaos_filed_fast_header *)(uintptr_t)page_addr;
    header->magic = PACHAOS_FILED_FAST_MAGIC;
    header->version = PACHAOS_FILED_FAST_VERSION;
    header->request_capacity = PACHAOS_FILED_FAST_REQUEST_CAPACITY;
    header->completion_capacity = PACHAOS_FILED_FAST_COMPLETION_CAPACITY;
    header->payload_slot_count = PACHAOS_FILED_FAST_PAYLOAD_SLOT_COUNT;
    header->payload_slot_size = PACHAOS_FILED_PAGE_BYTES;
    header->payload_offset = PACHAOS_FILED_FAST_PAYLOAD_OFFSET;
    header->generation_offset = PACHAOS_FILED_FAST_GENERATION_OFFSET;
    header->generation_capacity = PACHAOS_FILED_FAST_GENERATION_CAPACITY;

    struct pachaos_raw_ipc_fd request_fds[2];
    struct pachaos_raw_ipc_msg request;
    struct pachaos_raw_ipc_msg reply;
    memset(request_fds, 0, sizeof(request_fds));
    memset(&request, 0, sizeof(request));
    memset(&reply, 0, sizeof(reply));

    request_fds[0].fd = pair[1];
    request_fds[0].rights =
        PACHAOS_FD_RIGHT_CLOSE |
        PACHAOS_FD_RIGHT_WAIT |
        PACHAOS_FD_RIGHT_POLL |
        PACHAOS_FD_RIGHT_SEND |
        PACHAOS_FD_RIGHT_RECV;
    request_fds[1].fd = (uint64_t)page_fd;
    request_fds[1].rights = page_rights;
    request.word0 = PACHAOS_FILED_REQUEST_MAGIC;
    request.word1 = PACHAOS_FILED_OP_CONNECT;
    request.word3 = 0x1000;
    request.fds = request_fds;
    request.fd_count = 2;

    long reply_fd = pachaos_raw2(PACHAOS_SYSCALL_IPC_CALL, PACHAOS_FILED_ENDPOINT_FD, (long)&request);
    close_raw_fd(pair[1]);
    if (reply_fd < 16) {
        unmap_raw((uint64_t)page_addr, PACHAOS_FILED_SESSION_PAGE_BYTES);
        close_raw_fd((uint64_t)page_fd);
        close_raw_fd(pair[0]);
        return -9;
    }
    status = ipc_recv_wait((uint64_t)reply_fd, &reply);
    close_raw_fd((uint64_t)reply_fd);
    if (status != 0 ||
        reply.word0 != PACHAOS_FILED_REPLY_MAGIC ||
        reply.word1 != 0 ||
        reply.word3 != request.word3)
    {
        unmap_raw((uint64_t)page_addr, PACHAOS_FILED_SESSION_PAGE_BYTES);
        close_raw_fd((uint64_t)page_fd);
        close_raw_fd(pair[0]);
        return status != 0 ? status : -71;
    }

    *out_fd = pair[0];
    *out_page_addr = (uint64_t)page_addr;
    *out_page_fd = (uint64_t)page_fd;
    return 0;
}

static long filed_session_call_for_bench_timed(
    uint64_t session_fd,
    uint64_t session_page_addr,
    uint64_t op,
    uint64_t word2,
    uint64_t request_id,
    struct pachaos_raw_ipc_msg *reply,
    struct pachaos_filed_call_breakdown *breakdown)
{
    if (session_page_addr < 4096) return -22;
    struct pachaos_filed_fast_header *header =
        (struct pachaos_filed_fast_header *)(uintptr_t)session_page_addr;
    if (header->magic != PACHAOS_FILED_FAST_MAGIC ||
        header->version != PACHAOS_FILED_FAST_VERSION ||
        header->request_capacity != PACHAOS_FILED_FAST_REQUEST_CAPACITY ||
        header->completion_capacity != PACHAOS_FILED_FAST_COMPLETION_CAPACITY ||
        header->payload_offset != PACHAOS_FILED_FAST_PAYLOAD_OFFSET ||
        header->generation_offset != PACHAOS_FILED_FAST_GENERATION_OFFSET ||
        header->generation_capacity != PACHAOS_FILED_FAST_GENERATION_CAPACITY)
    {
        return -71;
    }

    struct pachaos_filed_fast_request *requests =
        (struct pachaos_filed_fast_request *)(uintptr_t)(
            session_page_addr + sizeof(struct pachaos_filed_fast_header));
    struct pachaos_filed_fast_completion *completions =
        (struct pachaos_filed_fast_completion *)(uintptr_t)(
            session_page_addr +
            sizeof(struct pachaos_filed_fast_header) +
            sizeof(struct pachaos_filed_fast_request) * PACHAOS_FILED_FAST_REQUEST_CAPACITY);
    if (header->request_tail - header->request_head >= header->request_capacity) {
        return -11;
    }

    const uint64_t enqueue_start = now_ns();
    const uint64_t enqueue_start_cycles = read_tsc();
    uint64_t tail = header->request_tail;
    struct pachaos_filed_fast_request *fast_request = &requests[tail % header->request_capacity];
    memset(fast_request, 0, sizeof(*fast_request));
    fast_request->request_id = request_id;
    fast_request->opcode = op;
    fast_request->word2 = word2;
    fast_request->payload_slot = 0;
    fast_request->payload_length = PACHAOS_FILED_PAGE_BYTES;
    __sync_synchronize();
    header->request_tail = tail + 1;
    const uint64_t enqueue_end_cycles = read_tsc();
    const uint64_t enqueue_end = now_ns();
    if (breakdown != NULL && enqueue_end >= enqueue_start) {
        breakdown->enqueue_ns += enqueue_end - enqueue_start;
        if (enqueue_start_cycles != 0 && enqueue_end_cycles >= enqueue_start_cycles) {
            breakdown->enqueue_cycles += enqueue_end_cycles - enqueue_start_cycles;
        }
    }

    struct pachaos_raw_ipc_msg request;
    memset(&request, 0, sizeof(request));
    memset(reply, 0, sizeof(*reply));
    request.word0 = PACHAOS_FILED_REQUEST_MAGIC;
    request.word1 = PACHAOS_FILED_OP_FAST_DOORBELL;
    request.word2 = ++header->doorbell_seq;
    request.word3 = request_id;

    const uint64_t send_start = now_ns();
    const uint64_t send_start_cycles = read_tsc();
    long status = ipc_send_wait(session_fd, &request);
    const uint64_t send_end_cycles = read_tsc();
    const uint64_t send_end = now_ns();
    if (breakdown != NULL && send_end >= send_start) {
        breakdown->send_ns += send_end - send_start;
        if (send_start_cycles != 0 && send_end_cycles >= send_start_cycles) {
            breakdown->send_cycles += send_end_cycles - send_start_cycles;
        }
    }
    if (status != 0) return status;

    const uint64_t recv_start = now_ns();
    const uint64_t recv_start_cycles = read_tsc();
    status = ipc_recv_wait(session_fd, reply);
    const uint64_t recv_end_cycles = read_tsc();
    const uint64_t recv_end = now_ns();
    if (breakdown != NULL && recv_end >= recv_start) {
        breakdown->recv_ns += recv_end - recv_start;
        if (recv_start_cycles != 0 && recv_end_cycles >= recv_start_cycles) {
            breakdown->recv_cycles += recv_end_cycles - recv_start_cycles;
        }
    }
    if (status != 0) return status;
    if (reply->word0 != PACHAOS_FILED_REPLY_MAGIC ||
        reply->word1 != 0 ||
        reply->word3 != request_id)
    {
        return -71;
    }

    const uint64_t completion_start = now_ns();
    const uint64_t completion_start_cycles = read_tsc();
    if (header->completion_head == header->completion_tail) return -71;
    __sync_synchronize();
    struct pachaos_filed_fast_completion *completion =
        &completions[header->completion_head % header->completion_capacity];
    if (completion->request_id != request_id) return -71;
    reply->word0 = PACHAOS_FILED_REPLY_MAGIC;
    reply->word1 = (uint64_t)completion->status;
    reply->word2 = completion->result;
    reply->word3 = completion->request_id;
    header->completion_head++;
    const uint64_t completion_end_cycles = read_tsc();
    const uint64_t completion_end = now_ns();
    if (breakdown != NULL && completion_end >= completion_start) {
        breakdown->completion_ns += completion_end - completion_start;
        if (completion_start_cycles != 0 && completion_end_cycles >= completion_start_cycles) {
            breakdown->completion_cycles += completion_end_cycles - completion_start_cycles;
        }
    }
    return (long)completion->status;
}

static long filed_session_call_for_bench(
    uint64_t session_fd,
    uint64_t session_page_addr,
    uint64_t op,
    uint64_t word2,
    uint64_t request_id,
    struct pachaos_raw_ipc_msg *reply)
{
    return filed_session_call_for_bench_timed(
        session_fd,
        session_page_addr,
        op,
        word2,
        request_id,
        reply,
        NULL);
}

static long filed_raw_open_for_bench(
    uint64_t session_fd,
    uint64_t session_page_addr,
    uint64_t page_addr,
    const char *path,
    uint64_t rights,
    uint64_t open_flags,
    uint64_t *out_handle)
{
    if (page_addr < 4096 || path == NULL || out_handle == NULL) return -22;
    *out_handle = 0;
    memset((void *)(uintptr_t)page_addr, 0, PACHAOS_FILED_PAGE_BYTES);
    struct pachaos_filed_openat *openat = (struct pachaos_filed_openat *)(uintptr_t)page_addr;
    openat->dir_handle = 0;
    openat->rights = rights;
    openat->open_flags = open_flags | PACHAOS_FILED_OPEN_CLOEXEC;
    snprintf(openat->name, sizeof(openat->name), "%s", path);

    struct pachaos_raw_ipc_msg reply;
    long status = filed_session_call_for_bench(
        session_fd,
        session_page_addr,
        PACHAOS_FILED_OP_OPENAT,
        0,
        0x90000000u,
        &reply);
    if (status != 0) return status;
    if (reply.word2 == 0) return -71;
    *out_handle = reply.word2;
    return 0;
}

static void filed_raw_close_for_bench(uint64_t session_fd, uint64_t session_page_addr, uint64_t handle)
{
    if (session_fd < 16 || handle == 0) return;
    struct pachaos_raw_ipc_msg reply;
    (void)filed_session_call_for_bench(
        session_fd,
        session_page_addr,
        PACHAOS_FILED_OP_CLOSE,
        handle,
        0x90000001u,
        &reply);
}

static long filed_raw_truncate_for_bench(
    uint64_t session_fd,
    uint64_t session_page_addr,
    uint64_t page_addr,
    uint64_t handle,
    uint64_t size,
    uint64_t request_id)
{
    if (page_addr < 4096 || handle == 0) return -22;
    memset((void *)(uintptr_t)page_addr, 0, PACHAOS_FILED_PAGE_BYTES);
    struct pachaos_filed_truncate *truncate = (struct pachaos_filed_truncate *)(uintptr_t)page_addr;
    truncate->handle = handle;
    truncate->size = size;
    struct pachaos_raw_ipc_msg reply;
    return filed_session_call_for_bench(
        session_fd,
        session_page_addr,
        PACHAOS_FILED_OP_TRUNCATE,
        0,
        request_id,
        &reply);
}

static long filed_raw_unlink_for_bench(
    uint64_t session_fd,
    uint64_t session_page_addr,
    uint64_t page_addr,
    const char *path,
    uint64_t request_id)
{
    if (page_addr < 4096 || path == NULL) return -22;
    memset((void *)(uintptr_t)page_addr, 0, PACHAOS_FILED_PAGE_BYTES);
    struct pachaos_filed_unlink *unlink = (struct pachaos_filed_unlink *)(uintptr_t)page_addr;
    unlink->dir_handle = 0;
    snprintf(unlink->name, sizeof(unlink->name), "%s", path);
    struct pachaos_raw_ipc_msg reply;
    return filed_session_call_for_bench(
        session_fd,
        session_page_addr,
        PACHAOS_FILED_OP_UNLINK,
        0,
        request_id,
        &reply);
}

static long filed_raw_mkdir_for_bench(
    uint64_t session_fd,
    uint64_t session_page_addr,
    uint64_t page_addr,
    const char *path,
    uint64_t request_id)
{
    if (page_addr < 4096 || path == NULL) return -22;
    memset((void *)(uintptr_t)page_addr, 0, PACHAOS_FILED_PAGE_BYTES);
    struct pachaos_filed_mkdir *mkdir = (struct pachaos_filed_mkdir *)(uintptr_t)page_addr;
    mkdir->dir_handle = 0;
    mkdir->mode = 040755u;
    snprintf(mkdir->name, sizeof(mkdir->name), "%s", path);
    struct pachaos_raw_ipc_msg reply;
    return filed_session_call_for_bench(
        session_fd,
        session_page_addr,
        PACHAOS_FILED_OP_MKDIR,
        0,
        request_id,
        &reply);
}

static long filed_raw_rmdir_for_bench(
    uint64_t session_fd,
    uint64_t session_page_addr,
    uint64_t page_addr,
    const char *path,
    uint64_t request_id)
{
    if (page_addr < 4096 || path == NULL) return -22;
    memset((void *)(uintptr_t)page_addr, 0, PACHAOS_FILED_PAGE_BYTES);
    struct pachaos_filed_rmdir *rmdir = (struct pachaos_filed_rmdir *)(uintptr_t)page_addr;
    rmdir->dir_handle = 0;
    snprintf(rmdir->name, sizeof(rmdir->name), "%s", path);
    struct pachaos_raw_ipc_msg reply;
    return filed_session_call_for_bench(
        session_fd,
        session_page_addr,
        PACHAOS_FILED_OP_RMDIR,
        0,
        request_id,
        &reply);
}

static long filed_raw_rename_for_bench(
    uint64_t session_fd,
    uint64_t session_page_addr,
    uint64_t page_addr,
    const char *old_path,
    const char *new_path,
    uint64_t request_id)
{
    if (page_addr < 4096 || old_path == NULL || new_path == NULL) return -22;
    memset((void *)(uintptr_t)page_addr, 0, PACHAOS_FILED_PAGE_BYTES);
    struct pachaos_filed_rename *rename = (struct pachaos_filed_rename *)(uintptr_t)page_addr;
    rename->old_dir_handle = 0;
    rename->new_dir_handle = 0;
    snprintf(rename->old_name, sizeof(rename->old_name), "%s", old_path);
    snprintf(rename->new_name, sizeof(rename->new_name), "%s", new_path);
    struct pachaos_raw_ipc_msg reply;
    return filed_session_call_for_bench(
        session_fd,
        session_page_addr,
        PACHAOS_FILED_OP_RENAME,
        0,
        request_id,
        &reply);
}

static int run_filed_raw_operation_bench(const char *path)
{
    uint64_t session_fd = 0;
    uint64_t session_page_addr = 0;
    uint64_t session_page_fd = 0;
    long status = filed_session_connect_for_bench(&session_fd, &session_page_addr, &session_page_fd);
    if (status != 0) {
        return ipc_bench_fail("filed raw bench connect", status);
    }
    const uint64_t payload_addr = session_page_addr + PACHAOS_FILED_FAST_PAYLOAD_OFFSET;

    printf("[libc-vfs-exec-smoke] filed raw bench start\n");
    fflush(stdout);

    unsigned long long t0 = now_ns();
    unsigned long long t1 = t0;
    unsigned long long open_close_iters = 0;
    for (;;) {
        uint64_t handle = 0;
        status = filed_raw_open_for_bench(
            session_fd,
            session_page_addr,
            payload_addr,
            path,
            PACHAOS_FILED_RIGHT_READ | PACHAOS_FILED_RIGHT_WRITE | PACHAOS_FILED_RIGHT_STAT,
            0,
            &handle);
        if (status != 0) {
            unmap_raw(session_page_addr, PACHAOS_FILED_SESSION_PAGE_BYTES);
            close_raw_fd(session_page_fd);
            close_raw_fd(session_fd);
            return ipc_bench_fail("filed raw open", status);
        }
        filed_raw_close_for_bench(session_fd, session_page_addr, handle);
        open_close_iters++;
        t1 = now_ns();
        if (bench_done(t0, t1, open_close_iters)) break;
    }
    metric_avg("filed_raw_open_close_roundtrip", t0, t1, open_close_iters);

    uint64_t handle = 0;
    status = filed_raw_open_for_bench(
        session_fd,
        session_page_addr,
        payload_addr,
        path,
        PACHAOS_FILED_RIGHT_READ | PACHAOS_FILED_RIGHT_WRITE | PACHAOS_FILED_RIGHT_STAT,
        0,
        &handle);
    if (status != 0) {
        unmap_raw(session_page_addr, PACHAOS_FILED_SESSION_PAGE_BYTES);
        close_raw_fd(session_page_fd);
        close_raw_fd(session_fd);
        return ipc_bench_fail("filed raw open steady", status);
    }

    struct pachaos_raw_ipc_msg reply;
    unsigned long long stat_iters = 0;
    t0 = now_ns();
    for (;;) {
        memset((void *)(uintptr_t)payload_addr, 0, PACHAOS_FILED_PAGE_BYTES);
        struct pachaos_filed_statx *st = (struct pachaos_filed_statx *)(uintptr_t)payload_addr;
        st->handle = handle;
        status = filed_session_call_for_bench(
            session_fd,
            session_page_addr,
            PACHAOS_FILED_OP_STAT,
            0,
            0x91000000u + stat_iters,
            &reply);
        if (status != 0 || st->size == 0) {
            filed_raw_close_for_bench(session_fd, session_page_addr, handle);
            unmap_raw(session_page_addr, PACHAOS_FILED_SESSION_PAGE_BYTES);
            close_raw_fd(session_page_fd);
            close_raw_fd(session_fd);
            return ipc_bench_fail("filed raw stat", status != 0 ? status : -71);
        }
        stat_iters++;
        t1 = now_ns();
        if (bench_done(t0, t1, stat_iters)) break;
    }
    metric_avg("filed_raw_stat_roundtrip", t0, t1, stat_iters);

    unsigned long long pread_iters = 0;
    t0 = now_ns();
    for (;;) {
        memset((void *)(uintptr_t)payload_addr, 0, PACHAOS_FILED_PAGE_BYTES);
        struct pachaos_filed_io *io = (struct pachaos_filed_io *)(uintptr_t)payload_addr;
        io->handle = handle;
        io->offset = 0;
        io->length = 5;
        status = filed_session_call_for_bench(
            session_fd,
            session_page_addr,
            PACHAOS_FILED_OP_PREAD,
            0,
            0x92000000u + pread_iters,
            &reply);
        if (status != 0 || reply.word2 != 5) {
            filed_raw_close_for_bench(session_fd, session_page_addr, handle);
            unmap_raw(session_page_addr, PACHAOS_FILED_SESSION_PAGE_BYTES);
            close_raw_fd(session_page_fd);
            close_raw_fd(session_fd);
            return ipc_bench_fail("filed raw pread", status != 0 ? status : -71);
        }
        pread_iters++;
        t1 = now_ns();
        if (bench_done(t0, t1, pread_iters)) break;
    }
    metric_avg("filed_raw_pread_roundtrip", t0, t1, pread_iters);

    const char *rewrite = "BETA\n";
    unsigned long long pwrite_iters = 0;
    t0 = now_ns();
    for (;;) {
        memset((void *)(uintptr_t)payload_addr, 0, PACHAOS_FILED_PAGE_BYTES);
        struct pachaos_filed_io *io = (struct pachaos_filed_io *)(uintptr_t)payload_addr;
        io->handle = handle;
        io->offset = 6;
        io->length = strlen(rewrite);
        memcpy(io->data, rewrite, strlen(rewrite));
        status = filed_session_call_for_bench(
            session_fd,
            session_page_addr,
            PACHAOS_FILED_OP_PWRITE,
            0,
            0x93000000u + pwrite_iters,
            &reply);
        if (status != 0 || reply.word2 != strlen(rewrite)) {
            filed_raw_close_for_bench(session_fd, session_page_addr, handle);
            unmap_raw(session_page_addr, PACHAOS_FILED_SESSION_PAGE_BYTES);
            close_raw_fd(session_page_fd);
            close_raw_fd(session_fd);
            return ipc_bench_fail("filed raw pwrite", status != 0 ? status : -71);
        }
        pwrite_iters++;
        t1 = now_ns();
        if (bench_done(t0, t1, pwrite_iters)) break;
    }
    metric_avg("filed_raw_pwrite_roundtrip", t0, t1, pwrite_iters);

    filed_raw_close_for_bench(session_fd, session_page_addr, handle);

    const unsigned long long mutation_iters = 8;
    unsigned long long create_close_total_ns = 0;
    unsigned long long truncate_total_ns = 0;
    unsigned long long truncate_supported_iters = 0;
    unsigned long long truncate_skipped = 0;
    unsigned long long rename_total_ns = 0;
    unsigned long long rename_supported_iters = 0;
    unsigned long long rename_skipped = 0;
    unsigned long long unlink_total_ns = 0;
    unsigned long long unlink_supported_iters = 0;
    unsigned long long unlink_skipped = 0;
    const unsigned long long mutation_tag = now_ns();
    for (unsigned long long i = 0; i < mutation_iters; ++i) {
        char create_path[96];
        char rename_path[96];
        snprintf(create_path, sizeof(create_path), "/tmp/rawbench-file-%llx-%llu.tmp", mutation_tag, i);
        snprintf(rename_path, sizeof(rename_path), "/tmp/rawbench-file-%llx-%llu.renamed", mutation_tag, i);
        (void)filed_raw_unlink_for_bench(
            session_fd,
            session_page_addr,
            payload_addr,
            create_path,
            0x99000000u + i);
        (void)filed_raw_unlink_for_bench(
            session_fd,
            session_page_addr,
            payload_addr,
            rename_path,
            0x99100000u + i);
        uint64_t created = 0;
        t0 = now_ns();
        status = filed_raw_open_for_bench(
            session_fd,
            session_page_addr,
            payload_addr,
            create_path,
            PACHAOS_FILED_RIGHT_READ |
                PACHAOS_FILED_RIGHT_WRITE |
                PACHAOS_FILED_RIGHT_STAT |
                PACHAOS_FILED_RIGHT_CREATE,
            PACHAOS_FILED_OPEN_CREATE |
                PACHAOS_FILED_OPEN_EXCLUSIVE |
                PACHAOS_FILED_OPEN_TRUNCATE,
            &created);
        t1 = now_ns();
        if (status != 0 || created == 0) {
            unmap_raw(session_page_addr, PACHAOS_FILED_SESSION_PAGE_BYTES);
            close_raw_fd(session_page_fd);
            close_raw_fd(session_fd);
            return ipc_bench_fail("filed raw create", status != 0 ? status : -71);
        }
        create_close_total_ns += t1 - t0;

        t0 = now_ns();
        status = filed_raw_truncate_for_bench(
            session_fd,
            session_page_addr,
            payload_addr,
            created,
            4,
            0x94000000u + i);
        t1 = now_ns();
        if (status != 0) {
            if (status == -95) {
                truncate_skipped++;
            } else {
                filed_raw_close_for_bench(session_fd, session_page_addr, created);
                unmap_raw(session_page_addr, PACHAOS_FILED_SESSION_PAGE_BYTES);
                close_raw_fd(session_page_fd);
                close_raw_fd(session_fd);
                return ipc_bench_fail("filed raw truncate", status);
            }
        } else {
            truncate_total_ns += t1 - t0;
            truncate_supported_iters++;
        }
        filed_raw_close_for_bench(session_fd, session_page_addr, created);

        t0 = now_ns();
        status = filed_raw_rename_for_bench(
            session_fd,
            session_page_addr,
            payload_addr,
            create_path,
            rename_path,
            0x95000000u + i);
        t1 = now_ns();
        const char *unlink_path = rename_path;
        if (status == -95) {
            rename_skipped++;
            unlink_path = create_path;
        } else if (status != 0) {
            unmap_raw(session_page_addr, PACHAOS_FILED_SESSION_PAGE_BYTES);
            close_raw_fd(session_page_fd);
            close_raw_fd(session_fd);
            return ipc_bench_fail("filed raw rename", status);
        } else {
            rename_total_ns += t1 - t0;
            rename_supported_iters++;
        }

        t0 = now_ns();
        status = filed_raw_unlink_for_bench(
            session_fd,
            session_page_addr,
            payload_addr,
            unlink_path,
            0x96000000u + i);
        t1 = now_ns();
        if (status == -95) {
            unlink_skipped++;
        } else if (status != 0) {
            unmap_raw(session_page_addr, PACHAOS_FILED_SESSION_PAGE_BYTES);
            close_raw_fd(session_page_fd);
            close_raw_fd(session_fd);
            return ipc_bench_fail("filed raw unlink", status);
        } else {
            unlink_total_ns += t1 - t0;
            unlink_supported_iters++;
        }
    }
    metric_total_avg("filed_raw_create_open_roundtrip", create_close_total_ns, mutation_iters);
    if (truncate_supported_iters > 0) {
        metric_total_avg("filed_raw_truncate_roundtrip", truncate_total_ns, truncate_supported_iters);
    }
    printf("[libc-vfs-exec-smoke] metric op=filed_raw_truncate_skipped iterations=%llu total=%llu avg=%llu\n",
        truncate_skipped,
        truncate_skipped,
        truncate_skipped == 0 ? 0ull : 1ull);
    if (rename_supported_iters > 0) {
        metric_total_avg("filed_raw_rename_roundtrip", rename_total_ns, rename_supported_iters);
    }
    printf("[libc-vfs-exec-smoke] metric op=filed_raw_rename_skipped iterations=%llu total=%llu avg=%llu\n",
        rename_skipped,
        rename_skipped,
        rename_skipped == 0 ? 0ull : 1ull);
    if (unlink_supported_iters > 0) {
        metric_total_avg("filed_raw_unlink_roundtrip", unlink_total_ns, unlink_supported_iters);
    }
    printf("[libc-vfs-exec-smoke] metric op=filed_raw_unlink_skipped iterations=%llu total=%llu avg=%llu\n",
        unlink_skipped,
        unlink_skipped,
        unlink_skipped == 0 ? 0ull : 1ull);

    unsigned long long mkdir_total_ns = 0;
    unsigned long long mkdir_supported_iters = 0;
    unsigned long long mkdir_skipped = 0;
    unsigned long long rmdir_total_ns = 0;
    unsigned long long rmdir_supported_iters = 0;
    unsigned long long rmdir_skipped = 0;
    for (unsigned long long i = 0; i < mutation_iters; ++i) {
        char dir_path[96];
        snprintf(dir_path, sizeof(dir_path), "/tmp/rawbench-dir-%llx-%llu", mutation_tag, i);
        (void)filed_raw_rmdir_for_bench(
            session_fd,
            session_page_addr,
            payload_addr,
            dir_path,
            0x99200000u + i);
        t0 = now_ns();
        status = filed_raw_mkdir_for_bench(
            session_fd,
            session_page_addr,
            payload_addr,
            dir_path,
            0x97000000u + i);
        t1 = now_ns();
        if (status == -95) {
            mkdir_skipped++;
            continue;
        }
        if (status != 0) {
            unmap_raw(session_page_addr, PACHAOS_FILED_SESSION_PAGE_BYTES);
            close_raw_fd(session_page_fd);
            close_raw_fd(session_fd);
            return ipc_bench_fail("filed raw mkdir", status);
        }
        mkdir_total_ns += t1 - t0;
        mkdir_supported_iters++;

        t0 = now_ns();
        status = filed_raw_rmdir_for_bench(
            session_fd,
            session_page_addr,
            payload_addr,
            dir_path,
            0x98000000u + i);
        t1 = now_ns();
        if (status == -95) {
            rmdir_skipped++;
            continue;
        }
        if (status != 0) {
            unmap_raw(session_page_addr, PACHAOS_FILED_SESSION_PAGE_BYTES);
            close_raw_fd(session_page_fd);
            close_raw_fd(session_fd);
            return ipc_bench_fail("filed raw rmdir", status);
        }
        rmdir_total_ns += t1 - t0;
        rmdir_supported_iters++;
    }
    if (mkdir_supported_iters > 0) {
        metric_total_avg("filed_raw_mkdir_roundtrip", mkdir_total_ns, mkdir_supported_iters);
    }
    printf("[libc-vfs-exec-smoke] metric op=filed_raw_mkdir_skipped iterations=%llu total=%llu avg=%llu\n",
        mkdir_skipped,
        mkdir_skipped,
        mkdir_skipped == 0 ? 0ull : 1ull);
    if (rmdir_supported_iters > 0) {
        metric_total_avg("filed_raw_rmdir_roundtrip", rmdir_total_ns, rmdir_supported_iters);
    }
    printf("[libc-vfs-exec-smoke] metric op=filed_raw_rmdir_skipped iterations=%llu total=%llu avg=%llu\n",
        rmdir_skipped,
        rmdir_skipped,
        rmdir_skipped == 0 ? 0ull : 1ull);

    unmap_raw(session_page_addr, PACHAOS_FILED_SESSION_PAGE_BYTES);
    close_raw_fd(session_page_fd);
    close_raw_fd(session_fd);
    printf("[libc-vfs-exec-smoke] filed raw bench ok\n");
    fflush(stdout);
    return 0;
}

static int run_ipc_microbench(void)
{
    const uint64_t channel_rights =
        PACHAOS_FD_RIGHT_INSPECT |
        PACHAOS_FD_RIGHT_WAIT |
        PACHAOS_FD_RIGHT_POLL |
        PACHAOS_FD_RIGHT_CLOSE |
        PACHAOS_FD_RIGHT_SEND |
        PACHAOS_FD_RIGHT_RECV |
        PACHAOS_FD_RIGHT_TRANSFER;

    printf("[libc-vfs-exec-smoke] ipc bench start\n");
    fflush(stdout);

    volatile uint64_t sink = 0;
    const int empty_iters = 8192;
    unsigned long long t0 = now_ns();
    for (int i = 0; i < empty_iters; ++i) {
        sink += (uint64_t)i;
    }
    unsigned long long t1 = now_ns();
    metric_avg("ipc_empty_loop", t0, t1, empty_iters);

    const int create_iters = 256;
    t0 = now_ns();
    for (int i = 0; i < create_iters; ++i) {
        uint64_t pair[2] = {0, 0};
        long status = pachaos_raw3(
            PACHAOS_SYSCALL_IPC_CHANNEL_CREATE,
            (long)pair,
            (long)channel_rights,
            PACHAOS_FD_FLAG_CLOEXEC);
        if (status != 0 || pair[0] < 16 || pair[1] < 16) {
            close_raw_fd(pair[0]);
            close_raw_fd(pair[1]);
            return ipc_bench_fail("ipc channel create", status != 0 ? status : -22);
        }
        close_raw_fd(pair[0]);
        close_raw_fd(pair[1]);
    }
    t1 = now_ns();
    metric_avg("ipc_channel_create_close", t0, t1, create_iters);

    uint64_t pair[2] = {0, 0};
    long status = pachaos_raw3(
        PACHAOS_SYSCALL_IPC_CHANNEL_CREATE,
        (long)pair,
        (long)channel_rights,
        PACHAOS_FD_FLAG_CLOEXEC);
    if (status != 0 || pair[0] < 16 || pair[1] < 16) {
        close_raw_fd(pair[0]);
        close_raw_fd(pair[1]);
        return ipc_bench_fail("ipc channel create steady", status != 0 ? status : -22);
    }

    struct pachaos_raw_ipc_msg request;
    struct pachaos_raw_ipc_msg reply;
    memset(&request, 0, sizeof(request));
    memset(&reply, 0, sizeof(reply));

    for (int i = 0; i < 16; ++i) {
        request.word0 = 0x49504342454e4348ull;
        request.word1 = (uint64_t)i;
        status = pachaos_raw2(PACHAOS_SYSCALL_IPC_SEND, (long)pair[0], (long)&request);
        if (status != 0) {
            close_raw_fd(pair[0]);
            close_raw_fd(pair[1]);
            return ipc_bench_fail("ipc warm send", status);
        }
        memset(&reply, 0, sizeof(reply));
        status = pachaos_raw2(PACHAOS_SYSCALL_IPC_RECV, (long)pair[1], (long)&reply);
        if (status != 0 || reply.word1 != (uint64_t)i) {
            close_raw_fd(pair[0]);
            close_raw_fd(pair[1]);
            return ipc_bench_fail("ipc warm recv", status != 0 ? status : -71);
        }
    }

    const int msg_iters = 8192;
    t0 = now_ns();
    for (int i = 0; i < msg_iters; ++i) {
        request.word0 = 0x49504342454e4348ull;
        request.word1 = (uint64_t)i;
        request.word2 = sink;
        request.word3 = (uint64_t)(i ^ 0x5a5a);
        status = pachaos_raw2(PACHAOS_SYSCALL_IPC_SEND, (long)pair[0], (long)&request);
        if (status != 0) {
            close_raw_fd(pair[0]);
            close_raw_fd(pair[1]);
            return ipc_bench_fail("ipc send", status);
        }
        memset(&reply, 0, sizeof(reply));
        status = pachaos_raw2(PACHAOS_SYSCALL_IPC_RECV, (long)pair[1], (long)&reply);
        if (status != 0 ||
            reply.word0 != request.word0 ||
            reply.word1 != request.word1 ||
            reply.word2 != request.word2 ||
            reply.word3 != request.word3)
        {
            close_raw_fd(pair[0]);
            close_raw_fd(pair[1]);
            return ipc_bench_fail("ipc recv", status != 0 ? status : -71);
        }
    }
    t1 = now_ns();
    metric_avg("ipc_send_recv_pair", t0, t1, msg_iters);
    metric_avg("ipc_send_or_recv_syscall", t0, t1, msg_iters * 2);

    close_raw_fd(pair[0]);
    close_raw_fd(pair[1]);

    const int hello_iters = 256;
    t0 = now_ns();
    for (int i = 0; i < hello_iters; ++i) {
        status = filed_endpoint_hello_once((uint64_t)(0x1100 + i));
        if (status != 0) {
            return ipc_bench_fail("filed endpoint hello", status);
        }
    }
    t1 = now_ns();
    metric_avg("filed_endpoint_hello_roundtrip", t0, t1, hello_iters);

    uint64_t session_fd = 0;
    uint64_t session_page_addr = 0;
    uint64_t session_page_fd = 0;
    status = filed_session_connect_for_bench(&session_fd, &session_page_addr, &session_page_fd);
    if (status != 0) {
        return ipc_bench_fail("filed session connect", status);
    }

    const int ping_iters = 2048;
    struct pachaos_filed_call_breakdown ping_breakdown;
    memset(&ping_breakdown, 0, sizeof(ping_breakdown));
    t0 = now_ns();
    for (int i = 0; i < ping_iters; ++i) {
        memset(&reply, 0, sizeof(reply));
        const uint64_t ping_value = (uint64_t)(0x70000000u + i);
        const uint64_t request_id = (uint64_t)(0x2100 + i);
        status = filed_session_call_for_bench_timed(
            session_fd,
            session_page_addr,
            PACHAOS_FILED_OP_PING,
            ping_value,
            request_id,
            &reply,
            &ping_breakdown);
        if (status != 0 ||
            reply.word0 != PACHAOS_FILED_REPLY_MAGIC ||
            reply.word1 != 0 ||
            reply.word2 != ping_value ||
            reply.word3 != request_id)
        {
            unmap_raw(session_page_addr, PACHAOS_FILED_SESSION_PAGE_BYTES);
            close_raw_fd(session_page_fd);
            close_raw_fd(session_fd);
            return ipc_bench_fail("filed session ping", status != 0 ? status : -71);
        }
    }
    t1 = now_ns();
    metric_avg("filed_session_ping_roundtrip", t0, t1, ping_iters);
    metric_avg("filed_session_ping_send_or_recv", t0, t1, ping_iters * 2);
    metric_total_avg("filed_session_ping.enqueue", ping_breakdown.enqueue_ns, ping_iters);
    metric_total_avg("filed_session_ping.send", ping_breakdown.send_ns, ping_iters);
    metric_total_avg("filed_session_ping.recv", ping_breakdown.recv_ns, ping_iters);
    metric_total_avg("filed_session_ping.completion", ping_breakdown.completion_ns, ping_iters);
    metric_total_avg_cycles("filed_session_ping.enqueue_cycles", ping_breakdown.enqueue_cycles, ping_iters);
    metric_total_avg_cycles("filed_session_ping.send_cycles", ping_breakdown.send_cycles, ping_iters);
    metric_total_avg_cycles("filed_session_ping.recv_cycles", ping_breakdown.recv_cycles, ping_iters);
    metric_total_avg_cycles("filed_session_ping.completion_cycles", ping_breakdown.completion_cycles, ping_iters);

    struct pachaos_filed_call_breakdown ping_forever_breakdown;
    memset(&ping_forever_breakdown, 0, sizeof(ping_forever_breakdown));
    t0 = now_ns();
    for (int i = 0; i < ping_iters; ++i) {
        memset(&reply, 0, sizeof(reply));
        const uint64_t ping_value = (uint64_t)(0x71000000u + i);
        const uint64_t request_id = (uint64_t)(0x3100 + i);
        status = filed_session_call_for_bench_timed(
            session_fd,
            session_page_addr,
            PACHAOS_FILED_OP_PING,
            ping_value,
            request_id,
            &reply,
            &ping_forever_breakdown);
        if (status != 0 ||
            reply.word0 != PACHAOS_FILED_REPLY_MAGIC ||
            reply.word1 != 0 ||
            reply.word2 != ping_value ||
            reply.word3 != request_id)
        {
            unmap_raw(session_page_addr, PACHAOS_FILED_SESSION_PAGE_BYTES);
            close_raw_fd(session_page_fd);
            close_raw_fd(session_fd);
            return ipc_bench_fail("filed session ping forever", status != 0 ? status : -71);
        }
    }
    t1 = now_ns();
    metric_avg("filed_session_ping_forever_roundtrip", t0, t1, ping_iters);
    metric_avg("filed_session_ping_forever_send_or_recv", t0, t1, ping_iters * 2);
    metric_total_avg("filed_session_ping_forever.enqueue", ping_forever_breakdown.enqueue_ns, ping_iters);
    metric_total_avg("filed_session_ping_forever.send", ping_forever_breakdown.send_ns, ping_iters);
    metric_total_avg("filed_session_ping_forever.recv", ping_forever_breakdown.recv_ns, ping_iters);
    metric_total_avg("filed_session_ping_forever.completion", ping_forever_breakdown.completion_ns, ping_iters);
    metric_total_avg_cycles("filed_session_ping_forever.enqueue_cycles", ping_forever_breakdown.enqueue_cycles, ping_iters);
    metric_total_avg_cycles("filed_session_ping_forever.send_cycles", ping_forever_breakdown.send_cycles, ping_iters);
    metric_total_avg_cycles("filed_session_ping_forever.recv_cycles", ping_forever_breakdown.recv_cycles, ping_iters);
    metric_total_avg_cycles("filed_session_ping_forever.completion_cycles", ping_forever_breakdown.completion_cycles, ping_iters);

    unmap_raw(session_page_addr, PACHAOS_FILED_SESSION_PAGE_BYTES);
    close_raw_fd(session_page_fd);
    close_raw_fd(session_fd);
    printf("[libc-vfs-exec-smoke] ipc bench ok\n");
    fflush(stdout);
    return 0;
}
#else
static int run_ipc_microbench(void)
{
    printf("[libc-vfs-exec-smoke] ipc bench skipped on linux\n");
    fflush(stdout);
    return 0;
}

static int run_filed_raw_operation_bench(const char *path)
{
    (void)path;
    printf("[libc-vfs-exec-smoke] filed raw bench skipped on linux\n");
    fflush(stdout);
    return 0;
}
#endif

static int run_child(int argc, char **argv, char **envp)
{
    unsigned long long child_start_ns = now_ns();
    printf("[libc-vfs-exec-smoke] child start\n");
    fflush(stdout);
    if (argc != 2 || strcmp(argv[1], "--child") != 0) {
        return fail("child argv");
    }
    int saw_child = 0;
    int saw_exec_start = 0;
    for (char **it = envp; it != NULL && *it != NULL; ++it) {
        const char *prefix = "PACHA_LIBC_VFS_EXEC_START_NS=";
        const size_t prefix_len = strlen(prefix);
        if (strncmp(*it, prefix, prefix_len) == 0) {
            unsigned long long exec_start_ns = strtoull(*it + prefix_len, NULL, 10);
            if (exec_start_ns != 0 && child_start_ns >= exec_start_ns) {
                metric("execve_to_child_start", exec_start_ns, child_start_ns);
                saw_exec_start = 1;
            }
        }
        if (strcmp(*it, "PACHA_LIBC_VFS_EXEC_CHILD=1") == 0) {
            saw_child = 1;
        }
    }
    if (!saw_child) return fail("child env");
    if (!saw_exec_start) {
        printf("[libc-vfs-exec-smoke] metric op=execve_to_child_start ns=missing\n");
        fflush(stdout);
    }
    printf("[libc-vfs-exec-smoke] child ok\n");
    fflush(stdout);
    return 0;
}

static void fill_stress_page(char *page, size_t page_size, unsigned long long file_index, unsigned long long page_index)
{
    for (size_t i = 0; i < page_size; i++) {
        page[i] = (char)('A' + ((file_index + page_index + i) % 26));
    }
}

static int run_fs_write_stress(void)
{
    enum {
        stress_files = 16,
        stress_pages_per_file = 8,
        stress_page_size = 4096,
    };
    char page[stress_page_size];
    char readback[stress_page_size];
    unsigned long long create_ns = 0;
    unsigned long long write_ns = 0;
    unsigned long long read_ns = 0;
    unsigned long long truncate_ns = 0;
    unsigned long long truncate_skipped = 0;
    unsigned long long close_ns = 0;
    unsigned long long rename_ns = 0;
    unsigned long long rename_skipped = 0;
    unsigned long long reopen_ns = 0;
    unsigned long long unlink_ns = 0;
    unsigned long long unlink_skipped = 0;
    unsigned long long checksum = 0;
    const unsigned long long tag = now_ns();
    unsigned long long t0 = now_ns();

    for (unsigned long long file_index = 0; file_index < stress_files; file_index++) {
        char path[128];
        char renamed[128];
        snprintf(path, sizeof(path), "/tmp/libc-vfs-write-stress-%llx-%llu.tmp", tag, file_index);
        snprintf(renamed, sizeof(renamed), "/tmp/libc-vfs-write-stress-%llx-%llu.renamed", tag, file_index);
        (void)unlink(path);
        (void)unlink(renamed);

        unsigned long long part0 = now_ns();
        int fd = open(path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
        unsigned long long part1 = now_ns();
        create_ns += part1 - part0;
        if (fd < 0) return fail("fs write stress create");

        for (unsigned long long page_index = 0; page_index < stress_pages_per_file; page_index++) {
            fill_stress_page(page, sizeof(page), file_index, page_index);
            part0 = now_ns();
            ssize_t wrote = write(fd, page, sizeof(page));
            part1 = now_ns();
            write_ns += part1 - part0;
            if (wrote != (ssize_t)sizeof(page)) {
                (void)close(fd);
                return fail("fs write stress write");
            }
        }

        if (lseek(fd, 0, SEEK_SET) != 0) {
            (void)close(fd);
            return fail("fs write stress seek readback");
        }
        memset(readback, 0, sizeof(readback));
        part0 = now_ns();
        ssize_t got = read(fd, readback, sizeof(readback));
        part1 = now_ns();
        read_ns += part1 - part0;
        if (got != (ssize_t)sizeof(readback)) {
            (void)close(fd);
            return fail("fs write stress readback");
        }
        fill_stress_page(page, sizeof(page), file_index, 0);
        if (memcmp(page, readback, sizeof(page)) != 0) {
            (void)close(fd);
            return fail("fs write stress content");
        }
        checksum += (unsigned char)readback[0];
        checksum += (unsigned char)readback[sizeof(readback) - 1];

        off_t expected_size = (off_t)(stress_page_size * stress_pages_per_file);
        part0 = now_ns();
        if (ftruncate(fd, (off_t)(stress_page_size * 4)) == 0) {
            part1 = now_ns();
            truncate_ns += part1 - part0;
            expected_size = (off_t)(stress_page_size * 4);
        } else if (errno == ENOSYS || errno == ENOTSUP || errno == EOPNOTSUPP) {
            part1 = now_ns();
            truncate_ns += part1 - part0;
            truncate_skipped++;
        } else {
            (void)close(fd);
            return fail("fs write stress truncate");
        }

        part0 = now_ns();
        if (close(fd) != 0) return fail("fs write stress close");
        part1 = now_ns();
        close_ns += part1 - part0;

        const char *final_path = renamed;
        part0 = now_ns();
        if (rename(path, renamed) == 0) {
            part1 = now_ns();
            rename_ns += part1 - part0;
        } else if (errno == ENOSYS || errno == ENOTSUP || errno == EOPNOTSUPP) {
            part1 = now_ns();
            rename_ns += part1 - part0;
            rename_skipped++;
            final_path = path;
        } else {
            return fail("fs write stress rename");
        }

        part0 = now_ns();
        fd = open(final_path, O_RDONLY | O_CLOEXEC);
        part1 = now_ns();
        reopen_ns += part1 - part0;
        if (fd < 0) return fail("fs write stress reopen");
        struct stat st;
        memset(&st, 0, sizeof(st));
        if (fstat(fd, &st) != 0 || st.st_size != expected_size) {
            (void)close(fd);
            return fail("fs write stress stat");
        }
        if (close(fd) != 0) return fail("fs write stress close reopen");

        part0 = now_ns();
        if (unlink(final_path) == 0) {
            part1 = now_ns();
            unlink_ns += part1 - part0;
        } else if (errno == ENOSYS || errno == ENOTSUP || errno == EOPNOTSUPP) {
            part1 = now_ns();
            unlink_ns += part1 - part0;
            unlink_skipped++;
        } else {
            return fail("fs write stress unlink");
        }
    }

    unsigned long long t1 = now_ns();
    metric_avg("fs_write_stress", t0, t1, stress_files);
    metric_total_avg("fs_write_stress.create", create_ns, stress_files);
    metric_total_avg("fs_write_stress.write_page", write_ns, stress_files * stress_pages_per_file);
    metric_total_avg("fs_write_stress.readback", read_ns, stress_files);
    metric_total_avg("fs_write_stress.truncate", truncate_ns, stress_files);
    printf(
        "[libc-vfs-exec-smoke] metric op=fs_write_stress.truncate_skipped iterations=%d total=%llu avg=%llu\n",
        stress_files,
        truncate_skipped,
        truncate_skipped == 0 ? 0ull : 1ull);
    metric_total_avg("fs_write_stress.close", close_ns, stress_files);
    metric_total_avg("fs_write_stress.rename", rename_ns, stress_files);
    printf(
        "[libc-vfs-exec-smoke] metric op=fs_write_stress.rename_skipped iterations=%d total=%llu avg=%llu\n",
        stress_files,
        rename_skipped,
        rename_skipped == 0 ? 0ull : 1ull);
    metric_total_avg("fs_write_stress.reopen", reopen_ns, stress_files);
    metric_total_avg("fs_write_stress.unlink", unlink_ns, stress_files);
    printf(
        "[libc-vfs-exec-smoke] metric op=fs_write_stress.unlink_skipped iterations=%d total=%llu avg=%llu\n",
        stress_files,
        unlink_skipped,
        unlink_skipped == 0 ? 0ull : 1ull);
    printf("[libc-vfs-exec-smoke] metric op=fs_write_stress_checksum total=%llu\n", checksum);
    fflush(stdout);
    return 0;
}

static int run_vfs(void)
{
    const char *path = "/tmp/libc-vfs-exec-smoke.txt";
    const char *first = "alpha\n";
    const char *second = "beta\n";
    char buf[64];
    struct stat st;

    printf("[libc-vfs-exec-smoke] vfs start\n");
    fflush(stdout);
    unsigned long long t0 = now_ns();
    unsigned long long t1 = now_ns();
    metric("clock_pair", t0, t1);

    const char *ipc_bench = getenv("PACHA_LIBC_VFS_IPC_BENCH");
    if (ipc_bench != NULL && strcmp(ipc_bench, "1") == 0) {
        if (run_ipc_microbench() != 0) return 1;
    }

    t0 = now_ns();
    FILE *f = fopen(path, "w+");
    t1 = now_ns();
    metric("fopen_wplus", t0, t1);
    if (f == NULL) return fail("fopen w+");

    t0 = now_ns();
    if (fwrite(first, 1, strlen(first), f) != strlen(first)) return fail("fwrite first");
    t1 = now_ns();
    metric("fwrite_first", t0, t1);

    t0 = now_ns();
    if (fflush(f) != 0) return fail("fflush first");
    t1 = now_ns();
    metric("fflush_first", t0, t1);

    t0 = now_ns();
    if (fseek(f, 0, SEEK_SET) != 0) return fail("fseek set");
    t1 = now_ns();
    metric("fseek_set", t0, t1);

    memset(buf, 0, sizeof(buf));
    t0 = now_ns();
    if (fread(buf, 1, strlen(first), f) != strlen(first)) return fail("fread first");
    t1 = now_ns();
    metric("fread_first", t0, t1);
    if (strcmp(buf, first) != 0) return fail("readback first");

    t0 = now_ns();
    if (fclose(f) != 0) return fail("fclose first");
    t1 = now_ns();
    metric("fclose_first", t0, t1);

    t0 = now_ns();
    f = fopen(path, "a");
    t1 = now_ns();
    metric("fopen_append", t0, t1);
    if (f == NULL) return fail("fopen append");

    t0 = now_ns();
    if (fwrite(second, 1, strlen(second), f) != strlen(second)) return fail("fwrite append");
    t1 = now_ns();
    metric("fwrite_append", t0, t1);

    t0 = now_ns();
    if (fclose(f) != 0) return fail("fclose append");
    t1 = now_ns();
    metric("fclose_append", t0, t1);

    t0 = now_ns();
    int fd = open(path, O_RDWR | O_CLOEXEC);
    t1 = now_ns();
    metric("open_rw_cloexec", t0, t1);
    if (fd < 0) return fail("open rw");

    if (ipc_bench != NULL && strcmp(ipc_bench, "1") == 0) {
        if (run_filed_raw_operation_bench(path) != 0) {
            (void)close(fd);
            return 1;
        }
    }

    t0 = now_ns();
    int fd_flags = fcntl(fd, F_GETFD);
    t1 = now_ns();
    metric("fcntl_getfd", t0, t1);
    if (fd_flags < 0 || (fd_flags & FD_CLOEXEC) == 0) {
        (void)close(fd);
        return fail("fcntl getfd");
    }

    t0 = now_ns();
    if (fcntl(fd, F_SETFD, 0) != 0) {
        (void)close(fd);
        return fail("fcntl setfd");
    }
    t1 = now_ns();
    metric("fcntl_setfd", t0, t1);

    t0 = now_ns();
    fd_flags = fcntl(fd, F_GETFD);
    t1 = now_ns();
    metric("fcntl_getfd_after_set", t0, t1);
    if (fd_flags < 0 || (fd_flags & FD_CLOEXEC) != 0) {
        (void)close(fd);
        return fail("fcntl setfd");
    }

    t0 = now_ns();
    int dup_fd = fcntl(fd, F_DUPFD_CLOEXEC, fd + 3);
    t1 = now_ns();
    metric("fcntl_dupfd_cloexec", t0, t1);
    if (dup_fd < fd + 3) {
        (void)close(fd);
        return fail("fcntl dupfd");
    }

    t0 = now_ns();
    fd_flags = fcntl(dup_fd, F_GETFD);
    t1 = now_ns();
    metric("fcntl_dup_getfd", t0, t1);
    if (fd_flags < 0 || (fd_flags & FD_CLOEXEC) == 0) {
        (void)close(dup_fd);
        (void)close(fd);
        return fail("fcntl dupfd cloexec");
    }

    t0 = now_ns();
    if (close(dup_fd) != 0) {
        (void)close(fd);
        return fail("close dupfd");
    }
    t1 = now_ns();
    metric("close_dupfd", t0, t1);

    t0 = now_ns();
    if (lseek(fd, 0, SEEK_END) != (off_t)(strlen(first) + strlen(second))) {
        (void)close(fd);
        return fail("lseek end");
    }
    t1 = now_ns();
    metric("lseek_end", t0, t1);

    const char *rewrite = "BETA\n";
    t0 = now_ns();
    if (pwrite(fd, rewrite, strlen(rewrite), (off_t)strlen(first)) != (ssize_t)strlen(rewrite)) {
        (void)close(fd);
        return fail("pwrite rewrite");
    }
    t1 = now_ns();
    metric("pwrite_rewrite", t0, t1);

    memset(buf, 0, sizeof(buf));
    t0 = now_ns();
    if (pread(fd, buf, strlen(rewrite), (off_t)strlen(first)) != (ssize_t)strlen(rewrite) ||
        strcmp(buf, rewrite) != 0)
    {
        (void)close(fd);
        return fail("pread rewrite");
    }
    t1 = now_ns();
    metric("pread_rewrite", t0, t1);

    t0 = now_ns();
    if (lseek(fd, 0, SEEK_END) < 0) {
        (void)close(fd);
        return fail("lseek append");
    }
    t1 = now_ns();
    metric("lseek_append", t0, t1);

    const char *vec0 = "vec";
    const char *vec1 = "tor\n";
    struct iovec iov[2];
    iov[0].iov_base = (void *)vec0;
    iov[0].iov_len = strlen(vec0);
    iov[1].iov_base = (void *)vec1;
    iov[1].iov_len = strlen(vec1);
    t0 = now_ns();
    if (writev(fd, iov, 2) != (ssize_t)(strlen(vec0) + strlen(vec1))) {
        (void)close(fd);
        return fail("writev");
    }
    t1 = now_ns();
    metric("writev_append", t0, t1);

    t0 = now_ns();
    if (lseek(fd, 0, SEEK_SET) != 0) {
        (void)close(fd);
        return fail("lseek set");
    }
    t1 = now_ns();
    metric("lseek_set", t0, t1);

    memset(&st, 0, sizeof(st));
    t0 = now_ns();
    if (fstat(fd, &st) != 0 ||
        st.st_size < (off_t)(strlen(first) + strlen(rewrite) + strlen(vec0) + strlen(vec1)))
    {
        (void)close(fd);
        return fail("fstat readback");
    }
    t1 = now_ns();
    metric("fstat_readback", t0, t1);

    memset(buf, 0, sizeof(buf));
    char left[8];
    char right[32];
    memset(left, 0, sizeof(left));
    memset(right, 0, sizeof(right));
    iov[0].iov_base = left;
    iov[0].iov_len = strlen(first);
    iov[1].iov_base = right;
    iov[1].iov_len = sizeof(right) - 1;
    t0 = now_ns();
    ssize_t got = readv(fd, iov, 2);
    t1 = now_ns();
    metric("readv_full", t0, t1);
    if (got < 0) {
        (void)close(fd);
        return fail("read readback");
    }
    snprintf(buf, sizeof(buf), "%s%s", left, right);
    if (strstr(buf, first) == NULL || strstr(buf, rewrite) == NULL || strstr(buf, "vector\n") == NULL) {
        (void)close(fd);
        return fail("content readback");
    }

    t0 = now_ns();
    if (close(fd) != 0) return fail("close readback");
    t1 = now_ns();
    metric("close_readback", t0, t1);

    t0 = now_ns();
    DIR *dir = opendir("/");
    t1 = now_ns();
    metric("opendir_root", t0, t1);
    if (dir == NULL) return fail("opendir /");

    int saw_tmp = 0;
    int entries = 0;
    t0 = now_ns();
    for (;;) {
        struct dirent *de = readdir(dir);
        if (de == NULL) break;
        entries++;
        if (strcmp(de->d_name, "tmp") == 0) saw_tmp = 1;
    }
    t1 = now_ns();
    metric("readdir_root_all", t0, t1);
    printf("[libc-vfs-exec-smoke] metric op=readdir_root_entries count=%d\n", entries);
    fflush(stdout);

    t0 = now_ns();
    if (closedir(dir) != 0) return fail("closedir /");
    t1 = now_ns();
    metric("closedir_root", t0, t1);
    if (!saw_tmp) return fail("readdir tmp");

    unsigned long long open_close_iters = 0;
    unsigned long long open_close_open_ns = 0;
    unsigned long long open_close_close_ns = 0;
    unsigned long long open_close_open_cycles = 0;
    unsigned long long open_close_close_cycles = 0;
    int warm_fd = open(path, O_RDONLY | O_CLOEXEC);
    if (warm_fd < 0) return fail("batch open warmup");
    if (close(warm_fd) != 0) return fail("batch close warmup");
    t0 = now_ns();
    for (;;) {
        unsigned long long part0 = now_ns();
        unsigned long long part0_cycles = read_tsc();
        int loop_fd = open(path, O_RDONLY | O_CLOEXEC);
        unsigned long long part1_cycles = read_tsc();
        unsigned long long part1 = now_ns();
        if (loop_fd < 0) return fail("batch open");
        unsigned long long part2 = now_ns();
        unsigned long long part2_cycles = read_tsc();
        if (close(loop_fd) != 0) return fail("batch close");
        unsigned long long part3_cycles = read_tsc();
        unsigned long long part3 = now_ns();
        open_close_open_ns += part1 - part0;
        open_close_close_ns += part3 - part2;
        if (part1_cycles >= part0_cycles) open_close_open_cycles += part1_cycles - part0_cycles;
        if (part3_cycles >= part2_cycles) open_close_close_cycles += part3_cycles - part2_cycles;
        open_close_iters++;
        t1 = now_ns();
        if (bench_done(t0, t1, open_close_iters)) break;
    }
    metric_avg("batch_open_close", t0, t1, open_close_iters);
    if (tsc_calibration_cycles != 0) {
        metric_total_avg_from_cycles("batch_open_close.open", open_close_open_cycles, open_close_iters);
        metric_total_avg_from_cycles("batch_open_close.close", open_close_close_cycles, open_close_iters);
    } else {
        metric_total_avg("batch_open_close.open", open_close_open_ns, open_close_iters);
        metric_total_avg("batch_open_close.close", open_close_close_ns, open_close_iters);
    }

    int root_dir_fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (root_dir_fd < 0) return fail("batch openat dirfd root");
    unsigned long long openat_dir_iters = 0;
    unsigned long long openat_dir_open_ns = 0;
    unsigned long long openat_dir_close_ns = 0;
    unsigned long long openat_dir_open_cycles = 0;
    unsigned long long openat_dir_close_cycles = 0;
    t0 = now_ns();
    for (;;) {
        unsigned long long part0 = now_ns();
        unsigned long long part0_cycles = read_tsc();
        int tmp_fd = openat(root_dir_fd, "tmp", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        unsigned long long part1_cycles = read_tsc();
        unsigned long long part1 = now_ns();
        if (tmp_fd < 0) {
            (void)close(root_dir_fd);
            return fail("batch openat dirfd tmp");
        }
        unsigned long long part2 = now_ns();
        unsigned long long part2_cycles = read_tsc();
        if (close(tmp_fd) != 0) {
            (void)close(root_dir_fd);
            return fail("batch openat dirfd close");
        }
        unsigned long long part3_cycles = read_tsc();
        unsigned long long part3 = now_ns();
        openat_dir_open_ns += part1 - part0;
        openat_dir_close_ns += part3 - part2;
        if (part1_cycles >= part0_cycles) openat_dir_open_cycles += part1_cycles - part0_cycles;
        if (part3_cycles >= part2_cycles) openat_dir_close_cycles += part3_cycles - part2_cycles;
        openat_dir_iters++;
        t1 = now_ns();
        if (bench_done(t0, t1, openat_dir_iters)) break;
    }
    metric_avg("batch_openat_dirfd_close", t0, t1, openat_dir_iters);
    if (tsc_calibration_cycles != 0) {
        metric_total_avg_from_cycles("batch_openat_dirfd_close.openat", openat_dir_open_cycles, openat_dir_iters);
        metric_total_avg_from_cycles("batch_openat_dirfd_close.close", openat_dir_close_cycles, openat_dir_iters);
    } else {
        metric_total_avg("batch_openat_dirfd_close.openat", openat_dir_open_ns, openat_dir_iters);
        metric_total_avg("batch_openat_dirfd_close.close", openat_dir_close_ns, openat_dir_iters);
    }

#ifndef __pachaos__
    unsigned long long fstatat_dir_iters = 0;
    t0 = now_ns();
    for (;;) {
        if (fstatat(root_dir_fd, "tmp", &st, 0) != 0) {
            (void)close(root_dir_fd);
            return fail("batch fstatat dirfd");
        }
        fstatat_dir_iters++;
        t1 = now_ns();
        if (bench_done(t0, t1, fstatat_dir_iters)) break;
    }
    metric_avg("batch_fstatat_dirfd", t0, t1, fstatat_dir_iters);
#else
    printf("[libc-vfs-exec-smoke] metric op=batch_fstatat_dirfd_skipped iterations=1 total=1 avg=1\n");
    fflush(stdout);
#endif
    if (close(root_dir_fd) != 0) return fail("batch openat root close");

    int tmp_dir_fd = open("/tmp", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (tmp_dir_fd < 0) return fail("batch openat file tmp dir");
    unsigned long long openat_file_iters = 0;
    unsigned long long openat_file_open_ns = 0;
    unsigned long long openat_file_close_ns = 0;
    unsigned long long openat_file_open_cycles = 0;
    unsigned long long openat_file_close_cycles = 0;
    t0 = now_ns();
    for (;;) {
        unsigned long long part0 = now_ns();
        unsigned long long part0_cycles = read_tsc();
        int tmp_file_fd = openat(tmp_dir_fd, "libc-vfs-exec-smoke.txt", O_RDONLY | O_CLOEXEC);
        unsigned long long part1_cycles = read_tsc();
        unsigned long long part1 = now_ns();
        if (tmp_file_fd < 0) {
            (void)close(tmp_dir_fd);
            return fail("batch openat file");
        }
        unsigned long long part2 = now_ns();
        unsigned long long part2_cycles = read_tsc();
        if (close(tmp_file_fd) != 0) {
            (void)close(tmp_dir_fd);
            return fail("batch openat file close");
        }
        unsigned long long part3_cycles = read_tsc();
        unsigned long long part3 = now_ns();
        openat_file_open_ns += part1 - part0;
        openat_file_close_ns += part3 - part2;
        if (part1_cycles >= part0_cycles) openat_file_open_cycles += part1_cycles - part0_cycles;
        if (part3_cycles >= part2_cycles) openat_file_close_cycles += part3_cycles - part2_cycles;
        openat_file_iters++;
        t1 = now_ns();
        if (bench_done(t0, t1, openat_file_iters)) break;
    }
    metric_avg("batch_openat_file_close", t0, t1, openat_file_iters);
    if (tsc_calibration_cycles != 0) {
        metric_total_avg_from_cycles("batch_openat_file_close.openat", openat_file_open_cycles, openat_file_iters);
        metric_total_avg_from_cycles("batch_openat_file_close.close", openat_file_close_cycles, openat_file_iters);
    } else {
        metric_total_avg("batch_openat_file_close.openat", openat_file_open_ns, openat_file_iters);
        metric_total_avg("batch_openat_file_close.close", openat_file_close_ns, openat_file_iters);
    }
    if (close(tmp_dir_fd) != 0) return fail("batch openat file dir close");

#ifndef __pachaos__
    unsigned long long stat_path_iters = 0;
    t0 = now_ns();
    for (;;) {
        if (fstatat(AT_FDCWD, path, &st, 0) != 0) return fail("batch stat path");
        stat_path_iters++;
        t1 = now_ns();
        if (bench_done(t0, t1, stat_path_iters)) break;
    }
    metric_avg("batch_stat_path", t0, t1, stat_path_iters);
#else
    printf("[libc-vfs-exec-smoke] metric op=batch_stat_path_skipped iterations=1 total=1 avg=1\n");
    fflush(stdout);
#endif

    unsigned long long stdio_iters = 0;
    unsigned long long stdio_fopen_ns = 0;
    unsigned long long stdio_fread_ns = 0;
    unsigned long long stdio_fclose_ns = 0;
    unsigned long long stdio_fopen_cycles = 0;
    unsigned long long stdio_fread_cycles = 0;
    unsigned long long stdio_fclose_cycles = 0;
    t0 = now_ns();
    for (;;) {
        unsigned long long part0 = now_ns();
        unsigned long long part0_cycles = read_tsc();
        FILE *loop_file = fopen(path, "r");
        unsigned long long part1_cycles = read_tsc();
        unsigned long long part1 = now_ns();
        if (loop_file == NULL) return fail("batch stdio fopen");
        memset(buf, 0, sizeof(buf));
        unsigned long long part2 = now_ns();
        unsigned long long part2_cycles = read_tsc();
        size_t read_bytes = fread(buf, 1, strlen(first), loop_file);
        unsigned long long part3_cycles = read_tsc();
        unsigned long long part3 = now_ns();
        if (read_bytes != strlen(first)) {
            (void)fclose(loop_file);
            return fail("batch stdio fread");
        }
        unsigned long long part4 = now_ns();
        unsigned long long part4_cycles = read_tsc();
        if (fclose(loop_file) != 0) return fail("batch stdio fclose");
        unsigned long long part5_cycles = read_tsc();
        unsigned long long part5 = now_ns();
        stdio_fopen_ns += part1 - part0;
        stdio_fread_ns += part3 - part2;
        stdio_fclose_ns += part5 - part4;
        if (part1_cycles >= part0_cycles) stdio_fopen_cycles += part1_cycles - part0_cycles;
        if (part3_cycles >= part2_cycles) stdio_fread_cycles += part3_cycles - part2_cycles;
        if (part5_cycles >= part4_cycles) stdio_fclose_cycles += part5_cycles - part4_cycles;
        stdio_iters++;
        t1 = now_ns();
        if (bench_done(t0, t1, stdio_iters)) break;
    }
    metric_avg("batch_stdio_fopen_fread_fclose", t0, t1, stdio_iters);
    if (tsc_calibration_cycles != 0) {
        metric_total_avg_from_cycles("batch_stdio_fopen_fread_fclose.fopen", stdio_fopen_cycles, stdio_iters);
        metric_total_avg_from_cycles("batch_stdio_fopen_fread_fclose.fread", stdio_fread_cycles, stdio_iters);
        metric_total_avg_from_cycles("batch_stdio_fopen_fread_fclose.fclose", stdio_fclose_cycles, stdio_iters);
    } else {
        metric_total_avg("batch_stdio_fopen_fread_fclose.fopen", stdio_fopen_ns, stdio_iters);
        metric_total_avg("batch_stdio_fopen_fread_fclose.fread", stdio_fread_ns, stdio_iters);
        metric_total_avg("batch_stdio_fopen_fread_fclose.fclose", stdio_fclose_ns, stdio_iters);
    }

    fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) return fail("batch open rw");

    unsigned long long dup_close_iters = 0;
    unsigned long long dup_close_dup_ns = 0;
    unsigned long long dup_close_close_ns = 0;
    unsigned long long dup_close_dup_cycles = 0;
    unsigned long long dup_close_close_cycles = 0;
    t0 = now_ns();
    for (;;) {
        unsigned long long part0 = now_ns();
        unsigned long long part0_cycles = read_tsc();
        int loop_dup_fd = fcntl(fd, F_DUPFD_CLOEXEC, fd + 8);
        unsigned long long part1_cycles = read_tsc();
        unsigned long long part1 = now_ns();
        if (loop_dup_fd < fd + 8) {
            (void)close(fd);
            return fail("batch dup close dupfd");
        }
        unsigned long long part2 = now_ns();
        unsigned long long part2_cycles = read_tsc();
        if (close(loop_dup_fd) != 0) {
            (void)close(fd);
            return fail("batch dup close close");
        }
        unsigned long long part3_cycles = read_tsc();
        unsigned long long part3 = now_ns();
        dup_close_dup_ns += part1 - part0;
        dup_close_close_ns += part3 - part2;
        if (part1_cycles >= part0_cycles) dup_close_dup_cycles += part1_cycles - part0_cycles;
        if (part3_cycles >= part2_cycles) dup_close_close_cycles += part3_cycles - part2_cycles;
        dup_close_iters++;
        t1 = now_ns();
        if (bench_done(t0, t1, dup_close_iters)) break;
    }
    metric_avg("batch_fcntl_dupfd_close", t0, t1, dup_close_iters);
    if (tsc_calibration_cycles != 0) {
        metric_total_avg_from_cycles("batch_fcntl_dupfd_close.dupfd", dup_close_dup_cycles, dup_close_iters);
        metric_total_avg_from_cycles("batch_fcntl_dupfd_close.close", dup_close_close_cycles, dup_close_iters);
    } else {
        metric_total_avg("batch_fcntl_dupfd_close.dupfd", dup_close_dup_ns, dup_close_iters);
        metric_total_avg("batch_fcntl_dupfd_close.close", dup_close_close_ns, dup_close_iters);
    }

    unsigned long long fcntl_iters = 0;
    t0 = now_ns();
    for (;;) {
        if (fcntl(fd, F_GETFD) < 0) {
            (void)close(fd);
            return fail("batch fcntl getfd");
        }
        fcntl_iters++;
        t1 = now_ns();
        if (bench_done(t0, t1, fcntl_iters)) break;
    }
    metric_avg("batch_fcntl_getfd", t0, t1, fcntl_iters);

    unsigned long long lseek_iters = 0;
    t0 = now_ns();
    for (;;) {
        if (lseek(fd, 0, SEEK_SET) != 0) {
            (void)close(fd);
            return fail("batch lseek");
        }
        lseek_iters++;
        t1 = now_ns();
        if (bench_done(t0, t1, lseek_iters)) break;
    }
    metric_avg("batch_lseek_set", t0, t1, lseek_iters);

    unsigned long long lseek_read_iters = 0;
    unsigned long long lseek_read_lseek_ns = 0;
    unsigned long long lseek_read_read_ns = 0;
    unsigned long long lseek_read_lseek_cycles = 0;
    unsigned long long lseek_read_read_cycles = 0;
    t0 = now_ns();
    for (;;) {
        memset(buf, 0, sizeof(buf));
        unsigned long long part0 = now_ns();
        unsigned long long part0_cycles = read_tsc();
        off_t seek_status = lseek(fd, 0, SEEK_SET);
        unsigned long long part1_cycles = read_tsc();
        unsigned long long part1 = now_ns();
        ssize_t got = -1;
        unsigned long long part2 = part1;
        unsigned long long part3 = part1;
        unsigned long long part2_cycles = part1_cycles;
        unsigned long long part3_cycles = part1_cycles;
        if (seek_status == 0) {
            part2 = now_ns();
            part2_cycles = read_tsc();
            got = read(fd, buf, strlen(first));
            part3_cycles = read_tsc();
            part3 = now_ns();
        }
        if (seek_status != 0 || got != (ssize_t)strlen(first)) {
            (void)close(fd);
            return fail("batch lseek read");
        }
        lseek_read_lseek_ns += part1 - part0;
        lseek_read_read_ns += part3 - part2;
        if (part1_cycles >= part0_cycles) lseek_read_lseek_cycles += part1_cycles - part0_cycles;
        if (part3_cycles >= part2_cycles) lseek_read_read_cycles += part3_cycles - part2_cycles;
        lseek_read_iters++;
        t1 = now_ns();
        if (bench_done(t0, t1, lseek_read_iters)) break;
    }
    metric_avg("batch_lseek_read", t0, t1, lseek_read_iters);
    if (tsc_calibration_cycles != 0) {
        metric_total_avg_from_cycles("batch_lseek_read.lseek", lseek_read_lseek_cycles, lseek_read_iters);
        metric_total_avg_from_cycles("batch_lseek_read.read", lseek_read_read_cycles, lseek_read_iters);
    } else {
        metric_total_avg("batch_lseek_read.lseek", lseek_read_lseek_ns, lseek_read_iters);
        metric_total_avg("batch_lseek_read.read", lseek_read_read_ns, lseek_read_iters);
    }

    unsigned long long lseek_write_iters = 0;
    unsigned long long lseek_write_lseek_ns = 0;
    unsigned long long lseek_write_write_ns = 0;
    unsigned long long lseek_write_lseek_cycles = 0;
    unsigned long long lseek_write_write_cycles = 0;
    t0 = now_ns();
    for (;;) {
        unsigned long long part0 = now_ns();
        unsigned long long part0_cycles = read_tsc();
        off_t seek_status = lseek(fd, (off_t)strlen(first), SEEK_SET);
        unsigned long long part1_cycles = read_tsc();
        unsigned long long part1 = now_ns();
        ssize_t got = -1;
        unsigned long long part2 = part1;
        unsigned long long part3 = part1;
        unsigned long long part2_cycles = part1_cycles;
        unsigned long long part3_cycles = part1_cycles;
        if (seek_status == (off_t)strlen(first)) {
            part2 = now_ns();
            part2_cycles = read_tsc();
            got = write(fd, rewrite, strlen(rewrite));
            part3_cycles = read_tsc();
            part3 = now_ns();
        }
        if (seek_status != (off_t)strlen(first) || got != (ssize_t)strlen(rewrite)) {
            (void)close(fd);
            return fail("batch lseek write");
        }
        lseek_write_lseek_ns += part1 - part0;
        lseek_write_write_ns += part3 - part2;
        if (part1_cycles >= part0_cycles) lseek_write_lseek_cycles += part1_cycles - part0_cycles;
        if (part3_cycles >= part2_cycles) lseek_write_write_cycles += part3_cycles - part2_cycles;
        lseek_write_iters++;
        t1 = now_ns();
        if (bench_done(t0, t1, lseek_write_iters)) break;
    }
    metric_avg("batch_lseek_write", t0, t1, lseek_write_iters);
    if (tsc_calibration_cycles != 0) {
        metric_total_avg_from_cycles("batch_lseek_write.lseek", lseek_write_lseek_cycles, lseek_write_iters);
        metric_total_avg_from_cycles("batch_lseek_write.write", lseek_write_write_cycles, lseek_write_iters);
    } else {
        metric_total_avg("batch_lseek_write.lseek", lseek_write_lseek_ns, lseek_write_iters);
        metric_total_avg("batch_lseek_write.write", lseek_write_write_ns, lseek_write_iters);
    }

    unsigned long long fstat_iters = 0;
    t0 = now_ns();
    for (;;) {
        if (fstat(fd, &st) != 0) {
            (void)close(fd);
            return fail("batch fstat");
        }
        fstat_iters++;
        t1 = now_ns();
        if (bench_done(t0, t1, fstat_iters)) break;
    }
    metric_avg("batch_fstat", t0, t1, fstat_iters);

    unsigned long long pread_iters = 0;
    t0 = now_ns();
    for (;;) {
        errno = 0;
        ssize_t got = pread(fd, buf, strlen(first), 0);
        if (got != (ssize_t)strlen(first)) {
            fprintf(
                stderr,
                "[libc-vfs-exec-smoke] batch pread got=%lld want=%llu iter=%llu errno=%d\n",
                (long long)got,
                (unsigned long long)strlen(first),
                pread_iters,
                errno);
            (void)close(fd);
            return fail("batch pread");
        }
        pread_iters++;
        t1 = now_ns();
        if (bench_done(t0, t1, pread_iters)) break;
    }
    metric_avg("batch_pread", t0, t1, pread_iters);

    unsigned long long pwrite_iters = 0;
    t0 = now_ns();
    for (;;) {
        errno = 0;
        ssize_t got = pwrite(fd, rewrite, strlen(rewrite), (off_t)strlen(first));
        if (got != (ssize_t)strlen(rewrite)) {
            fprintf(
                stderr,
                "[libc-vfs-exec-smoke] batch pwrite got=%lld want=%llu iter=%llu errno=%d\n",
                (long long)got,
                (unsigned long long)strlen(rewrite),
                pwrite_iters,
                errno);
            (void)close(fd);
            return fail("batch pwrite");
        }
        pwrite_iters++;
        t1 = now_ns();
        if (bench_done(t0, t1, pwrite_iters)) break;
    }
    metric_avg("batch_pwrite", t0, t1, pwrite_iters);

#ifndef __pachaos__
    {
        char mutation_probe_path[128];
        snprintf(mutation_probe_path, sizeof(mutation_probe_path), "/tmp/libc-vfs-mutation-probe-%llu.tmp", now_ns());
        int mutation_probe_fd = open(mutation_probe_path, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
        int mutation_supported = 0;
        if (mutation_probe_fd >= 0) {
            if (close(mutation_probe_fd) != 0) {
                (void)close(fd);
                return fail("batch create unlink probe close");
            }
            if (unlink(mutation_probe_path) == 0) {
                mutation_supported = 1;
            } else if (!errno_is_unsupported(errno)) {
                (void)close(fd);
                return fail("batch create unlink probe unlink");
            }
        } else if (!errno_is_unsupported(errno)) {
            (void)close(fd);
            return fail("batch create unlink probe create");
        }

        if (!mutation_supported) {
            printf("[libc-vfs-exec-smoke] metric op=batch_create_close_unlink_skipped iterations=1 total=1 avg=1\n");
            fflush(stdout);
        } else {
        unsigned long long create_unlink_iters = 0;
        unsigned long long create_unlink_create_ns = 0;
        unsigned long long create_unlink_close_ns = 0;
        unsigned long long create_unlink_unlink_ns = 0;
        unsigned long long create_unlink_create_cycles = 0;
        unsigned long long create_unlink_close_cycles = 0;
        unsigned long long create_unlink_unlink_cycles = 0;
        const unsigned long long create_unlink_tag = now_ns();
        t0 = now_ns();
        for (;;) {
            char mutation_path[128];
            snprintf(
                mutation_path,
                sizeof(mutation_path),
                "/tmp/libc-vfs-mutation-%llx-%llu.tmp",
                create_unlink_tag,
                create_unlink_iters);
            unsigned long long part0 = now_ns();
            unsigned long long part0_cycles = read_tsc();
            int mutation_fd = open(mutation_path, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
            unsigned long long part1_cycles = read_tsc();
            unsigned long long part1 = now_ns();
            if (mutation_fd < 0) {
                (void)close(fd);
                return fail("batch create unlink create");
            }
            unsigned long long part2 = now_ns();
            unsigned long long part2_cycles = read_tsc();
            if (close(mutation_fd) != 0) {
                (void)close(fd);
                return fail("batch create unlink close");
            }
            unsigned long long part3_cycles = read_tsc();
            unsigned long long part3 = now_ns();
            unsigned long long part4 = now_ns();
            unsigned long long part4_cycles = read_tsc();
            if (unlink(mutation_path) != 0) {
                (void)close(fd);
                return fail("batch create unlink unlink");
            }
            unsigned long long part5_cycles = read_tsc();
            unsigned long long part5 = now_ns();
            create_unlink_create_ns += part1 - part0;
            create_unlink_close_ns += part3 - part2;
            create_unlink_unlink_ns += part5 - part4;
            if (part1_cycles >= part0_cycles) create_unlink_create_cycles += part1_cycles - part0_cycles;
            if (part3_cycles >= part2_cycles) create_unlink_close_cycles += part3_cycles - part2_cycles;
            if (part5_cycles >= part4_cycles) create_unlink_unlink_cycles += part5_cycles - part4_cycles;
            create_unlink_iters++;
            t1 = now_ns();
            if (bench_done(t0, t1, create_unlink_iters)) break;
        }
        metric_avg("batch_create_close_unlink", t0, t1, create_unlink_iters);
        if (tsc_calibration_cycles != 0) {
            metric_total_avg_from_cycles("batch_create_close_unlink.create", create_unlink_create_cycles, create_unlink_iters);
            metric_total_avg_from_cycles("batch_create_close_unlink.close", create_unlink_close_cycles, create_unlink_iters);
            metric_total_avg_from_cycles("batch_create_close_unlink.unlink", create_unlink_unlink_cycles, create_unlink_iters);
        } else {
            metric_total_avg("batch_create_close_unlink.create", create_unlink_create_ns, create_unlink_iters);
            metric_total_avg("batch_create_close_unlink.close", create_unlink_close_ns, create_unlink_iters);
            metric_total_avg("batch_create_close_unlink.unlink", create_unlink_unlink_ns, create_unlink_iters);
        }
    }
    }
#else
    printf("[libc-vfs-exec-smoke] metric op=batch_create_close_unlink_skipped iterations=1 total=1 avg=1\n");
    fflush(stdout);
#endif

    unsigned long long readv_iters = 0;
    unsigned long long readv_lseek_ns = 0;
    unsigned long long readv_readv_ns = 0;
    unsigned long long readv_lseek_cycles = 0;
    unsigned long long readv_readv_cycles = 0;
    t0 = now_ns();
    for (;;) {
        memset(left, 0, sizeof(left));
        memset(right, 0, sizeof(right));
        iov[0].iov_base = left;
        iov[0].iov_len = strlen(first);
        iov[1].iov_base = right;
        iov[1].iov_len = sizeof(right) - 1;
        errno = 0;
        unsigned long long part0 = now_ns();
        unsigned long long part0_cycles = read_tsc();
        off_t seek_status = lseek(fd, 0, SEEK_SET);
        unsigned long long part1_cycles = read_tsc();
        unsigned long long part1 = now_ns();
        ssize_t got = -1;
        unsigned long long part2 = part1;
        unsigned long long part3 = part1;
        unsigned long long part2_cycles = part1_cycles;
        unsigned long long part3_cycles = part1_cycles;
        if (seek_status == 0) {
            part2 = now_ns();
            part2_cycles = read_tsc();
            got = readv(fd, iov, 2);
            part3_cycles = read_tsc();
            part3 = now_ns();
        }
        if (seek_status != 0 || got < 0) {
            fprintf(
                stderr,
                "[libc-vfs-exec-smoke] batch readv seek=%lld got=%lld iter=%llu errno=%d\n",
                (long long)seek_status,
                (long long)got,
                readv_iters,
                errno);
            (void)close(fd);
            return fail("batch readv");
        }
        readv_lseek_ns += part1 - part0;
        readv_readv_ns += part3 - part2;
        if (part1_cycles >= part0_cycles) readv_lseek_cycles += part1_cycles - part0_cycles;
        if (part3_cycles >= part2_cycles) readv_readv_cycles += part3_cycles - part2_cycles;
        readv_iters++;
        t1 = now_ns();
        if (bench_done(t0, t1, readv_iters)) break;
    }
    metric_avg("batch_lseek_readv", t0, t1, readv_iters);
    if (tsc_calibration_cycles != 0) {
        metric_total_avg_from_cycles("batch_lseek_readv.lseek", readv_lseek_cycles, readv_iters);
        metric_total_avg_from_cycles("batch_lseek_readv.readv", readv_readv_cycles, readv_iters);
    } else {
        metric_total_avg("batch_lseek_readv.lseek", readv_lseek_ns, readv_iters);
        metric_total_avg("batch_lseek_readv.readv", readv_readv_ns, readv_iters);
    }

    if (close(fd) != 0) return fail("batch close fd");

    unsigned long long dir_iters = 0;
    unsigned long long dir_opendir_ns = 0;
    unsigned long long dir_readdir_ns = 0;
    unsigned long long dir_closedir_ns = 0;
    unsigned long long dir_opendir_cycles = 0;
    unsigned long long dir_readdir_cycles = 0;
    unsigned long long dir_closedir_cycles = 0;
    t0 = now_ns();
    for (;;) {
        unsigned long long part0 = now_ns();
        unsigned long long part0_cycles = read_tsc();
        DIR *loop_dir = opendir("/");
        unsigned long long part1_cycles = read_tsc();
        unsigned long long part1 = now_ns();
        if (loop_dir == NULL) return fail("batch opendir");
        unsigned long long part2 = now_ns();
        unsigned long long part2_cycles = read_tsc();
        for (;;) {
            struct dirent *de = readdir(loop_dir);
            if (de == NULL) break;
        }
        unsigned long long part3_cycles = read_tsc();
        unsigned long long part3 = now_ns();
        unsigned long long part3_close_cycles = read_tsc();
        if (closedir(loop_dir) != 0) return fail("batch closedir");
        unsigned long long part4_cycles = read_tsc();
        unsigned long long part4 = now_ns();
        dir_opendir_ns += part1 - part0;
        dir_readdir_ns += part3 - part2;
        dir_closedir_ns += part4 - part3;
        if (part1_cycles >= part0_cycles) dir_opendir_cycles += part1_cycles - part0_cycles;
        if (part3_cycles >= part2_cycles) dir_readdir_cycles += part3_cycles - part2_cycles;
        if (part4_cycles >= part3_close_cycles) dir_closedir_cycles += part4_cycles - part3_close_cycles;
        dir_iters++;
        t1 = now_ns();
        if (bench_done(t0, t1, dir_iters)) break;
    }
    metric_avg("batch_opendir_readdir_closedir", t0, t1, dir_iters);
    if (tsc_calibration_cycles != 0) {
        metric_total_avg_from_cycles("batch_opendir_readdir_closedir.opendir", dir_opendir_cycles, dir_iters);
        metric_total_avg_from_cycles("batch_opendir_readdir_closedir.readdir_all", dir_readdir_cycles, dir_iters);
        metric_total_avg_from_cycles("batch_opendir_readdir_closedir.closedir", dir_closedir_cycles, dir_iters);
    } else {
        metric_total_avg("batch_opendir_readdir_closedir.opendir", dir_opendir_ns, dir_iters);
        metric_total_avg("batch_opendir_readdir_closedir.readdir_all", dir_readdir_ns, dir_iters);
        metric_total_avg("batch_opendir_readdir_closedir.closedir", dir_closedir_ns, dir_iters);
    }

    unsigned long long raw_dir_iters = 0;
    unsigned long long raw_dir_open_cycles = 0;
    unsigned long long raw_dir_getdents_first_cycles = 0;
    unsigned long long raw_dir_scan_cycles = 0;
    unsigned long long raw_dir_getdents_eof_cycles = 0;
    unsigned long long raw_dir_close_cycles = 0;
    unsigned long long raw_dir_entries = 0;
    unsigned long long raw_dir_bytes = 0;
    t0 = now_ns();
    for (;;) {
        unsigned char dents[4096];
        unsigned long long part0_cycles = read_tsc();
        int raw_dir_fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        unsigned long long part1_cycles = read_tsc();
        if (raw_dir_fd < 0) return fail("raw dir open");

        int dent_bytes = (int)smoke_getdents(raw_dir_fd, (struct dirent *)dents, sizeof(dents));
        unsigned long long part2_cycles = read_tsc();
        if (dent_bytes < 0) {
            (void)close(raw_dir_fd);
            return fail("raw dir getdents first");
        }

        int offset = 0;
        while (offset < dent_bytes) {
            const struct dirent *de = (const struct dirent *)(const void *)(dents + offset);
            if (de->d_reclen == 0 || offset + de->d_reclen > dent_bytes) {
                (void)close(raw_dir_fd);
                return fail("raw dir scan");
            }
            raw_dir_entries++;
            offset += de->d_reclen;
        }
        unsigned long long part3_cycles = read_tsc();

        int eof_bytes = (int)smoke_getdents(raw_dir_fd, (struct dirent *)dents, sizeof(dents));
        unsigned long long part4_cycles = read_tsc();
        if (eof_bytes != 0) {
            (void)close(raw_dir_fd);
            return fail("raw dir getdents eof");
        }

        if (close(raw_dir_fd) != 0) return fail("raw dir close");
        unsigned long long part5_cycles = read_tsc();

        if (part1_cycles >= part0_cycles) raw_dir_open_cycles += part1_cycles - part0_cycles;
        if (part2_cycles >= part1_cycles) raw_dir_getdents_first_cycles += part2_cycles - part1_cycles;
        if (part3_cycles >= part2_cycles) raw_dir_scan_cycles += part3_cycles - part2_cycles;
        if (part4_cycles >= part3_cycles) raw_dir_getdents_eof_cycles += part4_cycles - part3_cycles;
        if (part5_cycles >= part4_cycles) raw_dir_close_cycles += part5_cycles - part4_cycles;
        raw_dir_bytes += (unsigned long long)dent_bytes;
        raw_dir_iters++;
        t1 = now_ns();
        if (bench_done(t0, t1, raw_dir_iters)) break;
    }
    metric_avg("batch_dir_raw", t0, t1, raw_dir_iters);
    metric_total_avg_from_cycles("batch_dir_raw.open", raw_dir_open_cycles, raw_dir_iters);
    metric_total_avg_from_cycles("batch_dir_raw.getdents_first", raw_dir_getdents_first_cycles, raw_dir_iters);
    metric_total_avg_from_cycles("batch_dir_raw.scan", raw_dir_scan_cycles, raw_dir_iters);
    metric_total_avg_from_cycles("batch_dir_raw.getdents_eof", raw_dir_getdents_eof_cycles, raw_dir_iters);
    metric_total_avg_from_cycles("batch_dir_raw.close", raw_dir_close_cycles, raw_dir_iters);
    printf(
        "[libc-vfs-exec-smoke] metric op=batch_dir_raw.entries iterations=%llu total=%llu avg=%llu\n",
        raw_dir_iters,
        raw_dir_entries,
        raw_dir_iters > 0 ? raw_dir_entries / raw_dir_iters : 0);
    printf(
        "[libc-vfs-exec-smoke] metric op=batch_dir_raw.bytes iterations=%llu total=%llu avg=%llu\n",
        raw_dir_iters,
        raw_dir_bytes,
        raw_dir_iters > 0 ? raw_dir_bytes / raw_dir_iters : 0);
    fflush(stdout);

    if (run_filed_raw_operation_bench(path) != 0) {
        return 1;
    }

    if (run_fs_write_stress() != 0) {
        return 1;
    }

    printf("[libc-vfs-exec-smoke] vfs ok\n");
    fflush(stdout);
    return 0;
}

int main(int argc, char **argv, char **envp)
{
    calibrate_tsc_timebase();

    if (argc == 2 && strcmp(argv[1], "--child") == 0) {
        return run_child(argc, argv, envp);
    }

    if (run_vfs() != 0) {
        return 1;
    }

    printf("[libc-vfs-exec-smoke] exec child\n");
    fflush(stdout);
    char *const child_argv[] = {
#ifdef __linux__
        argv[0],
#else
        "/cmd/libc_vfs_exec_smoke.elf",
#endif
        "--child",
        NULL,
    };
    char exec_start_env[96];
    unsigned long long exec_start_ns = now_ns();
    snprintf(exec_start_env, sizeof(exec_start_env), "PACHA_LIBC_VFS_EXEC_START_NS=%llu", exec_start_ns);
    char *const child_envp[] = {
        "PACHA_LIBC_VFS_EXEC_CHILD=1",
        exec_start_env,
        NULL,
    };
    execve(child_argv[0], child_argv, child_envp);
    return fail("execve child");
}
