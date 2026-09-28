/* SPDX-License-Identifier: MIT */
/* Headless, bounded Mesa/radeonsi render check. Never requests KMS master. */
#define _GNU_SOURCE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <errno.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <gbm.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <xf86drm.h>

static int fail(const char *stage, const char *operation)
{
    fprintf(stderr, "MESA_RENDER_FAIL stage=%s op=%s errno=%d egl=0x%x gl=0x%x\n",
            stage, operation, errno, (unsigned)eglGetError(), (unsigned)glGetError());
    fflush(stderr);
    return 1;
}

static void mark(const char *stage)
{
    printf("MESA_RENDER_STAGE %s\n", stage);
    fflush(stdout);
}

static GLuint shader(GLenum type, const char *source)
{
    GLuint object = glCreateShader(type);
    if (!object) return 0;
    glShaderSource(object, 1, &source, NULL);
    glCompileShader(object);
    GLint compiled = GL_FALSE;
    glGetShaderiv(object, GL_COMPILE_STATUS, &compiled);
    if (compiled == GL_TRUE) return object;
    char log[512] = {0};
    glGetShaderInfoLog(object, sizeof(log), NULL, log);
    fprintf(stderr, "MESA_SHADER_FAIL type=0x%x log=%s\n", (unsigned)type, log);
    glDeleteShader(object);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--runtime-check") == 0) {
        /* QEMU has no emulated AMDGPU. Loading radeonsi and querying EGL
         * verifies the bootfs closure without claiming a rendered frame. */
        const char *extensions = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
        if (!extensions) return fail("runtime-egl", "eglQueryString");
        void *driver = dlopen("/usr/lib/dri/radeonsi_dri.so", RTLD_NOW | RTLD_LOCAL);
        if (!driver) {
            fprintf(stderr, "MESA_RUNTIME_FAIL stage=radeonsi-load reason=%s\n", dlerror());
            return 1;
        }
        dlclose(driver);
        puts("MESA_RUNTIME_READY egl=loaded radeonsi=loaded gpu=not-probed");
        fflush(stdout);
        return 0;
    }
    if (argc != 1) {
        fputs("usage: amdgpu_mesa_render [--runtime-check]\n", stderr);
        return 2;
    }
    int result = 1;
    int fd = -1;
    struct gbm_device *gbm = NULL;
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;
    GLuint vertex = 0, fragment = 0, program = 0, framebuffer = 0, texture = 0;
    const char *stage = "render-open";

    /* fflush after every boundary: the caller can retain this transcript in
     * RAM even when the sandbox or the display disappears during a probe. */
    puts("MESA_RENDER_BEGIN node=/dev/dri/renderD128 surface=surfaceless-fbo size=64x64");
    fflush(stdout);
    fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (fd < 0) goto error;
    drmVersionPtr version = drmGetVersion(fd);
    if (!version || !version->name || strcmp(version->name, "amdgpu")) {
        if (version) drmFreeVersion(version);
        stage = "driver-identity";
        goto error;
    }
    printf("MESA_RENDER_DRM name=%s version=%d.%d.%d\n", version->name,
           version->version_major, version->version_minor,
           version->version_patchlevel);
    fflush(stdout);
    drmFreeVersion(version);

    stage = "render-node-identity";
    errno = 0;
    int node_type = drmGetNodeTypeFromFd(fd);
    printf("MESA_RENDER_NODE type=%d expected=%d errno=%d\n", node_type, DRM_NODE_RENDER, errno);
    fflush(stdout);
    if (node_type != DRM_NODE_RENDER) goto error;
    stage = "pci-discovery";
    drmDevicePtr device = NULL;
    int discovery = drmGetDevice2(fd, 0, &device);
    printf("MESA_RENDER_DISCOVERY status=%d\n", discovery);
    fflush(stdout);
    if (discovery || !device) goto error;
    if (device->bustype != DRM_BUS_PCI) {
        drmFreeDevice(&device);
        goto error;
    }
    printf("MESA_RENDER_PCI slot=%04x:%02x:%02x.%u vendor=%04x device=%04x\n",
        device->businfo.pci->domain, device->businfo.pci->bus,
        device->businfo.pci->dev, device->businfo.pci->func,
        device->deviceinfo.pci->vendor_id, device->deviceinfo.pci->device_id);
    fflush(stdout);
    drmFreeDevice(&device);

    stage = "gbm-create-device";
    gbm = gbm_create_device(fd);
    if (!gbm) goto error;
    printf("MESA_RENDER_GBM backend=%s\n", gbm_device_get_backend_name(gbm));
    fflush(stdout);

    stage = "egl-get-display";
    display = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm, NULL);
    if (display == EGL_NO_DISPLAY) goto error;
    stage = "egl-initialize";
    EGLint major = 0, minor = 0;
    if (!eglInitialize(display, &major, &minor)) goto error;
    printf("MESA_RENDER_EGL version=%d.%d vendor=%s\n", major, minor,
           eglQueryString(display, EGL_VENDOR));
    fflush(stdout);

    stage = "egl-choose-config";
    const EGLint config_attrs[] = {
        EGL_SURFACE_TYPE, 0,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_NONE,
    };
    EGLConfig config = NULL;
    EGLint count = 0;
    if (!eglChooseConfig(display, config_attrs, &config, 1, &count) || count < 1)
        goto error;
    stage = "egl-bind-api";
    if (!eglBindAPI(EGL_OPENGL_ES_API)) goto error;
    stage = "egl-create-context";
    const EGLint context_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attrs);
    if (context == EGL_NO_CONTEXT) goto error;
    /* GBM exports window configurations, not pbuffers. A surfaceless
     * context with an explicit FBO exercises rendering without KMS. */
    stage = "egl-surfaceless-support";
    const char *extensions = eglQueryString(display, EGL_EXTENSIONS);
    if (!extensions || !strstr(extensions, "EGL_KHR_surfaceless_context")) goto error;
    stage = "egl-make-current";
    if (!eglMakeCurrent(display, surface, surface, context)) goto error;

    stage = "renderer-identity";
    const char *vendor = (const char *)glGetString(GL_VENDOR);
    const char *renderer = (const char *)glGetString(GL_RENDERER);
    const char *gl_version = (const char *)glGetString(GL_VERSION);
    printf("MESA_RENDER_GL vendor=%s renderer=%s version=%s\n",
           vendor ? vendor : "(null)", renderer ? renderer : "(null)",
           gl_version ? gl_version : "(null)");
    fflush(stdout);
    /* EGL may retry in software after a failed driver load. Requiring both
     * the amdgpu render node and a Radeon renderer makes that a hard failure. */
    if (!renderer || (!strcasestr(renderer, "radeon") &&
                      !strcasestr(renderer, "amdgpu")) ||
        strcasestr(renderer, "llvmpipe") || strcasestr(renderer, "softpipe") ||
        strcasestr(renderer, "virgl") || strcasestr(renderer, "swrast"))
        goto error;

    stage = "framebuffer-create";
    mark("texture-create-begin");
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    mark("texture-create-done");
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    mark("framebuffer-attach-done");
    if (!texture || !framebuffer || glCheckFramebufferStatus(GL_FRAMEBUFFER) !=
        GL_FRAMEBUFFER_COMPLETE || glGetError() != GL_NO_ERROR) goto error;

    stage = "shader-compile";
    vertex = shader(GL_VERTEX_SHADER,
        "attribute vec2 position;\n"
        "void main() { gl_Position = vec4(position, 0.0, 1.0); }\n");
    fragment = shader(GL_FRAGMENT_SHADER,
        "precision mediump float;\n"
        "void main() { gl_FragColor = vec4(0.25, 0.5, 0.75, 1.0); }\n");
    if (!vertex || !fragment) goto error;
    mark("shader-compile-done");
    stage = "shader-link";
    program = glCreateProgram();
    if (!program) goto error;
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glBindAttribLocation(program, 0, "position");
    glLinkProgram(program);
    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) goto error;
    mark("shader-link-done");

    stage = "shader-draw";
    static const GLfloat vertices[] = {-1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f};
    glViewport(0, 0, 64, 64);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(program);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, vertices);
    glEnableVertexAttribArray(0);
    mark("draw-begin");
    glDrawArrays(GL_TRIANGLES, 0, 3);
    mark("draw-submitted");
    glFinish();
    mark("draw-finished");
    if (glGetError() != GL_NO_ERROR) goto error;
    stage = "readback";
    GLubyte pixel[4] = {0};
    mark("readback-begin");
    glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    mark("readback-done");
    if (glGetError() != GL_NO_ERROR || pixel[0] < 60 || pixel[0] > 68 ||
        pixel[1] < 124 || pixel[1] > 132 || pixel[2] < 187 ||
        pixel[2] > 195 || pixel[3] != 255) {
        fprintf(stderr, "MESA_RENDER_PIXEL_FAIL rgba=%u,%u,%u,%u\n",
                pixel[0], pixel[1], pixel[2], pixel[3]);
        goto error;
    }
    printf("MESA_RENDER_PASS renderer=%s rgba=%u,%u,%u,%u\n", renderer,
           pixel[0], pixel[1], pixel[2], pixel[3]);
    fflush(stdout);
    result = 0;
    goto done;
error:
    result = fail(stage, "Mesa-render");
done:
    if (framebuffer) glDeleteFramebuffers(1, &framebuffer);
    if (texture) glDeleteTextures(1, &texture);
    if (program) glDeleteProgram(program);
    if (vertex) glDeleteShader(vertex);
    if (fragment) glDeleteShader(fragment);
    if (display != EGL_NO_DISPLAY) {
        (void)eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (surface != EGL_NO_SURFACE) (void)eglDestroySurface(display, surface);
        if (context != EGL_NO_CONTEXT) (void)eglDestroyContext(display, context);
        (void)eglTerminate(display);
    }
    if (gbm) gbm_device_destroy(gbm);
    if (fd >= 0) close(fd);
    return result;
}
