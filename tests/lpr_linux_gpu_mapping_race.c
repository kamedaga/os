#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

struct resource_create {
    uint32_t target, format, bind, width, height, depth, array_size;
    uint32_t last_level, nr_samples, flags, bo_handle, res_handle, size, stride;
};
struct gem_map { uint64_t offset; uint32_t handle, pad; };
struct gem_close { uint32_t handle, pad; };
#define RESOURCE_CREATE _IOWR('d', 0x44, struct resource_create)
#define GEM_MAP _IOWR('d', 0x41, struct gem_map)
#define GEM_CLOSE _IOW('d', 0x09, struct gem_close)

static atomic_uint failures, completed;

static void *worker(void *argument)
{
    unsigned id = (unsigned)(uintptr_t)argument;
    int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (fd < 0) { atomic_fetch_add(&failures, 1); return NULL; }
    for (unsigned round = 0; round < 500; ++round) {
        struct resource_create resource = {
            .target = 2, .format = 1, .bind = 2, .width = 32, .height = 32,
            .depth = 1, .array_size = 1, .size = 4096, .stride = 128,
        };
        if (ioctl(fd, RESOURCE_CREATE, &resource)) {
            fprintf(stderr, "GPU_MAP create worker=%u round=%u errno=%d\n", id, round, errno);
            atomic_fetch_add(&failures, 1);
            break;
        }
        struct gem_map map = {.handle = resource.bo_handle};
        struct gem_close release = {.handle = resource.bo_handle};
        int mapped = ioctl(fd, GEM_MAP, &map);
        volatile uint32_t *data = mapped ? MAP_FAILED : mmap(NULL, 4096,
            PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)map.offset);
        /* The mapping must keep the object alive after its GEM handle closes. */
        int closed = ioctl(fd, GEM_CLOSE, &release);
        if (data == MAP_FAILED || closed) {
            fprintf(stderr, "GPU_MAP map/close worker=%u round=%u errno=%d\n", id, round, errno);
            if (data != MAP_FAILED) munmap((void *)data, 4096);
            atomic_fetch_add(&failures, 1);
            break;
        }
        uint32_t pattern = 0x13570000u ^ (id << 16) ^ round;
        for (unsigned i = 0; i < 1024; ++i) data[i] = pattern ^ i;
        sched_yield();
        for (unsigned i = 0; i < 1024; ++i) {
            if (data[i] != (pattern ^ i)) {
                atomic_fetch_add(&failures, 1);
                break;
            }
        }
        if (munmap((void *)data, 4096)) atomic_fetch_add(&failures, 1);
        atomic_fetch_add(&completed, 1);
    }
    if (close(fd)) atomic_fetch_add(&failures, 1);
    return NULL;
}

int main(void)
{
    pthread_t threads[2];
    for (unsigned i = 0; i < 2; ++i)
        if (pthread_create(&threads[i], NULL, worker, (void *)(uintptr_t)(i + 1))) return 2;
    for (unsigned i = 0; i < 2; ++i) pthread_join(threads[i], NULL);
    printf("GPU_MAPPING_RACE completed=%u failures=%u\n",
        atomic_load(&completed), atomic_load(&failures));
    return atomic_load(&failures) || atomic_load(&completed) != 1000;
}
