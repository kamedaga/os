/* Actual owner admission helpers + real socket/pthread round trips. No GPU backend. */
#define main lifetime_fixture_main
#include "gpud_push_service_unit.c"
#undef main
#include <inttypes.h>
struct client_args { int fd, rounds; uint64_t *latency; };
static uint64_t request_now(void) {
    struct timespec t; assert(!clock_gettime(CLOCK_MONOTONIC, &t));
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
static void *client_roundtrips(void *arg) {
    struct client_args *a = arg;
    for (int i = 0; i < a->rounds; i++) {
        uint64_t begin = request_now(), value = (uint64_t)i, reply;
        assert(write(a->fd, &value, 8) == 8);
        assert(read(a->fd, &reply, 8) == 8 && reply == value);
        a->latency[i] = request_now() - begin;
    }
    return NULL;
}
static int compare_ns(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}
static uint64_t roundtrip(unsigned n, int mode) {
    enum { ROUNDS = 2000 };
    uint64_t latency[ROUNDS]; int pair[2];
    assert(!socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair));
    pair[0] = lift(pair[0]); pair[1] = lift(pair[1]);
    static struct gpud_drm_service s; memset(&s, 0, sizeof(s));
    struct gpud_drm_connection connection = {.fd = pair[0]};
    s.connections = &connection; s.ready = gpud_ready_create(); assert(s.ready);
    struct gpud_ready_source **sources = calloc(n + 1, sizeof(*sources));
    pacha_fd_t *fds = calloc(n + 1, sizeof(*fds));
    struct pollfd *oldfds = calloc(n + 1, sizeof(*oldfds));
    assert(sources && fds && oldfds);
    for (unsigned i = 0; i < n; i++) {
        assert(!gpud_ready_os_event(fds + i));
        assert(!gpud_ready_add(s.ready, fds[i], PACHA_FD_EVENT_HANGUP, 1, NULL, sources + i));
        oldfds[i] = (struct pollfd){fds[i], POLLIN, 0};
    }
    oldfds[n] = (struct pollfd){pair[0], POLLIN, 0};
    if (mode == 1)
        assert(!gpud_ready_add(s.ready, pair[0], PACHA_FD_EVENT_READABLE, 2, NULL, sources + n));
    else if (mode == 2)
        assert(!gpud_ready_add(s.ready, pair[0], PACHA_FD_EVENT_HANGUP, 2, NULL, sources + n));
    struct client_args args = {pair[1], ROUNDS, latency}; pthread_t client;
    assert(!pthread_create(&client, NULL, client_roundtrips, &args));
    for (int i = 0; i < ROUNDS; i++) {
        if (mode == 0) {
            /* Old pre-receive whole-pool readiness scan, then direct wait. */
            assert(poll(oldfds, n + 1, 0) >= 0);
            struct pollfd request = {pair[0], POLLIN, 0};
            assert(poll(&request, 1, -1) > 0);
            /* Real old main loops through pre-receive reap after wake too. */
            assert(poll(oldfds, n + 1, 0) >= 0);
        } else if (mode == 1) {
            struct gpud_ready_event e;
            while (!gpud_ready_next(s.ready, &e)) {
                struct pollfd p = {gpud_ready_fd(s.ready), POLLIN, 0};
                assert(poll(&p, 1, -1) > 0);
            }
            assert(e.source == sources[n]);
        } else {
            assert(!refresh_requests(&s));
            if (!connection.ready) {
                assert(!gpud_drm_service_reserve_pollfds(&s, 0));
                size_t k = gpud_drm_service_pollfds(&s, s.pollfds, s.poll_capacity);
                assert(k == 2); /* independent of n */
                struct pollfd direct[2];
                for (size_t j = 0; j < k; j++) direct[j] = (struct pollfd){s.pollfds[j].fd, POLLIN, 0};
                assert(poll(direct, k, -1) > 0);
                assert(!refresh_requests(&s) && connection.ready);
            }
        }
        uint64_t v; assert(read(pair[0], &v, 8) == 8 && v == (uint64_t)i);
        assert(write(pair[0], &v, 8) == 8);
        if (mode == 1) assert(!gpud_ready_rearm(sources[n]));
    }
    assert(!pthread_join(client, NULL));
    qsort(latency + 100, ROUNDS - 100, sizeof(*latency), compare_ns);
    printf("REQUEST mode=%s N=%u p50_us=%.3f p99_us=%.3f\n",
           mode == 0 ? "old-full-poll" : mode == 1 ? "mc7-hop" : "mc8-direct", n,
           latency[100 + (ROUNDS - 100)/2]/1000., latency[100 + (ROUNDS - 100)*99/100]/1000.);
    uint64_t median = latency[100 + (ROUNDS - 100)/2];
    if (mode) assert(!gpud_ready_remove(sources + n));
    for (unsigned i = 0; i < n; i++) {
        assert(!gpud_ready_remove(sources + i)); assert(!gpud_ready_os_close(fds[i]));
    }
    assert(!gpud_ready_destroy(s.ready)); close(pair[0]); close(pair[1]);
    free(sources); free(fds); free(oldfds); free(s.pollfds);
    return median;
}
int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    unsigned counts[] = {0,256,1024,4096,16384};
    uint64_t minimum = UINT64_MAX, maximum = 0;
    for (unsigned i = 0; i < sizeof(counts)/sizeof(*counts); i++) {
        uint64_t results[3];
        for (int mode = 0; mode < 3; mode++) results[mode] = roundtrip(counts[i], mode);
        if (getenv("GPUD_BENCH_PIN") && strcmp(getenv("GPUD_BENCH_PIN"), "0"))
            assert(results[2] <= results[0] * 1.25 + 1000); /* timing noise budget */
        if (results[2] < minimum) minimum = results[2];
        if (results[2] > maximum) maximum = results[2];
    }
    if (getenv("GPUD_BENCH_PIN") && strcmp(getenv("GPUD_BENCH_PIN"), "0"))
        assert(maximum <= minimum * 1.5 + 1000);
    puts("REQUEST_LATENCY_PASS: actual owner admission, thread-hop and direct-wait round trips");
}
