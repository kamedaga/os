#include "service.h"
#include "pacha/bootstrap.h"
#include "pacha/ipc.h"
#include "pacha/syscall.h"

#include <stdio.h>

int main(int argc, char **argv)
{
    (void)argc;
    struct unix_boot_config config = {0};
    const int bootstrap = pacha_bootstrap_fd_from_argv(argv);
    if (bootstrap < 16 ||
        pacha_fd_read(bootstrap, &config, sizeof(config)) != (long)sizeof(config) ||
        config.magic != UNIX_BOOT_MAGIC || config.version != UNIX_SERVICE_VERSION ||
        config.admin_endpoint < 16 || config.filed_path_channel < 16 || config.ready_channel < 16) {
        fputs("[unixd] invalid bootstrap\n", stderr);
        return 2;
    }
    (void)pacha_fd_close(bootstrap);
    /* The launch policy already supplies a private client-only path handle. */
    if (config.filed_path_channel >= PACHA_FD_TABLE_LIMIT) return 2;
    return unix_service_run(&config);
}
