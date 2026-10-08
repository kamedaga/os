/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

enum { THREADS = 4, ITERATIONS = 16, LARGE = 1024 * 1024, RECORD = 64 };
static pthread_barrier_t start;
static int source_fd, append_fd;
static unsigned char *seed;
struct worker { unsigned index; int failed; uint64_t chunks[ITERATIONS]; };

struct blocked_open {
    int dir, failed;
    atomic_int started, finished;
};

static void *open_reader(void *raw)
{
    struct blocked_open *request = raw;
    atomic_store(&request->started, 1);
    int fd = openat(request->dir, "blocked", O_RDONLY);
    request->failed = fd < 0;
    if (fd >= 0 && close(fd)) request->failed = 1;
    atomic_store(&request->finished, 1);
    return NULL;
}

static int blocked_vfs(int dir)
{
    int trigger[2], reply[2];
    if (mkfifoat(dir, "blocked", 0600) || pipe(trigger) || pipe(reply)) return 1;
    /* Fork before starting a thread: the child gets its own authenticated
     * queue without inheriting an in-progress producer or libc thread lock. */
    pid_t child = fork();
    if (child < 0) return 1;
    if (!child) {
        char byte;
        struct stat st;
        close(trigger[1]); close(reply[0]);
        int failed = read(trigger[0], &byte, 1) != 1 ||
            fstatat(dir, "blocked", &st, 0) || !S_ISFIFO(st.st_mode);
        byte = failed ? 'E' : 'K';
        if (write(reply[1], &byte, 1) != 1) failed = 1;
        close(trigger[0]); close(reply[1]);
        _exit(failed);
    }
    close(trigger[0]); close(reply[1]);
    struct blocked_open request = { .dir = dir };
    pthread_t reader;
    int error = pthread_create(&reader, NULL, open_reader, &request);
    if (error) { errno = error; return 1; }
    while (!atomic_load(&request.started)) sched_yield();
    usleep(50000);
    struct stat st;
    char byte = 'K';
    int failed = atomic_load(&request.finished) ||
        fstatat(dir, "blocked", &st, 0) || !S_ISFIFO(st.st_mode);
    if (write(trigger[1], &byte, 1) != 1 ||
        read(reply[0], &byte, 1) != 1 || byte != 'K') failed = 1;
    int status;
    if (waitpid(child, &status, 0) != child ||
        !WIFEXITED(status) || WEXITSTATUS(status)) failed = 1;
    close(trigger[1]); close(reply[0]);
    if (atomic_load(&request.finished)) failed = 1;
    printf("FS_BLOCKED_VFS_PROGRESS same_client=%s other_client=%s reader_pending=%s\n",
        failed ? "error" : "ok", byte == 'K' ? "ok" : "error",
        atomic_load(&request.finished) ? "no" : "yes");
    fflush(stdout);
    /* A blocking FIFO open cannot finish without its counterpart. This pair
     * therefore proves concurrent VFS execution, not just queued ring entries. */
    int writer = openat(dir, "blocked", O_WRONLY);
    if (writer < 0 || (writer >= 0 && close(writer))) failed = 1;
    if (pthread_join(reader, NULL) || request.failed ||
        !atomic_load(&request.finished) || unlinkat(dir, "blocked", 0)) failed = 1;
    if (!failed) puts("FS_BLOCKED_VFS_SMOKE_OK");
    return failed;
}

static int exec_lifetime(int argc, char **argv)
{
    if (argc != 4) return 1;
    int shared = atoi(argv[2]), cloexec = atoi(argv[3]);
    char byte;
    errno = 0;
    if (fcntl(cloexec, F_GETFD) != -1 || errno != EBADF ||
        read(shared, &byte, 1) != 1 || byte != 'A' || close(shared)) return 1;
    puts("FS_EXEC_OFD_SMOKE_OK cloexec=closed inherited_ofd=ok");
    return 0;
}

static int fork_lifetime(int dir, const char *executable)
{
    int fd = openat(dir, "fork-shared", O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0 || write(fd, "AB", 2) != 2 || lseek(fd, 0, SEEK_SET)) return 1;
    int cloexec = dup(fd);
    if (cloexec < 0 || fcntl(cloexec, F_SETFD, FD_CLOEXEC)) return 1;
    pid_t child = fork();
    if (child < 0) return 1;
    if (!child) {
        int inherited = dup(fd);
        if (inherited < 0 || close(fd)) _exit(1);
        char shared_number[24], cloexec_number[24];
        snprintf(shared_number, sizeof(shared_number), "%d", inherited);
        snprintf(cloexec_number, sizeof(cloexec_number), "%d", cloexec);
        /* Exec must retire the child's old queue, keep the transferred OFD,
         * and discard only its own CLOEXEC descriptor, not the parent's copy. */
        execl(executable, executable, "--exec-lifetime", shared_number, cloexec_number, NULL);
        perror("multiflight child exec");
        _exit(1);
    }
    int status;
    char byte;
    int failed = waitpid(child, &status, 0) != child ||
        !WIFEXITED(status) || WEXITSTATUS(status);
    int flags = fcntl(cloexec, F_GETFD);
    if (read(fd, &byte, 1) != 1 || byte != 'B' ||
        flags < 0 || !(flags & FD_CLOEXEC)) failed = 1;
    if (close(fd) || close(cloexec) || unlinkat(dir, "fork-shared", 0)) failed = 1;
    if (!failed) puts("FS_FORK_EXEC_LIFETIME_OK shared_ofd=ok parent_survives_child_exit=ok");
    return failed;
}

static void *run(void *raw)
{
    struct worker *w = raw;
    unsigned char *large = malloc(LARGE);
    int fd = dup(source_fd), append = dup(append_fd);
    pthread_barrier_wait(&start);
    if (!large || fd < 0 || append < 0) { w->failed = 1; goto done; }
    if (pread(fd, large, LARGE, 0) != LARGE || memcmp(large, seed, LARGE)) {
        fprintf(stderr, "multiflight large pread thread=%u errno=%d\n", w->index, errno);
        w->failed = 1; goto done;
    }
    for (unsigned i = 0; i < ITERATIONS; ++i) {
        unsigned char a[31], b[33], record[RECORD];
        struct iovec input[] = {{a, sizeof(a)}, {b, sizeof(b)}};
        if (readv(fd, input, 2) != RECORD) { w->failed = 1; break; }
        uint64_t chunk;
        memcpy(&chunk, a, sizeof(chunk));
        if (chunk >= LARGE / RECORD || memcmp(a, seed + chunk * RECORD, sizeof(a)) ||
            memcmp(b, seed + chunk * RECORD + sizeof(a), sizeof(b))) {
            fprintf(stderr, "multiflight shared-offset vector interleave thread=%u iteration=%u\n", w->index, i);
            w->failed = 1; break;
        }
        w->chunks[i] = chunk;
        memset(record, (unsigned char)(w->index * ITERATIONS + i), sizeof(record));
        struct iovec output[] = {{record, 17}, {record + 17, sizeof(record) - 17}};
        if (writev(append, output, 2) != RECORD) { w->failed = 1; break; }
        struct stat st;
        if (fstat(fd, &st) || st.st_size != LARGE) { w->failed = 1; break; }
    }
done:
    if (fd >= 0) close(fd);
    if (append >= 0) close(append);
    free(large);
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--exec-lifetime")) return exec_lifetime(argc, argv);
    char directory[256];
    if (snprintf(directory, sizeof(directory), "%s/fs-multiflight-XXXXXX",
        argc > 1 ? argv[1] : "/tmp") >= (int)sizeof(directory) || !mkdtemp(directory)) {
        perror("multiflight directory"); return 1;
    }
    int dir = open(directory, O_RDONLY | O_DIRECTORY);
    if (dir < 0 || blocked_vfs(dir)) { perror("multiflight blocked VFS"); return 1; }
    if (fork_lifetime(dir, argv[0])) { perror("multiflight fork/exec lifetime"); return 1; }
    source_fd = openat(dir, "source", O_CREAT | O_RDWR | O_TRUNC, 0600);
    append_fd = openat(dir, "append", O_CREAT | O_RDWR | O_APPEND, 0600);
    if (dir < 0 || source_fd < 0 || append_fd < 0) { perror("multiflight open"); return 1; }
    seed = malloc(LARGE);
    if (!seed) return 1;
    for (uint64_t i = 0; i < LARGE / RECORD; ++i) {
        memset(seed + i * RECORD, (unsigned char)(i % 251), RECORD);
        memcpy(seed + i * RECORD, &i, sizeof(i));
    }
    if (write(source_fd, seed, LARGE) != LARGE || lseek(source_fd, 0, SEEK_SET)) {
        perror("multiflight seed"); return 1;
    }
    struct worker workers[THREADS] = {{0}};
    pthread_t threads[THREADS];
    if (pthread_barrier_init(&start, NULL, THREADS + 1)) return 1;
    for (unsigned i = 0; i < THREADS; ++i) {
        workers[i].index = i;
        if (pthread_create(&threads[i], NULL, run, &workers[i])) return 1;
    }
    pthread_barrier_wait(&start);
    int failed = 0;
    for (unsigned i = 0; i < THREADS; ++i) {
        if (pthread_join(threads[i], NULL)) failed = 1;
        failed |= workers[i].failed;
    }
    unsigned char seen[THREADS * ITERATIONS] = {0};
    for (unsigned i = 0; !failed && i < THREADS; ++i)
        for (unsigned j = 0; j < ITERATIONS; ++j) {
            uint64_t chunk = workers[i].chunks[j];
            if (chunk >= sizeof(seen) || seen[chunk]++) failed = 1;
        }
    unsigned char records[THREADS * ITERATIONS * RECORD];
    memset(seen, 0, sizeof(seen));
    if (pread(append_fd, records, sizeof(records), 0) != sizeof(records)) failed = 1;
    for (unsigned i = 0; !failed && i < sizeof(seen); ++i) {
        unsigned id = records[i * RECORD];
        if (id >= sizeof(seen) || seen[id]++) { failed = 1; break; }
        for (unsigned j = 0; j < RECORD; ++j)
            if (records[i * RECORD + j] != id) failed = 1;
    }
    struct stat st;
    if (fstat(append_fd, &st) || st.st_size != sizeof(records) || fsync(append_fd)) failed = 1;
    if (close(source_fd) || close(append_fd)) failed = 1;
    if (unlinkat(dir, "source", 0) || unlinkat(dir, "append", 0) || close(dir) || rmdir(directory)) failed = 1;
    pthread_barrier_destroy(&start); free(seed);
    if (failed) { fprintf(stderr, "FS_MULTIFLIGHT_SMOKE_ERROR errno=%d\n", errno); return 1; }
    printf("FS_MULTIFLIGHT_SMOKE_OK threads=%u iterations=%u large=%u append=%u\n",
        THREADS, ITERATIONS, LARGE, RECORD);
    return 0;
}
