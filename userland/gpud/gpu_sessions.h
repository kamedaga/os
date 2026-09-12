/* SPDX-License-Identifier: MIT */
#ifndef PACHA_GPUD_GPU_SESSIONS_H
#define PACHA_GPUD_GPU_SESSIONS_H

#include <kobox2/gpu.h>

enum { GPUD_GPU_SESSION_CAPACITY = 16 };
enum gpud_gpu_session_state {
    GPUD_GPU_SESSION_FREE,
    GPUD_GPU_SESSION_OPENING,
    GPUD_GPU_SESSION_OPEN,
    GPUD_GPU_SESSION_CLOSING,
    GPUD_GPU_SESSION_FAILED,
};

struct gpud_gpu_session {
    uint64_t id, client, file_cookie;
    uint32_t in_flight;
    enum gpud_gpu_session_state state;
};

/* One owner and one table for the GPU generation, shared by all its channels.
 * client arguments come from authenticated channel bindings, not peer data.
 * file_cookie is a private GPL ownership-ledger key, never serialized.
 */
struct gpud_gpu_sessions {
    uint64_t generation, sequence;
    size_t limit, occupied;
    struct gpud_gpu_session entries[GPUD_GPU_SESSION_CAPACITY];
};

int ph_gpu_sessions_init(struct gpud_gpu_sessions *sessions, uint64_t generation, size_t limit);
uint32_t ph_gpu_session_open_begin(struct gpud_gpu_sessions *sessions,
                                   uint64_t generation,
                                   uint64_t client,
                                   uint32_t node,
                                   uint64_t *id_out);
uint32_t ph_gpu_session_open_finish(struct gpud_gpu_sessions *sessions,
                                    uint64_t generation,
                                    uint64_t client,
                                    uint64_t id,
                                    uint64_t cookie,
                                    uint32_t status);
uint32_t ph_gpu_session_acquire(struct gpud_gpu_sessions *sessions,
                                uint64_t generation,
                                uint64_t client,
                                uint64_t id,
                                uint64_t *cookie_out);
uint32_t ph_gpu_session_release(struct gpud_gpu_sessions *sessions,
                                uint64_t generation,
                                uint64_t client,
                                uint64_t id);
uint32_t ph_gpu_session_close_begin(struct gpud_gpu_sessions *sessions,
                                    uint64_t generation,
                                    uint64_t client,
                                    uint64_t id);
/* BUSY means CLOSING is retained and accepted requests must drain; it is not
 * permission to publish a successful CLOSE or to admit another COMMAND. */
uint32_t ph_gpu_session_close_ready(struct gpud_gpu_sessions *sessions,
                                    uint64_t generation,
                                    uint64_t client,
                                    uint64_t id,
                                    uint64_t *cookie_out);
uint32_t ph_gpu_session_close_finish(struct gpud_gpu_sessions *sessions,
                                     uint64_t generation,
                                     uint64_t client,
                                     uint64_t id,
                                     uint32_t status);

#endif
