#include "pacha/abi.h"
#include "pacha/ipc.h"
#include "pacha/syscall.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

enum {
    SMP_STRESS_ITERATIONS = 1024,
    SMP_STRESS_IPC_PERIOD = 16,
    SMP_STRESS_PAGE_SIZE = 4096,
    SMP_STRESS_FAST_ROUNDS = 16,
    SMP_STRESS_FAST_OP = 0x535452455353ull,
};

static int expect_status_ok(long status)
{
    return pacha_status_to_int(status);
}

static uint64_t now_ms(void)
{
    struct timespec ts = {0, 0};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

static uint64_t align_page(uint64_t value)
{
    return (value + (SMP_STRESS_PAGE_SIZE - 1ull)) & ~(SMP_STRESS_PAGE_SIZE - 1ull);
}

static uint64_t channel_rights(void)
{
    return PACHA_FD_RIGHT_INSPECT |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_SEND |
        PACHA_FD_RIGHT_RECV |
        PACHA_FD_RIGHT_CALL |
        PACHA_FD_RIGHT_WAIT |
        PACHA_FD_RIGHT_POLL |
        PACHA_FD_RIGHT_TRANSFER;
}

static void close_channel_pair(struct pacha_ipc_channel_pair *pair)
{
    if (pair->a >= 16) {
        (void)pacha_fd_close(pair->a);
        pair->a = -1;
    }
    if (pair->b >= 16) {
        (void)pacha_fd_close(pair->b);
        pair->b = -1;
    }
}

static void close_fast_channel(struct pacha_ipc_fast_channel *fast)
{
    const uint64_t ring_size = align_page(sizeof(struct pacha_ipc_fast_ring));
    if (fast->request != NULL) {
        (void)pacha_munmap(fast->request, ring_size);
        fast->request = NULL;
    }
    if (fast->completion != NULL) {
        (void)pacha_munmap(fast->completion, ring_size);
        fast->completion = NULL;
    }
    if (fast->request_vmo_fd >= 16) {
        (void)pacha_fd_close(fast->request_vmo_fd);
        fast->request_vmo_fd = -1;
    }
    if (fast->completion_vmo_fd >= 16) {
        (void)pacha_fd_close(fast->completion_vmo_fd);
        fast->completion_vmo_fd = -1;
    }
}

static uint64_t stress_pattern(unsigned iteration, uint64_t index)
{
    return 0x514d505f53545253ull ^ ((uint64_t)iteration << 32) ^ (index * 0x9e3779b97f4a7c15ull);
}

static int exercise_memory(unsigned iteration)
{
    const uint64_t size = (iteration & 1u) != 0 ? SMP_STRESS_PAGE_SIZE * 2ull : SMP_STRESS_PAGE_SIZE;
    unsigned char *mem = pacha_mmap_anonymous(size, PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_PRIVATE);
    if (mem == NULL) {
        return -10;
    }

    for (uint64_t i = 0; i < size; i += 257) {
        mem[i] = (unsigned char)(iteration + i);
    }
    for (uint64_t i = 0; i < size; i += 521) {
        mem[i] ^= (unsigned char)(i >> 3);
    }

    int status = expect_status_ok(pacha_syscall3(
        PACHA_FD_SYSCALL_MPROTECT,
        (uint64_t)(uintptr_t)mem,
        size,
        PACHA_PROT_READ));
    if (status != 0) {
        (void)pacha_munmap(mem, size);
        return -11;
    }

    volatile unsigned char sample = mem[(iteration * 97u) % size];
    (void)sample;

    status = expect_status_ok(pacha_syscall3(
        PACHA_FD_SYSCALL_MPROTECT,
        (uint64_t)(uintptr_t)mem,
        size,
        PACHA_PROT_READ | PACHA_PROT_WRITE));
    if (status != 0) {
        (void)pacha_munmap(mem, size);
        return -12;
    }
    mem[(iteration * 131u) % size] = (unsigned char)(iteration ^ 0xa5u);

    status = pacha_munmap(mem, size);
    if (status != 0) {
        return -13;
    }
    return 0;
}

static int exercise_shared_vmo(unsigned iteration)
{
    const uint64_t rights =
        PACHA_FD_RIGHT_INSPECT |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE |
        PACHA_FD_RIGHT_TRANSFER;
    const int fd = pacha_vmo_create(SMP_STRESS_PAGE_SIZE, rights, 0);
    if (fd < 16) {
        return -50;
    }

    uint64_t *left = pacha_mmap(fd, SMP_STRESS_PAGE_SIZE, PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
    uint64_t *right = pacha_mmap(fd, SMP_STRESS_PAGE_SIZE, PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
    if (left == NULL || right == NULL) {
        if (left != NULL) {
            (void)pacha_munmap(left, SMP_STRESS_PAGE_SIZE);
        }
        if (right != NULL) {
            (void)pacha_munmap(right, SMP_STRESS_PAGE_SIZE);
        }
        (void)pacha_fd_close(fd);
        return -51;
    }

    for (uint64_t i = 0; i < 64; i++) {
        left[i] = stress_pattern(iteration, i);
    }
    for (uint64_t i = 0; i < 64; i++) {
        if (right[i] != stress_pattern(iteration, i)) {
            (void)pacha_munmap(left, SMP_STRESS_PAGE_SIZE);
            (void)pacha_munmap(right, SMP_STRESS_PAGE_SIZE);
            (void)pacha_fd_close(fd);
            return -52;
        }
        right[i] ^= 0xa5a5a5a5a5a5a5a5ull;
    }
    for (uint64_t i = 0; i < 64; i++) {
        if (left[i] != (stress_pattern(iteration, i) ^ 0xa5a5a5a5a5a5a5a5ull)) {
            (void)pacha_munmap(left, SMP_STRESS_PAGE_SIZE);
            (void)pacha_munmap(right, SMP_STRESS_PAGE_SIZE);
            (void)pacha_fd_close(fd);
            return -53;
        }
    }

    if (pacha_munmap(left, SMP_STRESS_PAGE_SIZE) != 0 ||
        pacha_munmap(right, SMP_STRESS_PAGE_SIZE) != 0 ||
        pacha_fd_close(fd) != 0) {
        return -54;
    }
    return 0;
}

static int exercise_ipc_vmo_transfer(unsigned iteration)
{
    struct pacha_ipc_channel_pair pair = { .a = -1, .b = -1 };
    int status = pacha_ipc_channel_create(&pair, channel_rights(), 0);
    if (status != 0 || pair.a < 16 || pair.b < 16) {
        return -60;
    }

    const uint64_t vmo_rights =
        PACHA_FD_RIGHT_INSPECT |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE |
        PACHA_FD_RIGHT_TRANSFER;
    const int fd = pacha_vmo_create(SMP_STRESS_PAGE_SIZE, vmo_rights, 0);
    if (fd < 16) {
        close_channel_pair(&pair);
        return -61;
    }
    uint64_t *mapped = pacha_mmap(fd, SMP_STRESS_PAGE_SIZE, PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
    if (mapped == NULL) {
        (void)pacha_fd_close(fd);
        close_channel_pair(&pair);
        return -62;
    }
    for (uint64_t i = 0; i < 32; i++) {
        mapped[i] = stress_pattern(iteration, i + 1000);
    }

    struct pacha_ipc_fd send_fd = {
        .fd = (uint64_t)(uint32_t)fd,
        .rights = PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE,
        .flags = 0,
        .transfer_flags = 0,
    };
    const struct pacha_ipc_msg send_msg = {
        .word0 = 0x564d4f5452414e53ull,
        .word1 = iteration,
        .fds = &send_fd,
        .fd_count = 1,
    };
    status = pacha_ipc_send(pair.a, &send_msg);
    if (status != 0) {
        (void)pacha_munmap(mapped, SMP_STRESS_PAGE_SIZE);
        (void)pacha_fd_close(fd);
        close_channel_pair(&pair);
        return -63;
    }

    struct pacha_ipc_fd recv_fd = {0};
    struct pacha_ipc_msg recv_msg = {
        .fds = &recv_fd,
        .fd_capacity = 1,
    };
    status = pacha_ipc_recv(pair.b, &recv_msg);
    if (status != 0 || recv_msg.word0 != send_msg.word0 || recv_msg.word1 != iteration || recv_msg.fd_count != 1) {
        (void)pacha_munmap(mapped, SMP_STRESS_PAGE_SIZE);
        (void)pacha_fd_close(fd);
        close_channel_pair(&pair);
        return -64;
    }

    uint64_t *received = pacha_mmap((int)recv_fd.fd, SMP_STRESS_PAGE_SIZE, PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
    if (received == NULL) {
        (void)pacha_munmap(mapped, SMP_STRESS_PAGE_SIZE);
        (void)pacha_fd_close((int)recv_fd.fd);
        (void)pacha_fd_close(fd);
        close_channel_pair(&pair);
        return -65;
    }
    for (uint64_t i = 0; i < 32; i++) {
        if (received[i] != stress_pattern(iteration, i + 1000)) {
            (void)pacha_munmap(received, SMP_STRESS_PAGE_SIZE);
            (void)pacha_munmap(mapped, SMP_STRESS_PAGE_SIZE);
            (void)pacha_fd_close((int)recv_fd.fd);
            (void)pacha_fd_close(fd);
            close_channel_pair(&pair);
            return -66;
        }
        received[i] ^= 0x1111222233334444ull;
    }
    for (uint64_t i = 0; i < 32; i++) {
        if (mapped[i] != (stress_pattern(iteration, i + 1000) ^ 0x1111222233334444ull)) {
            (void)pacha_munmap(received, SMP_STRESS_PAGE_SIZE);
            (void)pacha_munmap(mapped, SMP_STRESS_PAGE_SIZE);
            (void)pacha_fd_close((int)recv_fd.fd);
            (void)pacha_fd_close(fd);
            close_channel_pair(&pair);
            return -67;
        }
    }

    if (pacha_munmap(received, SMP_STRESS_PAGE_SIZE) != 0 ||
        pacha_munmap(mapped, SMP_STRESS_PAGE_SIZE) != 0 ||
        pacha_fd_close((int)recv_fd.fd) != 0 ||
        pacha_fd_close(fd) != 0) {
        close_channel_pair(&pair);
        return -68;
    }
    close_channel_pair(&pair);
    return 0;
}

static int fast_echo_handler(void *ctx, const struct pacha_ipc_fast_entry *request, struct pacha_ipc_fast_entry *response)
{
    const uint64_t salt = *(const uint64_t *)ctx;
    response->op = request->op;
    response->offset = request->offset;
    response->len = request->len ^ salt;
    response->flags = request->flags;
    response->status = request->offset + request->len + salt;
    return 0;
}

static int exercise_fast_ipc_ring(unsigned iteration)
{
    struct pacha_ipc_channel_pair pair = { .a = -1, .b = -1 };
    int status = pacha_ipc_channel_create(&pair, channel_rights(), 0);
    if (status != 0 || pair.a < 16 || pair.b < 16) {
        return -70;
    }

    struct pacha_ipc_fast_channel client;
    struct pacha_ipc_fast_channel server;
    memset(&client, 0, sizeof(client));
    memset(&server, 0, sizeof(server));
    client.request_vmo_fd = -1;
    client.completion_vmo_fd = -1;
    server.request_vmo_fd = -1;
    server.completion_vmo_fd = -1;

    status = pacha_ipc_fast_channel_offer(&client, pair.a, 0, 0);
    if (status != 0) {
        close_fast_channel(&client);
        close_channel_pair(&pair);
        return -71;
    }
    status = pacha_ipc_fast_channel_accept(&server, pair.b, 0, 0);
    if (status != 0) {
        close_fast_channel(&server);
        close_fast_channel(&client);
        close_channel_pair(&pair);
        return -72;
    }
    if (!pacha_ipc_fast_channel_uses_ring(&client) || !pacha_ipc_fast_channel_uses_ring(&server)) {
        close_fast_channel(&server);
        close_fast_channel(&client);
        close_channel_pair(&pair);
        return -73;
    }

    const uint64_t salt = stress_pattern(iteration, 0x55);
    for (uint64_t i = 0; i < SMP_STRESS_FAST_ROUNDS; i++) {
        struct pacha_ipc_fast_entry request;
        struct pacha_ipc_fast_entry response;
        pacha_ipc_fast_entry_init(&request, SMP_STRESS_FAST_OP, iteration * 100 + i, i + 1, i << 4);
        status = pacha_ipc_fast_send(&client, &request);
        if (status != 0) {
            close_fast_channel(&server);
            close_fast_channel(&client);
            close_channel_pair(&pair);
            return -74;
        }
        status = pacha_ipc_fast_serve_once(&server, fast_echo_handler, (void *)&salt);
        if (status != 0) {
            close_fast_channel(&server);
            close_fast_channel(&client);
            close_channel_pair(&pair);
            return -75;
        }
        status = pacha_ipc_fast_recv(&client, &response);
        if (status != 0 ||
            response.op != SMP_STRESS_FAST_OP ||
            response.offset != request.offset ||
            response.len != (request.len ^ salt) ||
            response.flags != request.flags ||
            response.status != request.offset + request.len + salt) {
            close_fast_channel(&server);
            close_fast_channel(&client);
            close_channel_pair(&pair);
            return -76;
        }
    }

    close_fast_channel(&server);
    close_fast_channel(&client);
    close_channel_pair(&pair);
    return 0;
}

static int exercise_ipc_mix(unsigned iteration)
{
    int status = exercise_shared_vmo(iteration);
    if (status != 0) {
        return status;
    }
    status = exercise_ipc_vmo_transfer(iteration);
    if (status != 0) {
        return status;
    }
    return exercise_fast_ipc_ring(iteration);
}

static int exercise_eventfd(unsigned iteration)
{
    const uint64_t rights =
        PACHA_FD_RIGHT_INSPECT |
        PACHA_FD_RIGHT_WAIT |
        PACHA_FD_RIGHT_POLL |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_READ |
        PACHA_FD_RIGHT_WRITE;
    const int fd = pacha_eventfd_create(0, rights, 0);
    if (fd < 16) {
        return -20;
    }

    const uint64_t value = 1ull + (uint64_t)(iteration & 7u);
    if (pacha_fd_write(fd, &value, sizeof(value)) != (long)sizeof(value)) {
        (void)pacha_fd_close(fd);
        return -21;
    }

    struct pacha_pollfd pfd = {
        .fd = fd,
        .events = PACHA_FD_EVENT_READABLE,
        .revents = 0,
    };
    const long poll_status = pacha_fd_poll(&pfd, 1);
    if (poll_status < 0 || (pfd.revents & PACHA_FD_EVENT_READABLE) == 0) {
        (void)pacha_fd_close(fd);
        return -22;
    }

    pfd.revents = 0;
    const long wait_status = pacha_fd_wait_many(&pfd, 1, 1);
    if (wait_status < 0 || (pfd.revents & PACHA_FD_EVENT_READABLE) == 0) {
        (void)pacha_fd_close(fd);
        return -23;
    }

    uint64_t read_value = 0;
    if (pacha_fd_read(fd, &read_value, sizeof(read_value)) != (long)sizeof(read_value) || read_value != value) {
        (void)pacha_fd_close(fd);
        return -24;
    }

    if (pacha_fd_close(fd) != 0) {
        return -25;
    }
    return 0;
}

static int exercise_timerfd(void)
{
    const uint64_t rights =
        PACHA_FD_RIGHT_INSPECT |
        PACHA_FD_RIGHT_WAIT |
        PACHA_FD_RIGHT_POLL |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_READ;
    const int fd = pacha_timerfd_create(1000000ull, 0, rights, 0);
    if (fd < 16) {
        return -30;
    }

    struct pacha_pollfd pfd = {
        .fd = fd,
        .events = PACHA_FD_EVENT_READABLE,
        .revents = 0,
    };
    for (unsigned i = 0; i < 128 && (pfd.revents & PACHA_FD_EVENT_READABLE) == 0; i++) {
        (void)pacha_fd_wait_many(&pfd, 1, 1);
    }
    if ((pfd.revents & PACHA_FD_EVENT_READABLE) == 0) {
        (void)pacha_fd_close(fd);
        return -31;
    }

    uint64_t expirations = 0;
    if (pacha_fd_read(fd, &expirations, sizeof(expirations)) != (long)sizeof(expirations) || expirations == 0) {
        (void)pacha_fd_close(fd);
        return -32;
    }
    if (pacha_fd_close(fd) != 0) {
        return -33;
    }
    return 0;
}

static int exercise_runtime(void)
{
    unsigned char random_bytes[32];
    if (pacha_getrandom(random_bytes, sizeof(random_bytes), 0) != (long)sizeof(random_bytes)) {
        return -40;
    }

    volatile uint32_t futex_word = 1;
    const long wait_status = pacha_syscall3(
        PACHA_RUNTIME_SYSCALL_FUTEX_WAIT,
        (uint64_t)(uintptr_t)&futex_word,
        0,
        0);
    if (wait_status != PACHA_SYSCALL_ERR_NOT_READY && wait_status != -PACHA_SYSCALL_ERR_NOT_READY) {
        return -41;
    }

    const long wake_status = pacha_syscall3(
        PACHA_RUNTIME_SYSCALL_FUTEX_WAKE,
        (uint64_t)(uintptr_t)&futex_word,
        1,
        0);
    if (wake_status < 0) {
        return -42;
    }
    return 0;
}

int main(void)
{
    const long pid = pacha_syscall0(PACHA_RUNTIME_SYSCALL_GETPID);
    const long tid = pacha_syscall0(PACHA_RUNTIME_SYSCALL_GETTID);
    const uint64_t start_ms = now_ms();
    printf("[smp-stress] start pid=%ld tid=%ld\n", pid, tid);
    fflush(stdout);

    unsigned runtime_ops = 0;
    unsigned memory_ops = 0;
    unsigned eventfd_ops = 0;
    unsigned ipc_mix_ops = 0;

    int status = exercise_timerfd();
    if (status != 0) {
        fprintf(stderr, "[smp-stress] failed step=timerfd status=%d pid=%ld tid=%ld\n", status, pid, tid);
        fflush(stderr);
        return 1;
    }

    for (unsigned i = 0; i < SMP_STRESS_ITERATIONS; i++) {
        status = exercise_runtime();
        if (status != 0) {
            fprintf(stderr, "[smp-stress] failed step=runtime iter=%u status=%d pid=%ld tid=%ld\n", i, status, pid, tid);
            fflush(stderr);
            return 1;
        }
        runtime_ops++;
        status = exercise_memory(i);
        if (status != 0) {
            fprintf(stderr, "[smp-stress] failed step=memory iter=%u status=%d pid=%ld tid=%ld\n", i, status, pid, tid);
            fflush(stderr);
            return 1;
        }
        memory_ops++;
        status = exercise_eventfd(i);
        if (status != 0) {
            fprintf(stderr, "[smp-stress] failed step=eventfd iter=%u status=%d pid=%ld tid=%ld\n", i, status, pid, tid);
            fflush(stderr);
            return 1;
        }
        eventfd_ops++;
        if ((i % SMP_STRESS_IPC_PERIOD) == 0) {
            status = exercise_ipc_mix(i);
            if (status != 0) {
                fprintf(stderr, "[smp-stress] failed step=ipc_mix iter=%u status=%d pid=%ld tid=%ld\n", i, status, pid, tid);
                fflush(stderr);
                return 1;
            }
            ipc_mix_ops++;
        }
    }

    const uint64_t end_ms = now_ms();
    const uint64_t elapsed_ms = end_ms >= start_ms ? end_ms - start_ms : 0;
    printf("[smp-stress] ok pid=%ld tid=%ld iterations=%u elapsed_ms=%llu runtime=%u memory=%u eventfd=%u ipc_mix=%u\n",
        pid,
        tid,
        SMP_STRESS_ITERATIONS,
        (unsigned long long)elapsed_ms,
        runtime_ops,
        memory_ops,
        eventfd_ops,
        ipc_mix_ops);
    fflush(stdout);
    return 0;
}
