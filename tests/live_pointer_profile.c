/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <linux/input.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

enum { DEVICES = 16, BUCKETS = 1001 };
struct distribution { uint64_t count, sum, max, buckets[BUCKETS]; };
struct pointer {
    int fd;
    unsigned index;
    uint64_t frames, reads, max_batch, dropped, future, backwards;
    uint64_t previous_stamp, previous_receive;
    int64_t dx, dy;
    struct distribution age, capture_interval, delivery_interval;
};
static struct pointer pointers[DEVICES];

/* This reader neither grabs input nor logs each report: tracing itself must
 * not become the event-loop latency being measured. */
static uint64_t now_us(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) { perror("clock"); exit(1); }
    return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}
static void sample(struct distribution *d, uint64_t value) {
    d->count++; d->sum += value;
    if (value > d->max) d->max = value;
    unsigned bucket = value / 100 < BUCKETS ? (unsigned)(value / 100) : BUCKETS - 1;
    d->buckets[bucket]++;
}
static void report(unsigned node, const char *name, const struct distribution *d) {
    uint64_t seen = 0, percentile = 0;
    for (unsigned i = 0; i < BUCKETS; ++i) {
        seen += d->buckets[i];
        if (seen >= (d->count * 95 + 99) / 100) { percentile = (i + 1) * 100; break; }
    }
    printf("POINTER_TIMING node=event%u span=%s samples=%llu mean_us=%llu p95_bucket_upper_us=%llu max_us=%llu overflow_bucket=%llu\n",
        node, name, (unsigned long long)d->count,
        (unsigned long long)(d->count ? d->sum / d->count : 0),
        (unsigned long long)(d->count ? percentile : 0),
        (unsigned long long)d->max, (unsigned long long)d->buckets[BUCKETS - 1]);
}
static void consume(struct pointer *p, const struct input_event *events,
    unsigned count, uint64_t received) {
    unsigned frames = 0;
    for (unsigned i = 0; i < count; ++i) {
        const struct input_event *e = &events[i];
        if (e->type == EV_REL && e->code == REL_X) p->dx += e->value;
        if (e->type == EV_REL && e->code == REL_Y) p->dy += e->value;
        if (e->type != EV_SYN) continue;
        if (e->code == SYN_DROPPED) { p->dropped++; p->previous_stamp = 0; continue; }
        if (e->code != SYN_REPORT) continue;
        uint64_t stamp = (uint64_t)e->time.tv_sec * 1000000 + e->time.tv_usec;
        p->frames++; frames++;
        if (stamp > received) p->future++;
        else sample(&p->age, received - stamp);
        if (p->previous_stamp) {
            if (stamp < p->previous_stamp) p->backwards++;
            else sample(&p->capture_interval, stamp - p->previous_stamp);
        }
        p->previous_stamp = stamp;
    }
    if (frames) {
        if (p->previous_receive) sample(&p->delivery_interval, received - p->previous_receive);
        p->previous_receive = received;
        if (frames > p->max_batch) p->max_batch = frames;
    }
}
int main(int argc, char **argv) {
    char *end = NULL;
    unsigned long seconds = argc == 2 ? strtoul(argv[1], &end, 10) : 0;
    if (argc != 2 || !end || *end || seconds < 1 || seconds > 120) {
        fprintf(stderr, "usage: %s SECONDS (1..120)\n", argv[0]); return 2;
    }
    unsigned devices = 0;
    struct pollfd pollers[DEVICES];
    for (unsigned i = 0; i < 64 && devices < DEVICES; ++i) {
        char path[64], name[128] = {0};
        snprintf(path, sizeof(path), "/dev/input/event%u", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        unsigned char rel[8] = {0};
        if (ioctl(fd, EVIOCGBIT(EV_REL, sizeof(rel)), rel) < 0 || (rel[0] & 3) != 3) {
            close(fd); continue;
        }
        int clock = CLOCK_MONOTONIC;
        if (ioctl(fd, EVIOCSCLOCKID, &clock)) { perror("clock id"); close(fd); continue; }
        (void)ioctl(fd, EVIOCGNAME(sizeof(name)), name);
        printf("POINTER_DEVICE node=event%u name=%s\n", i, name);
        pointers[devices].fd = fd; pointers[devices].index = i;
        pollers[devices++] = (struct pollfd){.fd = fd, .events = POLLIN};
    }
    if (!devices) { fputs("No relative pointer input\n", stderr); return 1; }
    uint64_t start = now_us(), until = start + seconds * 1000000;
    printf("POINTER_READY seconds=%lu no_grab=1 no_per_event_log=1\n", seconds);
    fflush(stdout);
    int status = 0;
    while (now_us() < until) {
        int ready = poll(pollers, devices, 100);
        if (ready < 0) { if (errno == EINTR) continue; perror("poll"); status = 1; break; }
        for (unsigned i = 0; i < devices; ++i) {
            if (pollers[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                pollers[i].fd = -1; continue;
            }
            if (!(pollers[i].revents & POLLIN)) continue;
            struct input_event events[64];
            /* Fairness and the absolute deadline also hold under 1000-Hz
             * traffic; a disconnected node cannot spin the sampling loop. */
            for (unsigned batch = 0; batch < 8 && now_us() < until; ++batch) {
                ssize_t n = read(pointers[i].fd, events, sizeof(events));
                if (n < 0 && errno == EINTR) continue;
                if (n < 0 && errno == EAGAIN) break;
                if (n <= 0 || n % sizeof(events[0])) {
                    pollers[i].fd = -1; status = 1; break;
                }
                pointers[i].reads++;
                consume(&pointers[i], events, (unsigned)n / sizeof(events[0]), now_us());
            }
        }
    }
    printf("POINTER_FINISHED elapsed_us=%llu\n", (unsigned long long)(now_us() - start));
    for (unsigned i = 0; i < devices; ++i) {
        struct pointer *p = &pointers[i];
        printf("POINTER_COUNTS node=event%u frames=%llu reads=%llu max_frames_per_read=%llu dropped=%llu future=%llu backwards=%llu dx=%lld dy=%lld\n",
            p->index, (unsigned long long)p->frames, (unsigned long long)p->reads,
            (unsigned long long)p->max_batch, (unsigned long long)p->dropped,
            (unsigned long long)p->future, (unsigned long long)p->backwards,
            (long long)p->dx, (long long)p->dy);
        report(p->index, "capture_to_evdev", &p->age);
        report(p->index, "capture_interval", &p->capture_interval);
        report(p->index, "delivery_interval", &p->delivery_interval);
        close(p->fd);
    }
    return status;
}
