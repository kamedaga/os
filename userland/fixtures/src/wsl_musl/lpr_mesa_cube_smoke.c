#define _GNU_SOURCE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <errno.h>
#include <fcntl.h>
#include <gbm.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

enum { CUBE_BO_MAX = 4 };

struct cube_bo {
    struct gbm_bo *bo;
    uint32_t fb_id;
};

struct cube_state {
    int fd;
    struct gbm_device *gbm;
    struct gbm_surface *gbm_surface;
    EGLDisplay display;
    EGLConfig config;
    EGLContext context;
    EGLSurface surface;
    GLuint program;
    GLint angle_uniform;
    uint32_t crtc_id;
    uint32_t connector_id;
    drmModeModeInfo mode;
    struct cube_bo bos[CUBE_BO_MAX];
    struct gbm_bo *displayed_bo;
    unsigned completed_frame;
    unsigned completed_sequence;
    unsigned completed_sec;
    unsigned completed_usec;
};

static int fail(const char *stage, const char *operation)
{
    fprintf(stderr, "CUBE_FAIL stage=%s op=%s errno=%d egl=0x%04x\n",
        stage, operation, errno, (unsigned)eglGetError());
    fflush(stderr);
    return 1;
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
        fprintf(stderr, "CUBE_SHADER_FAIL type=0x%x log=%.*s\n",
            (unsigned)type, (int)length, log);
        return 0;
    }
    return shader;
}

static int prepare_program(struct cube_state *state)
{
    static const char vertex_source[] =
        "attribute vec3 position;\n"
        "attribute vec3 color;\n"
        "uniform float angle;\n"
        "varying vec3 v_color;\n"
        "void main() {\n"
        "  float c=cos(angle), s=sin(angle);\n"
        "  vec3 p=vec3(c*position.x+s*position.z, position.y, -s*position.x+c*position.z);\n"
        "  v_color=color;\n"
        "  gl_Position=vec4(1.35*p.x,1.35*p.y,-p.z,4.0-p.z);\n"
        "}\n";
    static const char fragment_source[] =
        "precision mediump float;\n"
        "varying vec3 v_color;\n"
        "void main() { gl_FragColor=vec4(v_color,1.0); }\n";
    const GLuint vertex = compile_shader(GL_VERTEX_SHADER, vertex_source);
    const GLuint fragment = compile_shader(GL_FRAGMENT_SHADER, fragment_source);
    if (vertex == 0 || fragment == 0) return fail("gl-init", "compile-shader");
    state->program = glCreateProgram();
    glAttachShader(state->program, vertex);
    glAttachShader(state->program, fragment);
    glBindAttribLocation(state->program, 0, "position");
    glBindAttribLocation(state->program, 1, "color");
    glLinkProgram(state->program);
    GLint ok = GL_FALSE;
    glGetProgramiv(state->program, GL_LINK_STATUS, &ok);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    if (ok != GL_TRUE) return fail("gl-init", "link-program");
    state->angle_uniform = glGetUniformLocation(state->program, "angle");
    return state->angle_uniform < 0 ? fail("gl-init", "angle-uniform") : 0;
}

static int choose_config(struct cube_state *state)
{
    const EGLint attributes[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 0,
        EGL_DEPTH_SIZE, 16,
        EGL_NONE,
    };
    EGLConfig configs[64];
    EGLint count = 0;
    if (!eglChooseConfig(state->display, attributes, configs, 64, &count))
        return fail("egl-init", "eglChooseConfig");
    for (EGLint i = 0; i < count; i++) {
        EGLint visual = 0;
        if (eglGetConfigAttrib(state->display, configs[i], EGL_NATIVE_VISUAL_ID, &visual) &&
            (uint32_t)visual == GBM_FORMAT_XRGB8888) {
            state->config = configs[i];
            return 0;
        }
    }
    errno = 0;
    return fail("egl-init", "XRGB8888-config");
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
            if (*crtc_id == 0 && resources->count_crtcs > 0) *crtc_id = resources->crtcs[0];
            return connector;
        }
        drmModeFreeConnector(connector);
    }
    return NULL;
}

static int init_display(struct cube_state *state)
{
    state->fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (state->fd < 0) return fail("open", "card0");
    state->gbm = gbm_create_device(state->fd);
    if (state->gbm == NULL) return fail("gbm-init", "gbm_create_device");
    state->display = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, state->gbm, NULL);
    if (state->display == EGL_NO_DISPLAY) return fail("egl-init", "eglGetPlatformDisplay");
    EGLint major = 0, minor = 0;
    if (!eglInitialize(state->display, &major, &minor)) return fail("egl-init", "eglInitialize");
    if (choose_config(state) != 0 || !eglBindAPI(EGL_OPENGL_ES_API))
        return fail("egl-init", "eglBindAPI");
    const EGLint context_attributes[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    state->context = eglCreateContext(
        state->display, state->config, EGL_NO_CONTEXT, context_attributes);
    if (state->context == EGL_NO_CONTEXT) return fail("egl-init", "eglCreateContext");
    state->gbm_surface = gbm_surface_create(
        state->gbm, 1024, 768, GBM_FORMAT_XRGB8888,
        GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    if (state->gbm_surface == NULL) return fail("gbm-init", "gbm_surface_create");
    state->surface = eglCreateWindowSurface(
        state->display, state->config, (EGLNativeWindowType)state->gbm_surface, NULL);
    if (state->surface == EGL_NO_SURFACE) return fail("egl-init", "eglCreateWindowSurface");
    if (!eglMakeCurrent(state->display, state->surface, state->surface, state->context))
        return fail("egl-init", "eglMakeCurrent");
    if (prepare_program(state) != 0) return 1;

    if (drmSetMaster(state->fd) != 0) return fail("kms-init", "drmSetMaster");
    drmModeRes *resources = drmModeGetResources(state->fd);
    if (resources == NULL) return fail("kms-init", "drmModeGetResources");
    drmModeConnector *connector = find_connector(
        state->fd, resources, &state->mode, &state->crtc_id);
    if (connector == NULL || state->crtc_id == 0) return fail("kms-init", "find-connector");
    state->connector_id = connector->connector_id;
    drmModeFreeConnector(connector);
    drmModeFreeResources(resources);
    printf("CUBE_INIT renderer=%s mode=%ux%u crtc=%u connector=%u egl=%d.%d\n",
        glGetString(GL_RENDERER), state->mode.hdisplay, state->mode.vdisplay,
        state->crtc_id, state->connector_id, major, minor);
    fflush(stdout);
    return 0;
}

static void draw_cube(struct cube_state *state, float angle)
{
#define V(x,y,z,r,g,b) x,y,z,r,g,b
    static const GLfloat vertices[] = {
        V(-1,-1, 1, 1,0,0), V( 1,-1, 1, 1,0,0), V( 1, 1, 1, 1,0,0),
        V(-1,-1, 1, 1,0,0), V( 1, 1, 1, 1,0,0), V(-1, 1, 1, 1,0,0),
        V(-1,-1,-1, 1,1,0), V(-1, 1,-1, 1,1,0), V( 1, 1,-1, 1,1,0),
        V(-1,-1,-1, 1,1,0), V( 1, 1,-1, 1,1,0), V( 1,-1,-1, 1,1,0),
        V(-1,-1,-1, 0,1,0), V(-1,-1, 1, 0,1,0), V(-1, 1, 1, 0,1,0),
        V(-1,-1,-1, 0,1,0), V(-1, 1, 1, 0,1,0), V(-1, 1,-1, 0,1,0),
        V( 1,-1,-1, 1,0,1), V( 1, 1,-1, 1,0,1), V( 1, 1, 1, 1,0,1),
        V( 1,-1,-1, 1,0,1), V( 1, 1, 1, 1,0,1), V( 1,-1, 1, 1,0,1),
        V(-1, 1,-1, 0,1,1), V(-1, 1, 1, 0,1,1), V( 1, 1, 1, 0,1,1),
        V(-1, 1,-1, 0,1,1), V( 1, 1, 1, 0,1,1), V( 1, 1,-1, 0,1,1),
        V(-1,-1,-1, 0,0,1), V( 1,-1,-1, 0,0,1), V( 1,-1, 1, 0,0,1),
        V(-1,-1,-1, 0,0,1), V( 1,-1, 1, 0,0,1), V(-1,-1, 1, 0,0,1),
    };
#undef V
    glViewport(0, 0, state->mode.hdisplay, state->mode.vdisplay);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glClearColor(0.0f, 0.18f, 0.70f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glUseProgram(state->program);
    glUniform1f(state->angle_uniform, angle);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(GLfloat), vertices);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(GLfloat), vertices + 3);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLES, 0, 36);
}

static int framebuffer_for_bo(struct cube_state *state, struct gbm_bo *bo, uint32_t *fb_id)
{
    for (unsigned i = 0; i < CUBE_BO_MAX; i++) {
        if (state->bos[i].bo == bo) {
            *fb_id = state->bos[i].fb_id;
            return 0;
        }
    }
    unsigned free_slot = CUBE_BO_MAX;
    for (unsigned i = 0; i < CUBE_BO_MAX; i++) {
        if (state->bos[i].bo == NULL) { free_slot = i; break; }
    }
    if (free_slot == CUBE_BO_MAX) {
        errno = ENOSPC;
        return fail("kms-fb", "bo-table-full");
    }
    const uint32_t handles[4] = { gbm_bo_get_handle(bo).u32, 0, 0, 0 };
    const uint32_t pitches[4] = { gbm_bo_get_stride(bo), 0, 0, 0 };
    const uint32_t offsets[4] = { 0, 0, 0, 0 };
    uint32_t id = 0;
    if (drmModeAddFB2(state->fd, gbm_bo_get_width(bo), gbm_bo_get_height(bo),
            DRM_FORMAT_XRGB8888, handles, pitches, offsets, &id, 0) != 0)
        return fail("kms-fb", "drmModeAddFB2");
    state->bos[free_slot].bo = bo;
    state->bos[free_slot].fb_id = id;
    *fb_id = id;
    return 0;
}

static void page_flip_handler(
    int fd,
    unsigned sequence,
    unsigned sec,
    unsigned usec,
    void *data)
{
    (void)fd;
    struct cube_state *state = data;
    state->completed_frame++;
    state->completed_sequence = sequence;
    state->completed_sec = sec;
    state->completed_usec = usec;
}

static int wait_for_flip(struct cube_state *state, unsigned frame)
{
    struct pollfd pollfd = { .fd = state->fd, .events = POLLIN };
    const int poll_status = poll(&pollfd, 1, 2000);
    if (poll_status <= 0 || (pollfd.revents & POLLIN) == 0) {
        fprintf(stderr,
            "CUBE_FAIL stage=poll op=page-flip frame=%u status=%d revents=0x%x errno=%d\n",
            frame, poll_status, (unsigned)(uint16_t)pollfd.revents, errno);
        return 1;
    }
    drmEventContext context;
    memset(&context, 0, sizeof(context));
    context.version = DRM_EVENT_CONTEXT_VERSION;
    context.page_flip_handler = page_flip_handler;
    const unsigned before = state->completed_frame;
    if (drmHandleEvent(state->fd, &context) != 0)
        return fail("read-event", "drmHandleEvent");
    if (state->completed_frame != before + 1u || state->completed_usec >= 1000000u) {
        errno = EPROTO;
        return fail("read-event", "event-fields");
    }
    printf("CUBE_EVENT frame=%u sequence=%u timestamp=%u.%06u\n",
        frame, state->completed_sequence, state->completed_sec, state->completed_usec);
    return 0;
}

static int present_initial(struct cube_state *state)
{
    draw_cube(state, -0.20f);
    if (glGetError() != GL_NO_ERROR || !eglSwapBuffers(state->display, state->surface))
        return fail("initial-draw", "eglSwapBuffers");
    struct gbm_bo *bo = gbm_surface_lock_front_buffer(state->gbm_surface);
    if (bo == NULL) return fail("initial-draw", "lock-front-buffer");
    uint32_t fb_id = 0;
    if (framebuffer_for_bo(state, bo, &fb_id) != 0) return 1;
    if (drmModeSetCrtc(state->fd, state->crtc_id, fb_id, 0, 0,
            &state->connector_id, 1, &state->mode) != 0)
        return fail("initial-draw", "drmModeSetCrtc");
    state->displayed_bo = bo;
    return 0;
}

static int draw_frames(struct cube_state *state, unsigned frames)
{
    const float half_pi = 1.57079632679f;
    for (unsigned frame = 1; frame <= frames; frame++) {
        const float angle = frames == 1 ? half_pi :
            ((float)(frame - 1u) * half_pi) / (float)(frames - 1u);
        draw_cube(state, angle);
        if (glGetError() != GL_NO_ERROR || !eglSwapBuffers(state->display, state->surface))
            return fail("draw", "eglSwapBuffers");
        struct gbm_bo *next = gbm_surface_lock_front_buffer(state->gbm_surface);
        if (next == NULL) return fail("draw", "lock-front-buffer");
        uint32_t fb_id = 0;
        if (framebuffer_for_bo(state, next, &fb_id) != 0) return 1;
        printf("CUBE_EVENT_REQUEST frame=%u fb=%u flags=EVENT\n", frame, fb_id);
        fflush(stdout);
        errno = 0;
        if (drmModePageFlip(state->fd, state->crtc_id, fb_id,
                DRM_MODE_PAGE_FLIP_EVENT, state) != 0) {
            fprintf(stderr,
                "CUBE_FAIL stage=submit op=drmModePageFlip-event frame=%u errno=%d\n",
                frame, errno);
            return 1;
        }
        if (wait_for_flip(state, frame) != 0) return 1;
        if (state->displayed_bo != NULL)
            gbm_surface_release_buffer(state->gbm_surface, state->displayed_bo);
        state->displayed_bo = next;
        if (frame == 1 || frame == frames) {
            printf("CUBE_FRAME_READY frame=%u phase=%s\n",
                frame, frame == 1 ? "front-red" : "side-green");
            fflush(stdout);
        }
    }
    return 0;
}

static uint64_t monotonic_ns(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}

int main(int argc, char **argv)
{
    unsigned frames = 8;
    if (argc == 2) {
        const unsigned long parsed = strtoul(argv[1], NULL, 10);
        if (parsed >= 2 && parsed <= 1000) frames = (unsigned)parsed;
    }
    struct cube_state state;
    memset(&state, 0, sizeof(state));
    state.fd = -1;
    state.display = EGL_NO_DISPLAY;
    state.context = EGL_NO_CONTEXT;
    state.surface = EGL_NO_SURFACE;
    printf("CUBE_BEGIN frames=%u\n", frames);
    fflush(stdout);
    if (init_display(&state) != 0 || present_initial(&state) != 0)
        return 1;
    const uint64_t start_ns = monotonic_ns();
    if (draw_frames(&state, frames) != 0) return 1;
    const uint64_t elapsed_ns = monotonic_ns() - start_ns;
    const uint64_t fps_milli = elapsed_ns == 0 ? 0 :
        (uint64_t)frames * 1000000000000ull / elapsed_ns;
    printf("CUBE_ANIMATION_PASS frames=%u events=%u fps=%llu.%03llu first_phase=front-red final_phase=side-green\n",
        frames, state.completed_frame,
        (unsigned long long)(fps_milli / 1000ull),
        (unsigned long long)(fps_milli % 1000ull));
    fflush(stdout);
    sleep(1);
    close(state.fd);
    _Exit(EXIT_SUCCESS);
}
