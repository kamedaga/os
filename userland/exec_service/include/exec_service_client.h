#ifndef CAPABILITYOS_EXEC_SERVICE_CLIENT_H
#define CAPABILITYOS_EXEC_SERVICE_CLIENT_H

#include "exec_service_abi.h"

struct exec_service_spawn_options {
    const char *path;
    const char * const *argv;
    unsigned long long argv_count;
    const char * const *envp;
    unsigned long long envp_count;
    unsigned long long request_va;
    unsigned long long response_va;
    unsigned long long wait_ticks;
};

struct exec_service_spawn_result {
    unsigned int status;
    unsigned long long linux_abi_process_slot;
    unsigned long long exec_process_slot;
};

int exec_service_spawn_linux(const struct exec_service_spawn_options *options,
                             struct exec_service_spawn_result *result);

#endif
