/* SPDX-License-Identifier: MIT */
#ifndef PACHA_GPUD_GPU_RPC_H
#define PACHA_GPUD_GPU_RPC_H

#include "gpu_channel.h"
#include "../kobox2_adapter/ipc.h"

/* One serialized request on the already bound sandbox channel. Borrowed IPC
 * and mapping stay alive through sandbox retirement, including on failure.
 * The chain is persistent: a failed exchange must not leave a stack pointer
 * in the queue's outstanding table. Errors permanently stop this instance. */
struct gpud_gpu_rpc {
    struct ph_ipc *ipc;
    struct gpud_gpu_channel *channel;
    unsigned char *mapping;
    kb2_vq_segment_t segments[2];
    kb2_vq_chain_t chain;
    struct ph_ipc_packet incoming;
    int error;
};

int gpud_gpu_rpc_call(struct gpud_gpu_rpc *rpc, unsigned int queue,
    const unsigned char *request, size_t request_size,
    unsigned char *reply, size_t reply_capacity, size_t *reply_size);

#endif
