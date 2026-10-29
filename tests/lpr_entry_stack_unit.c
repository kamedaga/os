/* SPDX-License-Identifier: MIT */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include "../userland/personality/linux/runtime/lpr_entry_stack.c"

static uint64_t test_tid = 1, next_fd = 16;
static unsigned closed, unmapped;
static uint64_t exited_fd;
void *lpr_memset(void *p, int v, size_t n) { return memset(p, v, n); }
void *lpr_memcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
int64_t lpr_pacha_syscall0(uint64_t nr)
{
    assert(nr == PACHAOS_SYSCALL_GETTID);
    return test_tid;
}
int64_t lpr_pacha_syscall1(uint64_t nr, uint64_t value)
{
    assert(nr == PACHAOS_SYSCALL_FD_CLOSE && value >= 16);
    ++closed;
    return 0;
}
int64_t lpr_pacha_syscall2(uint64_t nr, uint64_t p, uint64_t bytes)
{
    assert(nr == PACHAOS_SYSCALL_MUNMAP);
    ++unmapped;
    return munmap((void *)(uintptr_t)p, bytes);
}
int64_t lpr_pacha_syscall3(uint64_t nr, uint64_t p, uint64_t n, uint64_t flags)
{
    assert(nr == PACHAOS_SYSCALL_MPROTECT && !flags);
    return mprotect((void *)(uintptr_t)p, n, PROT_NONE);
}
int64_t lpr_pacha_syscall4(uint64_t nr, uint64_t first, uint64_t second, uint64_t third, uint64_t fourth)
{
    if (nr == PACHAOS_SYSCALL_FD_DUP) {
        assert(first == PACHAOS_THREAD_SELF_FD && second == 16);
        assert(third == (PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL | PACHA_FD_RIGHT_CLOSE));
        assert(fourth == (PACHA_FD_FLAG_PRIVATE | PACHA_FD_FLAG_CLOEXEC));
        return next_fd++;
    }
    assert(nr == PACHA_FD_SYSCALL_WAIT_MANY && second == 1 && !third && !fourth);
    struct pacha_pollfd *p = (void *)(uintptr_t)first;
    p->revents = p->fd == exited_fd ? PACHA_FD_EVENT_READABLE : 0;
    return p->revents ? 1 : 0;
}
int64_t lpr_pacha_syscall6(uint64_t nr, uint64_t a, uint64_t b, uint64_t bytes, uint64_t protection, uint64_t flags, uint64_t last)
{
    assert(nr == PACHAOS_SYSCALL_MMAP && !a && !b && !last);
    assert(protection == (PACHAOS_PROT_READ | PACHAOS_PROT_WRITE));
    assert(flags == (PACHAOS_MMAP_PRIVATE | PACHAOS_MMAP_ANONYMOUS));
    void *p = mmap(0, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(p != MAP_FAILED);
    return (int64_t)(uintptr_t)p;
}
int main(void)
{
    lpr_entry_stack_initialize_current();
    struct lpr_entry_context *first = lpr_entry_context_head;
    assert(first && first->tid == 1);
    assert(lpr_entry_stack_acquire_bootstrap() == (uintptr_t)lpr_entry_bootstrap_stack + LPR_ENTRY_BOOTSTRAP_BYTES);
    uintptr_t saved = ((uintptr_t)lpr_entry_bootstrap_stack + LPR_ENTRY_BOOTSTRAP_BYTES - LPR_SYSCALL_ENTRY_RESERVE_BYTES) & ~(uintptr_t)15;
    memset((void *)saved, 0xa5, LPR_SYSCALL_ENTRY_FRAME_BYTES);
    struct lpr_entry_stack_result moved = lpr_entry_stack_prepare(saved);
    assert(moved.release_bootstrap && moved.stack != saved);
    assert(((struct lpr_linux_user_frame *)(moved.stack + 56))->fp_state == moved.stack + 224);
    lpr_entry_stack_release_bootstrap();
    struct lpr_entry_stack_result stable = lpr_entry_stack_prepare(moved.stack);
    assert(stable.stack == moved.stack && !stable.release_bootstrap);
    uint64_t base, bytes;
    lpr_entry_stack_async_range(&base, &bytes);
    assert(base == first->async_base && bytes == first->async_size);
    /* Retaining a thread object does not reserve its numeric native TID:
     * the scheduler can reuse that slot while the old capability is exited. */
    exited_fd = first->thread_fd;
    lpr_entry_stack_initialize_current();
    first = lpr_entry_context_head;
    assert((uint64_t)first->thread_fd != exited_fd && first->tid == test_tid);
    assert(closed == 1 && unmapped == 1);
    closed = unmapped = 0;
    exited_fd = 0;
    test_tid = 2;
    lpr_entry_stack_initialize_current();
    assert(lpr_entry_context_head->async_base != first->async_base);
    assert(!closed && !unmapped);
    exited_fd = first->thread_fd;
    test_tid = 3;
    lpr_entry_stack_initialize_current();
    assert(closed == 1 && unmapped == 1);
    assert(lpr_entry_context_head->next->tid == 2 && !lpr_entry_context_head->next->next);
    /* Copied PRIVATE fd numbers are not capabilities in a fork child. */
    lpr_entry_stack_after_fork_child();
    assert(!lpr_entry_context_head && closed == 1 && unmapped == 3);
    assert(!lpr_entry_context_lock && !lpr_entry_bootstrap_lock);
    puts("LPR_ENTRY_STACK: relocation, independent stacks, exited reclamation and fork cleanup PASS");
}
