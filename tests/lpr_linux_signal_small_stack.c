/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static volatile sig_atomic_t finished, outer_seen, inner_seen;
static pthread_t main_thread;
static unsigned char fp_pattern[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
static unsigned char fp_observed[16];
extern void small_signal_stack(void *top);
__asm__(".text\n.global small_signal_stack\n.type small_signal_stack,@function\n"
    "small_signal_stack: push %r12; mov %rsp,%r12; mov %rdi,%rsp\n"
    "movdqu fp_pattern(%rip),%xmm0\n"
    "1: cmpl $0,finished(%rip); jne 2f; pause; jmp 1b\n"
    "2: movdqu %xmm0,fp_observed(%rip); mov %r12,%rsp; pop %r12; ret\n"
    ".size small_signal_stack,.-small_signal_stack\n");
static void handler(int sig)
{
    if (sig == SIGUSR2) { ++inner_seen; return; }
    ++outer_seen;
    /* The outer native frame copy must survive another signal and syscalls
     * from this handler: the thread-private native scratch can be reused. */
    if (raise(SIGUSR2) || inner_seen != 1) _exit(10);
    finished = 1;
}
static void *sender(void *unused)
{
    (void)unused;
    usleep(20000);
    if (pthread_kill(main_thread, SIGUSR1)) _exit(11);
    return 0;
}
int main(void)
{
    const size_t bytes = 65536;
    unsigned char *area = mmap(0, bytes, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    unsigned char *alternate = mmap(0, bytes, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (area == MAP_FAILED || alternate == MAP_FAILED) return 1;
    memset(area, 0xa5, bytes);
    stack_t alt = {.ss_sp = alternate, .ss_size = bytes};
    if (sigaltstack(&alt, 0)) return 2;
    struct sigaction action = {.sa_handler = handler, .sa_flags = SA_ONSTACK};
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR1, &action, 0) || sigaction(SIGUSR2, &action, 0)) return 3;
    pthread_t thread;
    main_thread = pthread_self();
    if (pthread_create(&thread, 0, sender, 0)) return 4;
    small_signal_stack(area + bytes - 16);
    if (pthread_join(thread, 0)) return 5;
    if (outer_seen != 1 || inner_seen != 1 || memcmp(fp_pattern, fp_observed, 16)) return 6;
    size_t changed = 0;
    for (size_t i = 0; i < bytes - 2048; ++i) changed += area[i] != 0xa5;
    printf("SIGNAL_SMALL_STACK: outer=%d inner=%d bytes_changed_below_2KiB=%zu\n",
        (int)outer_seen, (int)inner_seen, changed);
    return changed ? 7 : 0;
}
