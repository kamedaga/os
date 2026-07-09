#include "filed/dispatch.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "filed/fd_ipc.h"
#include "filed/exec.h"
#include "filed/exec_linux_lpr.h"
#include "filed/payload_v2.h"
#include "filed/ipc_protocol_v2.h"
#include "filed/page_cache.h"
#include "filed/tmpfs_backend.h"
#include "pacha/abi.h"
#include "pacha/ipc.h"
#include "pacha/status.h"
#include "pacha/syscall.h"
#include "pacha/trace.h"
#include "personality/linux_lpr.h"
#include "termd/ipc_protocol_v2.h"
#include "../internal/dispatch_state.h"

static void filed_file_vmo_cache_invalidate_object(filed_runtime_t *runtime, uint64_t backend_object);

#define filed_dispatch_metrics (runtime->dispatch_state->dispatch_metrics)
#define filed_fast_metrics (runtime->dispatch_state->fast_metrics)
#define filed_fast_op_metrics (runtime->dispatch_state->fast_op_metrics)
#define filed_page_cache (runtime->dispatch_state->page_cache)
#define filed_dir_cache (runtime->dispatch_state->dir_cache)
#define filed_negative_lookup_cache (runtime->dispatch_state->negative_lookup_cache)
#define filed_target_lookup_vfs_hits (runtime->dispatch_state->target_lookup_vfs_hits)
#define filed_target_lookup_backend_hits (runtime->dispatch_state->target_lookup_backend_hits)
#define filed_target_lookup_misses (runtime->dispatch_state->target_lookup_misses)
#define filed_file_vmo_cache_hits (runtime->dispatch_state->file_vmo_cache_hits)
#define filed_file_vmo_cache_misses (runtime->dispatch_state->file_vmo_cache_misses)
#define filed_file_vmo_cache_stores (runtime->dispatch_state->file_vmo_cache_stores)
#define filed_file_vmo_cache_evictions (runtime->dispatch_state->file_vmo_cache_evictions)
