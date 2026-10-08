/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <GL/gl.h>
#include <GL/glx.h>
#include <GL/glxext.h>
#include <EGL/egl.h>
#include <X11/xshmfence.h>
#include <xcb/xcb.h>
#include <xcb/dri3.h>
#include <gbm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static uint64_t now_us(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) { perror("clock_gettime"); exit(1); }
    return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
static int compare(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}
static void report(const char *name, uint64_t *samples, unsigned count)
{
    if (!count) { printf("%s samples=0\n", name); return; }
    uint64_t total = 0;
    for (unsigned i = 0; i < count; ++i) total += samples[i];
    qsort(samples, count, sizeof(*samples), compare);
    printf("%s samples=%u mean_us=%llu p50_us=%llu p95_us=%llu max_us=%llu\n", name, count,
        (unsigned long long)(total / count), (unsigned long long)samples[count / 2],
        (unsigned long long)samples[(count - 1) * 95 / 100],
        (unsigned long long)samples[count - 1]);
}
static int framebuffer(void)
{
    GLint viewport[4] = {0}, draw = 0, read_buffer = 0;
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetIntegerv(GL_DRAW_BUFFER, &draw);
    glGetIntegerv(GL_READ_BUFFER, &read_buffer);
    printf("FRAMEBUFFER viewport=%d,%d,%d,%d draw=0x%x read=0x%x error=0x%x\n",
        viewport[0], viewport[1], viewport[2], viewport[3], draw, read_buffer, glGetError());
    glViewport(0, 0, 320, 240);
    glClearColor(0.25f, 0.2f, 0.3f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    unsigned char pixel[4] = {0};
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    GLenum error = glGetError();
    printf("FRAMEBUFFER_PIXEL rgba=%u,%u,%u,%u error=0x%x\n",
        pixel[0], pixel[1], pixel[2], pixel[3], error);
    /* Fast no-op GL calls are not a rendering result. Require an actual
     * drawable and known pixel before collecting presentation timings. */
    if (error || pixel[0] < 63 || pixel[0] > 65 || pixel[1] < 50 || pixel[1] > 52 ||
        pixel[2] < 76 || pixel[2] > 78 || pixel[3] != 255) {
        puts("FRAMEBUFFER_VALIDATION_FAILED timings_not_valid=1");
        return 1;
    }
    return 0;
}
static int input(unsigned seconds)
{
    struct pollfd fds[64]; unsigned devices = 0;
    for (unsigned i = 0; i < 64; ++i) {
        char path[64], name[128] = {0};
        snprintf(path, sizeof(path), "/dev/input/event%u", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        int clock_id = CLOCK_MONOTONIC;
        if (ioctl(fd, EVIOCSCLOCKID, &clock_id)) { perror("EVIOCSCLOCKID"); close(fd); continue; }
        (void)ioctl(fd, EVIOCGNAME(sizeof(name)), name);
        printf("INPUT_WATCH node=%s name=%s\n", path, name);
        fds[devices++] = (struct pollfd){.fd = fd, .events = POLLIN};
    }
    uint64_t samples[8192]; unsigned count = 0, dropped = 0, future = 0;
    const uint64_t until = now_us() + (uint64_t)seconds * 1000000;
    printf("INPUT_WATCH_READY duration_seconds=%u no_grab=1\n", seconds);
    fflush(stdout);
    while (devices && now_us() < until) {
        int result = poll(fds, devices, 100);
        if (result < 0) { if (errno == EINTR) continue; perror("poll"); break; }
        for (unsigned i = 0; i < devices; ++i) {
            if (!(fds[i].revents & POLLIN)) continue;
            struct input_event events[64]; ssize_t bytes;
            /* A continuously active device must not hide the duration limit
             * or starve the next device behind an unbounded drain loop. */
            for (unsigned batch = 0; batch < 32 && now_us() < until; ++batch) {
                bytes = read(fds[i].fd, events, sizeof(events));
                if (bytes <= 0) break;
                uint64_t received = now_us();
                for (unsigned j = 0; j < (unsigned)bytes / sizeof(*events); ++j) {
                    if (events[j].type != EV_SYN) continue;
                    if (events[j].code == SYN_DROPPED) { ++dropped; continue; }
                    if (events[j].code != SYN_REPORT) continue;
                    uint64_t stamp = (uint64_t)events[j].time.tv_sec * 1000000 + events[j].time.tv_usec;
                    if (stamp > received) { ++future; continue; }
                    if (count < sizeof(samples) / sizeof(*samples)) samples[count++] = received - stamp;
                }
            }
        }
    }
    report("EVDEV_DELIVERY", samples, count);
    printf("INPUT_END syn_dropped=%u future_timestamp=%u\n", dropped, future);
    for (unsigned i = 0; i < devices; ++i) close(fds[i].fd);
    return 0;
}
static int display(int draw)
{
    Display *d = XOpenDisplay(NULL);
    if (!d) { puts("XOpenDisplay failed"); return 1; }
    char selection[64];
    snprintf(selection, sizeof(selection), "_NET_WM_CM_S%d", DefaultScreen(d));
    Window owner = XGetSelectionOwner(d, XInternAtom(d, selection, False));
    printf("COMPOSITOR_OWNER window=0x%lx\n", owner);
    if (owner) {
        char *name = NULL;
        if (XFetchName(d, owner, &name) && name) {
            printf("COMPOSITOR_NAME=%s\n", name); XFree(name);
        }
        Atom type; int format; unsigned long count, remaining;
        unsigned char *data = NULL;
        if (XGetWindowProperty(d, owner, XInternAtom(d, "_NET_WM_PID", False),
            0, 1, False, XA_CARDINAL, &type, &format, &count, &remaining,
            &data) == Success && data && format == 32 && count) {
            printf("COMPOSITOR_PID=%lu\n", *(unsigned long *)data);
        }
        if (data) XFree(data);
    }
    uint64_t samples[120];
    for (unsigned i = 0; i < 120; ++i) {
        Window focus; int revert;
        uint64_t begin = now_us();
        XGetInputFocus(d, &focus, &revert);
        samples[i] = now_us() - begin;
    }
    report("X11_ROUNDTRIP", samples, 120);
    if (!draw) { XCloseDisplay(d); return 0; }
    int attrs[] = {GLX_X_RENDERABLE, True, GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT,
        GLX_RENDER_TYPE, GLX_RGBA_BIT, GLX_DOUBLEBUFFER, True, GLX_RED_SIZE, 8,
        GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, None};
    int config_count = 0;
    GLXFBConfig *configs = glXChooseFBConfig(d, DefaultScreen(d), attrs, &config_count);
    if (!configs || !config_count) { puts("glXChooseFBConfig failed"); XCloseDisplay(d); return 1; }
    XVisualInfo *visual = glXGetVisualFromFBConfig(d, configs[0]);
    if (!visual) { puts("glXGetVisualFromFBConfig failed"); XFree(configs); XCloseDisplay(d); return 1; }
    XSetWindowAttributes attr = {.colormap = XCreateColormap(d,
        RootWindow(d, visual->screen), visual->visual, AllocNone)};
    Window w = XCreateWindow(d, RootWindow(d, visual->screen), 0, 0, 320, 240, 0,
        visual->depth, InputOutput, visual->visual, CWColormap, &attr);
    XStoreName(d, w, "PachaOS latency measurement");
    XMapWindow(d, w); XSync(d, False);
    /* XSync waits for Xorg, not for xfwm4 to process MapRequest. Swapping an
     * unmapped window is discarded by Mesa and cannot measure presentation. */
    const uint64_t mapped_until = now_us() + 3000000;
    XWindowAttributes window_attributes;
    for (;;) {
        if (!XGetWindowAttributes(d, w, &window_attributes)) return 1;
        if (window_attributes.map_state == IsViewable) break;
        if (now_us() >= mapped_until) { puts("GLX_WINDOW_MAP_TIMEOUT"); return 1; }
        const struct timespec delay = {.tv_nsec = 10000000};
        nanosleep(&delay, NULL);
    }
    puts("GLX_WINDOW_VIEWABLE");
    /* Use an explicit GLX drawable so completion accounting cannot depend on
     * legacy X Window-to-GLX drawable lookup in the test itself. */
    GLXWindow drawable = glXCreateWindow(d, configs[0], w, NULL);
    GLXContext context = glXCreateNewContext(d, configs[0], GLX_RGBA_TYPE, NULL, True);
    XFree(configs);
    if (!drawable || !context || !glXMakeContextCurrent(d, drawable, drawable, context)) {
        puts("GLX context failed"); return 1;
    }
    const char *renderer = (const char *)glGetString(GL_RENDERER);
    printf("GL_RENDERER=%s\n", renderer ? renderer : "NULL");
    GLboolean double_buffer = 0;
    glGetBooleanv(GL_DOUBLEBUFFER, &double_buffer);
    printf("GL_DOUBLEBUFFER=%u GL_ERROR=0x%x\n", double_buffer, glGetError());
    if (!renderer || strstr(renderer, "llvmpipe") || strstr(renderer, "softpipe")) return 1;
    if (framebuffer()) return 1;
    uint64_t swaps[120], finishes[120];
    for (unsigned i = 0; i < 120; ++i) {
        glClearColor((i & 1) ? 0.25f : 0.1f, 0.2f, 0.3f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        uint64_t begin = now_us();
        glFinish(); finishes[i] = now_us() - begin;
        begin = now_us();
        glXSwapBuffers(d, drawable); swaps[i] = now_us() - begin;
    }
    report("GL_FINISH", finishes, 120); report("GLX_SWAP", swaps, 120);
    /* Returning from SwapBuffers only queues a presentation. Measure actual
     * completion separately, otherwise an unflushed queue looks infinitely
     * fast even while the compositor is visibly falling behind. */
    const char *extensions = glXQueryExtensionsString(d, visual->screen);
    if (extensions && strstr(extensions, "GLX_OML_sync_control")) {
        PFNGLXGETSYNCVALUESOMLPROC get = (PFNGLXGETSYNCVALUESOMLPROC)
            glXGetProcAddressARB((const GLubyte *)"glXGetSyncValuesOML");
        PFNGLXWAITFORSBCOMLPROC wait = (PFNGLXWAITFORSBCOMLPROC)
            glXGetProcAddressARB((const GLubyte *)"glXWaitForSbcOML");
        int64_t ust = 0, msc = 0, sbc = 0;
        uint64_t completion[30]; unsigned completed = 0;
        if (get && wait && get(d, drawable, &ust, &msc, &sbc)) {
            printf("GLX_PRESENT_BEGIN ust=%lld msc=%lld sbc=%lld now_us=%llu\n",
                (long long)ust, (long long)msc, (long long)sbc,
                (unsigned long long)now_us());
            const uint64_t submitted_until = now_us() + 2000000;
            while (!sbc && now_us() < submitted_until) {
                XSync(d, False);
                const struct timespec delay = {.tv_nsec = 10000000};
                nanosleep(&delay, NULL);
                if (!get(d, drawable, &ust, &msc, &sbc)) break;
            }
            if (!sbc) {
                puts("GLX_PRESENT_NO_COMPLETED_SWAP_WITHIN_2S");
                goto close_display;
            }
            for (unsigned i = 0; i < 30; ++i) {
                glClearColor(0.2f, 0.3f, (i & 1) ? 0.4f : 0.6f, 1);
                glClear(GL_COLOR_BUFFER_BIT);
                uint64_t begin = now_us();
                glXSwapBuffers(d, drawable);
                if (!wait(d, drawable, sbc + 1, &ust, &msc, &sbc)) break;
                completion[completed++] = now_us() - begin;
            }
            report("GLX_PRESENT_COMPLETE", completion, completed);
        } else puts("GLX_PRESENT_UNAVAILABLE");
    } else puts("GLX_PRESENT_EXTENSION_UNAVAILABLE");
close_display:
    glXMakeContextCurrent(d, None, None, NULL); glXDestroyContext(d, context);
    glXDestroyWindow(d, drawable);
    XDestroyWindow(d, w); XFreeColormap(d, attr.colormap); XFree(visual);
    XCloseDisplay(d);
    return 0;
}
static int reads(void)
{
    int fd = -1;
    for (unsigned i = 0; i < 64; ++i) {
        char path[64]; unsigned long relative = 0;
        snprintf(path, sizeof(path), "/dev/input/event%u", i);
        fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        if (ioctl(fd, EVIOCGBIT(EV_REL, sizeof(relative)), &relative) >= 0 &&
            (relative & (1ul << REL_X)) && (relative & (1ul << REL_Y))) break;
        close(fd); fd = -1;
    }
    if (fd < 0) { puts("No relative pointer device"); return 1; }
    struct input_event events[64];
    uint64_t samples[120]; unsigned empty = 0;
    for (unsigned i = 0; i < 120; ++i) {
        uint64_t begin = now_us();
        ssize_t result = read(fd, events, sizeof(events));
        samples[i] = now_us() - begin;
        if (result < 0 && errno == EAGAIN) ++empty;
    }
    report("EVDEV_READ_RPC", samples, 120);
    printf("EVDEV_READ_EMPTY=%u\n", empty);
    close(fd); return 0;
}
static int egl_display(void)
{
    Display *d = XOpenDisplay(NULL);
    if (!d) return 1;
    EGLDisplay ed = eglGetDisplay((EGLNativeDisplayType)d);
    EGLint major, minor, config_count, visual_id;
    EGLConfig config;
    const EGLint attributes[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
    if (!eglInitialize(ed, &major, &minor) || !eglBindAPI(EGL_OPENGL_API) ||
        !eglChooseConfig(ed, attributes, &config, 1, &config_count) || !config_count ||
        !eglGetConfigAttrib(ed, config, EGL_NATIVE_VISUAL_ID, &visual_id)) {
        printf("EGL_CONFIG_FAILED error=0x%x\n", eglGetError()); return 1;
    }
    XVisualInfo template = {.visualid = (VisualID)visual_id};
    int visuals = 0;
    XVisualInfo *visual = XGetVisualInfo(d, VisualIDMask, &template, &visuals);
    if (!visual || !visuals) return 1;
    XSetWindowAttributes attr = {.colormap = XCreateColormap(d,
        RootWindow(d, visual->screen), visual->visual, AllocNone)};
    Window w = XCreateWindow(d, RootWindow(d, visual->screen), 0, 0, 320, 240, 0,
        visual->depth, InputOutput, visual->visual, CWColormap, &attr);
    XStoreName(d, w, "PachaOS EGL presentation measurement");
    XMapWindow(d, w); XSync(d, False);
    EGLSurface surface = eglCreateWindowSurface(ed, config, (EGLNativeWindowType)w, NULL);
    EGLContext context = eglCreateContext(ed, config, EGL_NO_CONTEXT, NULL);
    if (!eglMakeCurrent(ed, surface, surface, context)) {
        printf("EGL_CONTEXT_FAILED error=0x%x\n", eglGetError()); return 1;
    }
    const char *renderer = (const char *)glGetString(GL_RENDERER);
    printf("EGL_GL_RENDERER=%s\n", renderer ? renderer : "NULL");
    if (!renderer || strstr(renderer, "llvmpipe") || strstr(renderer, "softpipe")) return 1;
    if (framebuffer()) return 1;
    const uint64_t visible_until = now_us() + 3000000;
    XWindowAttributes wa = {0};
    while (XGetWindowAttributes(d, w, &wa) && wa.map_state != IsViewable && now_us() < visible_until) {
        const struct timespec delay = {.tv_nsec = 10000000};
        nanosleep(&delay, NULL);
    }
    if (wa.map_state != IsViewable) return 1;
    if (!eglSwapInterval(ed, 1)) return 1;
    uint64_t swaps[120]; unsigned completed = 0;
    uint64_t start = now_us();
    for (unsigned i = 0; i < 120; ++i) {
        glClearColor((i & 1) ? 0.25f : 0.1f, 0.2f, 0.3f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        uint64_t before = now_us();
        if (!eglSwapBuffers(ed, surface)) {
            printf("EGL_SWAP_FAILED frame=%u error=0x%x\n", i, eglGetError()); break;
        }
        swaps[completed++] = now_us() - before;
    }
    report("EGL_SWAP", swaps, completed);
    printf("EGL_SWAP_TOTAL frames=%u elapsed_us=%llu\n", completed,
        (unsigned long long)(now_us() - start));
    eglMakeCurrent(ed, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(ed, surface); eglDestroyContext(ed, context); eglTerminate(ed);
    XDestroyWindow(d, w); XFreeColormap(d, attr.colormap); XFree(visual); XCloseDisplay(d);
    return completed == 120 ? 0 : 1;
}
static int sequence(void)
{
    const int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("DRM open"); return 1; }
    drmModeRes *resources = drmModeGetResources(fd);
    if (!resources) { perror("DRM resources"); close(fd); return 1; }
    for (int c = 0; c < resources->count_crtcs; ++c) {
        uint32_t id = resources->crtcs[c];
        drmModeCrtc *crtc = drmModeGetCrtc(fd, id);
        if (!crtc || !crtc->mode_valid) { if (crtc) drmModeFreeCrtc(crtc); continue; }
        printf("DRM_CRTC id=%u mode=%s refresh=%u\n", id,
            crtc->mode.name, crtc->mode.vrefresh);
        printf("DRM_GEOMETRY x=%u y=%u w=%u h=%u clock=%u htotal=%u vtotal=%u\n",
            crtc->x, crtc->y, crtc->mode.hdisplay, crtc->mode.vdisplay,
            crtc->mode.clock, crtc->mode.htotal, crtc->mode.vtotal);
        drmModeFreeCrtc(crtc);
        for (unsigned i = 0; i < 6; ++i) {
            uint64_t count = 0, ns = 0;
            uint64_t before = now_us();
            int result = drmCrtcGetSequence(fd, id, &count, &ns);
            printf("DRM_SEQUENCE result=%d errno=%d sequence=%llu ns=%llu now_us=%llu call_us=%llu\n",
                result, result ? errno : 0, (unsigned long long)count,
                (unsigned long long)ns, (unsigned long long)before,
                (unsigned long long)(now_us() - before));
            struct drm_crtc_get_sequence raw = {.crtc_id = id};
            result = syscall(SYS_ioctl, fd, DRM_IOCTL_CRTC_GET_SEQUENCE, &raw);
            printf("DRM_RAW_SEQUENCE ioctl=0x%lx result=%d errno=%d sequence=%llu ns=%lld\n",
                (unsigned long)DRM_IOCTL_CRTC_GET_SEQUENCE, result, result ? errno : 0,
                (unsigned long long)raw.sequence, (long long)raw.sequence_ns);
            drmVBlank blank = {.request = {.type = DRM_VBLANK_RELATIVE}};
            result = drmWaitVBlank(fd, &blank);
            printf("DRM_VBLANK result=%d errno=%d sequence=%u sec=%ld usec=%ld\n",
                result, result ? errno : 0, blank.reply.sequence,
                blank.reply.tval_sec, blank.reply.tval_usec);
            const struct timespec delay = {.tv_nsec = 200000000};
            nanosleep(&delay, NULL);
        }
    }
    drmModeFreeResources(resources); close(fd); return 0;
}
static int shared_fence(void)
{
    errno = 0;
    int fd = xshmfence_alloc_shm();
    printf("SHMFENCE_ALLOC fd=%d errno=%d\n", fd, fd < 0 ? errno : 0);
    if (fd < 0) return 1;
    errno = 0;
    struct xshmfence *fence = xshmfence_map_shm(fd);
    printf("SHMFENCE_MAP success=%u errno=%d\n", fence != NULL, fence ? 0 : errno);
    if (!fence) { close(fd); return 1; }
    xshmfence_reset(fence); printf("SHMFENCE_RESET query=%d\n", xshmfence_query(fence));
    xshmfence_trigger(fence); printf("SHMFENCE_TRIGGER query=%d\n", xshmfence_query(fence));
    int screen_number;
    xcb_connection_t *c = xcb_connect(NULL, &screen_number);
    xcb_screen_iterator_t screen = xcb_setup_roots_iterator(xcb_get_setup(c));
    for (int i = 0; i < screen_number; ++i) xcb_screen_next(&screen);
    xcb_generic_error_t *error = xcb_request_check(c, xcb_dri3_fence_from_fd_checked(
        c, screen.data->root, xcb_generate_id(c), false, fd));
    printf("SHMFENCE_XORG_IMPORT error=%u connection_error=%d\n",
        error ? error->error_code : 0, xcb_connection_has_error(c));
    int failed = error != NULL || xcb_connection_has_error(c);
    free(error); xcb_disconnect(c); xshmfence_unmap_shm(fence);
    return failed;
}
static int shared_pixmap(void)
{
    int drm = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (drm < 0) { perror("render open"); return 1; }
    struct gbm_device *device = gbm_create_device(drm);
    if (!device) { perror("gbm device"); close(drm); return 1; }
    struct gbm_bo *bo = gbm_bo_create(device, 320, 240, GBM_FORMAT_XRGB8888,
        GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
    printf("GBM_BO_CREATED success=%u errno=%d\n", bo != NULL, bo ? 0 : errno);
    if (!bo) { gbm_device_destroy(device); close(drm); return 1; }
    int fd = gbm_bo_get_fd(bo);
    uint32_t stride = gbm_bo_get_stride(bo);
    printf("GBM_BO_EXPORT fd=%d errno=%d stride=%u modifier=0x%llx\n",
        fd, fd < 0 ? errno : 0, stride, (unsigned long long)gbm_bo_get_modifier(bo));
    int failed = fd < 0;
    if (!failed) {
        int screen_number;
        xcb_connection_t *c = xcb_connect(NULL, &screen_number);
        xcb_screen_iterator_t screen = xcb_setup_roots_iterator(xcb_get_setup(c));
        for (int i = 0; i < screen_number; ++i) xcb_screen_next(&screen);
        xcb_pixmap_t pixmap = xcb_generate_id(c);
        xcb_generic_error_t *error = xcb_request_check(c, xcb_dri3_pixmap_from_buffer_checked(
            c, pixmap, screen.data->root, stride * 240, 320, 240, stride, 24, 32, fd));
        printf("GBM_XORG_PIXMAP_IMPORT error=%u connection_error=%d\n",
            error ? error->error_code : 0, xcb_connection_has_error(c));
        failed = error != NULL || xcb_connection_has_error(c);
        if (!failed) xcb_free_pixmap(c, pixmap);
        free(error); xcb_disconnect(c);
    }
    gbm_bo_destroy(bo); gbm_device_destroy(device); close(drm);
    return failed;
}
int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2 || argc > 3) return 2;
    if (!strcmp(argv[1], "input")) {
        unsigned seconds = argc == 3 ? (unsigned)atoi(argv[2]) : 10;
        if (!seconds || seconds > 120) return 2;
        return input(seconds);
    }
    if (!strcmp(argv[1], "x11")) return display(0);
    if (!strcmp(argv[1], "glx")) return display(1);
    if (!strcmp(argv[1], "egl")) return egl_display();
    if (!strcmp(argv[1], "reads")) return reads();
    if (!strcmp(argv[1], "sequence")) return sequence();
    if (!strcmp(argv[1], "fence")) return shared_fence();
    if (!strcmp(argv[1], "pixmap")) return shared_pixmap();
    return 2;
}
