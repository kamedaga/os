#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>

#include "filed/runtime.h"
#include "filed/live_bootstrap.h"
#include "pacha/ipc.h"

int main(int argc, char **argv)
{
    static filed_runtime_t runtime;
    int status;

    (void)argc;
    (void)setenv("KOBOX_DAEMON_NAME", "filed", 1);
    filed_runtime_init(&runtime);

    status = filed_runtime_bootstrap(&runtime, argv);
    if (status != 0) {
        printf("[filed] fatal stage=bootstrap status=%d\n", status);
        fflush(stdout);
        return 1;
    }

    status = filed_runtime_mount_root(&runtime);
    if (status != 0) {
        printf("[filed] fatal stage=mount-root status=%d\n", status);
        fflush(stdout);
        return 1;
    }
    printf("[filed] ready\n");
    fflush(stdout);
    if (runtime.live_root) {
        const struct pacha_ipc_msg ready = { .word0 = FILED_LIVE_READY_MAGIC };
        status = pacha_ipc_send(runtime.live_ready_fd, &ready);
        (void)pacha_fd_close(runtime.live_ready_fd);
        runtime.live_ready_fd = -1;
        if (status != 0) {
            printf("[filed] fatal stage=live-ready status=%d\n", status);
            fflush(stdout);
            return 1;
        }
    }

    status = filed_runtime_serve(&runtime);
    printf("[filed] fatal stage=serve status=%d\n", status);
    fflush(stdout);
    return 1;
}
