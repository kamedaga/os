#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static unsigned long long available(void)
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) { perror("meminfo"); return 0; }
    char line[128];
    unsigned long long kb = 0;
    while (fgets(line, sizeof(line), f))
        if (sscanf(line, "MemFree: %llu kB", &kb) == 1) break;
    fclose(f);
    return kb * 1024;
}

int main(void)
{
    enum { bytes = 128 * 1024, rounds = 256 };
    setvbuf(stdout, NULL, _IONBF, 0);
    unsigned long long before = available();
    if (!before) return 8;
    printf("PRIVATE_LIFECYCLE before=%llu\n", before);
    for (unsigned i = 0; i < rounds; ++i) {
        int fd = memfd_create("private-lifecycle-probe", MFD_CLOEXEC);
        if (fd < 0 || ftruncate(fd, bytes)) { perror("memfd"); return 1; }
        unsigned char marker = (unsigned char)i;
        if (pwrite(fd, &marker, 1, 0) != 1) return 2;
        unsigned char *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
        if (p == MAP_FAILED) { perror("mmap"); return 3; }
        if (close(fd)) return 4;
        /* Closing the descriptor must not revoke an existing VMA. */
        if (p[0] != marker) return 5;
        p[0] ^= 0xff;
        if (p[0] != (unsigned char)(marker ^ 0xff)) return 6;
        if (munmap(p, bytes)) return 7;
        if ((i + 1) % 64 == 0)
            printf("PRIVATE_LIFECYCLE iteration=%u free=%llu\n", i + 1, available());
    }
    unsigned long long after = available();
    printf("PRIVATE_LIFECYCLE after=%llu delta=%lld PASS\n", after,
        (long long)after - (long long)before);
    return 0;
}
