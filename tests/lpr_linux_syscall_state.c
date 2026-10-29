/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>

static unsigned char pattern[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
static struct timespec clock_result;
extern long small_stack_call(void *top);
__asm__(".text\n.global small_stack_call\n.type small_stack_call,@function\n"
        "small_stack_call: push %r12; mov %rsp,%r12; mov %rdi,%rsp\n"
        "mov $228,%eax; mov $1,%edi; lea clock_result(%rip),%rsi; syscall\n"
        "mov %r12,%rsp; pop %r12; ret\n.size small_stack_call,.-small_stack_call\n");
static int simd_check(long number, long first, long second, long third)
{
    unsigned char xmm0[16], xmm1[16];
    long result = number;
    __asm__ volatile(
        "movdqu (%[pattern]),%%xmm0; movdqu (%[pattern]),%%xmm1; syscall; "
        "movdqu %%xmm0,(%[out0]); movdqu %%xmm1,(%[out1])"
        : "+a"(result), "+D"(first), "+S"(second), "+d"(third)
        : [pattern]"r"(pattern), [out0]"r"(xmm0), [out1]"r"(xmm1)
        : "rcx", "r11", "xmm0", "xmm1", "memory");
    return result < 0 || memcmp(pattern, xmm0, 16) || memcmp(pattern, xmm1, 16);
}
int main(void)
{
    char target[4096];
    if (simd_check(SYS_getpid, 0, 0, 0) ||
        simd_check(SYS_clock_gettime, CLOCK_MONOTONIC, (long)&clock_result, 0) ||
        simd_check(SYS_readlink, (long)"/proc/self/exe", (long)target, sizeof(target)) ||
        simd_check(SYS_getrandom, (long)target, 64, 0)) return 1;
    size_t bytes = 65536;
    unsigned char *stack = mmap(0, bytes, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stack == MAP_FAILED) return 2;
    memset(stack, 0xa5, bytes);
    if (small_stack_call(stack + bytes - 16)) return 3;
    /* A syscall from a 2 KiB goroutine must not write into a neighbour. */
    for (size_t i = 0; i < bytes - 2048; ++i)
        if (stack[i] != 0xa5) return 4;
    munmap(stack, bytes);
    puts("SYSCALL_STATE: SIMD preserved and no writes below 2 KiB PASS");
    return 0;
}
