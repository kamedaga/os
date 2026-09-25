#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* Exercise the dynamic loader's initial 960-byte read using real immutable
 * libraries, including after fork. This test never modifies the libraries. */
static const char *paths[] = {
    "/usr/lib/libgallium-25.1.9.so", "/usr/lib/libXxf86vm.so.1",
    "/usr/lib/libxcb-dri3.so.0", "/usr/lib/libxcb-present.so.0",
    "/usr/lib/libidn2.so.0", "/usr/lib/libunistring.so.5",
    "/usr/lib/libgcrypt.so.20"
};
enum { FILES = sizeof(paths) / sizeof(paths[0]), THREADS = 6, ROUNDS = 60 };
static unsigned char expected[FILES][960];
static atomic_uint failures;
static pthread_barrier_t gate;

static void *reader(void *argument)
{
    unsigned id = (unsigned)(size_t)argument;
    pthread_barrier_wait(&gate);
    for (unsigned round = 0; round < ROUNDS; ++round) {
        unsigned file = (id + round) % FILES;
        unsigned char data[960];
        int fd = open(paths[file], O_RDONLY | O_CLOEXEC);
        ssize_t count = fd < 0 ? -1 : read(fd, data, sizeof(data));
        int saved = errno;
        off_t position = fd < 0 ? -1 : lseek(fd, 0, SEEK_CUR);
        if (count != sizeof(data) || position != sizeof(data) ||
            memcmp(data, expected[file], sizeof(data))) {
            if (atomic_fetch_add(&failures, 1) < 12)
                fprintf(stderr, "ELF_READ path=%s count=%ld offset=%ld errno=%d\n",
                    paths[file], (long)count, (long)position, saved);
        }
        if (fd >= 0 && close(fd)) atomic_fetch_add(&failures, 1);
    }
    return NULL;
}

static int run(void)
{
    pthread_t threads[THREADS];
    atomic_store(&failures, 0);
    if (pthread_barrier_init(&gate, NULL, THREADS)) return 2;
    for (unsigned i = 0; i < THREADS; ++i)
        if (pthread_create(&threads[i], NULL, reader, (void *)(size_t)i)) _exit(2);
    for (unsigned i = 0; i < THREADS; ++i)
        if (pthread_join(threads[i], NULL)) return 2;
    pthread_barrier_destroy(&gate);
    printf("ELF_READ_CONCURRENCY pid=%ld failures=%u\n",
        (long)getpid(), atomic_load(&failures));
    fflush(stdout);
    return atomic_load(&failures) != 0;
}

int main(void)
{
    for (unsigned i = 0; i < FILES; ++i) {
        int fd = open(paths[i], O_RDONLY);
        if (fd < 0 || pread(fd, expected[i], sizeof(expected[i]), 0) != sizeof(expected[i]) ||
            memcmp(expected[i], "\177ELF", 4) || close(fd)) {
            fprintf(stderr, "ELF_READ baseline failed: %s\n", paths[i]);
            return 2;
        }
    }
    if (run()) return 1;
    for (unsigned i = 0; i < 3; ++i) {
        pid_t child = fork();
        if (child < 0) return 2;
        if (!child) _exit(run());
        int status;
        if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status))
            return 1;
    }
    puts("ELF_READ_CONCURRENCY=OK");
    return 0;
}
