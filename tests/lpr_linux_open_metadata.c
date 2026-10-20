#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "open metadata line %d: %s\n", __LINE__, #x); perror("open metadata"); return 1; } } while (0)

int main(void)
{
    char directory[] = "/tmp/open-metadata-XXXXXX";
    CHECK(mkdtemp(directory));
    int dir = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(dir >= 0);
    int fd = openat(dir, "regular", O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
    CHECK(fd >= 0 && write(fd, "hello", 5) == 5 && close(fd) == 0);
    CHECK(symlinkat("regular", dir, "regular-alias") == 0);
    struct stat st;
    char bytes[8];
    for (int i = 0; i < 100; ++i) {
        fd = openat(dir, "regular-alias", O_RDONLY | O_CLOEXEC);
        CHECK(fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode));
        CHECK(read(fd, bytes, sizeof(bytes)) == 5 && !memcmp(bytes, "hello", 5));
        CHECK(close(fd) == 0);
    }
    /* These paths deliberately bypass LPR's literal /dev pathname fast path. */
    CHECK(symlinkat("/dev/null", dir, "null-alias") == 0);
    fd = openat(dir, "null-alias", O_RDWR | O_CLOEXEC);
    CHECK(fd >= 0 && fstat(fd, &st) == 0 && S_ISCHR(st.st_mode));
    CHECK(major(st.st_rdev) == 1 && minor(st.st_rdev) == 3);
    CHECK(write(fd, "hello", 5) == 5 && read(fd, bytes, sizeof(bytes)) == 0);
    CHECK(close(fd) == 0);
    CHECK(symlinkat("/dev/zero", dir, "zero-alias") == 0);
    fd = openat(dir, "zero-alias", O_RDWR | O_CLOEXEC);
    CHECK(fd >= 0 && fstat(fd, &st) == 0 && S_ISCHR(st.st_mode));
    CHECK(major(st.st_rdev) == 1 && minor(st.st_rdev) == 5);
    memset(bytes, 1, sizeof(bytes));
    CHECK(read(fd, bytes, sizeof(bytes)) == sizeof(bytes));
    for (unsigned i = 0; i < sizeof(bytes); ++i) CHECK(!bytes[i]);
    unsigned char *mapped = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    CHECK(mapped != MAP_FAILED && !mapped[0] && !mapped[4095]);
    mapped[17] = 42;
    CHECK(munmap(mapped, 4096) == 0 && close(fd) == 0);
    CHECK(unlinkat(dir, "null-alias", 0) == 0);
    CHECK(unlinkat(dir, "zero-alias", 0) == 0);
    CHECK(unlinkat(dir, "regular-alias", 0) == 0);
    CHECK(unlinkat(dir, "regular", 0) == 0);
    CHECK(close(dir) == 0 && rmdir(directory) == 0);
    puts("OPEN_METADATA_SMOKE_OK");
    return 0;
}
