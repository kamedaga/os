#include <stdio.h>

#include "filed/runtime.h"

int main(int argc, char **argv)
{
    static filed_runtime_t runtime;
    int status;

    (void)argc;
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

    status = filed_runtime_serve(&runtime);
    printf("[filed] fatal stage=serve status=%d\n", status);
    fflush(stdout);
    return 1;
}
