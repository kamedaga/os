#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>

static atomic_uint failures;
static pthread_barrier_t start;
static char paths[6][96];
static void *reader(void *arg)
{
    unsigned id = (unsigned)(size_t)arg;
    unsigned char data[512];
    pthread_barrier_wait(&start);
    for (unsigned repeat = 0; repeat < 120; ++repeat) {
        int fd = open(paths[id], O_RDONLY);
        if (fd < 0) { atomic_fetch_add(&failures, 1); continue; }
        for (unsigned part = 0; part < 16; ++part) {
            struct iovec iov[2] = {{data, 193}, {data + 193, sizeof(data) - 193}};
            ssize_t n = repeat & 1 ? readv(fd, iov, 2) : read(fd, data, sizeof(data));
            int bad = n != sizeof(data);
            for (unsigned j = 0; !bad && j < sizeof(data); ++j)
                bad = data[j] != (unsigned char)(33 + id);
            if (bad) atomic_fetch_add(&failures, 1);
            sched_yield();
        }
        close(fd);
    }
    return NULL;
}
int main(void)
{
    pthread_t threads[6];
    unsigned char data[8192];
    for (unsigned i = 0; i < 6; ++i) {
        snprintf(paths[i], sizeof(paths[i]), "/tmp/read-cache-%d-%u", getpid(), i);
        int fd = open(paths[i], O_CREAT | O_EXCL | O_WRONLY, 0600);
        if (fd < 0) return 2;
        memset(data, 33 + i, sizeof(data));
        if (write(fd, data, sizeof(data)) != sizeof(data) || close(fd)) return 3;
    }
    pthread_barrier_init(&start, NULL, 6);
    for (unsigned i = 0; i < 6; ++i)
        if (pthread_create(&threads[i], NULL, reader, (void *)(size_t)i)) return 4;
    for (unsigned i = 0; i < 6; ++i) pthread_join(threads[i], NULL);
    for (unsigned i = 0; i < 6; ++i) unlink(paths[i]);
    printf("READ_CACHE_CONCURRENCY failures=%u\n", atomic_load(&failures));
    return atomic_load(&failures) != 0;
}
