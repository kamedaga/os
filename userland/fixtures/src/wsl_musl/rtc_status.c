#include <stdio.h>
#include <time.h>

int main(int argc, char **argv) {
    time_t now = time(NULL);
    struct tm tmv;
    if (gmtime_r(&now, &tmv) == NULL) {
        puts("rtc_status: gmtime failed");
        return 1;
    }
    if (argc >= 2) {
        printf("%04d-%02d-%02d %02d:%02d:%02d\n",
            tmv.tm_year + 1900,
            tmv.tm_mon + 1,
            tmv.tm_mday,
            tmv.tm_hour,
            tmv.tm_min,
            tmv.tm_sec);
        return 0;
    }
    printf("unix=%lld\n", (long long)now);
    printf("utc=%04d-%02d-%02dT%02d:%02d:%02dZ\n",
        tmv.tm_year + 1900,
        tmv.tm_mon + 1,
        tmv.tm_mday,
        tmv.tm_hour,
        tmv.tm_min,
        tmv.tm_sec);
    return 0;
}
