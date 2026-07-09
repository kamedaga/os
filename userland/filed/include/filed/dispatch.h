#pragma once

#include "filed/runtime.h"

int filed_dispatch_client_once(filed_runtime_t *runtime, int client_fd);
int filed_dispatch_session_once(filed_runtime_t *runtime, uint64_t session_index);
int filed_dispatch_sync_all(filed_runtime_t *runtime);
