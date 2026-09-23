#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* A file grows while a client retains a shared prefix, as database shared
 * regions do. This is a two-mapping lifetime regression, not a capacity test.
 * Preserve the uniquely-created file on failure for inspection. */
int main(int argc, char **argv)
{
    enum { PREFIX = 32768, GROWN = 65536 };
    char path[256];
    int extend_file = argc < 3 || strcmp(argv[2], "reserve") != 0;
    setvbuf(stdout, NULL, _IONBF, 0);
    if (snprintf(path, sizeof(path), "%s/shared-growth-XXXXXX",
            argc > 1 ? argv[1] : "/tmp") >= (int)sizeof(path)) return 2;
    int fd = mkstemp(path);
    if (fd < 0 || ftruncate(fd, PREFIX)) { perror("initial file"); return 1; }
    volatile unsigned char *old = mmap(NULL, PREFIX, PROT_READ | PROT_WRITE,
        MAP_SHARED, fd, 0);
    if (old == MAP_FAILED) { perror("prefix mmap"); return 1; }
    old[48] = 0x5a;
    old[PREFIX - 1] = 0xa5;
    printf("SHARED_GROWTH before path=%s mode=%s address=%p\n",
        path, extend_file ? "truncate" : "reserve", (void *)old);
    if (extend_file && ftruncate(fd, GROWN)) { perror("grow file"); return 1; }
    puts("SHARED_GROWTH file-size-ready; checking retained prefix");
    if (old[48] != 0x5a || old[PREFIX - 1] != 0xa5) return 1;
    volatile unsigned char *wide = mmap(NULL, GROWN, PROT_READ | PROT_WRITE,
        MAP_SHARED, fd, 0);
    if (wide == MAP_FAILED) { perror("wide mmap"); return 1; }
    puts("SHARED_GROWTH wide-map-ready; checking alias coherence");
    old[48] = 0x3c;
    if (wide[48] != 0x3c || wide[PREFIX - 1] != 0xa5) return 1;
    wide[48] = 0xc3;
    if (old[48] != 0xc3) return 1;
    if (extend_file) {
        for (size_t i = PREFIX; i < GROWN; ++i) if (wide[i]) return 1;
        wide[GROWN - 1] = 0x7e;
        unsigned char byte;
        if (pread(fd, &byte, 1, GROWN - 1) != 1 || byte != 0x7e) return 1;
    }
    if (munmap((void *)wide, GROWN) || munmap((void *)old, PREFIX) ||
        close(fd)) { perror("release"); return 1; }
    printf("SHARED_GROWTH PASS retained-prefix coherent-alias%s file=%s\n",
        extend_file ? " zero-tail coherent-pread" : "", path);
    return 0;
}
