#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/statfs.h>
#include <unistd.h>

static void snapshot(const char *label)
{
    struct statfs s;
    if (statfs("/dev/shm", &s)) {
        printf("TMPFS %s statfs errno=%d\n", label, errno);
        return;
    }
    printf("TMPFS %s inodes=%lu free=%lu blocks=%lu free=%lu\n", label,
        (unsigned long)s.f_files, (unsigned long)s.f_ffree,
        (unsigned long)s.f_blocks, (unsigned long)s.f_bfree);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    snapshot("before");
    for (unsigned i = 0; i < 256; ++i) {
        int fd = memfd_create("lifecycle-probe", MFD_CLOEXEC);
        if (fd < 0) {
            printf("TMPFS create iteration=%u errno=%d\n", i, errno);
            snapshot("failed");
            return 1;
        }
        if (ftruncate(fd, 4096)) { close(fd); return 2; }
        unsigned char *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (p == MAP_FAILED) { close(fd); return 3; }
        p[0] = 42;
        if (munmap(p, 4096) || close(fd)) return 4;
    }
    snapshot("after");
    puts("TMPFS_LIFECYCLE=OK");
    return 0;
}
