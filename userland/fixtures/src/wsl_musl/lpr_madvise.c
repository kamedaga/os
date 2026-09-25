#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { printf("LPR_MADVISE=FAIL line=%d errno=%d\n", __LINE__, errno); return 1; } } while (0)
int main(void)
{
    char *area = mmap(NULL, 3 * 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(area != MAP_FAILED);
    /* Untouched PROT_NONE pages are valid; advice must not read the range. */
    for (int advice = 0; advice <= 3; advice++) CHECK(!madvise(area, 3 * 4096 - 1, advice));
    CHECK(!mprotect(area + 4096, 4096, PROT_READ | PROT_WRITE));
    CHECK(!madvise(area, 3 * 4096, MADV_WILLNEED)); /* Adjacent split VMAs. */
    area[4096] = 42;
    CHECK(madvise(area + 1, 4096, MADV_WILLNEED) == -1 && errno == EINVAL);
    CHECK(madvise(area + 1, 0, MADV_WILLNEED) == -1 && errno == EINVAL);
    CHECK(!madvise(area, 0, MADV_WILLNEED));
    CHECK(madvise(area, 0, 999) == -1 && errno == EINVAL);
    CHECK(madvise(area, SIZE_MAX, MADV_WILLNEED) == -1 && errno == EINVAL);
    CHECK(madvise((void *)(UINTPTR_MAX - 4095), 4096, MADV_WILLNEED) == -1 && errno == EINVAL);
    CHECK(madvise(area + 4096, 4096, MADV_DONTNEED) == -1 && errno == EINVAL);
    CHECK(area[4096] == 42); /* Unsupported side effects cannot claim success. */
    CHECK(!munmap(area + 4096, 4096));
    CHECK(madvise(area, 3 * 4096, MADV_WILLNEED) == -1 && errno == ENOMEM);
    CHECK(madvise(area + 4096, 4096, MADV_WILLNEED) == -1 && errno == ENOMEM);
    CHECK(!munmap(area, 4096) && !munmap(area + 2 * 4096, 4096));
    int fd = open("/usr/lib/libreoffice/program/libswlo.so", O_RDONLY);
    CHECK(fd >= 0);
    area = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, fd, 0);
    CHECK(area != MAP_FAILED);
    CHECK(!madvise(area, 4096, MADV_WILLNEED));
    CHECK(area[0] == 0x7f && area[1] == 'E');
    CHECK(!munmap(area, 4096) && !close(fd));
    puts("LPR_MADVISE=OK hints split-vmas prot-none holes alignment overflow unsupported file-backed");
    return 0;
}
