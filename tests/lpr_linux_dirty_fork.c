#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

static unsigned long free_kb(void)
{
    FILE *f = fopen("/proc/meminfo", "r");
    char line[160]; unsigned long value = 0;
    if (!f) return 0;
    while (fgets(line, sizeof(line), f))
        if (sscanf(line, "MemFree: %lu", &value) == 1) break;
    fclose(f);
    return value;
}
static int reap(pid_t pid)
{
    int status = 0;
    return waitpid(pid, &status, 0) == pid && WIFEXITED(status) && !WEXITSTATUS(status);
}
static int exchange(int fd, char *byte, int sending)
{
    ssize_t n;
    do { n = sending ? write(fd, byte, 1) : read(fd, byte, 1); } while (n < 0 && errno == EINTR);
    return n == 1;
}
static int prepare_dirty(unsigned char *data, size_t size)
{
    memset(data, 17, size);
    pid_t child = fork();
    if (child < 0) return 0;
    if (!child) _exit(0);
    if (!reap(child)) return 0;
    /* The first fork converts the parent's anonymous VMA to fork-COW.
     * Rewriting it populates the dirty table exercised by the next fork. */
    memset(data, 17, size);
    return 1;
}
static int memory_probe(void)
{
    size_t size = 128u << 20;
    unsigned char *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    int ready[2], finish[2];
    if (data == MAP_FAILED || !prepare_dirty(data, size) || pipe(ready) || pipe(finish)) return 1;
    unsigned long before = free_kb();
    pid_t child = fork();
    if (child < 0) return 1;
    if (!child) {
        for (size_t i = 0; i < size; i += 4096) if (data[i] != 17) _exit(2);
        char byte = 'R';
        if (!exchange(ready[1], &byte, 1) || !exchange(finish[0], &byte, 0)) _exit(3);
        execl("/bin/true", "true", (char *)NULL);
        _exit(4);
    }
    char byte = 0;
    if (!exchange(ready[0], &byte, 0)) return 1;
    unsigned long during = free_kb();
    long delta = (long)before - (long)during;
    byte = 'F';
    if (!exchange(finish[1], &byte, 1) || !reap(child)) return 1;
    for (unsigned i = 0; i < 2; ++i) { close(ready[i]); close(finish[i]); }
    if (data[0] != 17 || data[size - 1] != 17 || munmap(data, size)) return 1;
    printf("DIRTY_FORK before_kb=%lu child_read_kb=%lu delta_kb=%ld\n", before, during, delta);
    return delta > 32768;
}
static atomic_int stop_readers, bad_reads;
static volatile unsigned char *read_address;
static void *reader(void *unused)
{
    (void)unused;
    while (!atomic_load_explicit(&stop_readers, memory_order_relaxed)) {
        for (unsigned i = 0; i < 1000; ++i)
            if (*read_address != 17) atomic_store(&bad_reads, 1);
        sched_yield();
    }
    return NULL;
}
static int isolation_probe(void)
{
    size_t size = 4u << 20;
    unsigned char *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (data == MAP_FAILED || !prepare_dirty(data, size)) return 1;
    /* Keep both sides in one VMA, and exercise an independent protected slice. */
    if (mprotect(data + size / 2, 4096, PROT_NONE) ||
        mprotect(data + size / 2, 4096, PROT_READ | PROT_WRITE)) return 1;
    for (unsigned round = 0; round < 12; ++round) {
        data[0] = data[4096] = 17;
        atomic_store(&stop_readers, 0); atomic_store(&bad_reads, 0);
        read_address = data + 4096;
        pthread_t threads[2];
        for (unsigned i = 0; i < 2; ++i) if (pthread_create(&threads[i], NULL, reader, NULL)) return 1;
        int ready[2], finish[2];
        if (pipe(ready) || pipe(finish)) return 1;
        pid_t child = fork();
        if (child < 0) return 1;
        if (!child) {
            read_address = data;
            pthread_t readers[2];
            for (unsigned i = 0; i < 2; ++i) if (pthread_create(&readers[i], NULL, reader, NULL)) _exit(5);
            char byte = 'R';
            if (round & 1) {
                if (!exchange(ready[1], &byte, 1) || !exchange(finish[0], &byte, 0)) _exit(6);
                data[4096] = 29;
            } else {
                data[4096] = 29;
                if (!exchange(ready[1], &byte, 1) || !exchange(finish[0], &byte, 0)) _exit(7);
            }
            for (unsigned i = 0; i < 10000; ++i) if (data[0] != 17) _exit(8);
            atomic_store(&stop_readers, 1);
            for (unsigned i = 0; i < 2; ++i) pthread_join(readers[i], NULL);
            _exit(atomic_load(&bad_reads) || data[4096] != 29 ? 9 : 0);
        }
        char byte = 0;
        if (!exchange(ready[0], &byte, 0)) return 1;
        /* Alternate CPU stores and kernel copyout into the private VMA. */
        if (round & 1) data[0] = 88;
        else {
            byte = 88;
            if (!exchange(ready[1], &byte, 1) || !exchange(ready[0], (char *)data, 0)) return 1;
        }
        byte = 'F';
        if (!exchange(finish[1], &byte, 1) || !reap(child)) return 1;
        atomic_store(&stop_readers, 1);
        for (unsigned i = 0; i < 2; ++i) pthread_join(threads[i], NULL);
        for (unsigned i = 0; i < 2; ++i) { close(ready[i]); close(finish[i]); }
        if (atomic_load(&bad_reads) || data[0] != 88 || data[4096] != 17) return 1;
    }
    return munmap(data, size) != 0;
}
int main(void)
{
    int memory = memory_probe();
    int isolation = isolation_probe();
    printf("DIRTY_FORK memory=%s isolation=%s\n", memory ? "FAIL" : "PASS", isolation ? "FAIL" : "PASS");
    return memory || isolation;
}
