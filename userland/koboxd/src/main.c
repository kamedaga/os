#include "bootstrap.h"
#include "ipc_service.h"
#include "storage_runtime.h"

#include <stdint.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    (void)argc;
    printf("[koboxd] start\n");
    fflush(stdout);
    koboxd_ipc_service_t ipc_service;
    koboxd_ipc_service_init(&ipc_service);
    int bootstrap_fd = -1;
    koboxd_bootstrap_t bootstrap;
    int status = koboxd_find_bootstrap_fd(argv, &bootstrap_fd);
    printf("[koboxd] bootstrap fd=%d status=%d\n", bootstrap_fd, status);
    fflush(stdout);
    if (status != 0) {
        return 4;
    }
    status = koboxd_read_bootstrap_fd(bootstrap_fd, &bootstrap);
    printf("[koboxd] bootstrap read status=%d magic=0x%llx device_fd=%llu control_fd=%llu modules=%llu\n",
        status,
        (unsigned long long)bootstrap.magic,
        (unsigned long long)bootstrap.device_fd,
        (unsigned long long)bootstrap.control_fd,
        (unsigned long long)bootstrap.module_count);
    fflush(stdout);
    if (status != 0 ||
        koboxd_validate_bootstrap_package(&bootstrap, sizeof(bootstrap)) != 0) {
        return 4;
    }
    if (koboxd_run_storage(&ipc_service, &bootstrap) != 0) {
        return 5;
    }
    koboxd_ipc_service_debug_dump(&ipc_service, stdout);
    printf("[koboxd] ready\n");
    fflush(stdout);
    fflush(stderr);
    return 0;
}
