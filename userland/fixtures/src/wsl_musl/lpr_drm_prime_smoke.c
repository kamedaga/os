#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <gbm.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <linux/memfd.h>
#include <linux/udmabuf.h>

#define DRM_FORMAT_XRGB8888 0x34325258u
#define DRM_FORMAT_MOD_LINEAR 0ull

static int fail(const char *op)
{
    fprintf(stderr, "PRIME_FAIL op=%s errno=%d\n", op, errno);
    return 1;
}

static int probe_udmabuf(void)
{
    int memfd = memfd_create("prime-smoke", MFD_ALLOW_SEALING);
    if (memfd < 0) return fail("udmabuf-memfd");
    if (ftruncate(memfd, 4096) != 0) return fail("udmabuf-ftruncate");
    if (fcntl(memfd, F_ADD_SEALS, F_SEAL_SHRINK) != 0) return fail("udmabuf-seal");
    errno = 0;
    if (ftruncate(memfd, 2048) == 0 || errno != EPERM) return fail("udmabuf-seal-enforce");
    int device = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
    if (device < 0) return fail("udmabuf-open");
    struct udmabuf_create create;
    memset(&create, 0, sizeof(create));
    create.memfd = (uint32_t)memfd;
    create.flags = UDMABUF_FLAGS_CLOEXEC;
    create.size = 4096;
    int dmabuf = ioctl(device, UDMABUF_CREATE, &create);
    if (dmabuf < 0) return fail("udmabuf-create");
    int duplicated = fcntl(dmabuf, F_DUPFD_CLOEXEC, 3);
    if (duplicated < 0) return fail("udmabuf-dup");
    void *mapping = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, dmabuf, 0);
    if (mapping == MAP_FAILED) return fail("udmabuf-mmap");
    if (munmap(mapping, 4096) != 0) return fail("udmabuf-munmap");
    close(duplicated);
    close(dmabuf);
    close(device);
    close(memfd);
    printf("PRIME_UDMABUF_OK\n");
    fflush(stdout);
    return 0;
}

static drmModeConnector *connected_connector(int fd, drmModeRes *resources)
{
    for (int i = 0; i < resources->count_connectors; i++) {
        drmModeConnector *connector = drmModeGetConnector(fd, resources->connectors[i]);
        if (connector != NULL && connector->connection == DRM_MODE_CONNECTED && connector->count_modes > 0) {
            return connector;
        }
        drmModeFreeConnector(connector);
    }
    return NULL;
}

static int import_and_write_child(
    int inherited_card,
    int prime_fd,
    uint32_t width,
    uint32_t height,
    uint32_t stride)
{
    if (inherited_card >= 0) close(inherited_card);
    int card = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (card < 0) return fail("child-open-card");
    uint32_t child_handle = 0;
    if (drmPrimeFDToHandle(card, prime_fd, &child_handle) != 0 || child_handle == 0) {
        return fail("child-prime-import");
    }
    const off_t dma_size = lseek(prime_fd, 0, SEEK_END);
    if (dma_size < (off_t)((size_t)stride * height) || lseek(prime_fd, 0, SEEK_SET) != 0) {
        return fail("child-dmabuf-size");
    }
    void *dma_probe = mmap(NULL, (size_t)stride * height,
        PROT_READ | PROT_WRITE, MAP_SHARED, prime_fd, 0);
    if (dma_probe == MAP_FAILED) return fail("child-dmabuf-map-probe");
    if (munmap(dma_probe, (size_t)stride * height) != 0) return fail("child-dmabuf-unmap-probe");
    struct gbm_device *gbm = gbm_create_device(card);
    if (gbm == NULL) return fail("child-gbm-device");
    uint32_t gbm_handle = 0;
    if (drmPrimeFDToHandle(gbm_device_get_fd(gbm), prime_fd, &gbm_handle) != 0 ||
        gbm_handle == 0) return fail("child-gbm-card-prime-import");
    struct gbm_import_fd_modifier_data data;
    memset(&data, 0, sizeof(data));
    data.width = width;
    data.height = height;
    data.format = DRM_FORMAT_XRGB8888;
    data.num_fds = 1;
    data.fds[0] = prime_fd;
    data.strides[0] = (int)stride;
    data.modifier = DRM_FORMAT_MOD_LINEAR;
    struct gbm_bo *imported = gbm_bo_import(
        gbm, GBM_BO_IMPORT_FD_MODIFIER, &data, 0);
    if (imported == NULL) return fail("child-gbm-import");
    if (gbm_bo_get_plane_count(imported) != 1) return fail("child-gbm-import-planes");
    int reexported = -1;
    if (drmPrimeHandleToFD(card, child_handle, DRM_CLOEXEC | DRM_RDWR, &reexported) != 0 ||
        reexported < 0) return fail("child-gbm-reexport");
    size_t bytes = (size_t)stride * height;
    uint32_t *pixels = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, reexported, 0);
    if (pixels == MAP_FAILED) return fail("child-dmabuf-mmap");
    const uint32_t words = stride / 4u;
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) pixels[y * words + x] = 0x0000ffffu;
    }
    if (munmap(pixels, bytes) != 0) return fail("child-dmabuf-munmap");
    printf("PRIME_CHILD_IMPORT_OK handle=%u reexport=1 color=00ffff\n",
        child_handle);
    fflush(stdout);
    close(reexported);
    gbm_bo_destroy(imported);
    gbm_device_destroy(gbm);
    close(card);
    return 0;
}

int main(int argc, char **argv)
{
    if (setenv("GBM_ALWAYS_SOFTWARE", "1", 1) != 0) return fail("set-gbm-software");
    if (argc == 6 && strcmp(argv[1], "--child") == 0) {
        return import_and_write_child(
            -1,
            atoi(argv[2]),
            (uint32_t)strtoul(argv[3], NULL, 10),
            (uint32_t)strtoul(argv[4], NULL, 10),
            (uint32_t)strtoul(argv[5], NULL, 10));
    }
    if (probe_udmabuf() != 0) return 1;
    int card = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (card < 0) return fail("open-card");
    uint64_t prime_cap = 0, modifier_cap = 0;
    if (drmGetCap(card, DRM_CAP_PRIME, &prime_cap) != 0 ||
        prime_cap != (DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT)) return fail("cap-prime");
    if (drmGetCap(card, DRM_CAP_ADDFB2_MODIFIERS, &modifier_cap) != 0 || modifier_cap != 1) {
        return fail("cap-modifiers");
    }
    drmModeRes *resources = drmModeGetResources(card);
    drmModeConnector *connector = resources != NULL ? connected_connector(card, resources) : NULL;
    if (resources == NULL || connector == NULL || resources->count_crtcs != 1) return fail("resources");
    drmModeModeInfo mode = connector->modes[0];
    const uint64_t modifiers[] = { DRM_FORMAT_MOD_LINEAR };
    struct gbm_device *gbm = gbm_create_device(card);
    if (gbm == NULL) return fail("gbm-device");
    struct gbm_bo *bo = gbm_bo_create_with_modifiers2(
        gbm, mode.hdisplay, mode.vdisplay, DRM_FORMAT_XRGB8888,
        modifiers, 1, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    if (bo == NULL) return fail("gbm-create-modifier");
    if (gbm_bo_get_plane_count(bo) != 1) return fail("gbm-plane-count");
    const uint32_t stride = gbm_bo_get_stride(bo);
    int prime_fd = gbm_bo_get_fd(bo);
    if (prime_fd < 0) return fail("gbm-export");
    uint32_t first_handle = 0;
    if (drmPrimeFDToHandle(card, prime_fd, &first_handle) != 0 || first_handle == 0) {
        return fail("gbm-fd-prime-import");
    }
    if ((fcntl(prime_fd, F_GETFL) & O_ACCMODE) != O_RDWR) {
        return fail("parent-dmabuf-access");
    }
    void *parent_probe = mmap(NULL, (size_t)stride * mode.vdisplay,
        PROT_READ | PROT_WRITE, MAP_SHARED, prime_fd, 0);
    if (parent_probe == MAP_FAILED) return fail("parent-dmabuf-map-probe");
    if (munmap(parent_probe, (size_t)stride * mode.vdisplay) != 0) {
        return fail("parent-dmabuf-unmap-probe");
    }
    printf("PRIME_GBM_EXPORT_OK handle=%u fd=%d stride=%u modifier=linear\n",
        first_handle, prime_fd, stride);
    fflush(stdout);

    pid_t child = fork();
    if (child < 0) return fail("fork");
    if (child == 0) {
        char fd_arg[16], width_arg[16], height_arg[16], stride_arg[16];
        if (fcntl(prime_fd, F_SETFD, 0) != 0) _exit(fail("child-clear-cloexec"));
        snprintf(fd_arg, sizeof(fd_arg), "%d", prime_fd);
        snprintf(width_arg, sizeof(width_arg), "%u", mode.hdisplay);
        snprintf(height_arg, sizeof(height_arg), "%u", mode.vdisplay);
        snprintf(stride_arg, sizeof(stride_arg), "%u", stride);
        execl("/cmd/lpr_drm_prime_smoke.elf", "lpr_drm_prime_smoke.elf", "--child",
            fd_arg, width_arg, height_arg, stride_arg, (char *)NULL);
        _exit(fail("child-exec"));
    }
    int child_status = 0;
    if (waitpid(child, &child_status, 0) != child || !WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) {
        return fail("child-status");
    }

    if (drmCloseBufferHandle(card, first_handle) != 0) return fail("close-first-handle");
    gbm_bo_destroy(bo);
    bo = NULL;
    struct gbm_import_fd_modifier_data data;
    memset(&data, 0, sizeof(data));
    data.width = mode.hdisplay;
    data.height = mode.vdisplay;
    data.format = DRM_FORMAT_XRGB8888;
    data.num_fds = 1;
    data.fds[0] = prime_fd;
    data.strides[0] = (int)stride;
    data.modifier = DRM_FORMAT_MOD_LINEAR;
    struct gbm_bo *display_bo = gbm_bo_import(
        gbm, GBM_BO_IMPORT_FD_MODIFIER, &data, GBM_BO_USE_SCANOUT);
    if (display_bo == NULL || gbm_bo_get_plane_count(display_bo) != 1) {
        return fail("parent-gbm-reimport-after-handle-close");
    }
    uint32_t display_handle = 0;
    if (drmPrimeFDToHandle(card, prime_fd, &display_handle) != 0 || display_handle == 0) {
        return fail("parent-prime-reimport-after-handle-close");
    }
    close(prime_fd);

    int error_source_fd = gbm_bo_get_fd(display_bo);
    int closed_fd = error_source_fd >= 0 ? dup(error_source_fd) : -1;
    if (closed_fd < 0) return fail("dup-error-fd");
    close(error_source_fd);
    close(closed_fd);
    uint32_t rejected_handle = 0;
    errno = 0;
    if (drmPrimeFDToHandle(card, closed_fd, &rejected_handle) == 0 || errno != EBADF) {
        return fail("closed-fd-rejected");
    }

    uint32_t handles[4] = { display_handle, 0, 0, 0 };
    uint32_t pitches[4] = { stride, 0, 0, 0 };
    uint32_t offsets[4] = { 0, 0, 0, 0 };
    uint64_t fb_modifiers[4] = { DRM_FORMAT_MOD_LINEAR, 0, 0, 0 };
    uint32_t fb_id = 0;
    if (drmModeAddFB2WithModifiers(
        card, mode.hdisplay, mode.vdisplay, DRM_FORMAT_XRGB8888,
        handles, pitches, offsets, fb_modifiers, &fb_id, DRM_MODE_FB_MODIFIERS) != 0) {
        return fail("addfb2-modifier");
    }
    if (drmModeSetCrtc(
        card, resources->crtcs[0], fb_id, 0, 0, &connector->connector_id, 1, &mode) != 0) {
        return fail("setcrtc");
    }
    printf("PRIME_CROSS_PROCESS_DISPLAY_OK imported_handle=%u fb=%u modifier=linear color=00ffff\n",
        display_handle, fb_id);
    printf("PRIME_SMOKE_DONE\n");
    fflush(stdout);
    sleep(1);
    close(card);
    return 0;
}
