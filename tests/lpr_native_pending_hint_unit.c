#include <stdint.h>
#include <stdio.h>
#include <setjmp.h>

#include "../userland/personality/linux/runtime/lpr_filed_internal.h"

lpr_state_t lpr_state;
static struct lpr_linux_user_frame frame;
static struct lpr_linux_user_frame *active_frame;
static unsigned queries;
static uint64_t queried_rax;
static int publish_during_query;
static int kernel_pending, deliveries;
static jmp_buf delivered;
static int64_t wait_status;
static lpr_signal_thread_state_t thread_hint_state;

const struct lpr_linux_user_frame *lpr_current_linux_user_frame(void)
{
    return active_frame;
}

/* Include the real function, including its hint state. The old pending-frame
 * unit stubs this function and cannot detect a hint=0 early-return bug. */
#include "../userland/personality/linux/runtime/lpr_signal.c"
#define lpr_current_linux_user_frame lpr_unused_dispatch_frame
#include "../userland/personality/linux/runtime/lpr_dispatch.c"
#undef lpr_current_linux_user_frame

int64_t lpr_pacha_syscall2(uint64_t nr, uint64_t operation, uint64_t argument)
{
    if (nr != PACHAOS_SYSCALL_PROCESS_SIGNAL_CTL ||
        operation != PACHAOS_PROCESS_SIGNAL_CTL_DELIVER_PENDING_FRAME ||
        argument != (uint64_t)(uintptr_t)active_frame) {
        fprintf(stderr, "unexpected pending-frame request\n");
        return -1;
    }
    ++queries;
    queried_rax = active_frame->rax;
    if (publish_during_query) {
        if (active_frame->runtime_signal_state)
            ((lpr_signal_thread_state_t *)(uintptr_t)active_frame->runtime_signal_state)->native_pending_hint = 1;
        else lpr_native_pending_hint = 1;
    }
    if (kernel_pending) { kernel_pending = 0; ++deliveries; longjmp(delivered, 1); }
    return 0; /* no claim: the existing dispatch frame must remain reusable */
}

int64_t lpr_pacha_syscall3(uint64_t n, uint64_t a, uint64_t b, uint64_t c)
{ (void)n; (void)a; (void)b; (void)c; return 0; }
int64_t lpr_pacha_syscall4(uint64_t n, uint64_t a, uint64_t b, uint64_t c, uint64_t flags)
{
    (void)a; (void)b; (void)c;
    if (n == PACHAOS_SYSCALL_FUTEX_WAIT && flags == 0) return wait_status;
    return 0;
}
int64_t lpr_pacha_clock_gettime(uint64_t id, struct pachaos_timespec *time)
{ (void)id; *time = (struct pachaos_timespec){0}; return 0; }

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static void reset(void)
{
    lpr_state = (lpr_state_t){0};
    lpr_state.thread_count = 1;
    frame = (struct lpr_linux_user_frame){0};
    frame.rax = 123;
    active_frame = &frame;
    queries = 0;
    queried_rax = 0;
    publish_during_query = 0;
    kernel_pending = deliveries = 0;
    wait_status = PACHA_SYSCALL_ERR_NOT_READY;
    lpr_native_pending_hint_active = 1;
    lpr_native_pending_hint = 0;
    thread_hint_state = (lpr_signal_thread_state_t){0};
}

int main(void)
{
    /* Deterministic wake-before-hint boundary: the blocking layer has already
     * unwound to an EINTR probe, but the sender has not published the word.
     * A 0 must not suppress the authoritative kernel query. */
    reset();
    lpr_linux_deliver_native_pending_frame(-LPR_LINUX_EINTR);
    CHECK(queries == 1);
    CHECK(queried_rax == (uint64_t)-LPR_LINUX_EINTR);
    CHECK(frame.rax == 123);
    CHECK(lpr_native_pending_hint == 0);

    /* Ordinary completions retain the existing zero-hint fast path. */
    reset();
    lpr_linux_deliver_native_pending_frame(0);
    lpr_linux_deliver_native_pending_frame(-LPR_LINUX_EAGAIN);
    CHECK(queries == 0 && frame.rax == 123);

    reset();
    lpr_native_pending_hint = 1;
    lpr_linux_deliver_native_pending_frame(37);
    CHECK(queries == 1 && queried_rax == 37 && frame.rax == 123);
    CHECK(lpr_native_pending_hint == 0);

    /* A sibling may own the pending bit; its advisory word is never cleared. */
    reset();
    lpr_state.thread_count = 2;
    lpr_native_pending_hint = 1;
    lpr_linux_deliver_native_pending_frame(0);
    CHECK(queries == 1 && lpr_native_pending_hint == 1);
    lpr_native_pending_hint = 0;
    lpr_linux_deliver_native_pending_frame(-LPR_LINUX_EINTR);
    CHECK(queries == 2 && lpr_native_pending_hint == 0 && frame.rax == 123);

    /* A registered thread slot lets siblings skip the native query. The
     * process-wide fallback above must continue probing when a sibling runs. */
    reset();
    lpr_state.thread_count = 2;
    thread_hint_state.native_pending_hint_active = 1;
    frame.runtime_signal_state = (uintptr_t)&thread_hint_state;
    lpr_linux_deliver_native_pending_frame(0);
    CHECK(queries == 0);
    thread_hint_state.native_pending_hint = 1;
    lpr_linux_deliver_native_pending_frame(0);
    CHECK(queries == 1 && thread_hint_state.native_pending_hint == 0);
    kernel_pending = 1;
    if (setjmp(delivered) == 0) {
        lpr_linux_deliver_native_pending_frame(-LPR_LINUX_EINTR);
        CHECK(0);
    }
    CHECK(queries == 2 && deliveries == 1 && kernel_pending == 0);

    reset();
    lpr_native_pending_hint_active = 0;
    lpr_linux_deliver_native_pending_frame(0);
    CHECK(queries == 1 && frame.rax == 123);

    reset();
    active_frame = 0;
    lpr_native_pending_hint = 1;
    lpr_linux_deliver_native_pending_frame(-LPR_LINUX_EINTR);
    CHECK(queries == 0 && lpr_native_pending_hint == 1);

    /* A new publication during the query survives the pre-query clearing. */
    reset();
    publish_during_query = 1;
    lpr_linux_deliver_native_pending_frame(-LPR_LINUX_EINTR);
    CHECK(queries == 1 && lpr_native_pending_hint == 1 && frame.rax == 123);
    /* The real futex handler carries NOT_READY through its ordinary EAGAIN
     * result. The real pending-frame helper consumes the local output once. */
    reset();
    uint32_t word = 0;
    unsigned observed = 0;
    int64_t result = lpr_sys_futex((uintptr_t)&word, 0, 0, 0, 0, 0, &observed);
    CHECK(result == -LPR_LINUX_EAGAIN && observed == 1);
    kernel_pending = 1;
    /* Before: the old ordinary-result hint fast path leaves it pending. */
    lpr_linux_deliver_native_pending_frame(result);
    CHECK(kernel_pending == 1 && queries == 0);
    if (setjmp(delivered) == 0) {
        lpr_linux_deliver_native_pending_frame_after_wait(result, &observed);
        CHECK(0);
    }
    CHECK(deliveries == 1 && queries == 1 && kernel_pending == 0);
    CHECK(queried_rax == (uint64_t)-LPR_LINUX_EAGAIN);

    reset(); observed = 0;
    result = lpr_sys_futex((uintptr_t)&word, 0, 0, 0, 0, 0, &observed);
    lpr_linux_deliver_native_pending_frame_after_wait(result, &observed);
    CHECK(result == -LPR_LINUX_EAGAIN && queries == 1 && deliveries == 0 && !observed);
    lpr_linux_deliver_native_pending_frame_after_wait(result, &observed);
    CHECK(queries == 1);
    reset(); observed = 0; wait_status = 0;
    result = lpr_sys_futex((uintptr_t)&word, 0, 0, 0, 0, 0, &observed);
    lpr_linux_deliver_native_pending_frame_after_wait(result, &observed);
    CHECK(result == 0 && !observed && queries == 0);

    puts("LPR native pending hint: real function and wake-before-hint cases PASS");
    return 0;
}

#include "lpr_ipc_mock.h"
int64_t lpr_ipc_message(uint64_t nr, uint64_t fd, uint64_t send, uint64_t receive,
    uint64_t timeout, uint64_t flags, int *committed) {
    return test_lpr_ipc_noncall(nr, fd, send, receive, timeout, flags, committed);
}
