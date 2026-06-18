#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int main(int argc, char **argv)
{
    printf("[seed0root] start argc=%d argv0=%s\n",
        argc,
        (argc > 0 && argv != NULL && argv[0] != NULL) ? argv[0] : "(null)");

    struct timespec ts = {0};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        printf("[seed0root] monotonic=%llu.%09llu\n",
            (unsigned long long)ts.tv_sec,
            (unsigned long long)ts.tv_nsec);
    } else {
        fprintf(stderr, "[seed0root] clock_gettime failed\n");
        return 2;
    }

    char *buf = malloc(64);
    if (buf == NULL) {
        fprintf(stderr, "[seed0root] malloc failed\n");
        return 3;
    }
    snprintf(buf, 64, "[seed0root] malloc/stdout OK\n");
    fputs(buf, stdout);
    free(buf);

    fprintf(stderr, "[seed0root] stderr OK\n");
    printf("[seed0root] ready\n");
    fflush(stdout);
    fflush(stderr);
    return 0;
}
