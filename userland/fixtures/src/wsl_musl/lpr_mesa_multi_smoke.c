#define _GNU_SOURCE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <gbm.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

enum {
    WIDTH = 79,
    HEIGHT = 61,
    FRAMES = 4,
    PHASE_BUFFER = 1,
    PHASE_FENCE = 16,
    PHASE_ACK = 32,
    PHASE_KILL_READY = 48,
};

struct message {
    uint32_t phase;
    uint32_t role;
    uint32_t value;
};

struct image {
    struct gbm_bo *bo;
    EGLImageKHR image;
    GLuint texture;
    GLuint framebuffer;
};

struct client {
    unsigned role;
    int socket;
    int fd;
    int own_dma_buf;
    int peer_dma_buf;
    struct gbm_device *gbm;
    EGLDisplay display;
    EGLContext context;
    struct image own;
    struct image peer;
};

static PFNEGLCREATEIMAGEKHRPROC create_image;
static PFNEGLDESTROYIMAGEKHRPROC destroy_image;
static PFNEGLCREATESYNCKHRPROC create_sync;
static PFNEGLDESTROYSYNCKHRPROC destroy_sync;
static PFNEGLDUPNATIVEFENCEFDANDROIDPROC duplicate_fence;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target;

static const uint8_t colors[2][FRAMES][4] = {
    {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {0, 255, 255, 255}},
    {{0, 0, 255, 255}, {255, 255, 0, 255}, {255, 0, 255, 255}, {255, 255, 255, 255}},
};

static int fail(const struct client *client, const char *stage, const char *operation)
{
    fprintf(stderr,
        "MESA_MULTI_FAIL role=%u stage=%s op=%s errno=%d egl=0x%04x gl=0x%04x\n",
        client ? client->role : 99u, stage, operation, errno,
        (unsigned)eglGetError(), (unsigned)glGetError());
    fflush(stderr);
    return 1;
}

static int wait_readable(int fd, int timeout_ms)
{
    struct pollfd event = {.fd = fd, .events = POLLIN};
    int status;
    do {
        status = poll(&event, 1, timeout_ms);
    } while (status < 0 && errno == EINTR);
    return status == 1 && (event.revents & POLLIN) != 0 &&
        (event.revents & (POLLERR | POLLNVAL)) == 0 ? 0 : -1;
}

static int send_message(
    const struct client *client, uint32_t phase, uint32_t value, int fd)
{
    struct message data = {.phase = phase, .role = client->role, .value = value};
    struct iovec iov = {.iov_base = &data, .iov_len = sizeof(data)};
    union {
        struct cmsghdr alignment;
        unsigned char bytes[CMSG_SPACE(sizeof(int))];
    } control = {0};
    struct msghdr header = {.msg_iov = &iov, .msg_iovlen = 1};
    if (fd >= 0) {
        header.msg_control = control.bytes;
        header.msg_controllen = sizeof(control.bytes);
        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&header);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(fd));
        memcpy(CMSG_DATA(cmsg), &fd, sizeof(fd));
    }
    return sendmsg(client->socket, &header, MSG_NOSIGNAL) == sizeof(data) ?
        0 : fail(client, "ipc", "sendmsg");
}

static int receive_message(
    const struct client *client, uint32_t phase, uint32_t *value, int *fd)
{
    struct message data = {0};
    struct iovec iov = {.iov_base = &data, .iov_len = sizeof(data)};
    union {
        struct cmsghdr alignment;
        unsigned char bytes[CMSG_SPACE(sizeof(int))];
    } control = {0};
    struct msghdr header = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = control.bytes,
        .msg_controllen = sizeof(control.bytes),
    };
    if (wait_readable(client->socket, 10000) != 0)
        return fail(client, "ipc", "poll-message");
    if (recvmsg(client->socket, &header, MSG_CMSG_CLOEXEC) != sizeof(data) ||
        (header.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0 ||
        data.phase != phase || data.role != (client->role ^ 1u))
        return fail(client, "ipc", "recvmsg");
    if (value) *value = data.value;
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&header);
    if (!fd)
        return cmsg == NULL ? 0 : fail(client, "ipc", "unexpected-fd");
    if (!cmsg || cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS ||
        cmsg->cmsg_len != CMSG_LEN(sizeof(*fd)) || CMSG_NXTHDR(&header, cmsg))
        return fail(client, "ipc", "invalid-fd");
    memcpy(fd, CMSG_DATA(cmsg), sizeof(*fd));
    if (*fd < 0 || (fcntl(*fd, F_GETFD) & FD_CLOEXEC) == 0)
        return fail(client, "ipc", "fd-cloexec");
    return 0;
}

static int choose_config(struct client *client, EGLConfig *config)
{
    const EGLint attributes[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE,
    };
    EGLint count = 0;
    if (!eglChooseConfig(client->display, attributes, config, 1, &count) || count != 1)
        return fail(client, "egl-init", "eglChooseConfig");
    return 0;
}

static int client_open(struct client *client)
{
    const char *node = client->role == 0 ? "/dev/dri/card0" : "/dev/dri/renderD128";
    client->fd = open(node, O_RDWR | O_CLOEXEC);
    if (client->fd < 0) return fail(client, "open", node);
    client->gbm = gbm_create_device(client->fd);
    if (!client->gbm) return fail(client, "gbm-init", "gbm_create_device");
    client->display = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, client->gbm, NULL);
    EGLint major = 0, minor = 0;
    if (client->display == EGL_NO_DISPLAY ||
        !eglInitialize(client->display, &major, &minor))
        return fail(client, "egl-init", "eglInitialize");
    const char *extensions = eglQueryString(client->display, EGL_EXTENSIONS);
    if (!extensions || !strstr(extensions, "EGL_EXT_image_dma_buf_import") ||
        !strstr(extensions, "EGL_ANDROID_native_fence_sync") ||
        !strstr(extensions, "EGL_KHR_surfaceless_context"))
        return fail(client, "egl-init", "required-extensions");
    EGLConfig config;
    if (choose_config(client, &config) != 0 || !eglBindAPI(EGL_OPENGL_ES_API))
        return fail(client, "egl-init", "eglBindAPI");
    const EGLint context_attributes[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE,
    };
    client->context = eglCreateContext(
        client->display, config, EGL_NO_CONTEXT, context_attributes);
    if (client->context == EGL_NO_CONTEXT ||
        !eglMakeCurrent(client->display, EGL_NO_SURFACE, EGL_NO_SURFACE, client->context))
        return fail(client, "egl-init", "eglMakeCurrent");
    create_image = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    destroy_image = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    create_sync = (PFNEGLCREATESYNCKHRPROC)eglGetProcAddress("eglCreateSyncKHR");
    destroy_sync = (PFNEGLDESTROYSYNCKHRPROC)eglGetProcAddress("eglDestroySyncKHR");
    duplicate_fence = (PFNEGLDUPNATIVEFENCEFDANDROIDPROC)
        eglGetProcAddress("eglDupNativeFenceFDANDROID");
    image_target = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
        eglGetProcAddress("glEGLImageTargetTexture2DOES");
    if (!create_image || !destroy_image || !create_sync || !destroy_sync ||
        !duplicate_fence || !image_target)
        return fail(client, "egl-init", "entry-points");
    const char *renderer = (const char *)glGetString(GL_RENDERER);
    if (!renderer || !strstr(renderer, "virgl (") || strcasestr(renderer, "llvmpipe") ||
        strcasestr(renderer, "softpipe") || strcasestr(renderer, "software")) {
        fprintf(stderr, "MESA_MULTI_RENDERER_REJECTED role=%u renderer=%s\n",
            client->role, renderer ? renderer : "(null)");
        return fail(client, "renderer", "hardware-virgl");
    }
    glDisable(GL_DITHER);
    char marker[512];
    const int marker_size = snprintf(marker, sizeof(marker),
        "MESA_MULTI_INIT role=%u node=%s renderer=%s egl=%d.%d\n",
        client->role, node + strlen("/dev/dri/"), renderer, major, minor);
    if (marker_size <= 0 || (size_t)marker_size >= sizeof(marker) ||
        write(STDOUT_FILENO, marker, (size_t)marker_size) != marker_size)
        return fail(client, "result", "write-init-marker");
    return 0;
}

static int image_import(
    struct client *client, struct image *target, int fd, uint32_t stride)
{
    const EGLint attributes[] = {
        EGL_WIDTH, WIDTH,
        EGL_HEIGHT, HEIGHT,
        EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_ARGB8888,
        EGL_DMA_BUF_PLANE0_FD_EXT, fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)stride,
        EGL_NONE,
    };
    if (stride < WIDTH * 4u || stride > 4096u)
        return fail(client, "buffer", "stride");
    target->image = create_image(client->display, EGL_NO_CONTEXT,
        EGL_LINUX_DMA_BUF_EXT, NULL, attributes);
    if (target->image == EGL_NO_IMAGE_KHR)
        return fail(client, "buffer", "eglCreateImageKHR");
    glGenTextures(1, &target->texture);
    glBindTexture(GL_TEXTURE_2D, target->texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    image_target(GL_TEXTURE_2D, target->image);
    glGenFramebuffers(1, &target->framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, target->framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
        GL_TEXTURE_2D, target->texture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE ||
        glGetError() != GL_NO_ERROR)
        return fail(client, "buffer", "framebuffer-complete");
    return 0;
}

static int exchange_buffers(struct client *client)
{
    client->own.bo = gbm_bo_create(client->gbm, WIDTH, HEIGHT,
        GBM_FORMAT_ARGB8888, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
    if (!client->own.bo || gbm_bo_get_plane_count(client->own.bo) != 1 ||
        gbm_bo_get_modifier(client->own.bo) != DRM_FORMAT_MOD_LINEAR)
        return fail(client, "buffer", "gbm_bo_create");
    const uint32_t stride = gbm_bo_get_stride(client->own.bo);
    int fd = gbm_bo_get_fd(client->own.bo);
    if (fd < 0) return fail(client, "buffer", "gbm_bo_get_fd");
    int status = image_import(client, &client->own, fd, stride);
    if (!status) status = send_message(client, PHASE_BUFFER, stride, fd);
    if (!status) {
        client->own_dma_buf = fd;
        fd = -1;
    }
    if (fd >= 0 && close(fd) != 0 && !status)
        status = fail(client, "buffer", "close-export");
    uint32_t peer_stride = 0;
    fd = -1;
    if (!status) status = receive_message(
        client, PHASE_BUFFER, &peer_stride, &fd);
    if (!status) status = image_import(client, &client->peer, fd, peer_stride);
    if (!status) {
        client->peer_dma_buf = fd;
        fd = -1;
    }
    if (fd >= 0 && close(fd) != 0 && !status)
        status = fail(client, "buffer", "close-import");
    return status;
}

static int render_fenced(
    struct client *client, const uint8_t color[4], int *out_fence)
{
    glBindFramebuffer(GL_FRAMEBUFFER, client->own.framebuffer);
    glViewport(0, 0, WIDTH, HEIGHT);
    glClearColor(color[0] / 255.0f, color[1] / 255.0f,
        color[2] / 255.0f, color[3] / 255.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    EGLSyncKHR sync = create_sync(
        client->display, EGL_SYNC_NATIVE_FENCE_ANDROID, NULL);
    if (sync == EGL_NO_SYNC_KHR)
        return fail(client, "fence", "eglCreateSyncKHR");
    glFlush();
    const int fd = duplicate_fence(client->display, sync);
    const EGLBoolean destroyed = destroy_sync(client->display, sync);
    if (!destroyed || fd < 0 || glGetError() != GL_NO_ERROR)
        return fail(client, "fence", "eglDupNativeFenceFDANDROID");
    *out_fence = fd;
    return 0;
}

static int verify_pixels(
    struct client *client, const struct image *image, const uint8_t expected[4],
    const char *which, unsigned frame)
{
    uint8_t pixels[WIDTH * HEIGHT * 4];
    glBindFramebuffer(GL_FRAMEBUFFER, image->framebuffer);
    const GLenum framebuffer_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glReadPixels(0, 0, WIDTH, HEIGHT, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    const GLenum error = glGetError();
    if (framebuffer_status != GL_FRAMEBUFFER_COMPLETE || error != GL_NO_ERROR) {
        fprintf(stderr,
            "MESA_MULTI_READBACK_FAIL role=%u which=%s frame=%u fbo=0x%04x gl=0x%04x\n",
            client->role, which, frame, framebuffer_status, error);
        return 1;
    }
    for (size_t offset = 0; offset < sizeof(pixels); offset += 4) {
        if (memcmp(pixels + offset, expected, 4) != 0) {
            fprintf(stderr,
                "MESA_MULTI_PIXEL_MISMATCH role=%u which=%s frame=%u pixel=%zu "
                "actual=%u,%u,%u,%u expected=%u,%u,%u,%u\n",
                client->role, which, frame, offset / 4,
                pixels[offset], pixels[offset + 1], pixels[offset + 2],
                pixels[offset + 3], expected[0], expected[1], expected[2], expected[3]);
            return 1;
        }
    }
    return 0;
}

static int shared_draw(struct client *client)
{
    if (exchange_buffers(client) != 0) return 1;
    for (unsigned frame = 0; frame < FRAMES; ++frame) {
        int fence = -1;
        if (render_fenced(client, colors[client->role][frame], &fence) != 0)
            return 1;
        int status = send_message(client, PHASE_FENCE + frame, frame, fence);
        if (close(fence) != 0 && !status)
            status = fail(client, "fence", "close-export");
        uint32_t peer_frame = UINT32_MAX;
        fence = -1;
        if (!status) status = receive_message(
            client, PHASE_FENCE + frame, &peer_frame, &fence);
        if (!status && (peer_frame != frame || wait_readable(fence, 10000) != 0))
            status = fail(client, "fence", "peer-wait");
        if (fence >= 0 && close(fence) != 0 && !status)
            status = fail(client, "fence", "close-import");
        if (!status) status = verify_pixels(
            client, &client->own, colors[client->role][frame], "own", frame);
        if (!status) status = verify_pixels(
            client, &client->peer, colors[client->role ^ 1u][frame], "peer", frame);
        if (!status) status = send_message(client, PHASE_ACK + frame, frame, -1);
        uint32_t ack = UINT32_MAX;
        if (!status) status = receive_message(
            client, PHASE_ACK + frame, &ack, NULL);
        if (!status && ack != frame) status = fail(client, "ipc", "frame-ack");
        if (status) return status;
    }
    char marker[160];
    const int marker_size = snprintf(marker, sizeof(marker),
        "MESA_MULTI_SHARED role=%u frames=%u pixels=%u "
        "dmabuf_scm=1 sync_file_scm=%u peer=ok\n",
        client->role, FRAMES, 2u * FRAMES * WIDTH * HEIGHT, FRAMES);
    if (marker_size <= 0 || (size_t)marker_size >= sizeof(marker) ||
        write(STDOUT_FILENO, marker, (size_t)marker_size) != marker_size)
        return fail(client, "result", "write-shared-marker");
    return 0;
}

static void image_close(struct client *client, struct image *image)
{
    if (image->framebuffer) glDeleteFramebuffers(1, &image->framebuffer);
    if (image->texture) glDeleteTextures(1, &image->texture);
    if (image->image != EGL_NO_IMAGE_KHR && image->image)
        (void)destroy_image(client->display, image->image);
    if (image->bo) gbm_bo_destroy(image->bo);
    memset(image, 0, sizeof(*image));
}

static void client_close(struct client *client)
{
    image_close(client, &client->peer);
    image_close(client, &client->own);
    if (client->display != EGL_NO_DISPLAY) {
        (void)eglMakeCurrent(client->display,
            EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (client->context != EGL_NO_CONTEXT)
            (void)eglDestroyContext(client->display, client->context);
        (void)eglTerminate(client->display);
        (void)eglReleaseThread();
    }
    if (client->gbm) gbm_device_destroy(client->gbm);
    if (client->peer_dma_buf >= 0) (void)close(client->peer_dma_buf);
    if (client->own_dma_buf >= 0) (void)close(client->own_dma_buf);
    if (client->fd >= 0) (void)close(client->fd);
    client->peer_dma_buf = -1;
    client->own_dma_buf = -1;
    client->fd = -1;
}

static int reopen_worker(void)
{
    struct client client = {
        .role = 2,
        .socket = -1,
        .fd = -1,
        .own_dma_buf = -1,
        .peer_dma_buf = -1,
        .display = EGL_NO_DISPLAY,
        .context = EGL_NO_CONTEXT,
    };
    static const uint8_t color[4] = {0, 255, 0, 255};
    if (client_open(&client) != 0) return 1;
    client.own.bo = gbm_bo_create(client.gbm, WIDTH, HEIGHT,
        GBM_FORMAT_ARGB8888, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
    if (!client.own.bo) return fail(&client, "reopen", "gbm_bo_create");
    int prime_fd = gbm_bo_get_fd(client.own.bo);
    if (prime_fd < 0 || image_import(
            &client, &client.own, prime_fd,
            gbm_bo_get_stride(client.own.bo)) != 0)
        return fail(&client, "reopen", "image-import");
    (void)close(prime_fd);
    int fence = -1;
    if (render_fenced(&client, color, &fence) != 0 ||
        wait_readable(fence, 10000) != 0 || close(fence) != 0 ||
        verify_pixels(&client, &client.own, color, "reopen", 0) != 0)
        return fail(&client, "reopen", "draw-fence-pixels");
    printf("MESA_MULTI_REOPEN draw=1 fence=1 pixels=%u\n", WIDTH * HEIGHT);
    fflush(stdout);
    client_close(&client);
    return 0;
}

static int wait_child(pid_t child, int expected_signal)
{
    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited != child) return -1;
    if (expected_signal)
        return WIFSIGNALED(status) && WTERMSIG(status) == expected_signal ? 0 : -1;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--reopen") == 0)
        return reopen_worker();
    if (argc != 1) return 2;
    int sockets[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) != 0)
        return fail(NULL, "setup", "socketpair");
    pid_t child = fork();
    if (child < 0) return fail(NULL, "setup", "fork");
    struct client client = {
        .role = child == 0 ? 1u : 0u,
        .socket = child == 0 ? sockets[1] : sockets[0],
        .fd = -1,
        .own_dma_buf = -1,
        .peer_dma_buf = -1,
        .display = EGL_NO_DISPLAY,
        .context = EGL_NO_CONTEXT,
    };
    (void)close(child == 0 ? sockets[0] : sockets[1]);
    if (client_open(&client) != 0 || shared_draw(&client) != 0) {
        if (child > 0) {
            (void)kill(child, SIGKILL);
            (void)wait_child(child, SIGKILL);
        }
        return 1;
    }
    if (child == 0) {
        if (client.own_dma_buf < 0 || client.peer_dma_buf < 0 ||
            fcntl(client.own_dma_buf, F_GETFD) < 0 ||
            fcntl(client.peer_dma_buf, F_GETFD) < 0 ||
            send_message(&client, PHASE_KILL_READY, 2, -1) != 0)
            _Exit(1);
        struct pollfd event = {.fd = client.socket, .events = POLLIN};
        for (;;) {
            int status = poll(&event, 1, -1);
            if (status < 0 && errno == EINTR) continue;
            _Exit(1);
        }
    }
    uint32_t ready = 0;
    if (receive_message(&client, PHASE_KILL_READY, &ready, NULL) != 0 || ready != 2 ||
        kill(child, SIGKILL) != 0 || wait_child(child, SIGKILL) != 0)
        return fail(&client, "kill", "SIGKILL-child");
    printf("MESA_MULTI_KILL signal=%d child-reaped=1 held_dmabuf_fds=%u\n",
        SIGKILL, ready);
    fflush(stdout);
    if (fcntl(client.peer_dma_buf, F_GETFD) < 0)
        return fail(&client, "survivor", "peer-dma-buf-fd");
    if (verify_pixels(&client, &client.peer, colors[1][FRAMES - 1],
            "retained-peer", FRAMES - 1) != 0)
        return 1;
    static const uint8_t post_kill[4] = {0, 255, 0, 255};
    int fence = -1;
    if (render_fenced(&client, post_kill, &fence) != 0 ||
        wait_readable(fence, 10000) != 0 || close(fence) != 0 ||
        verify_pixels(&client, &client.own, post_kill, "post-kill", FRAMES) != 0)
        return fail(&client, "survivor", "draw-fence-pixels");
    printf("MESA_MULTI_SURVIVOR retained_peer=1 draw_after_kill=1 fence=1 peer_fd=1\n");
    fflush(stdout);

    pid_t reopened = fork();
    if (reopened < 0) return fail(&client, "reopen", "fork");
    if (reopened == 0) {
        char *const arguments[] = {
            (char *)"lpr_mesa_multi_smoke.elf", (char *)"--reopen", NULL};
        execv("/cmd/lpr_mesa_multi_smoke.elf", arguments);
        _Exit(127);
    }
    if (wait_child(reopened, 0) != 0)
        return fail(&client, "reopen", "worker-status");
    (void)close(client.socket);
    client.socket = -1;
    client_close(&client);
    printf("MESA_MULTI_DONE clients=3 dmabuf_scm=2 sync_file_scm=8 "
        "client_kill=1 reopen=1\n");
    fflush(stdout);
    return 0;
}
