#define _GNU_SOURCE

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include "pachaos/abi.h"

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1

static void log_text(const char *s) {
    (void)fwrite(s, 1, strlen(s), stderr);
    (void)fflush(stderr);
}

int main(int argc, char **argv, char **envp) {
    (void)argc;
    (void)argv;
    (void)envp;

    printf("[pachaos-musl-smoke] hello from crt/syscall scaffold stdout=%d\n", 1);
    fprintf(stderr, "[pachaos-musl-smoke] stderr online\n");
    char fmt_buf[96];
    int fmt_len = snprintf(fmt_buf, sizeof(fmt_buf), "snprintf:%s:%d", "ok", 42);
    if (fmt_len <= 0 || strcmp(fmt_buf, "snprintf:ok:42") != 0) {
        log_text("[pachaos-musl-smoke] snprintf failed\n");
        return 12;
    }
    const char *fwrite_msg = "[pachaos-musl-smoke] fwrite online\n";
    const size_t fwrite_len = strlen(fwrite_msg);
    if (fwrite(fwrite_msg, 1, fwrite_len, stdout) != fwrite_len) {
        log_text("[pachaos-musl-smoke] fwrite failed\n");
        return 13;
    }
    if (fflush(stdout) != 0 || fflush(stderr) != 0) {
        log_text("[pachaos-musl-smoke] fflush failed\n");
        return 14;
    }
    const char *write_msg = "[pachaos-musl-smoke] write online\n";
    const size_t write_len = strlen(write_msg);
    if (write(1, write_msg, write_len) != (ssize_t)write_len) {
        log_text("[pachaos-musl-smoke] write failed\n");
        return 15;
    }
    struct pollfd pfd = { .fd = 1, .events = POLLOUT, .revents = 0 };
    if (poll(&pfd, 1, 0) != 1 || (pfd.revents & POLLOUT) == 0) {
        log_text("[pachaos-musl-smoke] poll stdout failed\n");
        return 16;
    }
    if (!isatty(1)) {
        log_text("[pachaos-musl-smoke] isatty stdout failed\n");
        return 19;
    }
    struct winsize wsz = {0};
    if (ioctl(1, TIOCGWINSZ, &wsz) != 0 || wsz.ws_col == 0 || wsz.ws_row == 0) {
        log_text("[pachaos-musl-smoke] ioctl winsize failed\n");
        return 20;
    }
    struct stat st = {0};
    if (fstat(1, &st) != 0 || !S_ISCHR(st.st_mode)) {
        log_text("[pachaos-musl-smoke] fstat stdout failed\n");
        return 21;
    }
    unsigned char *page = (unsigned char *)mmap(
        0,
        4096,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1,
        0
    );
    if (page == MAP_FAILED) {
        log_text("[pachaos-musl-smoke] mmap failed\n");
        return 1;
    }
    page[0] = 0x5a;
    page[4095] = 0xa5;
    if (page[0] != 0x5a || page[4095] != 0xa5) {
        log_text("[pachaos-musl-smoke] mmap memory check failed\n");
        return 2;
    }
    if (munmap(page, 4096) != 0) {
        log_text("[pachaos-musl-smoke] munmap failed\n");
        return 3;
    }

    struct timespec ts = {0, 0};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        log_text("[pachaos-musl-smoke] clock_gettime failed\n");
        return 4;
    }
    struct timespec rt = {0, 0};
    if (clock_gettime(CLOCK_REALTIME, &rt) != 0) {
        log_text("[pachaos-musl-smoke] realtime clock_gettime failed\n");
        return 22;
    }
    const struct timespec tiny_sleep = { .tv_sec = 0, .tv_nsec = 0 };
    if (nanosleep(&tiny_sleep, 0) != 0) {
        log_text("[pachaos-musl-smoke] nanosleep failed\n");
        return 23;
    }
    if (sleep(0) != 0) {
        log_text("[pachaos-musl-smoke] sleep(0) failed\n");
        return 24;
    }

    unsigned char *heap = (unsigned char *)malloc(128);
    if (heap == 0) {
        log_text("[pachaos-musl-smoke] malloc failed\n");
        return 5;
    }
    for (int i = 0; i < 128; i++) heap[i] = (unsigned char)i;
    heap = (unsigned char *)realloc(heap, 512);
    if (heap == 0) {
        log_text("[pachaos-musl-smoke] realloc failed\n");
        return 6;
    }
    for (int i = 0; i < 128; i++) {
        if (heap[i] != (unsigned char)i) {
            log_text("[pachaos-musl-smoke] realloc content check failed\n");
            return 7;
        }
    }
    unsigned char *zeroed = (unsigned char *)calloc(32, 4);
    if (zeroed == 0) {
        log_text("[pachaos-musl-smoke] calloc failed\n");
        return 8;
    }
    for (int i = 0; i < 128; i++) {
        if (zeroed[i] != 0) {
            log_text("[pachaos-musl-smoke] calloc zero check failed\n");
            return 9;
        }
    }
    free(zeroed);
    free(heap);

    void *large_heap = malloc(65536);
    if (large_heap == 0) {
        log_text("[pachaos-musl-smoke] large malloc failed\n");
        return 25;
    }
    memset(large_heap, 0x5c, 65536);
    free(large_heap);

    volatile int futex_word = 1;
    long wait_result = syscall(SYS_futex, &futex_word, FUTEX_WAIT, 0, 0);
    if (wait_result != -1) {
        log_text("[pachaos-musl-smoke] futex wait mismatch path failed\n");
        return 10;
    }
    long wake_result = syscall(SYS_futex, &futex_word, FUTEX_WAKE, 1);
    if (wake_result < 0) {
        log_text("[pachaos-musl-smoke] futex wake failed\n");
        return 11;
    }

    int event_fd = eventfd(0, 0);
    if (event_fd < 0) {
        log_text("[pachaos-musl-smoke] eventfd failed\n");
        return 26;
    }
    eventfd_t event_value = 7;
    if (eventfd_write(event_fd, event_value) != 0) {
        log_text("[pachaos-musl-smoke] eventfd_write failed\n");
        return 27;
    }
    struct pollfd event_pfd = { .fd = event_fd, .events = POLLIN, .revents = 0 };
    if (poll(&event_pfd, 1, 0) != 1 || (event_pfd.revents & POLLIN) == 0) {
        log_text("[pachaos-musl-smoke] eventfd poll failed\n");
        return 28;
    }
    event_value = 0;
    if (eventfd_read(event_fd, &event_value) != 0 || event_value != 7) {
        log_text("[pachaos-musl-smoke] eventfd_read failed\n");
        return 29;
    }
    if (close(event_fd) != 0) {
        log_text("[pachaos-musl-smoke] eventfd close failed\n");
        return 30;
    }

    int timer_fd = timerfd_create(CLOCK_MONOTONIC, 0);
    if (timer_fd < 0) {
        log_text("[pachaos-musl-smoke] timerfd_create failed\n");
        return 31;
    }
    struct itimerspec timer_spec;
    memset(&timer_spec, 0, sizeof(timer_spec));
    timer_spec.it_value.tv_nsec = 1000000;
    if (timerfd_settime(timer_fd, 0, &timer_spec, 0) != 0) {
        log_text("[pachaos-musl-smoke] timerfd_settime failed\n");
        return 32;
    }
    struct itimerspec timer_cur;
    memset(&timer_cur, 0, sizeof(timer_cur));
    if (timerfd_gettime(timer_fd, &timer_cur) != 0) {
        log_text("[pachaos-musl-smoke] timerfd_gettime failed\n");
        return 33;
    }
    struct pollfd timer_pfd = { .fd = timer_fd, .events = POLLIN, .revents = 0 };
    if (poll(&timer_pfd, 1, 50) != 1 || (timer_pfd.revents & POLLIN) == 0) {
        log_text("[pachaos-musl-smoke] timerfd poll timeout failed\n");
        return 34;
    }
    uint64_t expirations = 0;
    if (read(timer_fd, &expirations, sizeof(expirations)) != (ssize_t)sizeof(expirations) || expirations == 0) {
        log_text("[pachaos-musl-smoke] timerfd read failed\n");
        return 35;
    }
    if (close(timer_fd) != 0) {
        log_text("[pachaos-musl-smoke] timerfd close failed\n");
        return 36;
    }

    if (close(1) != 0) {
        log_text("[pachaos-musl-smoke] close stdout failed\n");
        return 18;
    }
    fprintf(stderr, "[pachaos-musl-smoke] OK\n");
    fflush(stderr);
    for (;;) __asm__ volatile("pause");
}
