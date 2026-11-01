#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include "../userland/personality/linux/runtime/lpr_dispatch.c"

static unsigned calls, clock_calls;
static uint64_t captured[5], selected_clock, previous_clock;
static int64_t native_status, clock_status;
static struct pachaos_timespec now = {10, 900000000};
static struct pachaos_timespec monotonic_now;
static unsigned override_monotonic, fail_second_clock;

int64_t lpr_pacha_syscall4(uint64_t nr, uint64_t word, uint64_t op,
    uint64_t ticks, uint64_t flags)
{
    ++calls;
    captured[0] = nr; captured[1] = word; captured[2] = op;
    captured[3] = ticks; captured[4] = flags;
    return native_status;
}
int64_t lpr_pacha_syscall2(uint64_t nr, uint64_t word, uint64_t count)
{
    ++calls;
    captured[0] = nr; captured[1] = word; captured[2] = count;
    return native_status;
}
int64_t lpr_pacha_syscall5(uint64_t nr, uint64_t a, uint64_t b,
    uint64_t c, uint64_t d, uint64_t e)
{ (void)nr; (void)a; (void)b; (void)c; (void)d; (void)e; assert(0); return 0; }
int64_t lpr_pacha_status_to_errno(int64_t status)
{ return pacha_kernel_status_to_errno(status); }
int64_t pacha_kernel_status_to_errno(int64_t status)
{
    assert(status < 0);
    switch (status) {
    case -1: return -EINVAL;
    case -3: return -ENOMEM;
    case -4: return -EFAULT;
    default: assert(0); return -EIO;
    }
}
int64_t lpr_pacha_clock_gettime(uint64_t clock, struct pachaos_timespec *out)
{
    ++clock_calls; previous_clock = selected_clock; selected_clock = clock;
    *out = override_monotonic && clock == LPR_LINUX_CLOCK_MONOTONIC ? monotonic_now : now;
    if (fail_second_clock && clock_calls % 2 == 0) return -EIO;
    return clock_status;
}
int64_t lpr_linux_validate_timespec(const struct pachaos_timespec *ts)
{
    return (int64_t)ts->tv_sec < 0 || ts->tv_nsec >= 1000000000ull ? -EINVAL : 0;
}

static int64_t request(uint64_t op, const struct pachaos_timespec *timeout,
    unsigned *interrupted)
{
    return lpr_sys_futex(0x1000, op, 0, (uintptr_t)timeout, 0, 0, interrupted);
}

int main(void)
{
    _Static_assert(PACHAOS_SYSCALL_FUTEX_PI == 40, "native PI ABI");
    const unsigned commands[] = {6, 7, 8, 13};
    const unsigned operations[] = {0, 2, 1, 0};
    for (unsigned i = 0; i < 4; ++i) {
        for (unsigned private = 0; private <= 128; private += 128) {
            unsigned before = calls;
            assert(request(commands[i] | private, NULL, NULL) == 0);
            assert(calls == before + 1 && captured[0] == 40);
            assert(captured[1] == 0x1000 && captured[2] == operations[i]);
            assert(captured[3] == 0 && captured[4] == (private ? 1u : 0u));
        }
    }
    /* glibc's zero-word unlock probe must preserve native EPERM, while
     * musl's shared-form stack-word LOCK probe must reach native flags=0. */
    native_status = 7;
    assert(request(7 | 128, NULL, NULL) == -EPERM);
    native_status = 0;
    assert(request(6, NULL, NULL) == 0 && captured[4] == 0);

    const int errors[] = {0, EINVAL, EINTR, ENOMEM, EFAULT, ETIMEDOUT,
        EIO, EPERM, EDEADLK, ESRCH, ENOSYS};
    for (unsigned i = 0; i <= 10; ++i) {
        for (int sign = -1; sign <= 1; sign += 2) {
            unsigned interrupted = 0;
            native_status = sign * (int)i;
            assert(request(6 | 128, NULL, &interrupted) == -errors[i]);
            assert(interrupted == (i == 2));
            interrupted = 0;
            assert(request(8, NULL, &interrupted) == -(i == 2 ? EAGAIN : errors[i]));
            assert(!interrupted);
        }
    }
    native_status = INT64_MIN;
    assert(request(6, NULL, NULL) == -EIO);
    native_status = 11;
    assert(request(6, NULL, NULL) == -EIO);
    native_status = 0;

    struct pachaos_timespec deadline = {11, 1};
    assert(request(6 | 128, &deadline, NULL) == 0);
    assert(previous_clock == LPR_LINUX_CLOCK_REALTIME &&
        selected_clock == LPR_LINUX_CLOCK_MONOTONIC && captured[3] == 11001);
    assert(request(13, &deadline, NULL) == 0);
    assert(previous_clock == LPR_LINUX_CLOCK_MONOTONIC &&
        selected_clock == LPR_LINUX_CLOCK_MONOTONIC && captured[3] == 11001);
    assert(request(13 | 256 | 128, &deadline, NULL) == 0);
    assert(previous_clock == LPR_LINUX_CLOCK_REALTIME && captured[3] == 11001 && captured[4] == 1);
    deadline = (struct pachaos_timespec){10, 900000001};
    assert(request(13, &deadline, NULL) == 0 && captured[3] == 10901);
    /* Epoch realtime must never leak into the native monotonic deadline. */
    override_monotonic = 1;
    monotonic_now = (struct pachaos_timespec){123, 456000000};
    deadline = (struct pachaos_timespec){11, 1};
    assert(request(6, &deadline, NULL) == 0 && captured[3] == 123557);
    assert(request(13 | 256, &deadline, NULL) == 0 && captured[3] == 123557);
    deadline = (struct pachaos_timespec){123, 556000001};
    assert(request(13, &deadline, NULL) == 0 && captured[3] == 123557);
    /* Both converting the clock and adding the relative budget are checked. */
    unsigned before_failure = calls;
    deadline = (struct pachaos_timespec){11, 1};
    monotonic_now = (struct pachaos_timespec){UINT64_MAX, 0};
    assert(request(6, &deadline, NULL) == -EINVAL);
    monotonic_now = (struct pachaos_timespec){UINT64_MAX / 1000, 615000000};
    assert(request(6, &deadline, NULL) == -EINVAL);
    monotonic_now = (struct pachaos_timespec){0, 0};
    assert(request(6, &deadline, NULL) == 0 && captured[3] == 101);
    assert(calls == before_failure + 1);
    fail_second_clock = 1; clock_calls = 0;
    assert(request(6, &deadline, NULL) == -EIO && calls == before_failure + 1);
    fail_second_clock = 0; override_monotonic = 0;
    deadline = now;
    assert(request(6, &deadline, NULL) == 0 && captured[2] == 1 && !captured[3]);
    native_status = 2;
    unsigned interrupted = 0;
    assert(request(13, &deadline, &interrupted) == -ETIMEDOUT && !interrupted);
    deadline.tv_sec = 9;
    assert(request(6, &deadline, NULL) == -ETIMEDOUT);
    native_status = 0;

    unsigned before = calls;
    deadline = (struct pachaos_timespec){11, 1000000000};
    assert(request(6, &deadline, NULL) == -EINVAL);
    deadline = (struct pachaos_timespec){UINT64_MAX, 0};
    assert(request(13, &deadline, NULL) == -EINVAL);
    deadline = (struct pachaos_timespec){INT64_MAX, 0};
    assert(request(6, &deadline, NULL) == -EINVAL);
    deadline = (struct pachaos_timespec){11, 0};
    clock_status = -EIO;
    assert(request(6, &deadline, NULL) == -EIO);
    clock_status = 0;
    assert(calls == before);
    /* Positive values, even native-status-sized ones, are counts. Only
     * negative native errors (e.g. mixed-domain INVALID) are translated. */
    const int64_t counts[] = {0, 1, 2, 3, 4, 5, 10, 100};
    for (unsigned i = 0; i < sizeof(counts)/sizeof(*counts); ++i) {
        native_status = counts[i];
        assert(lpr_sys_futex(0x1000, 1 | 128, 7, 0, 0, 0, NULL) == counts[i]);
        assert(captured[0] == PACHAOS_SYSCALL_FUTEX_WAKE && captured[2] == 7);
        assert(lpr_sys_futex(0x1000, 3 | 128, 2, 5, 0x2000, 0, NULL) == counts[i]);
        assert(captured[0] == PACHAOS_SYSCALL_FUTEX_REQUEUE && captured[1] == 0x1000);
        assert(captured[2] == 2 && captured[3] == 5 && captured[4] == 0x2000);
    }
    const int64_t failures[] = {-1, -3, -4};
    const int linux_errors[] = {EINVAL, ENOMEM, EFAULT};
    for (unsigned i = 0; i < 3; ++i) {
        native_status = failures[i];
        assert(lpr_sys_futex(0x1000, 1, 1, 0, 0, 0, NULL) == -linux_errors[i]);
        assert(lpr_sys_futex(0x1000, 3, 1, 1, 0x2000, 0, NULL) == -linux_errors[i]);
    }
    /* TRY/UNLOCK ignore the timeout argument, including inaccessible pointers. */
    native_status = 0;
    unsigned clocks_before = clock_calls;
    assert(request(8, (void *)1, NULL) == 0);
    assert(request(7, (void *)1, NULL) == 0);
    assert(clock_calls == clocks_before);

    before = calls;
    assert(lpr_sys_futex(0, 6, 0, 0, 0, 0, NULL) == -EFAULT);
    assert(lpr_sys_futex(1, 7, 0, 0, 0, 0, NULL) == -EINVAL);
    const unsigned unsupported[] = {11, 12, 127, 5, 6 | 512,
        6 | 256, 7 | 256, 8 | 256};
    for (unsigned i = 0; i < sizeof(unsupported)/sizeof(*unsupported); ++i)
        assert(lpr_sys_futex(1, unsupported[i], 0, 1, 0, 0, NULL) == -ENOSYS);
    assert(calls == before);
    puts("futex PI bridge: operations, private/shared flags, status errors, clocks/deadlines and unsupported requeue PASS");
    return 0;
}
