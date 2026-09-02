#include <stdint.h>
#include <stdio.h>

#include "../userland/personality/linux/runtime/lpr_filed_internal.h"

lpr_state_t lpr_state;

static uint64_t g_syscall_count;
static uint64_t g_syscall_nr;
static uint64_t g_syscall_a0;
static uint64_t g_syscall_a1;
static int64_t g_syscall_result;
static uint64_t g_native_frame_count;
static int64_t g_native_frame_result;
static uint64_t g_handler_count;
static struct lpr_linux_user_frame *g_active_user_frame;

const struct lpr_linux_user_frame *lpr_current_linux_user_frame(void)
{
    return g_active_user_frame;
}

int64_t lpr_pacha_syscall1(uint64_t nr, uint64_t a0)
{
    (void)nr;
    (void)a0;
    return 0;
}

int64_t lpr_pacha_syscall2(uint64_t nr, uint64_t a0, uint64_t a1)
{
    g_syscall_count++;
    g_syscall_nr = nr;
    g_syscall_a0 = a0;
    g_syscall_a1 = a1;
    return g_syscall_result;
}

int64_t lpr_pacha_status_to_errno(int64_t status)
{
    return status;
}

void lpr_linux_deliver_native_pending_frame(int64_t interrupted_result)
{
    g_native_frame_count++;
    g_native_frame_result = interrupted_result;
}

void lpr_linux_prepare_process_exit(uint64_t exit_code)
{
    (void)exit_code;
}

void lpr_signal_source_diag(
    const char *source,
    uint32_t sig,
    uint64_t arg0,
    uint64_t arg1)
{
    (void)source;
    (void)sig;
    (void)arg0;
    (void)arg1;
}

/* Single-threaded harness: back the per-thread signal state with slot 0 so
 * reset_state()'s lpr_state wipe resets it, matching the real slot storage. */
lpr_signal_thread_state_t *lpr_signal_thread_state_current(void)
{
    return &lpr_state.signal.threads[0].state;
}

void lpr_signal_thread_state_after_fork_child(void)
{
    lpr_signal_thread_state_t *state = lpr_signal_thread_state_current();
    state->pending_mask = 0;
    state->wait_restore_mask = 0;
    state->wait_restore_mask_active = 0;
    state->dispatching = 0;
}

#include "../userland/personality/linux/runtime/lpr_tty/client.c"

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        return 1; \
    } \
} while (0)

static void fake_handler(int sig)
{
    (void)sig;
    g_handler_count++;
}

static void reset_state(void)
{
    lpr_state = (lpr_state_t){0};
    g_syscall_count = 0;
    g_syscall_nr = 0;
    g_syscall_a0 = 0;
    g_syscall_a1 = 0;
    g_syscall_result = 0;
    g_native_frame_count = 0;
    g_native_frame_result = 0;
    g_handler_count = 0;
    g_active_user_frame = 0;
}

int main(void)
{
    const uint32_t custom_signal = LPR_LINUX_SIGPIPE;
    const uint64_t custom_bit = lpr_linux_signal_bit(custom_signal);

    reset_state();
    lpr_linux_sigactions[custom_signal].handler = (uint64_t)(uintptr_t)fake_handler;
    lpr_linux_queue_signal(custom_signal);
    CHECK(lpr_linux_dispatch_pending_signals() == 0);
    CHECK(g_handler_count == 0);
    CHECK(g_syscall_count == 0);
    CHECK(g_native_frame_count == 0);
    CHECK((lpr_linux_pending_signal_mask & custom_bit) != 0);

    struct lpr_linux_user_frame active_frame = {0};
    g_active_user_frame = &active_frame;
    CHECK(lpr_linux_dispatch_pending_signals() == -LPR_LINUX_EINTR);
    CHECK(g_handler_count == 0);
    CHECK(g_syscall_count == 1);
    CHECK(g_syscall_nr == PACHAOS_SYSCALL_PROCESS_SIGNAL);
    CHECK(g_syscall_a0 == PACHAOS_PROCESS_SELF_FD);
    CHECK(g_syscall_a1 == custom_signal);
    CHECK((lpr_linux_pending_signal_mask & custom_bit) == 0);
    CHECK(g_native_frame_count == 1);
    CHECK(g_native_frame_result == -LPR_LINUX_EINTR);
    CHECK(lpr_linux_signal_dispatching == 0);

    reset_state();
    lpr_linux_sigactions[custom_signal].handler = (uint64_t)(uintptr_t)fake_handler;
    lpr_linux_signal_mask = custom_bit;
    lpr_linux_queue_signal(custom_signal);
    CHECK(lpr_linux_dispatch_pending_signals() == 0);
    CHECK(g_handler_count == 0);
    CHECK(g_syscall_count == 0);
    CHECK(g_native_frame_count == 0);
    CHECK((lpr_linux_pending_signal_mask & custom_bit) != 0);
    CHECK(lpr_linux_signal_dispatching == 0);

    reset_state();
    lpr_linux_sigactions[custom_signal].handler = (uint64_t)(uintptr_t)fake_handler;
    g_syscall_result = -37;
    g_active_user_frame = &active_frame;
    lpr_linux_queue_signal(custom_signal);
    CHECK(lpr_linux_dispatch_pending_signals() == -37);
    CHECK(g_handler_count == 0);
    CHECK(g_syscall_count == 1);
    CHECK(g_native_frame_count == 0);
    CHECK((lpr_linux_pending_signal_mask & custom_bit) != 0);
    CHECK(lpr_linux_signal_dispatching == 0);

    reset_state();
    lpr_linux_sigactions[custom_signal].handler = LPR_LINUX_SIG_IGN;
    lpr_linux_queue_signal(custom_signal);
    CHECK(lpr_linux_dispatch_pending_signals() == 0);
    CHECK(g_handler_count == 0);
    CHECK(g_syscall_count == 0);
    CHECK(g_native_frame_count == 0);
    CHECK((lpr_linux_pending_signal_mask & custom_bit) == 0);

    reset_state();
    lpr_linux_queue_signal(LPR_LINUX_SIGCHLD);
    CHECK(lpr_linux_dispatch_pending_signals() == 0);
    CHECK(g_handler_count == 0);
    CHECK(g_syscall_count == 0);
    CHECK(g_native_frame_count == 0);
    CHECK((lpr_linux_pending_signal_mask & lpr_linux_signal_bit(LPR_LINUX_SIGCHLD)) == 0);

    reset_state();
    uint64_t pending_mask = UINT64_MAX;
    const uint64_t expected_pending_mask =
        custom_bit | lpr_linux_signal_bit(LPR_LINUX_SIGCHLD);
    lpr_linux_pending_signal_mask = expected_pending_mask;
    CHECK(lpr_linux_rt_sigpending(
        (uint64_t)(uintptr_t)&pending_mask, sizeof(pending_mask)) == 0);
    CHECK(pending_mask == expected_pending_mask);

    pending_mask = UINT64_MAX;
    CHECK(lpr_linux_rt_sigpending(
        (uint64_t)(uintptr_t)&pending_mask, sizeof(pending_mask) - 1u) == -LPR_LINUX_EINVAL);
    CHECK(pending_mask == UINT64_MAX);

    CHECK(lpr_linux_rt_sigpending(0, sizeof(pending_mask)) == -LPR_LINUX_EFAULT);

    reset_state();
    lpr_linux_pending_signal_mask = custom_bit | lpr_linux_signal_bit(LPR_LINUX_SIGCHLD);
    lpr_linux_wait_restore_mask = lpr_linux_signal_bit(LPR_LINUX_SIGCONT);
    lpr_linux_wait_restore_mask_active = 1;
    lpr_linux_signal_dispatching = 1;
    lpr_linux_sigactions[custom_signal].handler = (uint64_t)(uintptr_t)fake_handler;
    lpr_linux_signal_mask = lpr_linux_signal_bit(LPR_LINUX_SIGPIPE);
    lpr_linux_altstack_sp = 0x12345000u;
    lpr_linux_altstack_size = 0x4000u;
    lpr_linux_altstack_flags = 0x5u;
    lpr_linux_signal_runtime_registered = 1;
    lpr_linux_signal_after_fork_child();
    CHECK(lpr_linux_pending_signal_mask == 0);
    CHECK(lpr_linux_wait_restore_mask == 0);
    CHECK(lpr_linux_wait_restore_mask_active == 0);
    CHECK(lpr_linux_signal_dispatching == 0);
    CHECK(lpr_linux_sigactions[custom_signal].handler == (uint64_t)(uintptr_t)fake_handler);
    CHECK(lpr_linux_signal_mask == lpr_linux_signal_bit(LPR_LINUX_SIGPIPE));
    CHECK(lpr_linux_altstack_sp == 0x12345000u);
    CHECK(lpr_linux_altstack_size == 0x4000u);
    CHECK(lpr_linux_altstack_flags == 0x5u);
    CHECK(lpr_linux_signal_runtime_registered == 1);

    puts("LPR_PENDING_SIGNAL_FRAME_UNIT=OK");
    return 0;
}
