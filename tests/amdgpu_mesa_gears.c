/* SPDX-License-Identifier: MIT */
/* A bounded Linux DRM/GBM/EGL/GLES2 animation for an exclusive KMS session. */
#define _GNU_SOURCE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <gbm.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

enum { MAX_SCANOUT_BUFFERS = 8, DEFAULT_SECONDS = 60, MAX_SECONDS = 90,
       GEAR_COUNT = 3, VERTICES_PER_SEGMENT = 24 };

struct gear_vertex {
    GLfloat position[3];
    GLfloat normal[3];
};

struct gear_mesh {
    struct gear_vertex *vertices;
    GLsizei count;
    GLfloat center[2];
    GLfloat color[3];
    GLfloat speed;
};

struct scanout_buffer {
    struct gbm_bo *bo;
    uint32_t fb;
    uint32_t imported[4];
};

struct gears {
    int fd;
    struct gbm_device *gbm;
    struct gbm_surface *gbm_surface;
    struct gbm_bo *front;
    EGLDisplay display;
    EGLContext context;
    EGLSurface surface;
    GLuint program;
    GLint angle_uniform;
    GLint aspect_uniform;
    GLint center_uniform;
    GLint color_uniform;
    struct gear_mesh meshes[GEAR_COUNT];
    drmModeModeInfo mode;
    uint32_t connector;
    uint32_t crtc;
    uint32_t plane;
    uint32_t mode_blob;
    uint32_t connector_crtc;
    uint32_t crtc_mode;
    uint32_t crtc_active;
    uint32_t plane_fb;
    uint32_t plane_crtc;
    uint32_t src_x, src_y, src_w, src_h;
    uint32_t dst_x, dst_y, dst_w, dst_h;
    struct scanout_buffer buffers[MAX_SCANOUT_BUFFERS];
};

static void close_imported(int fd, const uint32_t handles[4])
{
    for (unsigned plane = 0; plane < 4; ++plane) {
        if (!handles[plane]) continue;
        bool seen = false;
        for (unsigned previous = 0; previous < plane; ++previous)
            if (handles[previous] == handles[plane]) seen = true;
        if (!seen) (void)drmCloseBufferHandle(fd, handles[plane]);
    }
}

static int fail(const char *stage)
{
    fprintf(stderr, "MESA_GEARS_FAIL stage=%s errno=%d egl=0x%x gl=0x%x\n",
            stage, errno, (unsigned)eglGetError(), (unsigned)glGetError());
    fflush(stderr);
    return -1;
}

static uint64_t monotonic_ns(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return 0;
    return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}

static int property_id(int fd, uint32_t object, uint32_t type,
                       const char *name, uint64_t *value)
{
    drmModeObjectPropertiesPtr properties =
        drmModeObjectGetProperties(fd, object, type);
    if (!properties) return 0;
    uint32_t result = 0;
    for (uint32_t i = 0; i < properties->count_props; ++i) {
        drmModePropertyPtr property = drmModeGetProperty(fd, properties->props[i]);
        if (!property) continue;
        if (strcmp(property->name, name) == 0) {
            result = property->prop_id;
            if (value) *value = properties->prop_values[i];
        }
        drmModeFreeProperty(property);
        if (result) break;
    }
    drmModeFreeObjectProperties(properties);
    return result;
}

static int pipeline(struct gears *g)
{
    drmModeResPtr resources = drmModeGetResources(g->fd);
    drmModePlaneResPtr planes = NULL;
    int found = 0;
    if (!resources) return fail("get-resources");
    for (int c = 0; c < resources->count_connectors && !found; ++c) {
        drmModeConnectorPtr connector =
            drmModeGetConnector(g->fd, resources->connectors[c]);
        if (!connector) continue;
        if (connector->connection != DRM_MODE_CONNECTED ||
            connector->count_modes <= 0) {
            drmModeFreeConnector(connector);
            continue;
        }
        uint32_t crtc_mask = 0;
        for (int e = 0; e < connector->count_encoders; ++e) {
            drmModeEncoderPtr encoder =
                drmModeGetEncoder(g->fd, connector->encoders[e]);
            if (!encoder) continue;
            crtc_mask |= encoder->possible_crtcs;
            drmModeFreeEncoder(encoder);
        }
        if (!crtc_mask) {
            drmModeFreeConnector(connector);
            continue;
        }
        for (int m = 0; m < connector->count_modes && !found; ++m) {
            /* The monitor chooses the preferred mode; no board or EDID ID is
             * embedded in this client. */
            if (!(connector->modes[m].type & DRM_MODE_TYPE_PREFERRED)) continue;
            g->mode = connector->modes[m];
            found = 1;
        }
        if (!found) {
            g->mode = connector->modes[0];
            found = 1;
        }
        planes = drmModeGetPlaneResources(g->fd);
        if (!planes) found = 0;
        for (uint32_t p = 0; found && p < planes->count_planes && !g->plane; ++p) {
            drmModePlanePtr plane = drmModeGetPlane(g->fd, planes->planes[p]);
            if (!plane) continue;
            uint64_t type = 0;
            int primary = property_id(g->fd, plane->plane_id,
                                      DRM_MODE_OBJECT_PLANE, "type", &type) &&
                          type == DRM_PLANE_TYPE_PRIMARY;
            int xrgb = 0;
            for (uint32_t f = 0; f < plane->count_formats; ++f)
                xrgb |= plane->formats[f] == DRM_FORMAT_XRGB8888;
            if (primary && xrgb) {
                for (int r = 0; r < resources->count_crtcs; ++r) {
                    if (r >= 32 || !(crtc_mask & plane->possible_crtcs & (1u << r)))
                        continue;
                    g->crtc = resources->crtcs[r];
                    g->plane = plane->plane_id;
                    g->connector = connector->connector_id;
                    break;
                }
            }
            drmModeFreePlane(plane);
        }
        if (planes) {
            drmModeFreePlaneResources(planes);
            planes = NULL;
        }
        drmModeFreeConnector(connector);
        if (!g->plane) found = 0;
    }
    drmModeFreeResources(resources);
    if (!found || !g->plane) return fail("connected-primary-plane");
#define PROP(member, object, type, name) do { \
    g->member = property_id(g->fd, object, type, name, NULL); \
    if (!g->member) return fail("property-" name); \
} while (0)
    PROP(connector_crtc, g->connector, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID");
    PROP(crtc_mode, g->crtc, DRM_MODE_OBJECT_CRTC, "MODE_ID");
    PROP(crtc_active, g->crtc, DRM_MODE_OBJECT_CRTC, "ACTIVE");
    PROP(plane_fb, g->plane, DRM_MODE_OBJECT_PLANE, "FB_ID");
    PROP(plane_crtc, g->plane, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
    PROP(src_x, g->plane, DRM_MODE_OBJECT_PLANE, "SRC_X");
    PROP(src_y, g->plane, DRM_MODE_OBJECT_PLANE, "SRC_Y");
    PROP(src_w, g->plane, DRM_MODE_OBJECT_PLANE, "SRC_W");
    PROP(src_h, g->plane, DRM_MODE_OBJECT_PLANE, "SRC_H");
    PROP(dst_x, g->plane, DRM_MODE_OBJECT_PLANE, "CRTC_X");
    PROP(dst_y, g->plane, DRM_MODE_OBJECT_PLANE, "CRTC_Y");
    PROP(dst_w, g->plane, DRM_MODE_OBJECT_PLANE, "CRTC_W");
    PROP(dst_h, g->plane, DRM_MODE_OBJECT_PLANE, "CRTC_H");
#undef PROP
    if (drmModeCreatePropertyBlob(g->fd, &g->mode, sizeof(g->mode),
                                  &g->mode_blob)) return fail("mode-blob");
    printf("MESA_GEARS_KMS connector=%u crtc=%u plane=%u mode=%ux%u@%u\n",
           g->connector, g->crtc, g->plane, g->mode.hdisplay,
           g->mode.vdisplay, g->mode.vrefresh);
    fflush(stdout);
    return 0;
}

static GLuint shader(GLenum kind, const char *source)
{
    GLuint object = glCreateShader(kind);
    if (!object) return 0;
    glShaderSource(object, 1, &source, NULL);
    glCompileShader(object);
    GLint compiled = GL_FALSE;
    glGetShaderiv(object, GL_COMPILE_STATUS, &compiled);
    if (compiled) return object;
    char log[512] = {0};
    glGetShaderInfoLog(object, sizeof(log), NULL, log);
    fprintf(stderr, "MESA_GEARS_SHADER_FAIL kind=0x%x log=%s\n", kind, log);
    glDeleteShader(object);
    return 0;
}

static void vertex(struct gear_vertex *v, float x, float y, float z,
                   float nx, float ny, float nz)
{
    *v = (struct gear_vertex){{x, y, z}, {nx, ny, nz}};
}

static void quad(struct gear_vertex **next, const float a[3],
                 const float b[3], const float c[3], const float d[3],
                 float nx, float ny, float nz)
{
    struct gear_vertex *v = *next;
    vertex(&v[0], a[0], a[1], a[2], nx, ny, nz);
    vertex(&v[1], b[0], b[1], b[2], nx, ny, nz);
    vertex(&v[2], c[0], c[1], c[2], nx, ny, nz);
    v[3] = v[0];
    v[4] = v[2];
    vertex(&v[5], d[0], d[1], d[2], nx, ny, nz);
    *next += 6;
}

static float tooth_radius(unsigned step, float radius)
{
    /* Eight samples per tooth leave distinct root, flank and crown faces. */
    switch (step % 8) {
    case 2:
    case 5: return radius * 1.055f;
    case 3:
    case 4: return radius * 1.12f;
    default: return radius;
    }
}

static int gear_mesh(struct gear_mesh *mesh, unsigned teeth, float radius,
                     float x, float y, float red, float green, float blue,
                     float speed)
{
    const unsigned segments = teeth * 8;
    const float inner = radius * 0.26f;
    const float half_depth = radius * 0.18f;
    const float tau = 6.2831853071795864769f;
    mesh->vertices = calloc(segments * VERTICES_PER_SEGMENT,
                            sizeof(*mesh->vertices));
    if (!mesh->vertices) return fail("gear-mesh-allocation");
    mesh->center[0] = x;
    mesh->center[1] = y;
    mesh->color[0] = red;
    mesh->color[1] = green;
    mesh->color[2] = blue;
    mesh->speed = speed;
    struct gear_vertex *next = mesh->vertices;
    for (unsigned step = 0; step < segments; ++step) {
        float angle0 = tau * (float)step / (float)segments;
        float angle1 = tau * (float)(step + 1) / (float)segments;
        float c0 = cosf(angle0), s0 = sinf(angle0);
        float c1 = cosf(angle1), s1 = sinf(angle1);
        float outer0 = tooth_radius(step, radius);
        float outer1 = tooth_radius(step + 1, radius);
        float i0f[3] = {inner*c0, inner*s0, half_depth};
        float i1f[3] = {inner*c1, inner*s1, half_depth};
        float o0f[3] = {outer0*c0, outer0*s0, half_depth};
        float o1f[3] = {outer1*c1, outer1*s1, half_depth};
        float i0b[3] = {inner*c0, inner*s0, -half_depth};
        float i1b[3] = {inner*c1, inner*s1, -half_depth};
        float o0b[3] = {outer0*c0, outer0*s0, -half_depth};
        float o1b[3] = {outer1*c1, outer1*s1, -half_depth};
        quad(&next, i0f, o0f, o1f, i1f, 0, 0, 1);
        quad(&next, i1b, o1b, o0b, i0b, 0, 0, -1);
        /* The flank's normal follows its actual slope, so each tooth's
         * extruded sides remain visibly faceted under the oblique light. */
        float dx = o1f[0] - o0f[0], dy = o1f[1] - o0f[1];
        float length = sqrtf(dx*dx + dy*dy);
        quad(&next, o0f, o0b, o1b, o1f, dy/length, -dx/length, 0);
        quad(&next, i0b, i0f, i1f, i1b,
             -(c0+c1)*0.5f, -(s0+s1)*0.5f, 0);
    }
    mesh->count = (GLsizei)(next - mesh->vertices);
    return 0;
}

static int meshes(struct gears *g)
{
    /* Mirror the recognizable large red / two small green-blue layout and
     * inverse rotation ratio without depending on GLX or X11. */
    if (gear_mesh(&g->meshes[0], 20, 0.95f, -0.75f, -0.50f,
                  0.86f, 0.12f, 0.09f, 1.0f) ||
        gear_mesh(&g->meshes[1], 10, 0.50f, 0.70f, -0.50f,
                  0.13f, 0.72f, 0.20f, -2.0f) ||
        gear_mesh(&g->meshes[2], 10, 0.50f, -0.75f, 0.95f,
                  0.16f, 0.35f, 0.95f, -2.0f)) return -1;
    return 0;
}

static int program(struct gears *g)
{
    static const char vertex_source[] =
        "attribute vec3 position; attribute vec3 normal;\n"
        "uniform float angle; uniform float aspect; uniform vec2 center;\n"
        "varying vec3 lit_normal;\n"
        "void main() {\n"
        "  float c=cos(angle), s=sin(angle);\n"
        "  vec3 p=vec3(c*position.x-s*position.y,\n"
        "              s*position.x+c*position.y,position.z);\n"
        "  vec3 n=vec3(c*normal.x-s*normal.y,\n"
        "              s*normal.x+c*normal.y,normal.z);\n"
        "  p.xy+=center;\n"
        "  p=vec3(0.93*p.x+0.37*p.z,p.y,-0.37*p.x+0.93*p.z);\n"
        "  n=vec3(0.93*n.x+0.37*n.z,n.y,-0.37*n.x+0.93*n.z);\n"
        "  p=vec3(p.x,0.82*p.y-0.57*p.z,0.57*p.y+0.82*p.z);\n"
        "  n=vec3(n.x,0.82*n.y-0.57*n.z,0.57*n.y+0.82*n.z);\n"
        "  lit_normal=n;\n"
        "  float depth=5.0-p.z;\n"
        "  gl_Position=vec4(2.5*p.x/aspect,2.5*p.y,\n"
        "                   1.222222*depth-2.222222,depth);\n"
        "}\n";
    static const char fragment_source[] =
        "precision mediump float;\n"
        "uniform vec3 color; varying vec3 lit_normal;\n"
        "void main() {\n"
        "  vec3 n=normalize(lit_normal);\n"
        "  vec3 light=normalize(vec3(-0.45,0.72,0.75));\n"
        "  float diffuse=max(dot(n,light),0.0);\n"
        "  float spec=pow(max(dot(n,normalize(light+vec3(0,0,1))),0.0),24.0);\n"
        "  gl_FragColor=vec4(color*(0.30+0.70*diffuse)+0.22*spec,1.0);\n"
        "}\n";
    GLuint vs = shader(GL_VERTEX_SHADER, vertex_source);
    GLuint fs = shader(GL_FRAGMENT_SHADER, fragment_source);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return fail("compile-shader");
    }
    g->program = glCreateProgram();
    glAttachShader(g->program, vs);
    glAttachShader(g->program, fs);
    glBindAttribLocation(g->program, 0, "position");
    glBindAttribLocation(g->program, 1, "normal");
    glLinkProgram(g->program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint linked = GL_FALSE;
    glGetProgramiv(g->program, GL_LINK_STATUS, &linked);
    if (!linked) return fail("link-shader");
    g->angle_uniform = glGetUniformLocation(g->program, "angle");
    g->aspect_uniform = glGetUniformLocation(g->program, "aspect");
    g->center_uniform = glGetUniformLocation(g->program, "center");
    g->color_uniform = glGetUniformLocation(g->program, "color");
    return g->angle_uniform < 0 || g->aspect_uniform < 0 ||
           g->center_uniform < 0 || g->color_uniform < 0 ?
        fail("shader-uniforms") : 0;
}

static int graphics(struct gears *g)
{
    g->gbm = gbm_create_device(g->fd);
    if (!g->gbm) return fail("gbm-device");
    g->display = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, g->gbm, NULL);
    if (g->display == EGL_NO_DISPLAY || !eglInitialize(g->display, NULL, NULL))
        return fail("egl-display");
    const EGLint attributes[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 0, EGL_DEPTH_SIZE, 16, EGL_NONE};
    EGLConfig configs[64];
    EGLint count = 0;
    EGLConfig config = NULL;
    if (!eglChooseConfig(g->display, attributes, configs, 64, &count))
        return fail("egl-configs");
    for (EGLint i = 0; i < count; ++i) {
        EGLint visual = 0;
        if (eglGetConfigAttrib(g->display, configs[i], EGL_NATIVE_VISUAL_ID,
                               &visual) && (uint32_t)visual == GBM_FORMAT_XRGB8888) {
            config = configs[i];
            break;
        }
    }
    if (!config || !eglBindAPI(EGL_OPENGL_ES_API)) return fail("egl-config");
    const EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    g->context = eglCreateContext(g->display, config, EGL_NO_CONTEXT,
                                  context_attributes);
    if (g->context == EGL_NO_CONTEXT) return fail("egl-context");
    g->gbm_surface = gbm_surface_create(g->gbm, g->mode.hdisplay,
        g->mode.vdisplay, GBM_FORMAT_XRGB8888,
        GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    if (!g->gbm_surface) return fail("gbm-surface");
    g->surface = eglCreateWindowSurface(g->display, config,
        (EGLNativeWindowType)g->gbm_surface, NULL);
    if (g->surface == EGL_NO_SURFACE ||
        !eglMakeCurrent(g->display, g->surface, g->surface, g->context))
        return fail("egl-surface");
    const char *renderer = (const char *)glGetString(GL_RENDERER);
    printf("MESA_GEARS_RENDERER %s\n", renderer ? renderer : "(null)");
    fflush(stdout);
    /* Mesa may retry in software. A real AMD DRM node plus an AMD renderer
     * are both required; software and host-virtual renderers are failures. */
    if (!renderer || (!strcasestr(renderer, "radeon") &&
                      !strcasestr(renderer, "amdgpu")) ||
        strcasestr(renderer, "llvmpipe") || strcasestr(renderer, "softpipe") ||
        strcasestr(renderer, "virgl") || strcasestr(renderer, "swrast"))
        return fail("hardware-renderer");
    if (program(g) || meshes(g)) return -1;
    printf("MESA_GEARS_SCENE gears=3 geometry=extruded teeth=20,10,10 view=oblique depth=16\n");
    fflush(stdout);
    return 0;
}

static int framebuffer(struct gears *g, struct gbm_bo *bo, uint32_t *fb)
{
    for (unsigned i = 0; i < MAX_SCANOUT_BUFFERS; ++i)
        if (g->buffers[i].bo == bo) {
            *fb = g->buffers[i].fb;
            return 0;
        }
    unsigned slot = 0;
    while (slot < MAX_SCANOUT_BUFFERS && g->buffers[slot].bo) ++slot;
    if (slot == MAX_SCANOUT_BUFFERS) {
        errno = ENOSPC;
        return fail("scanout-buffer-limit");
    }
    int plane_count = gbm_bo_get_plane_count(bo);
    if (plane_count < 1 || plane_count > 4) {
        errno = EINVAL;
        return fail("gbm-plane-count");
    }
    uint32_t handles[4] = {0}, pitches[4] = {0}, offsets[4] = {0};
    uint32_t imported[4] = {0};
    int export_errno[4] = {0}, import_errno[4] = {0};
    int handle_errno[4] = {0};
    uint64_t modifiers[4] = {0};
    uint64_t modifier = gbm_bo_get_modifier(bo);
    for (int plane = 0; plane < plane_count; ++plane) {
        errno = 0;
        handles[plane] = gbm_bo_get_handle_for_plane(bo, plane).u32;
        handle_errno[plane] = errno;
        pitches[plane] = gbm_bo_get_stride_for_plane(bo, plane);
        offsets[plane] = gbm_bo_get_offset(bo, plane);
        modifiers[plane] = modifier;
        if (plane == 0) {
            if (!handles[plane] || handles[plane] == UINT32_MAX)
                handles[plane] = gbm_bo_get_handle(bo).u32;
            if (!pitches[plane]) pitches[plane] = gbm_bo_get_stride(bo);
        }
        if (handles[plane] == UINT32_MAX) handles[plane] = 0;
        if (!handles[plane]) {
            /* The BO may belong to Mesa's distinct DRM file description.
             * PRIME import gives KMS a handle owned by our primary fd. */
            errno = 0;
            int dma_fd = gbm_bo_get_fd_for_plane(bo, plane);
            if (dma_fd >= 0) {
                uint32_t handle = 0;
                errno = 0;
                int status = drmPrimeFDToHandle(g->fd, dma_fd, &handle);
                if (status) import_errno[plane] = errno;
                close(dma_fd);
                if (!status && handle) {
                    handles[plane] = handle;
                    imported[plane] = handle;
                }
            } else export_errno[plane] = errno;
        }
    }
    printf("MESA_GEARS_FB_PLANES width=%u height=%u count=%d format=0x%x "
           "modifier=0x%llx handles=%u,%u,%u,%u pitches=%u,%u,%u,%u "
           "offsets=%u,%u,%u,%u handle_errno=%d,%d,%d,%d "
           "export_errno=%d,%d,%d,%d import_errno=%d,%d,%d,%d\n",
           gbm_bo_get_width(bo), gbm_bo_get_height(bo), plane_count,
           gbm_bo_get_format(bo), (unsigned long long)modifier,
           handles[0], handles[1], handles[2], handles[3],
           pitches[0], pitches[1], pitches[2], pitches[3],
           offsets[0], offsets[1], offsets[2], offsets[3],
           handle_errno[0], handle_errno[1], handle_errno[2], handle_errno[3],
           export_errno[0], export_errno[1], export_errno[2], export_errno[3],
           import_errno[0], import_errno[1], import_errno[2], import_errno[3]);
    fflush(stdout);
    bool valid = true;
    for (int plane = 0; plane < plane_count; ++plane)
        if (!handles[plane] || !pitches[plane]) valid = false;
    if (!valid) {
        close_imported(g->fd, imported);
        errno = EINVAL;
        return fail("gbm-plane-layout");
    }
    int result;
    if (modifier == DRM_FORMAT_MOD_INVALID)
        result = drmModeAddFB2(g->fd, gbm_bo_get_width(bo),
            gbm_bo_get_height(bo), DRM_FORMAT_XRGB8888, handles,
            pitches, offsets, fb, 0);
    else
        result = drmModeAddFB2WithModifiers(g->fd, gbm_bo_get_width(bo),
            gbm_bo_get_height(bo), DRM_FORMAT_XRGB8888, handles,
            pitches, offsets, modifiers, fb, DRM_MODE_FB_MODIFIERS);
    if (result) {
        int saved_errno = errno;
        close_imported(g->fd, imported);
        errno = saved_errno;
        return fail("add-framebuffer");
    }
    g->buffers[slot] = (struct scanout_buffer){.bo = bo, .fb = *fb};
    memcpy(g->buffers[slot].imported, imported, sizeof(imported));
    return 0;
}

static int atomic_add(drmModeAtomicReqPtr request, uint32_t object,
                      uint32_t property, uint64_t value)
{
    return drmModeAtomicAddProperty(request, object, property, value) < 0 ? -1 : 0;
}

static int present(struct gears *g, uint32_t fb, int initial)
{
    drmModeAtomicReqPtr request = drmModeAtomicAlloc();
    if (!request) return fail("atomic-allocate");
    int ok = 0;
    if (initial) {
        ok |= atomic_add(request, g->connector, g->connector_crtc, g->crtc);
        ok |= atomic_add(request, g->crtc, g->crtc_mode, g->mode_blob);
        ok |= atomic_add(request, g->crtc, g->crtc_active, 1);
    }
    ok |= atomic_add(request, g->plane, g->plane_fb, fb);
    ok |= atomic_add(request, g->plane, g->plane_crtc, g->crtc);
    if (initial) {
        ok |= atomic_add(request, g->plane, g->src_x, 0);
        ok |= atomic_add(request, g->plane, g->src_y, 0);
        ok |= atomic_add(request, g->plane, g->src_w,
                         (uint64_t)g->mode.hdisplay << 16);
        ok |= atomic_add(request, g->plane, g->src_h,
                         (uint64_t)g->mode.vdisplay << 16);
        ok |= atomic_add(request, g->plane, g->dst_x, 0);
        ok |= atomic_add(request, g->plane, g->dst_y, 0);
        ok |= atomic_add(request, g->plane, g->dst_w, g->mode.hdisplay);
        ok |= atomic_add(request, g->plane, g->dst_h, g->mode.vdisplay);
    }
    uint32_t flags = initial ? DRM_MODE_ATOMIC_ALLOW_MODESET : 0;
    int result = 0;
    if (ok || drmModeAtomicCommit(g->fd, request,
                                 flags | DRM_MODE_ATOMIC_TEST_ONLY, NULL))
        result = fail("atomic-test-only");
    else if (drmModeAtomicCommit(g->fd, request, flags, NULL))
        result = fail("atomic-commit");
    drmModeAtomicFree(request);
    return result;
}

static int draw(struct gears *g, float seconds, int initial)
{
    glViewport(0, 0, g->mode.hdisplay, g->mode.vdisplay);
    glClearColor(0.008f, 0.012f, 0.025f, 1.0f);
    glClearDepthf(1.0f);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glUseProgram(g->program);
    glUniform1f(g->aspect_uniform,
                (float)g->mode.hdisplay / (float)g->mode.vdisplay);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    for (unsigned i = 0; i < GEAR_COUNT; ++i) {
        const struct gear_mesh *mesh = &g->meshes[i];
        glUniform1f(g->angle_uniform, seconds * mesh->speed +
                    (i == 2 ? 0.26f : 0.0f));
        glUniform2fv(g->center_uniform, 1, mesh->center);
        glUniform3fv(g->color_uniform, 1, mesh->color);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
                              sizeof(*mesh->vertices),
                              &mesh->vertices[0].position[0]);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE,
                              sizeof(*mesh->vertices),
                              &mesh->vertices[0].normal[0]);
        glDrawArrays(GL_TRIANGLES, 0, mesh->count);
    }
    if (glGetError() != GL_NO_ERROR ||
        !eglSwapBuffers(g->display, g->surface)) return fail("mesa-draw");
    struct gbm_bo *next = gbm_surface_lock_front_buffer(g->gbm_surface);
    if (!next) return fail("lock-front-buffer");
    uint32_t fb = 0;
    int result = framebuffer(g, next, &fb);
    if (!result) result = present(g, fb, initial);
    if (result) {
        gbm_surface_release_buffer(g->gbm_surface, next);
        return result;
    }
    if (g->front) gbm_surface_release_buffer(g->gbm_surface, g->front);
    g->front = next;
    return 0;
}

static void cleanup(struct gears *g)
{
    for (unsigned i = 0; i < GEAR_COUNT; ++i) free(g->meshes[i].vertices);
    if (g->program) glDeleteProgram(g->program);
    if (g->display != EGL_NO_DISPLAY)
        (void)eglMakeCurrent(g->display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                             EGL_NO_CONTEXT);
    if (g->front && g->gbm_surface)
        gbm_surface_release_buffer(g->gbm_surface, g->front);
    if (g->fd >= 0) {
        for (unsigned i = 0; i < MAX_SCANOUT_BUFFERS; ++i) {
            if (g->buffers[i].fb) (void)drmModeRmFB(g->fd, g->buffers[i].fb);
        }
        if (g->mode_blob) (void)drmModeDestroyPropertyBlob(g->fd, g->mode_blob);
    }
    if (g->display != EGL_NO_DISPLAY) {
        if (g->surface != EGL_NO_SURFACE)
            (void)eglDestroySurface(g->display, g->surface);
        if (g->context != EGL_NO_CONTEXT)
            (void)eglDestroyContext(g->display, g->context);
        (void)eglTerminate(g->display);
    }
    if (g->gbm_surface) gbm_surface_destroy(g->gbm_surface);
    if (g->gbm) gbm_device_destroy(g->gbm);
    if (g->fd >= 0) {
        for (unsigned i = 0; i < MAX_SCANOUT_BUFFERS; ++i)
            close_imported(g->fd, g->buffers[i].imported);
        close(g->fd);
    }
}

int main(int argc, char **argv)
{
    unsigned seconds = DEFAULT_SECONDS;
    if (argc == 3 && strcmp(argv[1], "--seconds") == 0) {
        char *end = NULL;
        unsigned long parsed = strtoul(argv[2], &end, 10);
        if (!end || *end || parsed < 1 || parsed > MAX_SECONDS) {
            fputs("usage: amdgpu-mesa-gears [--seconds 1..90]\n", stderr);
            return 2;
        }
        seconds = (unsigned)parsed;
    } else if (argc != 1) {
        fputs("usage: amdgpu-mesa-gears [--seconds 1..90]\n", stderr);
        return 2;
    }
    struct gears g = {.fd = -1, .display = EGL_NO_DISPLAY,
        .context = EGL_NO_CONTEXT, .surface = EGL_NO_SURFACE};
    printf("MESA_GEARS_BEGIN seconds=%u node=/dev/dri/card0 api=GBM/EGL/GLES2/KMS-atomic\n",
           seconds);
    fflush(stdout);
    int result = 1;
    g.fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (g.fd < 0) {
        (void)fail("card-open");
        goto done;
    }
    drmVersionPtr version = drmGetVersion(g.fd);
    if (!version || !version->name || strcmp(version->name, "amdgpu")) {
        if (version) drmFreeVersion(version);
        (void)fail("amdgpu-driver");
        goto done;
    }
    drmFreeVersion(version);
    if (drmSetClientCap(g.fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) ||
        drmSetClientCap(g.fd, DRM_CLIENT_CAP_ATOMIC, 1)) {
        (void)fail("atomic-client-cap");
        goto done;
    }
    if (pipeline(&g) || graphics(&g)) goto done;
    uint64_t start = monotonic_ns();
    if (!start || draw(&g, 0.0f, 1)) goto done;
    printf("MESA_GEARS_DISPLAY_READY renderer=amdgpu mode=%ux%u\n",
           g.mode.hdisplay, g.mode.vdisplay);
    fflush(stdout);
    unsigned frames = 1;
    uint64_t last_report = start;
    unsigned last_report_frames = 0;
    while (monotonic_ns() - start < (uint64_t)seconds * 1000000000ull) {
        uint64_t now = monotonic_ns();
        if (draw(&g, (float)(now - start) / 1000000000.0f, 0)) goto done;
        ++frames;
        uint64_t after = monotonic_ns();
        if (after - last_report >= 5000000000ull) {
            uint64_t fps_100 = (uint64_t)(frames - last_report_frames) *
                               100000000000ull / (after - last_report);
            printf("MESA_GEARS_FPS fps=%llu.%02llu frames=%u elapsed_ms=%llu\n",
                   (unsigned long long)(fps_100 / 100),
                   (unsigned long long)(fps_100 % 100), frames,
                   (unsigned long long)((after - start) / 1000000ull));
            fflush(stdout);
            last_report = after;
            last_report_frames = frames;
        }
        /* Keep the animation active without busy-spinning a fast GPU. */
        if (after - now < 33333333ull) {
            uint64_t remaining = 33333333ull - (after - now);
            struct timespec pause = {.tv_sec = remaining / 1000000000ull,
                                     .tv_nsec = remaining % 1000000000ull};
            (void)nanosleep(&pause, NULL);
        }
    }
    uint64_t elapsed = monotonic_ns() - start;
    uint64_t fps_100 = (uint64_t)frames * 100000000000ull / elapsed;
    printf("MESA_GEARS_PASS frames=%u elapsed_ms=%llu fps_avg=%llu.%02llu\n",
           frames, (unsigned long long)(elapsed / 1000000ull),
           (unsigned long long)(fps_100 / 100),
           (unsigned long long)(fps_100 % 100));
    fflush(stdout);
    result = 0;
done:
    cleanup(&g);
    return result;
}
