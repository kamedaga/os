/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#define GL_GLEXT_PROTOTYPES
#include "live_desktop_frame_profile.h"
#include <X11/Xlib.h>
#include <GL/gl.h>
#include <GL/glx.h>
#include <GL/glxext.h>
#include <cpuid.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { FRAME_LIMIT = 240, WARMUP = 24 };
static const char *phases[] = {"draw", "submit", "gpu_wait", "swap", "present_wait", "retire"};
struct frame_sample { uint64_t cycles[FRAME_PHASE_COUNT], us[FRAME_PHASE_COUNT], total; };
static uint64_t mono_us(void)
{
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time)) exit(1);
    return (uint64_t)time.tv_sec * 1000000 + time.tv_nsec / 1000;
}
static int order(const void *a, const void *b)
{
    const uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}
static void distribution(const char *mode, const char *phase, const char *unit,
    uint64_t *values, unsigned count)
{
    uint64_t total = 0;
    for (unsigned i = 0; i < count; ++i) total += values[i];
    qsort(values, count, sizeof(*values), order);
    printf("FRAME_STAGE mode=%s phase=%s unit=%s n=%u mean=%llu p50=%llu p95=%llu p99=%llu max=%llu\n",
        mode, phase, unit, count, (unsigned long long)(total / count),
        (unsigned long long)values[count / 2], (unsigned long long)values[(count - 1) * 95 / 100],
        (unsigned long long)values[(count - 1) * 99 / 100], (unsigned long long)values[count - 1]);
}
static int check_clock(void)
{
    /* A sleep-only control distinguishes clock drift from presentation cost;
     * the host timestamps these markers, so no assumed CPU frequency is used. */
    for (unsigned i = 0; i < 3; ++i) {
        const uint64_t start = mono_us(), cycles = frame_cycles();
        printf("CLOCK_BEGIN sample=%u mono_us=%llu tsc=%llu\n", i,
            (unsigned long long)start, (unsigned long long)cycles);
        struct timespec remaining = {.tv_sec = 2};
        while (nanosleep(&remaining, &remaining)) {
            if (errno != EINTR) return 1;
        }
        printf("CLOCK_END sample=%u elapsed_us=%llu elapsed_cycles=%llu\n", i,
            (unsigned long long)(mono_us() - start),
            (unsigned long long)(frame_cycles() - cycles));
    }
    return 0;
}
int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc == 2 && !strcmp(argv[1], "clock")) return check_clock();
    if (argc != 2 || (strcmp(argv[1], "pipeline") && strcmp(argv[1], "split") &&
        strcmp(argv[1], "queued"))) return 2;
    const int split = !strcmp(argv[1], "split");
    const int queued = !strcmp(argv[1], "queued");
    void (*set_phase)(unsigned) = NULL, (*reset)(void) = NULL;
    void (*snapshot)(struct frame_trace_snapshot *) = NULL;
    *(void **)(&set_phase) = dlsym(RTLD_DEFAULT, "frame_trace_phase");
    *(void **)(&reset) = dlsym(RTLD_DEFAULT, "frame_trace_reset");
    *(void **)(&snapshot) = dlsym(RTLD_DEFAULT, "frame_trace_snapshot");
    if (!set_phase || !reset || !snapshot) { puts("FRAME_FAIL missing_test_interposer"); return 1; }
    unsigned a, b, c, d;
    const unsigned extended = __get_cpuid_max(0x80000000, NULL);
    d = 0;
    if (extended >= 0x80000007) __cpuid(0x80000007, a, b, c, d);
    struct timespec resolution = {0};
    const int resolution_status = clock_getres(CLOCK_MONOTONIC, &resolution);
    printf("FRAME_CLOCK resolution_status=%d resolution_ns=%llu tsc_invariant=%u cpuid_max=0x%x "
        "tsc_cycles_include_descheduling=1 rpc_phase=application_phase_at_call_start\n",
        resolution_status, (unsigned long long)((uint64_t)resolution.tv_sec * 1000000000 +
            resolution.tv_nsec), extended >= 0x80000007 && !!(d & 256), extended);
    Display *display = XOpenDisplay(NULL);
    if (!display) return 1;
    const int attributes[] = {GLX_X_RENDERABLE, True, GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT,
        GLX_RENDER_TYPE, GLX_RGBA_BIT, GLX_DOUBLEBUFFER, True, GLX_RED_SIZE, 8,
        GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, None};
    int count = 0;
    GLXFBConfig *configs = glXChooseFBConfig(display, DefaultScreen(display), attributes, &count);
    if (!configs || !count) return 1;
    XVisualInfo *visual = glXGetVisualFromFBConfig(display, configs[0]);
    if (!visual) return 1;
    XSetWindowAttributes attr = {.colormap = XCreateColormap(display,
        RootWindow(display, visual->screen), visual->visual, AllocNone)};
    Window window = XCreateWindow(display, RootWindow(display, visual->screen), 20, 20,
        640, 480, 0, visual->depth, InputOutput, visual->visual, CWColormap, &attr);
    XStoreName(display, window, "PachaOS frame interval profiling");
    XMapWindow(display, window); XSync(display, False);
    const uint64_t until = mono_us() + 3000000;
    XWindowAttributes state;
    do {
        if (!XGetWindowAttributes(display, window, &state)) return 1;
        if (state.map_state == IsViewable) break;
        struct timespec pause = {.tv_nsec = 10000000}; nanosleep(&pause, NULL);
    } while (mono_us() < until);
    if (state.map_state != IsViewable) return 1;
    GLXWindow drawable = glXCreateWindow(display, configs[0], window, NULL);
    GLXContext context = glXCreateNewContext(display, configs[0], GLX_RGBA_TYPE, NULL, True);
    XFree(configs);
    if (!drawable || !context || !glXMakeContextCurrent(display, drawable, drawable, context)) return 1;
    const char *renderer = (const char *)glGetString(GL_RENDERER);
    printf("FRAME_RENDERER %s\n", renderer ? renderer : "NULL");
    printf("FRAME_X11 fd=%d\n", ConnectionNumber(display));
    if (!renderer || strstr(renderer, "llvmpipe") || strstr(renderer, "softpipe")) return 1;
    glViewport(0, 0, 640, 480); glClearColor(.25f, .2f, .3f, 1); glClear(GL_COLOR_BUFFER_BIT);
    unsigned char pixel[4]; glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    if (glGetError() || pixel[0] < 63 || pixel[0] > 65 || pixel[1] < 50 ||
        pixel[1] > 52 || pixel[2] < 75 || pixel[2] > 78 || pixel[3] != 255) return 1;
    PFNGLXGETSYNCVALUESOMLPROC get = (PFNGLXGETSYNCVALUESOMLPROC)glXGetProcAddressARB((const GLubyte *)"glXGetSyncValuesOML");
    PFNGLXWAITFORSBCOMLPROC wait = (PFNGLXWAITFORSBCOMLPROC)glXGetProcAddressARB((const GLubyte *)"glXWaitForSbcOML");
    PFNGLXGETMSCRATEOMLPROC rate = (PFNGLXGETMSCRATEOMLPROC)glXGetProcAddressARB((const GLubyte *)"glXGetMscRateOML");
    int32_t numerator = 0, denominator = 0;
    int64_t ust = 0, msc = 0, sbc = 0;
    if (!get || !wait || !rate || !get(display, drawable, &ust, &msc, &sbc) ||
        !rate(display, drawable, &numerator, &denominator) || numerator <= 0 || denominator <= 0) return 1;
    printf("FRAME_DISPLAY rate=%d/%d mode=%s window=640x480 warmup=%u measured=%u "
        "queue_limit=%u explicit_gpu_wait=%u "
        "fps_scope=client_swap_completion not_entire_desktop=1\n", numerator, denominator,
        argv[1], WARMUP, FRAME_LIMIT, queued ? 3 : 1, split);
    struct frame_sample samples[FRAME_LIMIT] = {0};
    uint64_t start = 0, start_cycles = 0;
    int64_t first_msc = 0, first_sbc = 0;
    int64_t submitted = sbc;
    for (unsigned frame = 0; frame < FRAME_LIMIT + WARMUP; ++frame) {
        if (frame == WARMUP) {
            if (!wait(display, drawable, submitted, &ust, &msc, &sbc)) return 1;
            reset(); start = mono_us(); start_cycles = frame_cycles();
            first_msc = msc; first_sbc = sbc;
            printf("FRAME_BEGIN mode=%s mono_us=%llu tsc=%llu\n", argv[1],
                (unsigned long long)start, (unsigned long long)start_cycles);
        }
        struct frame_sample sample = {0};
        uint64_t previous_cycles = frame_cycles(), previous_us = mono_us();
        GLsync fence = NULL;
        for (unsigned phase = 0; phase < FRAME_PHASE_COUNT; ++phase) {
            set_phase(phase);
            switch (phase) {
            case FRAME_PHASE_DRAW:
                glClearColor(.1f, .15f, (frame & 1) ? .2f : .3f, 1);
                glClear(GL_COLOR_BUFFER_BIT);
                glMatrixMode(GL_MODELVIEW); glLoadIdentity(); glRotatef(frame * 2, 0, 0, 1);
                glBegin(GL_TRIANGLES);
                glColor3f(1, 0, 0); glVertex2f(-.7f, -.6f);
                glColor3f(0, 1, 0); glVertex2f(.7f, -.6f);
                glColor3f(0, 0, 1); glVertex2f(0, .7f); glEnd();
                if (split) fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
                break;
            case FRAME_PHASE_SUBMIT: glFlush(); break;
            case FRAME_PHASE_GPU_WAIT:
                if (split) {
                    if (!fence) return 1;
                    const GLenum result = glClientWaitSync(fence, 0, UINT64_C(2000000000));
                    if (result != GL_ALREADY_SIGNALED && result != GL_CONDITION_SATISFIED) {
                        printf("FRAME_FAIL gpu_wait=0x%x frame=%u\n", result, frame); return 1;
                    }
                }
                break;
            case FRAME_PHASE_SWAP:
                glXSwapBuffers(display, drawable); ++submitted; break;
            case FRAME_PHASE_PRESENT:
                /* Bound outstanding swaps without imposing the one-frame
                 * serialization of the diagnostic pipeline/split controls. */
                if ((!queued || submitted - sbc >= 3 ||
                    frame + 1 == FRAME_LIMIT + WARMUP) &&
                    !wait(display, drawable, queued && frame + 1 != FRAME_LIMIT + WARMUP ?
                        submitted - 2 : submitted, &ust, &msc, &sbc)) {
                    printf("FRAME_FAIL stage=present frame=%u submitted=%lld sbc=%lld msc=%lld\n",
                        frame, (long long)submitted, (long long)sbc, (long long)msc);
                    return 1;
                }
                break;
            case FRAME_PHASE_RETIRE:
                if (fence) glDeleteSync(fence);
                { const GLenum error = glGetError();
                  if (error) {
                      printf("FRAME_FAIL stage=retire frame=%u gl_error=0x%x\n", frame, error);
                      return 1;
                  } }
                break;
            }
            const uint64_t cycle_stamp = frame_cycles(), stamp = mono_us();
            sample.cycles[phase] = cycle_stamp - previous_cycles;
            sample.us[phase] = stamp - previous_us;
            sample.total += sample.us[phase];
            previous_cycles = cycle_stamp; previous_us = stamp;
        }
        if (frame >= WARMUP) samples[frame - WARMUP] = sample;
    }
    const uint64_t elapsed = mono_us() - start, elapsed_cycles = frame_cycles() - start_cycles;
    set_phase(FRAME_PHASE_COUNT);
    if (!elapsed || sbc <= first_sbc) {
        printf("FRAME_FAIL stage=completion first_msc=%lld msc=%lld first_sbc=%lld sbc=%lld elapsed_us=%llu\n",
            (long long)first_msc, (long long)msc, (long long)first_sbc,
            (long long)sbc, (unsigned long long)elapsed);
        return 1;
    }
    /* Virtio scanout has no hardware vblank counter on this driver. Its X11
     * swap-completion count still measures client progress, but cannot prove
     * refresh pacing. Never turn a zero MSC delta into a fictitious FPS. */
    const int scanout_counter_valid = msc > first_msc;
    printf("FRAME_END mode=%s completed=%lld elapsed_us=%llu elapsed_cycles=%llu "
        "monotonic_fps=%.3f msc_elapsed=%lld display_completion_fps=%.3f ust=%lld scanout_counter_valid=%u\n",
        argv[1], (long long)(sbc - first_sbc), (unsigned long long)elapsed,
        (unsigned long long)elapsed_cycles, FRAME_LIMIT * 1000000.0 / elapsed,
        (long long)(msc - first_msc), scanout_counter_valid ?
            (sbc - first_sbc) * (double)numerator / denominator / (msc - first_msc) : 0.0,
        (long long)ust, scanout_counter_valid);
    for (unsigned phase = 0; phase < FRAME_PHASE_COUNT; ++phase) {
        uint64_t values[FRAME_LIMIT];
        for (unsigned i = 0; i < FRAME_LIMIT; ++i) values[i] = samples[i].us[phase];
        distribution(argv[1], phases[phase], "monotonic_us", values, FRAME_LIMIT);
        for (unsigned i = 0; i < FRAME_LIMIT; ++i) values[i] = samples[i].cycles[phase];
        distribution(argv[1], phases[phase], "tsc_cycles", values, FRAME_LIMIT);
    }
    uint64_t totals[FRAME_LIMIT];
    for (unsigned i = 0; i < FRAME_LIMIT; ++i) totals[i] = samples[i].total;
    distribution(argv[1], "frame_total", "monotonic_us", totals, FRAME_LIMIT);
    for (unsigned i = 0; i < FRAME_LIMIT; ++i) {
        totals[i] = 0;
        for (unsigned phase = 0; phase < FRAME_PHASE_COUNT; ++phase)
            totals[i] += samples[i].cycles[phase];
    }
    distribution(argv[1], "frame_total", "tsc_cycles", totals, FRAME_LIMIT);
    unsigned selected[8] = {0};
    for (unsigned rank = 0; rank < 8; ++rank) {
        unsigned best = FRAME_LIMIT;
        uint64_t maximum = 0;
        for (unsigned frame = 0; frame < FRAME_LIMIT; ++frame) {
            unsigned seen = 0;
            for (unsigned i = 0; i < rank; ++i) seen |= selected[i] == frame;
            if (seen) continue;
            uint64_t cycles = 0;
            for (unsigned phase = 0; phase < FRAME_PHASE_COUNT; ++phase)
                cycles += samples[frame].cycles[phase];
            if (best == FRAME_LIMIT || cycles > maximum) { best = frame; maximum = cycles; }
        }
        selected[rank] = best;
        printf("FRAME_OUTLIER mode=%s rank=%u frame=%u total_us=%llu", argv[1], rank,
            best, (unsigned long long)samples[best].total);
        for (unsigned phase = 0; phase < FRAME_PHASE_COUNT; ++phase)
            printf(" %s_cycles=%llu", phases[phase],
                (unsigned long long)samples[best].cycles[phase]);
        putchar('\n');
    }
    struct frame_trace_snapshot traced; snapshot(&traced);
    for (unsigned i = 0; i < FRAME_TRACE_SLOTS; ++i) {
        const struct frame_trace_entry *entry = &traced.entries[i];
        if (!entry->calls) continue;
        printf("FRAME_RPC mode=%s phase=%s operation=0x%x calls=%llu cycles=%llu max=%llu errors=%llu\n",
            argv[1], phases[entry->phase], entry->operation, (unsigned long long)entry->calls,
            (unsigned long long)entry->cycles, (unsigned long long)entry->maximum,
            (unsigned long long)entry->errors);
    }
    printf("FRAME_RPC_OVERFLOW mode=%s count=%llu\n", argv[1], (unsigned long long)traced.overflow);
    for (unsigned i = 0; i < FRAME_POLL_SLOTS; ++i) {
        const struct frame_trace_poll_entry *entry = &traced.polls[i];
        if (!entry->calls) continue;
        printf("FRAME_POLL mode=%s phase=%s fd=%d x11=%u nfds=%u timeout_ms=%d "
            "events=0x%x revents=0x%x calls=%llu cycles=%llu max=%llu "
            "ready=%llu timeouts=%llu errors=%llu\n", argv[1], phases[entry->phase],
            entry->fd, entry->fd == ConnectionNumber(display), entry->nfds, entry->timeout,
            entry->events, entry->revents, (unsigned long long)entry->calls,
            (unsigned long long)entry->cycles, (unsigned long long)entry->maximum,
            (unsigned long long)entry->ready, (unsigned long long)entry->timeouts,
            (unsigned long long)entry->errors);
    }
    printf("FRAME_POLL_OVERFLOW mode=%s count=%llu\n", argv[1],
        (unsigned long long)traced.poll_overflow);
    /* No timed-loop prints: diagnostics must not manufacture frame stalls. */
    glXMakeContextCurrent(display, None, None, NULL); glXDestroyContext(display, context);
    glXDestroyWindow(display, drawable); XDestroyWindow(display, window);
    XFreeColormap(display, attr.colormap); XFree(visual); XCloseDisplay(display);
    return 0;
}
