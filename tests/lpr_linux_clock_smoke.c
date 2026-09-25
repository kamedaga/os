#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "CLOCK_FAIL line=%d\n", __LINE__); return 1; } } while (0)

static int reader(time_t initial)
{
    struct timespec last = {.tv_sec = initial}, now;
    for (int i = 0; i < 2000; ++i) {
        CHECK(clock_gettime(CLOCK_REALTIME, &now) == 0);
        CHECK(now.tv_sec >= last.tv_sec && now.tv_sec - initial < 15);
        CHECK(now.tv_nsec == 0); /* Preserve the existing native resolution. */
        last = now;
    }
    return 0;
}

int main(void)
{
    struct timespec start, finish, resolution, delay = {.tv_sec = 2, .tv_nsec = 200000000};
    CHECK(clock_getres(CLOCK_REALTIME, &resolution) == 0);
    CHECK(resolution.tv_sec == 1 && resolution.tv_nsec == 0);
    CHECK(clock_gettime(CLOCK_REALTIME, &start) == 0 && start.tv_sec > 1700000000);
    printf("CLOCK_START sec=%lld\n", (long long)start.tv_sec);
    fflush(stdout);
    pid_t children[4];
    for (int i = 0; i < 4; ++i) {
        children[i] = fork();
        CHECK(children[i] >= 0);
        if (children[i] == 0) _exit(reader(start.tv_sec));
    }
    for (int i = 0; i < 4; ++i) {
        int status;
        CHECK(waitpid(children[i], &status, 0) == children[i]);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    CHECK(nanosleep(&delay, NULL) == 0);
    CHECK(clock_gettime(CLOCK_REALTIME, &finish) == 0);
    CHECK(finish.tv_sec - start.tv_sec >= 2 && finish.tv_sec - start.tv_sec < 15);
    printf("CLOCK_SMOKE_PASS start=%lld end=%lld readers=4 calls=8000\n",
           (long long)start.tv_sec, (long long)finish.tv_sec);
    return 0;
}
