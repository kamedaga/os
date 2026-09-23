#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <pthread.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Short, read-only workloads. Separate map construction from first-touch;
 * no internet, browser profile, persistent files or cache dropping. */
static uint64_t now(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) { perror("clock"); exit(2); }
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
static void metric(const char *phase, unsigned round, uint64_t start, size_t bytes)
{
    uint64_t elapsed = now() - start;
    printf("PERF phase=%s round=%u ns=%llu bytes=%zu\n", phase, round,
        (unsigned long long)elapsed, bytes);
}
static void *echo_thread(void *raw)
{
    int fd = *(int *)raw;
    for (unsigned i = 0; i < 100; i++) {
        struct pollfd p = { .fd = fd, .events = POLLIN };
        char b;
        if (poll(&p, 1, 5000) != 1 || recv(fd, &b, 1, 0) != 1 ||
            send(fd, &b, 1, 0) != 1) return (void *)1;
    }
    return NULL;
}
static int gpu_baseline(void)
{
    struct resource_create {
        uint32_t target, format, bind, width, height, depth, array_size;
        uint32_t last_level, nr_samples, flags, bo_handle, res_handle, size, stride;
    };
    struct gem_close { uint32_t handle, pad; };
    struct get_cap { uint64_t capability, value; };
    uint64_t start = now();
    int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("DRM open"); return 2; }
    metric("drm_open", 0, start, 0);
    for (unsigned round = 0; round < 3; round++) {
        start = now();
        for (unsigned i = 0; i < 100; i++) {
            struct get_cap cap = { .capability = 1 };
            if (ioctl(fd, _IOWR('d', 0x0c, struct get_cap), &cap)) {
                perror("GET_CAP"); close(fd); return 2;
            }
        }
        metric("drm_getcap_100", round, start, 0);
        struct resource_create r = { .target = 2, .format = 1, .bind = 2,
            .width = 1920, .height = 1080, .depth = 1, .array_size = 1,
            .size = 1920 * 1080 * 4, .stride = 1920 * 4 };
        start = now();
        if (ioctl(fd, _IOWR('d', 0x44, struct resource_create), &r)) {
            perror("RESOURCE_CREATE"); close(fd); return 2;
        }
        metric("drm_fhd_create", round, start, r.size);
        struct gem_close c = { .handle = r.bo_handle };
        start = now();
        if (ioctl(fd, _IOW('d', 0x09, struct gem_close), &c)) {
            perror("GEM_CLOSE"); close(fd); return 2;
        }
        metric("drm_fhd_close", round, start, 0);
    }
    return close(fd) ? 2 : 0;
}
int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc > 1 && !strcmp(argv[1], "--gpu")) return gpu_baseline();
    if (argc > 1 && !strcmp(argv[1], "--loader")) {
        for (unsigned round = 0; round < 3; round++) {
            uint64_t start = now();
            pid_t child = fork();
            if (child < 0) return 2;
            if (!child) {
                int sink = open("/dev/null", O_WRONLY);
                if (sink < 0 || dup2(sink, 1) < 0) _exit(126);
                close(sink);
                execl("/usr/bin/epiphany", "epiphany", "--version", (char *)NULL);
                _exit(127);
            }
            int status;
            if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status)) return 2;
            metric("epiphany_version_startup", round, start, 0);
        }
        return 0;
    }
    const char *path = argc > 1 ? argv[1] : "/usr/lib/libwebkitgtk-6.0.so.4";
    printf("PERF file=%s\n", path);
    for (unsigned round = 0; round < 3; round++) {
        uint64_t start = now();
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) { perror("open"); return 2; }
        metric("open", round, start, 0);
        unsigned char header[960];
        start = now();
        if (pread(fd, header, sizeof(header), 0) != sizeof(header) ||
            memcmp(header, "\177ELF", 4)) { fprintf(stderr,"invalid ELF read\n"); return 2; }
        metric("pread960", round, start, sizeof(header));
        struct stat st;
        if (fstat(fd, &st) || st.st_size < 4096) return 2;
        start = now();
        unsigned char *p = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (p == MAP_FAILED) { perror("mmap"); return 2; }
        metric("mmap", round, start, st.st_size);
        volatile unsigned checksum = 0;
        start = now();
        for (size_t i = 0; i < (size_t)st.st_size; i += 4096) checksum += p[i];
        metric("touch", round, start, st.st_size);
        printf("PERF checksum=%u\n", checksum);
        start = now();
        if (munmap(p, st.st_size) || close(fd)) return 2;
        metric("unmap_close", round, start, st.st_size);
    }
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair)) return 2;
    for (unsigned round = 0; round < 3; round++) {
        uint64_t start = now();
        for (unsigned i = 0; i < 100; i++) {
            char byte;
            if (send(pair[0], "x", 1, 0) != 1 || recv(pair[1], &byte, 1, 0) != 1 || byte != 'x') return 2;
        }
        metric("unix_send_recv_100", round, start, 100);
    }
    for (unsigned round = 0; round < 3; round++) {
        pthread_t thread;
        if (pthread_create(&thread, NULL, echo_thread, &pair[1])) return 2;
        uint64_t start = now();
        for (unsigned i = 0; i < 100; i++) {
            char b;
            struct pollfd p = { .fd = pair[0], .events = POLLIN };
            if (send(pair[0], "x", 1, 0) != 1 || poll(&p, 1, 5000) != 1 ||
                recv(pair[0], &b, 1, 0) != 1 || b != 'x') return 2;
        }
        metric("unix_thread_roundtrip_100", round, start, 100);
        void *result;
        if (pthread_join(thread, &result) || result) return 2;
    }
    close(pair[0]); close(pair[1]);
    for (unsigned round = 0; round < 3; round++) {
        uint64_t start = now();
        pid_t child = fork();
        if (child < 0) return 2;
        if (!child) { execl("/bin/true", "true", (char *)NULL); _exit(127); }
        int status;
        if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status)) return 2;
        metric("fork_exec_wait", round, start, 0);
    }
    puts("PERF PASS");
    return 0;
}
