/* Native (not Linux) guest regression. Build with build-unix-native-contract.sh
 * and run /cmd/unix_native_contract.elf after deploying the matching kernel.
 * Host unit tests cannot prove scheduler release and cross-process wakeups. */
#include "pacha/ipc.h"
#include "pacha/syscall.h"
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#define OBSERVE (PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_WAIT | \
    PACHA_FD_RIGHT_POLL | PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_CLOSE)
#define CONTROL (OBSERVE | PACHA_FD_RIGHT_START | PACHA_FD_RIGHT_KILL)
#define CHANNEL (OBSERVE | PACHA_FD_RIGHT_SEND | PACHA_FD_RIGHT_RECV)
#define EVENT (OBSERVE | PACHA_FD_RIGHT_READ | PACHA_FD_RIGHT_WRITE)

static unsigned char worker_stack[65536] __attribute__((aligned(16)));
static int worker_control, ready_event, release_event;
static int worker_observer, worker_error;

static int observe_self(void)
{
    const long fd = pacha_syscall4(PACHA_FD_SYSCALL_DUP,
        PACHA_THREAD_SELF_FD, 16, OBSERVE, 0);
    return fd >= 16 ? (int)fd : -1;
}

static int wait_events(struct pacha_pollfd *polls, unsigned count)
{
    struct timespec start, now;
    if (clock_gettime(CLOCK_MONOTONIC, &start)) return -1;
    for (;;) {
        int status = pacha_fd_wait_many(polls, count, 1);
        if (status > 0) return status;
        if (status < 0 && status != PACHA_ERR_NOT_READY && status != PACHA_ERR_EMPTY) return status;
        if (clock_gettime(CLOCK_MONOTONIC, &now)) return -1;
        if ((now.tv_sec - start.tv_sec) * 1000000000ll + now.tv_nsec - start.tv_nsec >= 1000000000ll)
            return 0;
    }
}

static int wait_ready(int fd)
{
    struct pacha_pollfd poll = { .fd = fd, .events = PACHA_FD_EVENT_READABLE };
    return wait_events(&poll, 1) == 1 && poll.revents == PACHA_FD_EVENT_READABLE ? 0 : -1;
}

static int receive_message(int fd, struct pacha_ipc_msg *message)
{
    /* Native finite waits may return NOT_READY on a scheduler wake. That
     * is a retry, not proof that the peer died or the timeout elapsed. */
    struct timespec start, now;
    if (clock_gettime(CLOCK_MONOTONIC, &start)) return -1;
    for (;;) {
        int status = pacha_ipc_recv_wait(fd, message, 1);
        if (status != PACHA_ERR_NOT_READY && status != PACHA_ERR_EMPTY) return status;
        if (clock_gettime(CLOCK_MONOTONIC, &now)) return -1;
        if ((now.tv_sec - start.tv_sec) * 1000000000ll + now.tv_nsec - start.tv_nsec >= 1000000000ll)
            return PACHA_ERR_NOT_READY;
    }
}

static int signal_event(int fd)
{
    uint64_t value = 1;
    return pacha_fd_write(fd, &value, sizeof(value)) == sizeof(value) ? 0 : -1;
}

static _Noreturn void exit_process(unsigned code)
{
    (void)pacha_syscall1(PACHA_PROCESS_SYSCALL_EXIT, code);
    for (;;) __asm__ volatile("pause");
}

static _Noreturn void worker(void)
{
    const int fd = observe_self();
    /* A successful self-kill would return into a supposedly dead generation.
     * The ordinary control FD, not the restricted observation FD, tests it. */
    const long self_kill = pacha_syscall2(PACHA_THREAD_SYSCALL_KILL,
        (uint64_t)worker_control, 91);
    __atomic_store_n(&worker_error, fd < 16 || self_kill != 1, __ATOMIC_RELEASE);
    __atomic_store_n(&worker_observer, fd, __ATOMIC_RELEASE);
    (void)signal_event(ready_event);
    struct pacha_pollfd release = {
        .fd = release_event, .events = PACHA_FD_EVENT_READABLE,
    };
    while (pacha_fd_wait_many(&release, 1, PACHA_FD_WAIT_FOREVER) != 1 ||
        release.revents != PACHA_FD_EVENT_READABLE) {}
    (void)pacha_syscall3(PACHA_THREAD_SYSCALL_EXIT, 23, 0, 0);
    for (;;) __asm__ volatile("pause");
}

static int check_self_rights(void)
{
    const int fd = observe_self();
    struct pacha_fd_info info;
    if (fd < 16 || pacha_fd_get_info(fd, &info) != 0 ||
        info.kind != PACHA_FD_KIND_THREAD || info.rights != OBSERVE) return -1;
    (void)pacha_fd_close(fd);
    for (unsigned bit = 0; bit < 64; bit++) {
        const uint64_t extra = UINT64_C(1) << bit;
        if ((extra & OBSERVE) != 0) continue;
        const long result = pacha_syscall4(PACHA_FD_SYSCALL_DUP,
            PACHA_THREAD_SELF_FD, 16, OBSERVE | extra, 0);
        if (result >= 16) (void)pacha_fd_close((int)result);
        if (result != 1) return -1;
    }
    return pacha_syscall4(PACHA_FD_SYSCALL_DUP, PACHA_THREAD_SELF_FD,
        16, OBSERVE, UINT64_C(1) << 32) == 1 ? 0 : -1;
}

static _Noreturn void child_observer(int channel, int private_fd,
    int main_observer, int inherited_observer, unsigned expected_state,
    unsigned expected_code)
{
    struct pacha_fd_info info;
    /* Check before any allocation can reuse the excluded private slot. */
    if (pacha_fd_get_info(private_fd, &info) == 0) exit_process(10);
    if (check_self_rights() != 0) exit_process(11); /* fork's initial thread */
    (void)pacha_fd_close(inherited_observer);
    (void)pacha_fd_close(worker_control);
    struct pacha_ipc_fd capability = {0};
    struct pacha_ipc_msg message = { .fds = &capability, .fd_capacity = 1 };
    if (receive_message(channel, &message) != 0 ||
        message.fd_count != 1) exit_process(12);
    const int observed = (int)capability.fd;
    if (pacha_fd_get_info(observed, &info) != 0 ||
        info.kind != PACHA_FD_KIND_THREAD || info.rights != OBSERVE) exit_process(13);
    struct pacha_ipc_msg ack = { .word0 = 1 };
    if (pacha_ipc_send(channel, &ack) != 0) exit_process(14);
    struct pacha_pollfd polls[2] = {
        { .fd = observed, .events = PACHA_FD_EVENT_READABLE },
        { .fd = main_observer, .events = PACHA_FD_EVENT_READABLE },
    };
    if (wait_events(polls, 2) != 1 ||
        polls[0].revents != PACHA_FD_EVENT_READABLE || polls[1].revents != 0)
        exit_process(15);
    uint64_t status[4] = {0};
    if (pacha_syscall2(PACHA_THREAD_SYSCALL_WAIT, (uint64_t)observed,
        (uint64_t)(uintptr_t)status) != 0 || status[0] != expected_state ||
        status[1] != expected_code || status[3] == 0) exit_process(16);
    exit_process(0);
}

static int run_death_case(int kill, int main_observer, int previous_observer,
    int *retained_observer)
{
    ready_event = pacha_eventfd_create(0, EVENT, 0);
    release_event = pacha_eventfd_create(0, EVENT, 0);
    const int private_fd = pacha_eventfd_create(0, EVENT, PACHA_FD_FLAG_PRIVATE);
    struct pacha_ipc_channel_pair channel;
    if (ready_event < 16 || release_event < 16 || private_fd < 16 ||
        pacha_ipc_channel_create(&channel, CHANNEL, 0) != 0) return 1;
    worker_control = pacha_thread_create((int)PACHA_PROCESS_SELF_FD,
        (uint64_t)(uintptr_t)worker,
        (uint64_t)(uintptr_t)(worker_stack + sizeof(worker_stack) - 8),
        0, 0, CONTROL);
    if (worker_control < 16 || pacha_thread_start(worker_control) != 0 ||
        wait_ready(ready_event) != 0) return 2;
    const int observed = __atomic_load_n(&worker_observer, __ATOMIC_ACQUIRE);
    if (__atomic_load_n(&worker_error, __ATOMIC_ACQUIRE) != 0) return 3;
    if (previous_observer >= 16) {
        /* Reusing a scheduler slot must not reactivate the previous FD. */
        struct pacha_pollfd poll = {
            .fd = previous_observer, .events = PACHA_FD_EVENT_READABLE,
        };
        if (pacha_fd_poll(&poll, 1) != 1 || poll.revents != PACHA_FD_EVENT_READABLE)
            return 4;
    }
    const int child = pacha_process_clone(OBSERVE, PACHA_PROCESS_CLONE_CURRENT_THREAD);
    if (child < 0) return 5;
    if (child == 0) {
        (void)pacha_fd_close(channel.a);
        child_observer(channel.b, private_fd, main_observer, observed,
            kill ? 3u : 2u, kill ? 99u : 23u);
    }
    (void)pacha_fd_close(channel.b);
    struct pacha_ipc_fd cap = { .fd = (uint64_t)observed, .rights = OBSERVE };
    struct pacha_ipc_msg message = { .fds = &cap, .fd_count = 1 };
    if (pacha_ipc_send(channel.a, &message) != 0) return 6;
    struct pacha_ipc_msg ack = {0};
    const int ack_status = receive_message(channel.a, &ack);
    if (ack_status != 0 || ack.word0 != 1) {
        struct pacha_fd_info info = {0};
        int inspected = pacha_fd_get_info(child, &info);
        printf("UNIX_NATIVE_ACK_FAIL status=%d word=%llu child_info=%d state=%llu code=%llu\n",
            ack_status, (unsigned long long)ack.word0, inspected,
            (unsigned long long)info.extra, (unsigned long long)info.size);
        return 7;
    }
    if (kill) {
        long result = 2;
        for (unsigned attempt = 0; attempt < 1000 && result == 2; attempt++) {
            result = pacha_syscall2(PACHA_THREAD_SYSCALL_KILL,
                (uint64_t)worker_control, 99);
            if (result == 2) {
                /* A failed remote release must not publish death. */
                struct pacha_pollfd poll = {
                    .fd = observed, .events = PACHA_FD_EVENT_READABLE,
                };
                if (pacha_fd_poll(&poll, 1) != 0 || poll.revents != 0) return 8;
                (void)pacha_fd_wait_many(NULL, 0, 1);
            }
        }
        if (result != 0) return 9;
    } else if (signal_event(release_event) != 0) return 10;
    if (wait_ready(child) != 0) return 11;
    uint64_t status[4] = {0};
    if (pacha_syscall2(PACHA_PROCESS_SYSCALL_WAIT, (uint64_t)child,
        (uint64_t)(uintptr_t)status) != 0 || status[1] != 0) {
        printf("UNIX_NATIVE_CHILD_FAIL code=%llu\n", (unsigned long long)status[1]);
        return 12;
    }
    (void)pacha_fd_close(child);
    (void)pacha_fd_close(worker_control);
    (void)pacha_fd_close(ready_event);
    (void)pacha_fd_close(release_event);
    (void)pacha_fd_close(private_fd);
    (void)pacha_fd_close(channel.a);
    *retained_observer = observed;
    return 0;
}

int main(void)
{
    if (check_self_rights() != 0) {
        puts("UNIX_NATIVE_FAIL rights");
        return 1;
    }
    const int main_observer = observe_self();
    int previous = -1;
    for (unsigned iteration = 0; iteration < 16; iteration++) {
        int retained = -1;
        const int status = run_death_case(iteration & 1, main_observer, previous, &retained);
        if (status != 0) {
            printf("UNIX_NATIVE_FAIL iteration=%u stage=%d\n", iteration, status);
            return 1;
        }
        if (previous >= 16) (void)pacha_fd_close(previous);
        previous = retained;
    }
    (void)pacha_fd_close(previous);
    (void)pacha_fd_close(main_observer);
    puts("UNIX_NATIVE_CONTRACT=OK rights private-fork self-kill exit kill transfer generation");
    return 0;
}
