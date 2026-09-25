#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <unistd.h>

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "LPR_RANDOM=FAIL line=%d errno=%d\n", __LINE__, errno); return 1; } } while (0)

int main(void)
{
    unsigned char bytes[8193], untouched[8193];
    memset(untouched, 0xa5, sizeof(untouched));
    for (unsigned flags = 0; flags <= GRND_NONBLOCK; flags++) {
        memcpy(bytes, untouched, sizeof(bytes));
        CHECK(getrandom(bytes, sizeof(bytes), flags) == sizeof(bytes));
        CHECK(memcmp(bytes, untouched, sizeof(bytes)) != 0);
        for (size_t i = 0; i + 16 <= sizeof(bytes); i += 4096)
            CHECK(memcmp(bytes + i, untouched + i, sizeof(bytes) - i) != 0);
        CHECK(getrandom(bytes, 1, flags) == 1);
    }
    CHECK(getrandom(NULL, 0, 0) == 0);
    CHECK(getrandom(bytes, 1, 0x8000) == -1 && errno == EINVAL);
    CHECK(getrandom(NULL, 16, GRND_NONBLOCK) == -1 && errno == EFAULT);
    void *area = mmap(NULL, 8192, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(area != MAP_FAILED);
    CHECK(getrandom(area, 16, 0) == -1 && errno == EFAULT);
    CHECK(mprotect(area, 4096, PROT_READ | PROT_WRITE) == 0);
    CHECK(getrandom(area, 8192, 0) == 4096); /* Progress before inaccessible page. */
    unsigned char saved[16];
    memcpy(saved, area, sizeof(saved));
    CHECK(mprotect(area, 4096, PROT_READ) == 0);
    CHECK(*(volatile unsigned char *)area == saved[0]); /* Re-fault the read-only PTE. */
    CHECK(getrandom(area, 16, 0) == -1 && errno == EFAULT);
    CHECK(memcmp(saved, area, sizeof(saved)) == 0); /* Resident read-only PTE. */
    CHECK(munmap(area, 8192) == 0);
    int fd = open("/dev/urandom", O_RDONLY);
    CHECK(fd >= 0);
    CHECK(read(fd, bytes, sizeof(bytes)) == sizeof(bytes));
    CHECK(close(fd) == 0);

    /* Exercise the actual distribution library that XFCE/libSM uses. */
    void *library = dlopen("libuuid.so.1", RTLD_NOW | RTLD_LOCAL);
    CHECK(library != NULL);
    void (*generate)(unsigned char *) = (void (*)(unsigned char *))dlsym(library, "uuid_generate");
    CHECK(generate != NULL);
    unsigned char ids[1000][16];
    for (size_t i = 0; i < 1000; i++) {
        generate(ids[i]);
        CHECK((ids[i][6] & 0xf0) == 0x40);
        for (size_t j = 0; j < i; j++) CHECK(memcmp(ids[i], ids[j], 16) != 0);
    }
    CHECK(dlclose(library) == 0);
    puts("LPR_RANDOM=OK nonblock bytes errors readonly-resident chunking urandom libuuid-1000-unique");
    return 0;
}
