#define _GNU_SOURCE

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

static void expect_true(const char *name, int condition)
{
    if (!condition) {
        fprintf(stderr, "FAIL %s errno=%d (%s)\n", name, errno, strerror(errno));
        ++failures;
    }
}

static void test_production_plan(void)
{
    lpr_linux_timespec_t values[2];
    lpr_linux_utimens_plan_t plan;
    int64_t status;

    values[0].tv_sec = 1;
    values[0].tv_nsec = LPR_LINUX_UTIME_OMIT;
    values[1].tv_sec = 2;
    values[1].tv_nsec = LPR_LINUX_UTIME_OMIT;
    expect_true("both OMIT recognized", lpr_linux_utimens_both_omit(values));
    status = lpr_linux_utimens_plan(values, &plan);
    expect_true("both OMIT plan succeeds", status == 0);
    expect_true("both OMIT is a no-op", plan.mask == 0 && plan.needs_now == 0);

    status = lpr_linux_utimens_plan(0, &plan);
    expect_true("NULL times plan succeeds", status == 0);
    expect_true(
        "NULL times updates both from now",
        plan.mask == (FILED_UTIMENS_ATIME | FILED_UTIMENS_MTIME) &&
            plan.needs_now == 1);

    values[0].tv_nsec = 0;
    values[1].tv_nsec = 999999999ll;
    status = lpr_linux_utimens_plan(values, &plan);
    expect_true("explicit boundary plan succeeds", status == 0);
    expect_true(
        "explicit values do not request a clock read",
        plan.mask == (FILED_UTIMENS_ATIME | FILED_UTIMENS_MTIME) &&
            plan.needs_now == 0);

    values[0].tv_nsec = LPR_LINUX_UTIME_NOW;
    values[1].tv_nsec = LPR_LINUX_UTIME_OMIT;
    status = lpr_linux_utimens_plan(values, &plan);
    expect_true("one-sided NOW plan succeeds", status == 0);
    expect_true(
        "one-sided NOW requests one update and a clock read",
        plan.mask == FILED_UTIMENS_ATIME && plan.needs_now == 1);

    values[0].tv_nsec = -1;
    values[1].tv_nsec = LPR_LINUX_UTIME_OMIT;
    expect_true(
        "negative explicit nsec rejected",
        lpr_linux_utimens_plan(values, &plan) == -LPR_LINUX_EINVAL);
    values[0].tv_nsec = 1000000000ll;
    expect_true(
        "one billion explicit nsec rejected",
        lpr_linux_utimens_plan(values, &plan) == -LPR_LINUX_EINVAL);
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
