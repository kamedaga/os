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
    kb2_vq_segment_t event_segment;
    kb2_vq_chain_t event_chain;
    struct ph_ipc_packet incoming;
    struct ph_ipc_packet attachment;
    int error, event_started, event_notification;
};

int gpud_gpu_rpc_start_events(struct gpud_gpu_rpc *rpc);
/* Returns one for a copied event message, zero when no completed event lane
 * entry exists, or a negative terminal transport error. */
int gpud_gpu_rpc_next_event(struct gpud_gpu_rpc *rpc,
    unsigned char *message, size_t capacity, size_t *size_out);
int gpud_gpu_rpc_call(struct gpud_gpu_rpc *rpc, unsigned int queue,
    const unsigned char *request, size_t request_size,
    unsigned char *reply, size_t reply_capacity, size_t *reply_size);
int gpud_gpu_rpc_take_mapping(struct gpud_gpu_rpc *rpc,
    uint64_t correlation, uint64_t exchange_id, struct pacha_ipc_fd *fd);
int gpud_gpu_rpc_take_dma_buf(struct gpud_gpu_rpc *rpc,
    uint64_t correlation, uint64_t exchange_id, struct pacha_ipc_fd *fd);
int gpud_gpu_rpc_release_mapping(struct gpud_gpu_rpc *rpc,
    uint64_t correlation, uint64_t mapping_id);

#endif
