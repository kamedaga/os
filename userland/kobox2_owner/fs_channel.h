/* SPDX-License-Identifier: MIT */
#ifndef PACHA_KOBOX_FS_CHANNEL_H
#define PACHA_KOBOX_FS_CHANNEL_H

#include <kobox2/filesystem.h>
#include <kobox2/virtqueue_memory.h>

/* Private authenticated policy, not a peer-provided native structure. Arena
 * capacity is a negotiated transport budget, not a filesystem/path limit. */
struct ph_fs_channel_config {
    uint64_t generation, channel_id, arena_bytes;
    uint32_t queue_size;
};

struct ph_fs_pending;

struct ph_fs_channel {
    struct ph_fs_channel_config config;
    kb2_protocol_channel_t identity;
    kb2_protocol_queue_t queue;
    kb2_protocol_region_t region;
    kb2_vq_channel_t channel;
    kb2_vq_mapping_t mapping;
    kb2_vq_arena_t arena;
    kb2_vq_mapped_memory_t memory;
    kb2_vq_t lane;
    size_t arena_offset;
    kb2_vq_segment_t call_segments[2];
    kb2_vq_chain_t call_chain;
    struct ph_fs_pending *pending;
};

/* Caller zero-initializes inventory, which remains pinned from submit through collect, or
 * confirmed remote retirement on failure. The channel owner serializes short
 * submit/poll/arm/collect operations, never an external wait or VFS execution.
 * Completed requests retain their arena until their own caller collects them. */
struct ph_fs_pending {
    struct ph_fs_pending *next;
    struct ph_fs_channel *channel;
    kb2_vq_segment_t segments[2];
    kb2_vq_chain_t chain;
    kb2_fs_request_t request;
    size_t arena_begin, arena_length;
    unsigned int published, completed, result_read;
    uint64_t result_data_length;
};

/* Reserve/publish/release require the short queue-owner exclusion. Between
 * reserve and publish only that caller fills the leased request bytes; between
 * completion and release only that caller reads the leased response. These
 * copies do not touch ring cursors and may run outside the owner exclusion. */
int ph_fs_channel_reserve(struct ph_fs_channel *, struct ph_fs_pending *,
    const void *request, size_t request_size, size_t response_capacity, void **destination);
int ph_fs_channel_publish(struct ph_fs_pending *, int *notify);
int ph_fs_channel_cancel(struct ph_fs_pending *);
int ph_fs_channel_result_header(struct ph_fs_pending *, kb2_fs_response_t *, size_t *used);
int ph_fs_channel_result_data(struct ph_fs_pending *, uint64_t offset, void *, size_t);
int ph_fs_channel_read_result(struct ph_fs_pending *, kb2_fs_response_t *,
    void *data, size_t data_capacity, size_t *used);
int ph_fs_channel_release(struct ph_fs_pending *);

/* No callbacks, allocation or waits. -EAGAIN is local backpressure before
 * publication, not a filesystem limit and not permission to replay a published
 * request. Other publication failures may leave inventory owned on a faulted
 * channel. Notifications are hints; poll identifies the actual completed owner.
 * A caller polling another thread's request must record/wake that thread rather
 * than discard its completion. collect also checks the private sequence. */
int ph_fs_channel_submit(struct ph_fs_channel *, struct ph_fs_pending *,
    const void *request, size_t request_size, size_t response_capacity, int *notify);
int ph_fs_channel_poll(struct ph_fs_channel *, struct ph_fs_pending **completed);
int ph_fs_channel_arm(struct ph_fs_channel *, int *ready);
int ph_fs_channel_collect(struct ph_fs_pending *, kb2_fs_response_t *,
    void *data, size_t data_capacity, size_t *used);

/* Native establishment/doorbells only. Neither packet contains filesystem
 * data. Owner credentials use the separately authenticated management queue.
 * Oversized calls use a fresh owner-private VMO, not a granted LPR VMO:
 * PREPARE(generation, correlation=new monotonic channel ID, value=client),
 * BIND(same correlation, value=exact arena bytes, one RW VMO), one request,
 * RELEASE, then RELEASED before the owner unmaps. OWNER_PREPARE selects the
 * credential dispatcher instead. Temporary client authority comes only from
 * PREPARE; the shared header cannot choose another authenticated client.
 * Base BIND uses correlation=0 and value=channel ID; temporary doorbells use
 * correlation=temporary ID. RELEASE and RELEASED carry no FDs or value.
 * A large operation gets one larger transport lease, never multiple Linux
 * calls whose offset, append, signals, and short-I/O semantics would differ. */
enum {
    PH_FS_QUEUE_BIND = 0x301,
    PH_FS_QUEUE_NOTIFY = 0x302,
    PH_FS_OWNER_BIND = 0x303,
    PH_FS_OWNER_NOTIFY = 0x304,
    PH_FS_CLIENT_PREPARE = 0x305,
    PH_FS_CLIENT_BIND = 0x306,
    PH_FS_CLIENT_BOUND = 0x307,
    PH_FS_CLIENT_RELEASE = 0x308,
    PH_FS_CLIENT_RELEASED = 0x309,
    PH_FS_EXCHANGE_PREPARE = 0x30a,
    PH_FS_EXCHANGE_OWNER_PREPARE = 0x30b,
    PH_FS_EXCHANGE_BIND = 0x30c,
    PH_FS_EXCHANGE_NOTIFY = 0x30d,
    PH_FS_EXCHANGE_RELEASE = 0x30e,
    PH_FS_EXCHANGE_RELEASED = 0x30f,
};

/* Management endpoint only: PREPARE binds a fresh monotonic correlation to a
 * registered client; BIND carries the exact arena size, RW queue VMO and a
 * dedicated bidirectional doorbell. BOUND proves installation, not merely an
 * enqueued transfer. RELEASED proves no executing request or mapped lease
 * remains. Client endpoints accept only QUEUE_NOTIFY with correlation equal
 * to their binding, never PREPARE/BIND or an owner operation. */
#define PH_FS_CLIENT_DOORBELL_RIGHTS (PACHA_FD_RIGHT_INSPECT | \
    PACHA_FD_RIGHT_SEND | PACHA_FD_RIGHT_RECV | PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL)

int ph_fs_channel_layout(const struct ph_fs_channel_config *config,
                         size_t *mapping_bytes, size_t *arena_offset);
/* Caller zero-initializes state, slots and owners, with queue_size entries.
 * DRIVER initializes a fresh zeroed VMO before capability transfer. DEVICE
 * compares generic header and filesystem schema digest private snapshots
 * against the independently trusted model.
 * All mappings/model/storage stay pinned until terminal channel retirement. */
int ph_fs_channel_bind(struct ph_fs_channel *channel, void *mapping, size_t bytes,
                       const struct ph_fs_channel_config *config,
                       kb2_vq_side_t side, const kb2_vq_atomic_ops_t *atomics,
                       kb2_vq_chain_t **slots, uint16_t *owners);

/* Single-owner synchronous convenience over the same multiplexable queue.
 * notify emits an available doorbell; wait waits/drains a used doorbell and
 * may return -EAGAIN for a duplicate/spurious wake. No timeout is imposed on
 * upstream VFS operations here. Failure after publish faults the generation:
 * caller must not unmap until its remote process has retired. */
struct ph_fs_channel_wait {
    void *context;
    int (*notify)(void *, uint32_t notification_id);
    int (*wait)(void *, uint32_t notification_id);
};
int ph_fs_channel_call(struct ph_fs_channel *channel,
                       const void *request, size_t request_size,
                       void *response, size_t response_capacity, size_t *used,
                       const struct ph_fs_channel_wait *wait);
/* Same wire and ownership rules, but copy the validated response payload
 * directly to its final destination, without a contiguous header+data bounce. */
int ph_fs_channel_call_split(struct ph_fs_channel *channel,
    const void *request, size_t request_size, kb2_fs_response_t *response,
    void *data, size_t capacity, size_t *used,
    const struct ph_fs_channel_wait *wait);

#endif
