#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

/* Read-only comparison of loader-sized reads through library aliases. */
int main(int argc, char **argv)
{
    const char *paths[] = {
        "/usr/lib/libwebkitgtk-6.0.so.4",
        "/opt/pacha/webkitgtk-2.50.4/lib/libwebkitgtk-6.0.so.4",
        "/opt/pacha/webkitgtk-2.50.4/lib/libwebkitgtk-6.0.so.4.13.7",
        "/usr/lib/libjavascriptcoregtk-6.0.so.1",
        "/usr/lib/libjson-glib-1.0.so.0",
    };
    unsigned failures = 0;
    const unsigned path_count = argc > 1 ? (unsigned)argc - 1 :
        sizeof(paths) / sizeof(paths[0]);
    for (unsigned i = 0; i < path_count; ++i) {
        const char *path = argc > 1 ? argv[i + 1] : paths[i];
        unsigned char positional[960] = {0}, stream[960] = {0};
        struct stat st = {0};
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        int stat_status = fd < 0 ? -1 : fstat(fd, &st);
        ssize_t p = fd < 0 ? -1 : pread(fd, positional, sizeof(positional), 0);
        int pe = errno;
        ssize_t r = fd < 0 ? -1 : read(fd, stream, sizeof(stream));
        int re = errno;
        off_t offset = fd < 0 ? -1 : lseek(fd, 0, SEEK_CUR);
        int equal = p == sizeof(positional) && r == sizeof(stream) &&
            !memcmp(positional, stream, sizeof(stream));
        int elf = equal && !memcmp(stream, "\177ELF", 4);
        printf("ELF_ALIAS path=%s fd=%d stat=%d inode=%llu mode=%o size=%lld pread=%ld errno=%d read=%ld errno=%d offset=%lld equal=%d elf=%d first=%02x%02x%02x%02x prefirst=%02x%02x%02x%02x\n",
            path, fd, stat_status, (unsigned long long)st.st_ino,
            (unsigned)st.st_mode, (long long)st.st_size, (long)p, pe,
            (long)r, re, (long long)offset, equal, elf,
            stream[0], stream[1], stream[2], stream[3],
            positional[0], positional[1], positional[2], positional[3]);
        if (stat_status || !elf || offset != sizeof(stream)) ++failures;
        /* Large readv uses the VMO transfer path on PachaOS. Compare it with
         * small inline reads and a private mapping, without modifying data.
         * Log the small result first: these reads may change cache residency. */
        fflush(stdout);
        unsigned char large[65536];
        memset(large, 0xa5, sizeof(large));
        struct iovec vector = {large, sizeof(large)};
        off_t seek = fd < 0 ? -1 : lseek(fd, 0, SEEK_SET);
        ssize_t bulk = seek ? -1 : readv(fd, &vector, 1);
        int bulk_errno = errno;
        unsigned char *mapped = fd < 0 ? MAP_FAILED :
            mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, fd, 0);
        int map_errno = errno;
        unsigned char prefix[4] = {0};
        if (mapped != MAP_FAILED) memcpy(prefix, mapped, sizeof(prefix));
        printf("ELF_PATH_COMPARE path=%s bulk=%ld errno=%d first=%02x%02x%02x%02x map=%d errno=%d first=%02x%02x%02x%02x small_bulk_equal=%d small_map_equal=%d\n",
            path, (long)bulk, bulk_errno, large[0], large[1], large[2], large[3],
            mapped != MAP_FAILED, map_errno, prefix[0], prefix[1], prefix[2], prefix[3],
            bulk >= (ssize_t)sizeof(stream) && !memcmp(stream, large, sizeof(stream)),
            mapped != MAP_FAILED && !memcmp(stream, mapped, sizeof(stream)));
        if (bulk < (ssize_t)sizeof(stream) || memcmp(large, "\177ELF", 4) ||
            mapped == MAP_FAILED || memcmp(prefix, "\177ELF", 4)) ++failures;
        if (mapped != MAP_FAILED) munmap(mapped, 4096);
        if (fd >= 0) close(fd);
    }
    return failures ? 1 : 0;
}
