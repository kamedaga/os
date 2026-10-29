#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

int main(void)
{
    struct timespec before, after;
    struct timeval tv;
    struct timezone tz = {123, 123};
    assert(clock_gettime(CLOCK_REALTIME, &before) == 0);
    assert(syscall(SYS_gettimeofday, &tv, &tz) == 0);
    assert(clock_gettime(CLOCK_REALTIME, &after) == 0);
    assert(tv.tv_usec >= 0 && tv.tv_usec < 1000000);
    int64_t now = (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
    assert(now >= (int64_t)before.tv_sec * 1000000 + before.tv_nsec / 1000);
    assert(now <= (int64_t)after.tv_sec * 1000000 + after.tv_nsec / 1000);
    assert(tz.tz_minuteswest == 0 && tz.tz_dsttime == 0);
    assert(syscall(SYS_gettimeofday, NULL, NULL) == 0);
    assert(syscall(SYS_gettimeofday, NULL, &tz) == 0);
    time_t stored = 0;
    time_t seconds = syscall(SYS_time, &stored);
    assert(seconds == stored && seconds >= before.tv_sec);
    assert(clock_gettime(CLOCK_REALTIME, &after) == 0 && seconds <= after.tv_sec);
    seconds = syscall(SYS_time, NULL);
    assert(seconds >= before.tv_sec && seconds <= after.tv_sec + 1);
    void *page = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(page != MAP_FAILED);
    long readonly_result = syscall(SYS_gettimeofday, page, NULL);
    printf("readonly result=%ld errno=%d\n", readonly_result, errno);
    assert(readonly_result == -1 && errno == EFAULT);
    assert(munmap(page, 4096) == 0);
    assert(syscall(SYS_gettimeofday, (void *)1, NULL) == -1 && errno == EFAULT);
    puts("GETTIMEOFDAY=PASS realtime microseconds timezone null readonly invalid time-seconds");
    return 0;
}
