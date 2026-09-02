#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

#include <pacha/abi.h>
#include <pachaos/abi.h>
#include "../userland/personality/linux/runtime/lpr_filed_internal.h"

lpr_state_t lpr_state;

static unsigned char fake_session_page[4096];
static atomic_int fast_start;
static atomic_int fast_attempted;
static atomic_int fast_finished;
static atomic_int page_unmapped;
static atomic_int stale_page_observed;
static atomic_int zero_page_observed;
static atomic_int page_visible_at_munmap;
static atomic_uint munmap_calls;
static atomic_uint close_calls;
static int wait_for_fast_finish_in_munmap;
static int failures;

void lpr_file_image_cache_clear(void)
{
}

int64_t lpr_pacha_syscall1(uint64_t nr, uint64_t a0)
{
    (void)a0;
    if (nr == PACHAOS_SYSCALL_FD_CLOSE) {
        atomic_fetch_add_explicit(&close_calls, 1u, memory_order_relaxed);
    }
    return 0;
}

int64_t lpr_pacha_syscall2(uint64_t nr, uint64_t a0, uint64_t a1)
{
    (void)a0;
    (void)a1;
    if (nr != PACHAOS_SYSCALL_MUNMAP) {
        return 0;
    }

    atomic_fetch_add_explicit(&munmap_calls, 1u, memory_order_relaxed);
    atomic_store_explicit(&page_unmapped, 1, memory_order_release);
    if (lpr_session_page != 0) {
        atomic_store_explicit(
            &page_visible_at_munmap, 1, memory_order_release);
    }
    atomic_store_explicit(&fast_start, 1, memory_order_release);
    while (!atomic_load_explicit(&fast_attempted, memory_order_acquire)) {
        sched_yield();
    }
    if (wait_for_fast_finish_in_munmap) {
        while (!atomic_load_explicit(&fast_finished, memory_order_acquire)) {
            sched_yield();
        }
    }
    return 0;
}

int64_t lpr_pacha_syscall3(
    uint64_t nr,
    uint64_t a0,
    uint64_t a1,
    uint64_t a2)
{
    (void)nr;
    (void)a0;
    (void)a1;
    (void)a2;
    sched_yield();
    return 0;
}

static void expect(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

static void reset_case(void)
{
    lpr_state.filed_rpc.lock_word = 0;
    lpr_state.thread_count = 2;
    lpr_session_fd = 23;
    lpr_session_page_fd = 24;
    lpr_session_page = fake_session_page;
    lpr_session_checked = 1;
    lpr_session_payload_busy = 1;
    atomic_store_explicit(&fast_start, 0, memory_order_relaxed);
    atomic_store_explicit(&fast_attempted, 0, memory_order_relaxed);
    atomic_store_explicit(&fast_finished, 0, memory_order_relaxed);
    atomic_store_explicit(&page_unmapped, 0, memory_order_relaxed);
    atomic_store_explicit(&stale_page_observed, 0, memory_order_relaxed);
    atomic_store_explicit(&zero_page_observed, 0, memory_order_relaxed);
    atomic_store_explicit(&page_visible_at_munmap, 0, memory_order_relaxed);
    atomic_store_explicit(&munmap_calls, 0u, memory_order_relaxed);
    atomic_store_explicit(&close_calls, 0u, memory_order_relaxed);
}

static void *fast_path_probe(void *unused)
{
    (void)unused;
    while (!atomic_load_explicit(&fast_start, memory_order_acquire)) {
        sched_yield();
    }
    atomic_store_explicit(&fast_attempted, 1, memory_order_release);

    lpr_state_lock(&lpr_state.filed_rpc.lock_word);
    void *const page = lpr_session_page;
    if (page == 0) {
        atomic_store_explicit(&zero_page_observed, 1, memory_order_release);
    } else if (atomic_load_explicit(&page_unmapped, memory_order_acquire)) {
        atomic_store_explicit(
            &stale_page_observed, 1, memory_order_release);
    }
    lpr_state_unlock(&lpr_state.filed_rpc.lock_word);

    atomic_store_explicit(&fast_finished, 1, memory_order_release);
    return 0;
}

static void old_order_session_drop(void)
{
    if (lpr_session_page != 0) {
        (void)lpr_pacha_syscall2(
            PACHAOS_SYSCALL_MUNMAP,
            (uint64_t)(uintptr_t)lpr_session_page,
            FILED_SESSION_PAGE_BYTES);
    }
    if (lpr_session_page_fd >= 16) {
        (void)lpr_pacha_syscall1(
            PACHAOS_SYSCALL_FD_CLOSE,
            (uint64_t)(uint32_t)lpr_session_page_fd);
    }
    if (lpr_session_fd >= 16) {
        (void)lpr_pacha_syscall1(
            PACHAOS_SYSCALL_FD_CLOSE,
            (uint64_t)(uint32_t)lpr_session_fd);
    }
    lpr_session_fd = -1;
    lpr_session_page_fd = -1;
    lpr_session_page = 0;
    lpr_session_checked = 0;
    lpr_session_payload_busy = 0;
}

static void run_old_order_case(void)
{
    reset_case();
    wait_for_fast_finish_in_munmap = 1;
    pthread_t fast_thread;
    expect(pthread_create(&fast_thread, 0, fast_path_probe, 0) == 0,
           "old-order fast probe starts");
    old_order_session_drop();
    expect(pthread_join(fast_thread, 0) == 0,
           "old-order fast probe joins");

    expect(atomic_load_explicit(&munmap_calls, memory_order_relaxed) == 1u,
           "old order reaches munmap once");
    expect(atomic_load_explicit(
               &page_visible_at_munmap, memory_order_acquire) != 0,
           "old order leaves the session pointer visible at munmap");
    expect(atomic_load_explicit(
               &stale_page_observed, memory_order_acquire) != 0,
           "old order exposes the unmapped page to the fast path");
}

static void run_current_order_case(void)
{
    reset_case();
    wait_for_fast_finish_in_munmap = 0;
    pthread_t fast_thread;
    expect(pthread_create(&fast_thread, 0, fast_path_probe, 0) == 0,
           "current-order fast probe starts");
    lpr_filed_session_drop();
    expect(pthread_join(fast_thread, 0) == 0,
           "current-order fast probe joins");

    expect(atomic_load_explicit(&munmap_calls, memory_order_relaxed) == 1u,
           "current order reaches munmap once");
    expect(atomic_load_explicit(&close_calls, memory_order_relaxed) == 2u,
           "current order closes both session descriptors");
    expect(atomic_load_explicit(
               &page_visible_at_munmap, memory_order_acquire) == 0,
           "current order clears the session pointer before munmap");
    expect(atomic_load_explicit(
               &stale_page_observed, memory_order_acquire) == 0,
           "current order never exposes an unmapped page to the fast path");
    expect(atomic_load_explicit(
               &zero_page_observed, memory_order_acquire) != 0,
           "the blocked fast path observes a detached session after drop");
    expect(lpr_session_page == 0 &&
               lpr_session_page_fd == -1 && lpr_session_fd == -1,
           "current drop leaves the session detached");
}

int main(void)
{
    run_old_order_case();
    run_current_order_case();
    if (failures != 0) return 1;
    puts("lpr filed session lifetime unit: PASS (old order detected, current order safe)");
    return 0;
}
