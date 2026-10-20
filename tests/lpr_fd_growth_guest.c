#include <sys/eventfd.h>
#include <sys/resource.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
static int held[4200];
int main(int argc, char **argv)
{
    struct rlimit limit;
    if (argc > 1) {
        if (getrlimit(RLIMIT_NOFILE, &limit) || limit.rlim_cur != 3 || limit.rlim_max != 8192) return 10;
        errno = 0;
        if (eventfd(0, EFD_CLOEXEC) != -1 || errno != EMFILE) return 11;
        puts("FDLIMIT_LPR_4200_PASS opened=4200 exec_soft=3 exec_hard=8192 EMFILE=OK");
        return 0;
    }
    /* Leave room for LPR internal reserved descriptor numbers as well as stdio. */
    limit = (struct rlimit){8192, 8192};
    if (setrlimit(RLIMIT_NOFILE, &limit)) return 1;
    unsigned n = 0;
    for (; n < 4200; ++n) {
        held[n] = eventfd(0, EFD_CLOEXEC);
        if (held[n] < 0) break;
    }
    unsigned opened = n;
    while (n) if (close(held[--n])) return 2;
    if (opened != 4200) { printf("FDLIMIT_LPR_FAIL opened=%u errno=%d\n", opened, errno); return 3; }
    limit.rlim_cur = 3;
    if (setrlimit(RLIMIT_NOFILE, &limit)) return 4;
    execl(argv[0], argv[0], "--inherited", (char *)0);
    return 5;
}
