#include <pacha/ipc.h>
#include <stdio.h>
static pacha_fd_t held[4200];
int main(void) {
    /* Native callers explicitly grow their capability table. Unlike the
     * Linux runtime, libipc does not prepare capacity before each create. */
    struct pacha_fd_table_info table;
    int status = pacha_fd_table(0, &table);
    if (!status) status = pacha_fd_table(table.capacity - table.free_slots + 4200, &table);
    if (status || table.free_slots < 4200) {
        printf("FDLIMIT_NATIVE_4200_FAIL capacity_status=%d\n", status);
        return 1;
    }
    unsigned n = 0;
    const uint64_t rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_READ | PACHA_FD_RIGHT_WRITE;
    for (; n < 4200; ++n) {
        held[n] = pacha_eventfd_create(0, rights, 0);
        if (held[n] < 16 || held[n] >= PACHA_THREAD_SELF_FD) break;
    }
    unsigned opened = n;
    int failed = opened != 4200;
    while (n) if (pacha_fd_close(held[--n])) failed = 1;
    printf("FDLIMIT_NATIVE_4200_%s opened=%u\n", failed ? "FAIL" : "PASS", opened);
    return failed;
}
