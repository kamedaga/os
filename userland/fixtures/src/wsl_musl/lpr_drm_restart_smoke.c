#define _GNU_SOURCE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <errno.h>
#include <fcntl.h>
#include <gbm.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <drm_fourcc.h>
#include <drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

static int fail(unsigned iteration, const char *operation)
{
    fprintf(stderr, "DRM_RESTART_FAIL iter=%u op=%s errno=%d egl=0x%04x\n",
        iteration, operation, errno, (unsigned)eglGetError());
    return 1;
}

static drmModeConnector *find_connector(
    int fd,
    drmModeRes *resources,
    drmModeModeInfo *mode,
    uint32_t *crtc_id)
{
    for (int i = 0; i < resources->count_connectors; i++) {
        drmModeConnector *connector = drmModeGetConnector(fd, resources->connectors[i]);
        if (connector == NULL) continue;
        if (connector->connection == DRM_MODE_CONNECTED && connector->count_modes > 0) {
            *mode = connector->modes[0];
            if (connector->encoder_id != 0) {
                drmModeEncoder *encoder = drmModeGetEncoder(fd, connector->encoder_id);
                if (encoder != NULL) {
                    *crtc_id = encoder->crtc_id != 0 ? encoder->crtc_id : resources->crtcs[0];
                    drmModeFreeEncoder(encoder);
                }
            }
            if (*crtc_id == 0 && resources->count_crtcs > 0) {
                *crtc_id = resources->crtcs[0];
            }
            return connector;
        }
        drmModeFreeConnector(connector);
    }
    return NULL;
}

static int run_iteration(unsigned iteration)
{
    const int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) return fail(iteration, "open");
    struct gbm_device *gbm = gbm_create_device(fd);
    if (gbm == NULL) return fail(iteration, "gbm_create_device");
    EGLDisplay display = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm, NULL);
    if (display == EGL_NO_DISPLAY) return fail(iteration, "eglGetPlatformDisplay");
    EGLint egl_major = 0;
    EGLint egl_minor = 0;
    if (!eglInitialize(display, &egl_major, &egl_minor)) return fail(iteration, "eglInitialize");
    static const EGLint context_attributes[] = {
        EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR,
        EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR,
        EGL_CONTEXT_MAJOR_VERSION_KHR,
        3,
        EGL_CONTEXT_MINOR_VERSION_KHR,
        1,
        EGL_NONE,
    };
    if (!eglBindAPI(EGL_OPENGL_API)) return fail(iteration, "eglBindAPI");
    EGLContext context = eglCreateContext(
        display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, context_attributes);
    if (context == EGL_NO_CONTEXT) return fail(iteration, "eglCreateContext");
    if (!eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context)) {
        return fail(iteration, "eglMakeCurrent-context");
    }
    const GLubyte *renderer = glGetString(GL_RENDERER);
    if (renderer == NULL) return fail(iteration, "glGetString-renderer");
    printf("DRM_RESTART_EGL_CONTEXT iter=%u renderer=%s\n", iteration, renderer);
    fflush(stdout);

    errno = 0;
    if (drmSetMaster(fd) != 0) return fail(iteration, "drmSetMaster");
    drmModeRes *resources = drmModeGetResources(fd);
    if (resources == NULL) return fail(iteration, "drmModeGetResources");
    drmModeModeInfo mode;
    memset(&mode, 0, sizeof(mode));
    uint32_t crtc_id = 0;
    drmModeConnector *connector = find_connector(fd, resources, &mode, &crtc_id);
    if (connector == NULL || crtc_id == 0) return fail(iteration, "find_connector");

    struct drm_mode_create_dumb create;
    memset(&create, 0, sizeof(create));
    create.width = mode.hdisplay;
    create.height = mode.vdisplay;
    create.bpp = 32;
    if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) != 0) {
        return fail(iteration, "CREATE_DUMB");
    }
    struct drm_mode_map_dumb map;
    memset(&map, 0, sizeof(map));
    map.handle = create.handle;
    if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map) != 0) {
        return fail(iteration, "MAP_DUMB");
    }
    uint32_t *pixels = mmap(NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, map.offset);
    if (pixels == MAP_FAILED) return fail(iteration, "mmap");
    const uint32_t color = 0xff000000u |
        (((iteration * 11u) & 0xffu) << 16u) |
        (((iteration * 7u) & 0xffu) << 8u) |
        ((iteration * 3u) & 0xffu);
    for (uint64_t y = 0; y < create.height; y++) {
        uint32_t *row = (uint32_t *)((uint8_t *)pixels + y * create.pitch);
        for (uint64_t x = 0; x < create.width; x++) row[x] = color;
    }

    const uint32_t handles[4] = { create.handle, 0, 0, 0 };
    const uint32_t pitches[4] = { create.pitch, 0, 0, 0 };
    const uint32_t offsets[4] = { 0, 0, 0, 0 };
    uint32_t fb_id = 0;
    if (drmModeAddFB2(fd, create.width, create.height, DRM_FORMAT_XRGB8888,
            handles, pitches, offsets, &fb_id, 0) != 0) {
        return fail(iteration, "drmModeAddFB2");
    }
    uint32_t connector_id = connector->connector_id;
    if (drmModeSetCrtc(fd, crtc_id, fb_id, 0, 0, &connector_id, 1, &mode) != 0) {
        return fail(iteration, "drmModeSetCrtc");
    }

    drmModeFreeConnector(connector);
    drmModeFreeResources(resources);
    if (munmap(pixels, create.size) != 0) return fail(iteration, "munmap");
    printf("DRM_RESTART_EGL_CLEANUP_BEGIN iter=%u stage=make-current-null\n", iteration);
    fflush(stdout);
    if (!eglMakeCurrent(
            display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT)) {
        return fail(iteration, "eglMakeCurrent-null");
    }
    printf("DRM_RESTART_EGL_CLEANUP_DONE iter=%u stage=make-current-null\n", iteration);
    printf("DRM_RESTART_EGL_CLEANUP_BEGIN iter=%u stage=terminate\n", iteration);
    fflush(stdout);
    if (!eglTerminate(display)) return fail(iteration, "eglTerminate");
    printf("DRM_RESTART_EGL_CLEANUP_DONE iter=%u stage=terminate\n", iteration);
    printf("DRM_RESTART_EGL_CLEANUP_BEGIN iter=%u stage=gbm-destroy\n", iteration);
    fflush(stdout);
    gbm_device_destroy(gbm);
    printf("DRM_RESTART_EGL_CLEANUP_DONE iter=%u stage=gbm-destroy\n", iteration);
    printf("DRM_RESTART_ITER pass=%u egl=%d.%d fb=%u color=%06x\n",
        iteration, egl_major, egl_minor, fb_id, color & 0xffffffu);
    fflush(stdout);

    /* card0 and KMS objects deliberately remain open until LPR process teardown. */
    return 0;
}

int main(int argc, char **argv)
{
    unsigned iterations = 20;
    if (argc == 2) {
        const unsigned long parsed = strtoul(argv[1], NULL, 10);
        if (parsed >= 1 && parsed <= 1000) iterations = (unsigned)parsed;
    }
    for (unsigned iteration = 1; iteration <= iterations; iteration++) {
        const pid_t child = fork();
        if (child < 0) return fail(iteration, "fork");
        if (child == 0) _exit(run_iteration(iteration));
        int status = 0;
        if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            fprintf(stderr, "DRM_RESTART_LOOP_FAIL iter=%u status=%d\n", iteration, status);
            return 1;
        }
    }
    return 0;
}
