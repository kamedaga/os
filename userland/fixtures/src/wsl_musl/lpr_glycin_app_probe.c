#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

typedef struct _GdkPixbuf GdkPixbuf;
typedef struct _GError {
    unsigned int domain;
    int code;
    char *message;
} GError;

extern GdkPixbuf *gdk_pixbuf_new_from_file(const char *filename,
                                            GError **error);
extern void g_object_unref(void *object);
extern void g_error_free(GError *error);

static uint64_t monotonic_ns(void)
{
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
        return 0;
    }
    return (uint64_t)value.tv_sec * 1000000000ull + (uint64_t)value.tv_nsec;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s IMAGE ITERATIONS\n", argv[0]);
        return 2;
    }
    const int iterations = atoi(argv[2]);
    if (iterations < 1 || iterations > 20) {
        fprintf(stderr, "invalid iteration count\n");
        return 2;
    }
    for (int i = 0; i < iterations; ++i) {
        GError *error = NULL;
        const uint64_t start = monotonic_ns();
        GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file(argv[1], &error);
        const uint64_t end = monotonic_ns();
        if (pixbuf == NULL) {
            fprintf(stderr, "load %d failed: %s\n", i + 1,
                    error != NULL && error->message != NULL ?
                        error->message : "unknown error");
            if (error != NULL) {
                g_error_free(error);
            }
            return 1;
        }
        g_object_unref(pixbuf);
        printf("GLYCIN_APP_PROBE iteration=%d wall_us=%llu\n",
               i + 1,
               (unsigned long long)((end - start) / 1000ull));
    }
    return 0;
}
