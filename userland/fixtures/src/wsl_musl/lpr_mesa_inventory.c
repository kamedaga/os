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
#include <unistd.h>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

struct inventory {
    int fd;
    struct gbm_device *gbm;
    EGLDisplay display;
    EGLConfig config;
    EGLContext context;
    struct gbm_surface *gbm_surface;
    EGLSurface egl_surface;
    GLuint program;
};

static int stage_fail(char stage, const char *operation)
{
    const EGLint egl_error = eglGetError();
    fprintf(stderr, "MESA_STAGE_%c_FAIL op=%s errno=%d egl=0x%04x\n",
            stage, operation, errno, (unsigned)egl_error);
    fflush(stderr);
    return 1;
}

static int stage_a(struct inventory *state)
{
    printf("MESA_STAGE_A_BEGIN path=/dev/dri/card0\n");
    fflush(stdout);
    state->fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (state->fd < 0) return stage_fail('A', "open-card0");
    drmVersionPtr version = drmGetVersion(state->fd);
    if (version == NULL) return stage_fail('A', "drmGetVersion");
    printf("MESA_DRM_VERSION name=%.*s major=%d minor=%d patch=%d\n",
           version->name_len, version->name, version->version_major,
           version->version_minor, version->version_patchlevel);
    drmFreeVersion(version);
    drmDevicePtr device = NULL;
    errno = 0;
    const int device_status = drmGetDevice2(
        state->fd, DRM_DEVICE_GET_PCI_REVISION, &device);
    if (device_status != 0 || device == NULL || device->bustype != DRM_BUS_PCI ||
        device->deviceinfo.pci == NULL) {
        fprintf(stderr, "MESA_DRM_DEVICE_FAIL status=%d errno=%d device=%p bustype=%d\n",
            device_status, errno, (void *)device, device != NULL ? device->bustype : -1);
        if (device != NULL) drmFreeDevice(&device);
        return stage_fail('A', "drmGetDevice2");
    }
    printf("MESA_DRM_DEVICE_OK domain=%04x bus=%02x dev=%02x func=%u vendor=%04x device=%04x\n",
        device->businfo.pci->domain, device->businfo.pci->bus,
        device->businfo.pci->dev, device->businfo.pci->func,
        device->deviceinfo.pci->vendor_id, device->deviceinfo.pci->device_id);
    drmFreeDevice(&device);
    printf("MESA_CPU_AFFINITY online=%ld configured=%ld\n",
        sysconf(_SC_NPROCESSORS_ONLN), sysconf(_SC_NPROCESSORS_CONF));
    errno = 0;
    state->gbm = gbm_create_device(state->fd);
    if (state->gbm == NULL) return stage_fail('A', "gbm_create_device");
    const char *backend = gbm_device_get_backend_name(state->gbm);
    printf("MESA_STAGE_A_PASS fd=%d gbm_backend=%s\n", state->fd,
           backend != NULL ? backend : "(null)");
    fflush(stdout);
    return 0;
}

static int choose_config(struct inventory *state)
{
    const EGLint attributes[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 0,
        EGL_NONE,
    };
    EGLConfig configs[64];
    EGLint count = 0;
    if (!eglChooseConfig(state->display, attributes, configs, 64, &count) || count < 1)
        return stage_fail('B', "eglChooseConfig");
    for (EGLint i = 0; i < count; ++i) {
        EGLint visual = 0;
        if (eglGetConfigAttrib(state->display, configs[i], EGL_NATIVE_VISUAL_ID, &visual) &&
            (uint32_t)visual == GBM_FORMAT_XRGB8888) {
            state->config = configs[i];
            printf("MESA_EGL_CONFIG count=%d selected=%d visual=0x%08x\n",
                   count, i, (unsigned)visual);
            return 0;
        }
    }
    errno = 0;
    return stage_fail('B', "choose-XRGB8888-config");
}

static int stage_b(struct inventory *state)
{
    printf("MESA_STAGE_B_BEGIN platform=GBM\n");
    fflush(stdout);
    state->display = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, state->gbm, NULL);
    printf("MESA_EGL_GET_DISPLAY display=%p egl=0x%04x\n",
           (void *)state->display, (unsigned)eglGetError());
    if (state->display == EGL_NO_DISPLAY) return stage_fail('B', "eglGetPlatformDisplay");
    EGLint major = 0, minor = 0;
    if (!eglInitialize(state->display, &major, &minor)) return stage_fail('B', "eglInitialize");
    printf("MESA_EGL_INITIALIZED version=%d.%d vendor=%s client_apis=%s\n",
           major, minor,
           eglQueryString(state->display, EGL_VENDOR),
           eglQueryString(state->display, EGL_CLIENT_APIS));
    if (choose_config(state) != 0) return 1;
    if (!eglBindAPI(EGL_OPENGL_ES_API)) return stage_fail('B', "eglBindAPI");
    const EGLint context_attributes[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    state->context = eglCreateContext(state->display, state->config, EGL_NO_CONTEXT,
                                      context_attributes);
    if (state->context == EGL_NO_CONTEXT) return stage_fail('B', "eglCreateContext");
    printf("MESA_STAGE_B_PASS context=%p egl_vendor=%s egl_version=%s\n",
           (void *)state->context,
           eglQueryString(state->display, EGL_VENDOR),
           eglQueryString(state->display, EGL_VERSION));
    fflush(stdout);
    return 0;
}

static int stage_c(struct inventory *state)
{
    printf("MESA_STAGE_C_BEGIN size=1024x768 format=XRGB8888 usage=SCANOUT|RENDERING\n");
    fflush(stdout);
    errno = 0;
    state->gbm_surface = gbm_surface_create(state->gbm, 1024, 768,
                                            GBM_FORMAT_XRGB8888,
                                            GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    if (state->gbm_surface == NULL) return stage_fail('C', "gbm_surface_create");
    state->egl_surface = eglCreateWindowSurface(state->display, state->config,
                                                (EGLNativeWindowType)state->gbm_surface,
                                                NULL);
    if (state->egl_surface == EGL_NO_SURFACE)
        return stage_fail('C', "eglCreateWindowSurface");
    printf("MESA_STAGE_C_PASS gbm_surface=%p egl_surface=%p\n",
           (void *)state->gbm_surface, (void *)state->egl_surface);
    fflush(stdout);
    return 0;
}

static GLuint compile_shader(GLenum type, const char *source)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE) {
        char log[512];
        GLsizei length = 0;
        glGetShaderInfoLog(shader, sizeof(log), &length, log);
        fprintf(stderr, "MESA_SHADER_FAIL type=0x%x log=%.*s\n",
                (unsigned)type, (int)length, log);
        return 0;
    }
    return shader;
}

static int prepare_triangle(struct inventory *state)
{
    static const char vertex_source[] =
        "attribute vec2 position;\n"
        "attribute vec3 color;\n"
        "varying vec3 v_color;\n"
        "void main() { v_color=color; gl_Position=vec4(position,0.0,1.0); }\n";
    static const char fragment_source[] =
        "precision mediump float;\n"
        "varying vec3 v_color;\n"
        "void main() { gl_FragColor=vec4(v_color,1.0); }\n";
    GLuint vertex = compile_shader(GL_VERTEX_SHADER, vertex_source);
    GLuint fragment = compile_shader(GL_FRAGMENT_SHADER, fragment_source);
    if (vertex == 0 || fragment == 0) return stage_fail('D', "compile-shader");
    state->program = glCreateProgram();
    glAttachShader(state->program, vertex);
    glAttachShader(state->program, fragment);
    glBindAttribLocation(state->program, 0, "position");
    glBindAttribLocation(state->program, 1, "color");
    glLinkProgram(state->program);
    GLint ok = GL_FALSE;
    glGetProgramiv(state->program, GL_LINK_STATUS, &ok);
    if (ok != GL_TRUE) return stage_fail('D', "link-program");
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    return 0;
}

static int draw_triangle(struct inventory *state)
{
    static const GLfloat vertices[] = {
         0.0f,  0.8f, 1.0f, 0.0f, 0.0f,
        -0.8f, -0.8f, 0.0f, 1.0f, 0.0f,
         0.8f, -0.8f, 0.0f, 0.0f, 1.0f,
    };
    glViewport(0, 0, 1024, 768);
    glClearColor(0.02f, 0.02f, 0.02f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(state->program);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(GLfloat), vertices);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(GLfloat), vertices + 2);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glFinish();
    GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        errno = 0;
        fprintf(stderr, "MESA_GL_ERROR op=draw gl=0x%04x\n", (unsigned)error);
        return stage_fail('D', "glDrawArrays");
    }
    return 0;
}

static int stage_d(struct inventory *state)
{
    printf("MESA_STAGE_D_BEGIN draw=triangle\n");
    fflush(stdout);
    if (!eglMakeCurrent(state->display, state->egl_surface, state->egl_surface,
                        state->context))
        return stage_fail('D', "eglMakeCurrent");
    printf("MESA_GL_ID vendor=%s renderer=%s version=%s\n",
           glGetString(GL_VENDOR), glGetString(GL_RENDERER), glGetString(GL_VERSION));
    if (prepare_triangle(state) != 0 || draw_triangle(state) != 0) return 1;
    GLubyte center[4] = {0, 0, 0, 0};
    glReadPixels(512, 384, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, center);
    printf("MESA_GL_CENTER rgba=%u,%u,%u,%u error=0x%04x\n",
           center[0], center[1], center[2], center[3], (unsigned)glGetError());
    if (!eglSwapBuffers(state->display, state->egl_surface))
        return stage_fail('D', "eglSwapBuffers");
    printf("MESA_STAGE_D_PASS swap=1\n");
    fflush(stdout);
    return 0;
}

static drmModeConnector *find_connector(int fd, drmModeRes *resources,
                                        drmModeModeInfo *mode, uint32_t *crtc_id)
{
    for (int i = 0; i < resources->count_connectors; ++i) {
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
            if (*crtc_id == 0 && resources->count_crtcs > 0) *crtc_id = resources->crtcs[0];
            return connector;
        }
        drmModeFreeConnector(connector);
    }
    return NULL;
}

static int framebuffer_for_bo(struct inventory *state, struct gbm_bo *bo, uint32_t *fb_id)
{
    const uint32_t handle = gbm_bo_get_handle(bo).u32;
    const uint32_t stride = gbm_bo_get_stride(bo);
    const uint32_t width = gbm_bo_get_width(bo);
    const uint32_t height = gbm_bo_get_height(bo);
    printf("MESA_GBM_BO handle=%u stride=%u size=%ux%u format=0x%08x\n",
           handle, stride, width, height, gbm_bo_get_format(bo));
    errno = 0;
    int prime_fd = gbm_bo_get_fd(bo);
    printf("MESA_GBM_PRIME fd=%d errno=%d required_for_scanout=no\n", prime_fd, errno);
    if (prime_fd >= 0) close(prime_fd);
    const uint32_t handles[4] = { handle, 0, 0, 0 };
    const uint32_t strides[4] = { stride, 0, 0, 0 };
    const uint32_t offsets[4] = { 0, 0, 0, 0 };
    if (drmModeAddFB2(state->fd, width, height, DRM_FORMAT_XRGB8888,
                      handles, strides, offsets, fb_id, 0) != 0)
        return stage_fail('E', "drmModeAddFB2");
    return 0;
}

static int stage_e(struct inventory *state)
{
    printf("MESA_STAGE_E_BEGIN path=gbm-bo-to-kms\n");
    fflush(stdout);
    errno = 0;
    const int master_status = drmSetMaster(state->fd);
    const int master_errno = errno;
    printf("MESA_DRM_MASTER status=%d errno=%d\n", master_status, master_errno);
    if (master_status != 0) return stage_fail('E', "drmSetMaster");
    drmModeRes *resources = drmModeGetResources(state->fd);
    if (resources == NULL) return stage_fail('E', "drmModeGetResources");
    drmModeModeInfo mode;
    memset(&mode, 0, sizeof(mode));
    uint32_t crtc_id = 0;
    drmModeConnector *connector = find_connector(state->fd, resources, &mode, &crtc_id);
    if (connector == NULL || crtc_id == 0) return stage_fail('E', "find-connector-crtc");
    struct gbm_bo *first = gbm_surface_lock_front_buffer(state->gbm_surface);
    if (first == NULL) return stage_fail('E', "gbm_surface_lock_front_buffer-1");
    uint32_t first_fb = 0;
    if (framebuffer_for_bo(state, first, &first_fb) != 0) return 1;
    uint32_t connector_id = connector->connector_id;
    if (drmModeSetCrtc(state->fd, crtc_id, first_fb, 0, 0, &connector_id, 1, &mode) != 0)
        return stage_fail('E', "drmModeSetCrtc");
    printf("MESA_FRAME1_READY connector=%u crtc=%u fb=%u mode=%ux%u\n",
           connector_id, crtc_id, first_fb, mode.hdisplay, mode.vdisplay);
    fflush(stdout);

    glClearColor(0.0f, 1.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();
    if (!eglSwapBuffers(state->display, state->egl_surface))
        return stage_fail('E', "eglSwapBuffers-2");
    struct gbm_bo *second = gbm_surface_lock_front_buffer(state->gbm_surface);
    if (second == NULL) return stage_fail('E', "gbm_surface_lock_front_buffer-2");
    uint32_t second_fb = 0;
    if (framebuffer_for_bo(state, second, &second_fb) != 0) return 1;
    errno = 0;
    if (drmModePageFlip(state->fd, crtc_id, second_fb, 0, NULL) != 0)
        return stage_fail('E', "drmModePageFlip-sync");
    printf("MESA_FRAME2_READY color=00ffff fb=%u\n", second_fb);
    printf("MESA_STAGE_E_PASS setcrtc=1 page_flip_flags=0 event_queue=not-used\n");
    fflush(stdout);
    drmModeFreeConnector(connector);
    drmModeFreeResources(resources);
    sleep(1);
    return 0;
}

int main(int argc, char **argv)
{
    const char target = argc == 2 && argv[1][0] >= 'a' && argv[1][0] <= 'e'
        ? argv[1][0] : 'e';
    printf("MESA_INVENTORY_BEGIN target=%c pid=%ld\n", target, (long)getpid());
    fflush(stdout);
    struct inventory state;
    memset(&state, 0, sizeof(state));
    state.fd = -1;
    state.display = EGL_NO_DISPLAY;
    state.context = EGL_NO_CONTEXT;
    state.egl_surface = EGL_NO_SURFACE;
    if (stage_a(&state) != 0) return 10;
    if (target == 'a') return 0;
    if (stage_b(&state) != 0) return 20;
    if (target == 'b') return 0;
    if (stage_c(&state) != 0) return 30;
    if (target == 'c') return 0;
    if (stage_d(&state) != 0) return 40;
    if (target == 'd') return 0;
    if (stage_e(&state) != 0) return 50;
    return 0;
}
