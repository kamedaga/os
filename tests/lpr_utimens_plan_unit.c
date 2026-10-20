#define _GNU_SOURCE

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "../userland/personality/linux/runtime/lpr_filed_internal.h"

static int failures;
static unsigned rpc_calls;
static int64_t rpc_result;
static lpr_linux_timespec_t observed[2];
static uint64_t observed_flags;
static int observed_null;

static uint64_t get64(const unsigned char *bytes)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= (uint64_t)bytes[i] << (8 * i);
    return value;
}

lpr_filed_backend_t *lpr_filed_backend(uint64_t fd)
{
    static lpr_filed_backend_t file = {.active = 1, .handle = 42};
    return fd == 3 ? &file : NULL;
}

int64_t lpr_filed_fd_binding(uint64_t fd)
{
    return lpr_filed_backend(fd) ? 0 : -LPR_LINUX_EBADF;
}

int64_t lpr_dir_handle_for(uint64_t fd, const char *path, uint64_t *handle)
{
    if (fd != 3 && (int64_t)fd != LPR_LINUX_AT_FDCWD && path[0] != '/')
        return -LPR_LINUX_EBADF;
    *handle = 42;
    return 0;
}

int64_t lpr_fs_call(kb2_fs_request_t *request, const char *path,
    const char *second, const void *data, uint64_t bytes,
    void *output, uint64_t output_bytes, kb2_fs_response_t *response,
    pacha_fd_t transfer, pacha_fd_t *cap)
{
    assert(request->opcode == KB2_FILESYSTEM_OP_UTIMENSAT && request->handle == 42);
    assert(path && !second && !output && !output_bytes && transfer == -1 && !cap);
    (void)response;
    rpc_calls++;
    observed_flags = request->flags;
    observed_null = !data;
    if (data) {
        assert(bytes == 32);
        const unsigned char *wire = data;
        for (unsigned n = 0; n < 2; ++n) {
            observed[n].tv_sec = (int64_t)get64(wire + n * 16);
            observed[n].tv_nsec = (int64_t)get64(wire + n * 16 + 8);
        }
    } else assert(!bytes);
    return rpc_result;
}

static void expect_true(const char *name, int condition)
{
    if (!condition) {
        fprintf(stderr, "FAIL %s errno=%d (%s)\n", name, errno, strerror(errno));
        ++failures;
    }
}

static void test_production_plan(void)
{
    lpr_linux_timespec_t values[2] = {{1, LPR_LINUX_UTIME_OMIT},
        {2, LPR_LINUX_UTIME_OMIT}};
    expect_true("both OMIT recognized", lpr_linux_utimens_both_omit(values));
    expect_true("both OMIT bypasses missing dirfd and unknown flags",
        !lpr_linux_utimensat(UINT64_MAX, (uintptr_t)"missing",
            (uintptr_t)values, 0x80000000u) && !rpc_calls);
    expect_true("both OMIT bypasses NULL path validation",
        !lpr_linux_utimensat(UINT64_MAX, 0, (uintptr_t)values,
            0x80000000u) && !rpc_calls);
    expect_true("NULL times forwarded for Linux realtime clock",
        !lpr_linux_utimensat(3, (uintptr_t)"file", 0, 0) && observed_null && rpc_calls == 1);

    values[0].tv_sec = -123;
    values[0].tv_nsec = 0;
    values[1].tv_nsec = 999999999ll;
    expect_true("explicit boundaries serialized little-endian without host clock",
        !lpr_filed_utimens_handle(42, (uintptr_t)values) && !observed_null &&
        !memcmp(values, observed, sizeof(values)) &&
        observed_flags == LPR_LINUX_AT_EMPTY_PATH);
    values[0].tv_nsec = LPR_LINUX_UTIME_NOW;
    values[1].tv_nsec = LPR_LINUX_UTIME_OMIT;
    expect_true("NOW/OMIT sentinels preserved for Linux permission decisions",
        !lpr_linux_utimensat(3, 0, (uintptr_t)values, 0) &&
        !memcmp(values, observed, sizeof(values)) && observed_flags == LPR_LINUX_AT_EMPTY_PATH);
    const unsigned before = rpc_calls;
    expect_true("NULL path rejects nonzero flags before RPC",
        lpr_linux_utimensat(3, 0, (uintptr_t)values, LPR_LINUX_AT_SYMLINK_NOFOLLOW) ==
            -LPR_LINUX_EINVAL && rpc_calls == before);
    rpc_result = -LPR_LINUX_EINVAL;
    values[0].tv_nsec = -1;
    expect_true("negative nanoseconds sent unchanged and Linux EINVAL propagated",
        lpr_filed_utimens_handle(42, (uintptr_t)values) == -LPR_LINUX_EINVAL &&
        observed[0].tv_nsec == -1);
    values[0].tv_nsec = 1000000000ll;
    expect_true("one billion nanoseconds sent unchanged and Linux EINVAL propagated",
        lpr_filed_utimens_handle(42, (uintptr_t)values) == -LPR_LINUX_EINVAL &&
        observed[0].tv_nsec == 1000000000ll);
}

static void test_host_linux_ordering(void)
{
#ifdef SYS_utimensat
    struct timespec omit[2];
    struct timespec explicit_times[2];
    char missing_path[128];
    char temporary_path[] = "/tmp/capabilityos-utimens-unit-XXXXXX";
    const unsigned long unknown_flags = 0x80000000ul;

    memset(omit, 0, sizeof(omit));
    omit[0].tv_nsec = UTIME_OMIT;
    omit[1].tv_nsec = UTIME_OMIT;
    snprintf(
        missing_path,
        sizeof(missing_path),
        "/tmp/capabilityos-utimens-missing-%ld",
        (long)getpid());

    errno = 0;
    expect_true(
        "Linux both OMIT bypasses path and unknown-flag validation",
        syscall(SYS_utimensat, AT_FDCWD, missing_path, omit, unknown_flags) == 0);
    errno = 0;
    expect_true(
        "Linux both OMIT bypasses NULL-path fd and flag validation",
        syscall(SYS_utimensat, -1, (const char *)0, omit, unknown_flags) == 0);

    const int fd = mkstemp(temporary_path);
    expect_true("create host utimens fixture", fd >= 0);
    if (fd >= 0) {
        memset(explicit_times, 0, sizeof(explicit_times));
        explicit_times[0].tv_sec = 100;
        explicit_times[0].tv_nsec = 1;
        explicit_times[1].tv_sec = 200;
        explicit_times[1].tv_nsec = 2;
        errno = 0;
        expect_true(
            "Linux pathname NULL uses fd when flags are zero",
            syscall(SYS_utimensat, fd, (const char *)0, explicit_times, 0) == 0);
        errno = 0;
        expect_true(
            "Linux pathname NULL rejects nonzero flags",
            syscall(
                SYS_utimensat,
                fd,
                (const char *)0,
                explicit_times,
                (unsigned long)AT_SYMLINK_NOFOLLOW) == -1 &&
                errno == EINVAL);
        close(fd);
        unlink(temporary_path);
    }
#else
    puts("host has no SYS_utimensat; Linux ordering checks skipped");
#endif
}

int main(void)
{
    test_production_plan();
    test_host_linux_ordering();
    if (failures != 0) {
        fprintf(stderr, "lpr utimens plan tests failed: %d\n", failures);
        return 1;
    }
    puts("lpr utimens plan tests passed");
    return 0;
}
