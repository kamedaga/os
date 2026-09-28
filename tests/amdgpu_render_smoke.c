/* SPDX-License-Identifier: MIT */
/* One bounded SDMA fill through the public Linux DRM render-node ABI. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* Linux's exported DRM UAPI and musl both define ioctl encoding macros.
 * Keep the libc function declaration, then use the kernel's UAPI values. */
#undef _IOC
#undef _IO
#undef _IOR
#undef _IOW
#undef _IOWR
#include <drm/amdgpu_drm.h>

enum { BO_BYTES = 4096, FILL_BYTES = 64 };
static const uint32_t fill_value = UINT32_C(0x6a25d4c3);

static int call(int fd, unsigned long request, void *argument,
                const char *stage)
{
    if (ioctl(fd, request, argument) == 0) {
        printf("ok %s\n", stage);
        fflush(stdout);
        return 0;
    }
    fprintf(stderr, "fail %s errno=%d\n", stage, errno);
    fflush(stderr);
    return -1;
}

static int query_info(int fd, uint32_t query, uint32_t selector,
                      void *output, uint32_t bytes, const char *stage)
{
    struct drm_amdgpu_info info = {
        .return_pointer = (uintptr_t)output,
        .return_size = bytes,
        .query = query,
    };
    info.query_hw_ip.type = selector;
    return call(fd, DRM_IOCTL_AMDGPU_INFO, &info, stage);
}

static uint64_t align_up(uint64_t value, uint64_t alignment)
{
    if (!alignment || value > UINT64_MAX - (alignment - 1)) return 0;
    return ((value + alignment - 1) / alignment) * alignment;
}

static int create_bo(int fd, uint32_t *handle)
{
    union drm_amdgpu_gem_create create = {0};
    create.in.bo_size = BO_BYTES;
    create.in.alignment = BO_BYTES;
    create.in.domains = AMDGPU_GEM_DOMAIN_GTT;
    if (call(fd, DRM_IOCTL_AMDGPU_GEM_CREATE, &create, "gem-create"))
        return -1;
    if (!create.out.handle) return -1;
    *handle = create.out.handle;
    return 0;
}

static void *map_bo(int fd, uint32_t handle)
{
    union drm_amdgpu_gem_mmap map = {0};
    map.in.handle = handle;
    if (call(fd, DRM_IOCTL_AMDGPU_GEM_MMAP, &map, "gem-mmap"))
        return MAP_FAILED;
    void *address = mmap(NULL, BO_BYTES, PROT_READ | PROT_WRITE,
                         MAP_SHARED, fd, map.out.addr_ptr);
    if (address == MAP_FAILED)
        fprintf(stderr, "fail mmap-view errno=%d\n", errno);
    else
        printf("ok mmap-view\n");
    fflush(stdout);
    return address;
}

static int map_va(int fd, uint32_t handle, uint64_t address, uint32_t flags)
{
    struct drm_amdgpu_gem_va map = {
        .handle = handle,
        .operation = AMDGPU_VA_OP_MAP,
        .flags = flags,
        .va_address = address,
        .map_size = BO_BYTES,
    };
    return call(fd, DRM_IOCTL_AMDGPU_GEM_VA, &map, "gem-va-map");
}

int main(void)
{
    int result = 1, fd = -1;
    uint32_t ib_handle = 0, out_handle = 0, context = 0;
    uint32_t *ib = MAP_FAILED, *output = MAP_FAILED;
    struct drm_amdgpu_info_device device = {0};
    struct drm_amdgpu_info_hw_ip dma = {0};
    union drm_amdgpu_ctx ctx = {0};
    char driver_name[32] = {0};
    struct drm_version version = {
        .name_len = sizeof(driver_name),
        .name = driver_name,
    };
    struct drm_get_cap prime = {.capability = DRM_CAP_PRIME};
    uint32_t acceleration = 0, dma_count = 0;
    uint64_t ib_va, out_va, alignment;
    uint32_t ring, ib_bytes;

    fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "fail render-open errno=%d\n", errno);
        goto done;
    }
    puts("ok render-open");
    if (call(fd, DRM_IOCTL_VERSION, &version, "drm-version") ||
        version.name_len != strlen("amdgpu") ||
        memcmp(driver_name, "amdgpu", strlen("amdgpu")) ||
        call(fd, DRM_IOCTL_GET_CAP, &prime, "drm-get-cap"))
        goto done;
    if (query_info(fd, AMDGPU_INFO_ACCEL_WORKING, 0, &acceleration,
                   sizeof(acceleration), "info-acceleration") ||
        acceleration != 1 ||
        query_info(fd, AMDGPU_INFO_HW_IP_COUNT, AMDGPU_HW_IP_DMA,
                   &dma_count, sizeof(dma_count), "info-dma-count") ||
        !dma_count ||
        query_info(fd, AMDGPU_INFO_HW_IP_INFO, AMDGPU_HW_IP_DMA,
                   &dma, sizeof(dma), "info-dma-ip") ||
        query_info(fd, AMDGPU_INFO_DEV_INFO, 0,
                   &device, sizeof(device), "info-device"))
        goto done;

    /* The five-dword SDMA constant-fill packet is stable for the IP
     * generations accepted here. Selection is by advertised IP, not PCI ID. */
    if (dma.hw_ip_version_major < 2 ||
        (dma.hw_ip_version_major == 2 && dma.hw_ip_version_minor < 4) ||
        !dma.available_rings ||
        dma.ib_start_alignment > BO_BYTES ||
        dma.ib_size_alignment > BO_BYTES ||
        !device.virtual_address_alignment ||
        device.virtual_address_alignment > BO_BYTES)
        goto done;
    ring = (uint32_t)__builtin_ctz(dma.available_rings);
    alignment = device.virtual_address_alignment;
    if (alignment < BO_BYTES) alignment = BO_BYTES;
    ib_va = align_up(device.virtual_address_offset, alignment);
    if (!ib_va || ib_va > UINT64_MAX - BO_BYTES) goto done;
    out_va = align_up(ib_va + BO_BYTES, alignment);
    if (!out_va || device.virtual_address_max < BO_BYTES ||
        out_va > device.virtual_address_max - BO_BYTES)
        goto done;
    ib_bytes = (uint32_t)align_up(5 * sizeof(uint32_t),
                                 dma.ib_size_alignment ?
                                 dma.ib_size_alignment : sizeof(uint32_t));
    if (ib_bytes < 5 * sizeof(uint32_t) || ib_bytes > BO_BYTES ||
        ib_bytes % sizeof(uint32_t)) goto done;
    printf("dma ip=%u.%u count=%u ring=%u ib_align=%u va=%#" PRIx64 "\n",
           dma.hw_ip_version_major, dma.hw_ip_version_minor,
           dma_count, ring, dma.ib_size_alignment, ib_va);
    fflush(stdout);

    if (create_bo(fd, &ib_handle) || create_bo(fd, &out_handle)) goto done;
    ib = map_bo(fd, ib_handle);
    if (ib == MAP_FAILED) goto done;
    output = map_bo(fd, out_handle);
    if (output == MAP_FAILED) goto done;
    memset(ib, 0, BO_BYTES);
    memset(output, 0, BO_BYTES);
    /* SDMA_PACKET(CONSTANT_FILL, 0, 0) | fill-size dwords. The count
     * operand is bytes minus one even with a dword-sized fill. */
    ib[0] = 0x0bu | (2u << 30);
    ib[1] = (uint32_t)out_va;
    ib[2] = (uint32_t)(out_va >> 32);
    ib[3] = fill_value;
    ib[4] = FILL_BYTES - 1;
    if (map_va(fd, ib_handle, ib_va,
               AMDGPU_VM_PAGE_READABLE | AMDGPU_VM_PAGE_EXECUTABLE) ||
        map_va(fd, out_handle, out_va,
               AMDGPU_VM_PAGE_READABLE | AMDGPU_VM_PAGE_WRITEABLE))
        goto done;

    ctx.in.op = AMDGPU_CTX_OP_ALLOC_CTX;
    if (call(fd, DRM_IOCTL_AMDGPU_CTX, &ctx, "context-alloc")) goto done;
    context = ctx.out.alloc.ctx_id;
    if (!context) goto done;
    struct drm_amdgpu_cs_chunk_ib ib_chunk = {
        .va_start = ib_va, .ib_bytes = ib_bytes,
        .ip_type = AMDGPU_HW_IP_DMA, .ring = ring,
    };
    struct drm_amdgpu_bo_list_entry entries[2] = {
        {.bo_handle = ib_handle}, {.bo_handle = out_handle},
    };
    struct drm_amdgpu_bo_list_in bo_list = {
        .bo_number = 2,
        .bo_info_size = sizeof(entries[0]),
        .bo_info_ptr = (uintptr_t)entries,
    };
    struct drm_amdgpu_cs_chunk chunks[2] = {
        {.chunk_id = AMDGPU_CHUNK_ID_IB,
         .length_dw = sizeof(ib_chunk) / sizeof(uint32_t),
         .chunk_data = (uintptr_t)&ib_chunk},
        {.chunk_id = AMDGPU_CHUNK_ID_BO_HANDLES,
         .length_dw = sizeof(bo_list) / sizeof(uint32_t),
         .chunk_data = (uintptr_t)&bo_list},
    };
    uint64_t pointers[2] = {
        (uintptr_t)&chunks[0], (uintptr_t)&chunks[1],
    };
    union drm_amdgpu_cs cs = {0};
    cs.in.ctx_id = context;
    cs.in.num_chunks = 2;
    cs.in.chunks = (uintptr_t)pointers;
    if (call(fd, DRM_IOCTL_AMDGPU_CS, &cs, "cs-submit")) goto done;
    uint64_t sequence = cs.out.handle;
    if (!sequence) goto done;

    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) goto done;
    union drm_amdgpu_wait_cs wait = {0};
    wait.in.handle = sequence;
    wait.in.timeout = (uint64_t)now.tv_sec * UINT64_C(1000000000) +
        (uint64_t)now.tv_nsec + UINT64_C(3000000000);
    wait.in.ip_type = AMDGPU_HW_IP_DMA;
    wait.in.ring = ring;
    wait.in.ctx_id = context;
    if (call(fd, DRM_IOCTL_AMDGPU_WAIT_CS, &wait, "cs-wait") ||
        wait.out.status != 0) goto done;
    for (unsigned index = 0; index < FILL_BYTES / sizeof(uint32_t); ++index) {
        if (output[index] != fill_value) {
            fprintf(stderr, "fail output[%u]=%#x expected=%#x\n",
                    index, output[index], fill_value);
            goto done;
        }
    }
    printf("PASS sequence=%" PRIu64 " fence=complete output=%#x\n",
           sequence, fill_value);
    result = 0;

done:
    if (context) {
        memset(&ctx, 0, sizeof(ctx));
        ctx.in.op = AMDGPU_CTX_OP_FREE_CTX;
        ctx.in.ctx_id = context;
        (void)call(fd, DRM_IOCTL_AMDGPU_CTX, &ctx, "context-free");
    }
    if (ib != MAP_FAILED) (void)munmap(ib, BO_BYTES);
    if (output != MAP_FAILED) (void)munmap(output, BO_BYTES);
    if (ib_handle) {
        struct drm_gem_close close = {.handle = ib_handle};
        (void)call(fd, DRM_IOCTL_GEM_CLOSE, &close, "ib-close");
    }
    if (out_handle) {
        struct drm_gem_close close = {.handle = out_handle};
        (void)call(fd, DRM_IOCTL_GEM_CLOSE, &close, "output-close");
    }
    if (fd >= 0) (void)close(fd);
    return result;
}
