#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Copyout correctness checks, not a replacement for actual page-load timing. */
static int failures;
static void check(int condition, const char *name)
{
    printf("CLOCK_COPY %s %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) ++failures;
}
static int valid_timespec(const void *address)
{
    struct timespec value;
    memcpy(&value, address, sizeof(value));
    return value.tv_sec >= 0 && value.tv_nsec >= 0 && value.tv_nsec < 1000000000;
}
int main(void)
{
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    unsigned char *map = mmap(NULL, 2*page, PROT_READ|PROT_WRITE,
                              MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (map == MAP_FAILED) return 2;
    for (int clock = CLOCK_REALTIME; clock <= CLOCK_MONOTONIC; ++clock) {
        for (size_t offset = page-16; offset <= page-3; offset += 13) {
            memset(map+offset, 0xff, sizeof(struct timespec));
            check(syscall(SYS_clock_gettime, clock, map+offset) == 0 &&
                  valid_timespec(map+offset), "gettime aligned/unaligned cross-page");
            check(syscall(SYS_clock_getres, clock, map+offset) == 0 &&
                  valid_timespec(map+offset), "getres aligned/unaligned cross-page");
        }
    }
    check(mprotect(map+page, page, PROT_NONE) == 0, "guard second page");
    check(syscall(SYS_clock_gettime, CLOCK_MONOTONIC, map+page-8) < 0,
          "cross-page inaccessible tail rejected (partial output allowed)");
    check(mprotect(map, page, PROT_READ) == 0, "read-only first page");
    check(syscall(SYS_clock_gettime, CLOCK_MONOTONIC, map) < 0,
          "read-only output rejected");
    check(syscall(SYS_clock_gettime, CLOCK_MONOTONIC, (void *)0) < 0,
          "null output rejected");
    check(syscall(SYS_clock_gettime, CLOCK_MONOTONIC, (void *)(UINTPTR_MAX-7)) < 0,
          "overflowing output rejected");
    /* Restore separately: the preceding calls split the original VMA. Keep
     * this copyout regression independent of multi-VMA mprotect support. */
    int restored_first = mprotect(map, page, PROT_READ|PROT_WRITE) == 0;
    int restored_second = mprotect(map+page, page, PROT_READ|PROT_WRITE) == 0;
    check(restored_first && restored_second, "restore writable pages separately");
    if (!restored_first || !restored_second) return 1;
    memset(map+page-3, 0xa5, sizeof(struct timespec));
    fflush(stdout);
    pid_t child = fork();
    if (child == 0) {
        int ok = syscall(SYS_clock_gettime, CLOCK_MONOTONIC, map+page-3) == 0 &&
                 valid_timespec(map+page-3);
        _exit(ok ? 0 : 1);
    }
    int status = -1;
    check(child > 0 && waitpid(child, &status, 0) == child &&
          WIFEXITED(status) && WEXITSTATUS(status) == 0, "child COW copyout succeeds");
    unsigned char expected[sizeof(struct timespec)];
    memset(expected, 0xa5, sizeof(expected));
    check(memcmp(map+page-3, expected, sizeof(expected)) == 0,
          "parent cross-page bytes unchanged after child copyout");
    check(munmap(map, 2*page) == 0, "unmap");
    return failures ? 1 : 0;
}
