/* SPDX-License-Identifier: MIT */
#include "lpr_entry_stack.h"
#include "lpr_filed_internal.h"
#include <personality/lpr_signal_stack.h>
#include <pacha/poll.h>

/* Private stacks belong to native threads, not goroutines or libc TLS. A
 * retained native thread capability identifies its generation until its exited stack can
 * be collected by the next thread creation. No fixed thread-count limit. */
struct lpr_entry_context {
    struct lpr_entry_context *next;
    uint64_t tid;
    uintptr_t stack_top;
    uint64_t mapping, mapping_bytes;
    uint64_t async_base, async_size;
    pacha_fd_t thread_fd;
};
_Static_assert(offsetof(struct lpr_entry_context, next) == 0, "entry assembly next");
_Static_assert(offsetof(struct lpr_entry_context, tid) == 8, "entry assembly tid");
_Static_assert(offsetof(struct lpr_entry_context, stack_top) == 16, "entry assembly stack");
struct lpr_entry_context *lpr_entry_context_head;
volatile uint32_t lpr_entry_context_lock;
volatile uint32_t lpr_entry_bootstrap_lock;
unsigned char lpr_entry_bootstrap_stack[LPR_ENTRY_BOOTSTRAP_BYTES] __attribute__((aligned(16)));
/* epoll_wait -> poll_cached -> poll_scan nests their fixed work arrays.
 * The current compiled path exceeds 64 KiB; allow 128 KiB with guards,
 * rather than borrowing any space from a caller goroutine. */
enum { ENTRY_PAGE = 4096, ENTRY_STACK_BYTES = 131072, ASYNC_STACK_BYTES = 16384,
    ENTRY_MAPPING_BYTES = ENTRY_PAGE * 3 + ENTRY_STACK_BYTES + ASYNC_STACK_BYTES };

static void spin_lock(volatile uint32_t *lock)
{
    while (__atomic_exchange_n(lock, 1u, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause");
}
static void spin_unlock(volatile uint32_t *lock)
{
    __atomic_store_n(lock, 0u, __ATOMIC_RELEASE);
}
static _Noreturn void entry_failure(void)
{
    /* Returning to a caller whose stack cannot safely host LPR would corrupt
     * that caller. Failed private-stack setup is a process startup failure. */
    (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_PROCESS_EXIT, 127);
    __builtin_trap();
}

static void collect_exited(void)
{
    struct lpr_entry_context *retired = 0;
    spin_lock(&lpr_entry_context_lock);
    struct lpr_entry_context **link = &lpr_entry_context_head;
    while (*link) {
        struct lpr_entry_context *context = *link;
        struct pacha_pollfd poll = {.fd = (uint64_t)context->thread_fd,
            .events = PACHA_FD_EVENT_READABLE};
        const int64_t ready = lpr_pacha_syscall4(PACHA_FD_SYSCALL_WAIT_MANY,
            (uint64_t)(uintptr_t)&poll, 1, 0, 0);
        if (ready > 0 && (poll.revents & PACHA_FD_EVENT_READABLE)) {
            *link = context->next;
            context->next = retired;
            retired = context;
        } else link = &context->next;
    }
    spin_unlock(&lpr_entry_context_lock);
    /* Assembly readers hold the same lock and only borrow their own live
     * stack. Exit readiness proves no execution can still use a retired one. */
    while (retired) {
        struct lpr_entry_context *next = retired->next;
        const uint64_t mapping = retired->mapping, bytes = retired->mapping_bytes;
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, retired->thread_fd);
        (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP, mapping, bytes);
        retired = next;
    }
}

static struct lpr_entry_context *current_context(void)
{
    const int64_t tid = lpr_pacha_syscall0(PACHAOS_SYSCALL_GETTID);
    if (tid <= 0) entry_failure();
    /* GETTID is a reusable scheduler slot, not an object generation. A new
     * clone enters here on the bootstrap stack before running user code.
     * Retire old generations before matching that numeric TID, otherwise
     * another initializer could collect a reused thread's active stack. */
    collect_exited();
    spin_lock(&lpr_entry_context_lock);
    for (struct lpr_entry_context *context = lpr_entry_context_head; context; context = context->next) {
        if (context->tid == (uint64_t)tid) {
            spin_unlock(&lpr_entry_context_lock);
            return context;
        }
    }
    spin_unlock(&lpr_entry_context_lock);
    const int64_t mapping = lpr_pacha_syscall6(PACHAOS_SYSCALL_MMAP, 0, 0,
        ENTRY_MAPPING_BYTES, PACHAOS_PROT_READ | PACHAOS_PROT_WRITE,
        PACHAOS_MMAP_PRIVATE | PACHAOS_MMAP_ANONYMOUS, 0);
    if (mapping < ENTRY_PAGE) entry_failure();
    /* Metadata has its own page; separate guards bound dispatch and native
     * async scratch so overflow cannot silently damage a sibling stack. */
    const uint64_t dispatch_guard = (uint64_t)mapping + ENTRY_PAGE;
    const uint64_t async_guard = dispatch_guard + ENTRY_PAGE + ENTRY_STACK_BYTES;
    if (lpr_pacha_syscall3(PACHAOS_SYSCALL_MPROTECT, dispatch_guard, ENTRY_PAGE, 0) ||
        lpr_pacha_syscall3(PACHAOS_SYSCALL_MPROTECT, async_guard, ENTRY_PAGE, 0)) {
        (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP, mapping, ENTRY_MAPPING_BYTES);
        entry_failure();
    }
    const int64_t fd = lpr_pacha_syscall4(PACHAOS_SYSCALL_FD_DUP,
        PACHAOS_THREAD_SELF_FD, 16,
        PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL | PACHA_FD_RIGHT_CLOSE,
        PACHA_FD_FLAG_PRIVATE | PACHA_FD_FLAG_CLOEXEC);
    if (fd < 16) {
        (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP, mapping, ENTRY_MAPPING_BYTES);
        entry_failure();
    }
    struct lpr_entry_context *context = (void *)(uintptr_t)mapping;
    lpr_memset(context, 0, sizeof(*context));
    context->tid = (uint64_t)tid;
    context->stack_top = async_guard;
    context->mapping = (uint64_t)mapping;
    context->mapping_bytes = ENTRY_MAPPING_BYTES;
    context->async_base = async_guard + ENTRY_PAGE;
    context->async_size = ASYNC_STACK_BYTES;
    /* Interrupt return cannot resolve a user demand fault. Materialize scratch
     * before enabling delivery so a CPU-bound task can receive its first
     * signal without first entering another syscall. */
    for (uint64_t offset = 0; offset < ASYNC_STACK_BYTES; offset += ENTRY_PAGE)
        *(volatile unsigned char *)(uintptr_t)(context->async_base + offset) = 0;
    context->thread_fd = (pacha_fd_t)fd;
    spin_lock(&lpr_entry_context_lock);
    context->next = lpr_entry_context_head;
    lpr_entry_context_head = context;
    spin_unlock(&lpr_entry_context_lock);
    return context;
}

void lpr_entry_stack_initialize_current(void) { (void)current_context(); }
void lpr_entry_stack_async_range(uint64_t *base, uint64_t *size)
{
    struct lpr_entry_context *context = current_context();
    *base = context->async_base;
    *size = context->async_size;
}
struct lpr_entry_stack_result lpr_entry_stack_prepare(uintptr_t captured)
{
    const uintptr_t bootstrap = (uintptr_t)lpr_entry_bootstrap_stack;
    if (captured < bootstrap || captured >= bootstrap + sizeof(lpr_entry_bootstrap_stack))
        return (struct lpr_entry_stack_result){captured, 0};
    struct lpr_entry_context *context = current_context();
    const uintptr_t target = (context->stack_top - LPR_SYSCALL_ENTRY_RESERVE_BYTES) & ~(uintptr_t)15;
    lpr_memcpy((void *)target, (const void *)captured, LPR_SYSCALL_ENTRY_FRAME_BYTES);
    struct lpr_linux_user_frame *frame = (void *)(target + 56);
    frame->fp_state = target + 224;
    return (struct lpr_entry_stack_result){target, 1};
}
uintptr_t lpr_entry_stack_acquire_bootstrap(void)
{
    spin_lock(&lpr_entry_bootstrap_lock);
    return (uintptr_t)lpr_entry_bootstrap_stack + sizeof(lpr_entry_bootstrap_stack);
}
void lpr_entry_stack_release_bootstrap(void) { spin_unlock(&lpr_entry_bootstrap_lock); }
void lpr_entry_stack_after_fork_child(void)
{
    /* Native fork does not transfer these private capabilities. Do not close
     * their copied numbers, which can already identify unrelated child FDs. */
    struct lpr_entry_context *context = lpr_entry_context_head;
    lpr_entry_context_head = 0;
    lpr_entry_context_lock = lpr_entry_bootstrap_lock = 0;
    while (context) {
        struct lpr_entry_context *next = context->next;
        const uint64_t mapping = context->mapping, bytes = context->mapping_bytes;
        (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP, mapping, bytes);
        context = next;
    }
}
