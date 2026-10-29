#define _GNU_SOURCE
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

static sigjmp_buf fault_return;
static volatile sig_atomic_t expecting_fault;

static void fault_handler(int signo)
{
    if (signo != SIGSEGV || !expecting_fault) _exit(101);
    siglongjmp(fault_return, 1);
}

static int cannot_read(volatile unsigned char *address)
{
    expecting_fault = 1;
    if (!sigsetjmp(fault_return, 1)) {
        (void)*address;
        expecting_fault = 0;
        return 0;
    }
    expecting_fault = 0;
    return 1;
}

#define CHECK(test) do { if (!(test)) { \
    printf("MPROTECT_RANGES=FAIL line=%d errno=%d\n", __LINE__, errno); \
    return 1; } } while (0)

int main(void)
{
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    struct sigaction action = { .sa_handler = fault_handler };
    sigemptyset(&action.sa_mask);
    CHECK(!sigaction(SIGSEGV, &action, NULL));
    unsigned char *p = mmap(NULL, 3 * page, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(p != MAP_FAILED);
    /* Shared backing prevents the middle mapping from merging with its
     * private neighbors, including on the Linux reference host. */
    CHECK(mmap(p + page, page, PROT_READ | PROT_WRITE,
        MAP_SHARED | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == p + page);
    p[0] = 11; p[page] = 22; p[2 * page] = 33;
    CHECK(!mprotect(p, 3 * page, PROT_NONE));
    CHECK(cannot_read(p) && cannot_read(p + page) && cannot_read(p + 2 * page));
    CHECK(!mprotect(p, 3 * page, PROT_READ | PROT_WRITE));
    CHECK(p[0] == 11 && p[page] == 22 && p[2 * page] == 33);
    CHECK(!mprotect(p, 1, PROT_NONE));
    CHECK(cannot_read(p) && p[page] == 22);
    CHECK(!mprotect(p, 1, PROT_READ | PROT_WRITE));
    errno = 0;
    CHECK(mprotect(p + 1, page, PROT_NONE) == -1 && errno == EINVAL);
    CHECK(!munmap(p + page, page));
    errno = 0;
    CHECK(mprotect(p, 3 * page, PROT_NONE) == -1);
    /* Native INVALID currently also represents an unmapped range. */
    CHECK(errno == ENOMEM || errno == EINVAL);
    CHECK(p[2 * page] == 33);
    CHECK(!munmap(p, page) && !munmap(p + 2 * page, page));
    puts("MPROTECT_RANGES=PASS adjacent=3 guards=3 preserved=3 hole=rejected");
    return 0;
}
