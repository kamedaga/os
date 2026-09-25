#define _GNU_SOURCE
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

static unsigned iteration;

static void check(int ok, const char *step)
{
    if (!ok) {
        printf("MREMAP_GROWTH_FAIL step=%s iteration=%u errno=%d\n", step, iteration, errno);
        exit(1);
    }
}

int main(int argc, char **argv)
{
    setbuf(stdout, NULL);
    int move_only = argc > 1 && !strcmp(argv[1], "--move-only");
    size_t size = 4 * 1024 * 1024 + 4096;
    unsigned char *memory = mmap(NULL, size, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check(memory != MAP_FAILED, "sparse-map");
    memory[0] = 0x5a;
    memory[size - 1] = 0xa5;
    for (unsigned i = 0; i < 600; ++i) {
        iteration = i + 1;
        size_t next_size = size + (move_only ? 0 : 4096);
        unsigned char *target = NULL;
        if (move_only) {
            target = mmap(NULL, size, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            check(target != MAP_FAILED, "move-target");
        }
        unsigned char *next = mremap(memory, size, next_size,
            MREMAP_MAYMOVE | (move_only ? MREMAP_FIXED : 0), target);
        check(next != MAP_FAILED, "grow");
        check(next[0] == 0x5a && next[size - 1] == 0xa5, "preserve");
        if (!move_only) {
            check(next[size + 4095] == 0, "new-tail-zero");
            next[size + 4095] = 0xa5;
        }
        memory = next;
        size = next_size;
        if (i % 50 == 0) printf("MREMAP_GROWTH_PROGRESS iteration=%u size=%zu\n", i + 1, size);
    }
    pid_t child = fork();
    check(child >= 0, "fork");
    if (child == 0) {
        unsigned char *next = mremap(memory, size, size + 4096, MREMAP_MAYMOVE);
        check(next != MAP_FAILED && next[0] == 0x5a && next[size - 1] == 0xa5, "child-grow");
        next[0] = 0x33;
        check(next[size + 4095] == 0, "child-tail-zero");
        next[size + 4095] = 0x44;
        check(munmap(next, size + 4096) == 0, "child-unmap");
        _exit(0);
    }
    int status;
    check(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
        "child-status");
    check(memory[0] == 0x5a && memory[size - 1] == 0xa5, "parent-cow-isolation");
    /* A rejected request must not discard the previous mapping or content. */
    errno = 0;
    check(mremap(memory, size, 0, MREMAP_MAYMOVE) == MAP_FAILED && errno == EINVAL,
        "reject-zero-size");
    check(memory[0] == 0x5a && memory[size - 1] == 0xa5, "rejection-preserves-source");
    check(munmap(memory, size) == 0, "unmap");
    puts("MREMAP_GROWTH_DONE");
    return 0;
}
