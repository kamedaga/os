#include "lpr_filed_internal.h"

__attribute__((visibility("hidden")))
void *memset(void *dst, int c, size_t n)
{
    return lpr_memset(dst, c, n);
}

lpr_state_t lpr_state = {
    .thread_count = 1,
    .filed_rpc = {
        .request_id = 0x4c505246494c4501ull,
        .wire_page_fd = -1,
        .session_fd = -1,
        .session_page_fd = -1,
        .readv_vmo_fd = -1,
        .pread_vmo_page_fd = -1,
    },
    .termd_rpc = {
        .request_id = 0x4c50525445524d01ull,
        .wire_page_fd = -1,
    },
    .netd_rpc = {
        .request_id = 0x4c50524e45544401ull,
        .page_fd = -1,
        .next_ephemeral_port = 49152u,
    },
};
