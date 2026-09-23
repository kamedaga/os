#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#ifdef DELAY_PROVIDER
/* Only for exercising the opt-in diagnostic threshold on the host. */
void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
    int saved = errno;
    struct timespec delay = {.tv_nsec = 55000000};
    nanosleep(&delay, NULL);
    errno = saved;
    return (void *)syscall(SYS_mmap, addr, len, prot, flags, fd, off);
}
#else
int main(void)
{
    errno = EDOM;
    unsigned char *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(p != MAP_FAILED && errno == EDOM);
    p[4095] = 42;
    assert(p[4095] == 42 && munmap(p, 4096) == 0);
    assert(mmap(NULL, 0, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)
           == MAP_FAILED && errno == EINVAL);
    for (unsigned i = 0; i < 50; ++i) {
        p = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        assert(p != MAP_FAILED && munmap(p, 4096) == 0);
    }
    puts("mmap probe result/errno/bound checks passed");
}
#endif
