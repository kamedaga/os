/* SPDX-License-Identifier: MIT */
#include "fs_channel.h"
#include "fs_channel_profile.h"
#ifdef FILED_INTERNAL_MICROBENCH
#include "../filed/tests/internal_microbench_clock.h"
#endif

#include <errno.h>
#include <string.h>

#ifdef PH_FS_CHANNEL_DIAGNOSTICS
extern void ph_fs_channel_diagnostic(unsigned, int, uint64_t);
#define FS_DIAG(stage, status, value) ph_fs_channel_diagnostic(stage, status, value)
#else
#define FS_DIAG(stage, status, value) ((void)0)
#endif

static size_t align_up(size_t value, size_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

int ph_fs_channel_layout(const struct ph_fs_channel_config *config,
                         size_t *mapping_bytes, size_t *arena_offset) {
    if (!config || !mapping_bytes || !arena_offset || !config->generation ||
        !config->channel_id || config->queue_size < 2 ||
        config->queue_size > 32768 ||
        (config->queue_size & (config->queue_size - 1)) ||
        !config->arena_bytes || config->arena_bytes > SIZE_MAX - 4095)
        return -EINVAL;
    size_t header;
    if (kb2_protocol_channel_encoded_size(1, 1, &header)) return -EPROTO;
    header += KB2_FILESYSTEM_CHANNEL_PROFILE_SIZE;
    size_t descriptors = align_up(header, 16);
    size_t available = descriptors + (size_t)config->queue_size * 16;
    size_t used = align_up(available + 6 + (size_t)config->queue_size * 2, 4);
    size_t arena = align_up(used + 6 + (size_t)config->queue_size * 8, 4096);
    if (config->arena_bytes > SIZE_MAX - arena - 4095) return -EOVERFLOW;
    *arena_offset = arena;
    *mapping_bytes = align_up(arena + (size_t)config->arena_bytes, 4096);
    return 0;
}

int ph_fs_channel_bind(struct ph_fs_channel *channel, void *mapping, size_t bytes,
                       const struct ph_fs_channel_config *config,
                       kb2_vq_side_t side, const kb2_vq_atomic_ops_t *atomics,
                       kb2_vq_chain_t **slots, uint16_t *owners) {
    size_t expected_bytes, arena_offset, header;
    int error = ph_fs_channel_layout(config, &expected_bytes, &arena_offset);
    if (error) return error;
    if (!channel || channel->identity.generation || !mapping || bytes != expected_bytes ||
        !atomics || !slots || !owners ||
        (side != KB2_VQ_DRIVER && side != KB2_VQ_DEVICE)) return -EINVAL;
    if (kb2_protocol_channel_encoded_size(1, 1, &header)) return -EPROTO;
    header += KB2_FILESYSTEM_CHANNEL_PROFILE_SIZE;
    channel->config = *config;
    channel->arena_offset = arena_offset;
    channel->identity = (kb2_protocol_channel_t){
        .feature_bits = KB2_PROTOCOL_TRANSPORT_FEATURES_REQUIRED,
        .channel_id = config->channel_id, .generation = config->generation,
        .protocol_id = KB2_FILESYSTEM_PROTOCOL_ID,
        .flags = KB2_PROTOCOL_CHANNEL_FLAG_DATA};
    channel->queue = (kb2_protocol_queue_t){.queue_id = 1,
        .role = KB2_PROTOCOL_QUEUE_ROLE_REQUEST, .queue_size = config->queue_size,
        .descriptor_address = align_up(header, 16),
        .available_notification_id = 1, .used_notification_id = 2,
        .max_chain_length = config->queue_size,
        .max_indirect_length = config->queue_size,
        .max_outstanding = config->queue_size};
    channel->queue.available_address = channel->queue.descriptor_address +
        (uint64_t)config->queue_size * 16;
    channel->queue.used_address = align_up((size_t)channel->queue.available_address +
        6 + (size_t)config->queue_size * 2, 4);
    channel->region = (kb2_protocol_region_t){.region_id = 1,
        .rights = KB2_PROTOCOL_REGION_RIGHT_READ | KB2_PROTOCOL_REGION_RIGHT_WRITE,
        .length = bytes};
    channel->mapping = (kb2_vq_mapping_t){.address = mapping, .length = bytes,
        .local_rights = channel->region.rights};
    channel->arena = (kb2_vq_arena_t){.queue_id = 1,
        .rights = channel->region.rights, .transport_base = arena_offset,
        .length = config->arena_bytes};
    unsigned char expected[KB2_PROTOCOL_CHANNEL_HEADER_SIZE +
        KB2_PROTOCOL_QUEUE_DESCRIPTOR_SIZE + KB2_PROTOCOL_REGION_DESCRIPTOR_SIZE +
        KB2_FILESYSTEM_CHANNEL_PROFILE_SIZE];
    unsigned char snapshot[sizeof(expected)];
    size_t encoded;
    if (kb2_protocol_channel_encode(expected, sizeof(expected), &encoded,
            &channel->identity, &channel->queue, 1, &channel->region, 1) ||
        encoded + KB2_FILESYSTEM_CHANNEL_PROFILE_SIZE != sizeof(expected)) return -EPROTO;
    static const unsigned char filesystem_digest[32] = KB2_FILESYSTEM_SCHEMA_SHA256_BYTES;
    memcpy(expected + encoded + KB2_FILESYSTEM_CHANNEL_PROFILE_SCHEMA_DIGEST_OFFSET,
        filesystem_digest, sizeof(filesystem_digest));
    encoded += KB2_FILESYSTEM_CHANNEL_PROFILE_SIZE;
    if (side == KB2_VQ_DRIVER) memcpy(mapping, expected, encoded);
    else {
        memcpy(snapshot, mapping, encoded);
        if (memcmp(snapshot, expected, encoded)) return -EPROTO;
    }
    if (kb2_vq_mapped_memory_init(&channel->memory, &channel->mapping, 1,
            &channel->arena, 1, atomics) ||
        kb2_vq_channel_init(&channel->channel, &channel->identity,
            &channel->queue, 1, &channel->region, 1)) return -EPROTO;
    kb2_vq_memory_t memory = kb2_vq_mapped_memory_ops(&channel->memory);
    if (kb2_vq_bind(&channel->lane, &channel->channel, 1, side, &memory,
            slots, owners, config->queue_size)) return -EPROTO;
    return 0;
}

int ph_fs_channel_reserve(struct ph_fs_channel *channel, struct ph_fs_pending *pending,
    const void *request, size_t request_size, size_t response_capacity, void **destination_out) {
    if (!channel || !pending || !request || !destination_out || pending->channel ||
        pending->chain.state || channel->lane.side != KB2_VQ_DRIVER ||
        channel->call_chain.state ||
        request_size < KB2_FILESYSTEM_REQUEST_SIZE || request_size > UINT32_MAX ||
        response_capacity < KB2_FILESYSTEM_RESPONSE_SIZE || response_capacity > UINT32_MAX ||
        response_capacity > SIZE_MAX - 7 ||
        request_size > SIZE_MAX - response_capacity - 7)
        return -EINVAL;
    *destination_out = NULL;
    if (atomic_load_explicit(&channel->channel.faulted, memory_order_acquire))
        return -EIO;
    kb2_fs_request_t decoded;
    if (kb2_fs_request_decode(request, KB2_FILESYSTEM_REQUEST_SIZE, request_size, &decoded) ||
        decoded.generation != channel->config.generation)
        return -EPROTO;
    size_t length = align_up(request_size + response_capacity, 8);
    if (length > channel->config.arena_bytes) return -EAGAIN;
    uint16_t descriptors[2];
    unsigned found = 0;
    for (uint32_t i = 0; i < channel->config.queue_size && found < 2; ++i) {
        if (channel->lane.descriptor_owner[i]) continue;
        int reserved = 0;
        for (struct ph_fs_pending *p = channel->pending; p; p = p->next)
            if (p->segments[0].descriptor == i || p->segments[1].descriptor == i) {
                reserved = 1;
                break;
            }
        if (!reserved) descriptors[found++] = (uint16_t)i;
    }
    if (found != 2) return -EAGAIN;
    size_t begin = 0;
    struct ph_fs_pending **cursor = &channel->pending;
    while (*cursor) {
        if ((*cursor)->request.sequence == decoded.sequence) return -EEXIST;
        cursor = &(*cursor)->next;
    }
    cursor = &channel->pending;
    while (*cursor && (*cursor)->arena_begin - begin < length) {
        begin = (*cursor)->arena_begin + (*cursor)->arena_length;
        cursor = &(*cursor)->next;
    }
    if (begin > channel->config.arena_bytes ||
        length > channel->config.arena_bytes - begin) return -EAGAIN;
    size_t address = channel->arena_offset + begin;
    unsigned char *destination = (unsigned char *)channel->mapping.address + address;
    *pending = (struct ph_fs_pending){.channel = channel, .request = decoded,
        .arena_begin = begin, .arena_length = length, .next = *cursor};
    pending->segments[0] = (kb2_vq_segment_t){.address = address,
        .length = (uint32_t)request_size, .descriptor = descriptors[0]};
    pending->segments[1] = (kb2_vq_segment_t){.address = address + request_size,
        .length = (uint32_t)response_capacity, .descriptor = descriptors[1], .writable = 1};
    pending->chain = (kb2_vq_chain_t){.segments = pending->segments,
        .capacity = 2, .count = 2, .head = descriptors[0]};
    *cursor = pending;
    *destination_out = destination;
    return 0;
}

int ph_fs_channel_cancel(struct ph_fs_pending *pending) {
    if (!pending || !pending->channel || pending->published || pending->chain.state)
        return -EINVAL;
    struct ph_fs_pending **cursor = &pending->channel->pending;
    while (*cursor && *cursor != pending) cursor = &(*cursor)->next;
    if (!*cursor) return -EINVAL;
    *cursor = pending->next;
    memset(pending, 0, sizeof(*pending));
    return 0;
}

int ph_fs_channel_publish(struct ph_fs_pending *pending, int *notify) {
    if (!pending || !pending->channel || !notify || pending->published ||
        pending->chain.state) return -EINVAL;
    *notify = 0;
    struct ph_fs_channel *channel = pending->channel;
    /* Inventory was installed before the unlocked request copy. Find its
     * current list position: other callers may have inserted during that copy. */
    struct ph_fs_pending **cursor = &channel->pending;
    while (*cursor && *cursor != pending) cursor = &(*cursor)->next;
    if (!*cursor) return -EINVAL;
    kb2_vq_status_t status = kb2_vq_publish(&channel->lane,
        channel->config.generation, &pending->chain, notify);
    if (status == KB2_VQ_OK) { pending->published = 1; return 0; }
    if (!pending->chain.state) {
        *cursor = pending->next;
        memset(pending, 0, sizeof(*pending));
        if (status == KB2_VQ_FULL) return -EAGAIN;
    }
    kb2_vq_channel_fault(&channel->channel);
    return -EPROTO;
}

int ph_fs_channel_submit(struct ph_fs_channel *channel, struct ph_fs_pending *pending,
    const void *request, size_t request_size, size_t response_capacity, int *notify) {
    if (!notify) return -EINVAL;
    *notify = 0;
    void *destination;
    int status = ph_fs_channel_reserve(channel, pending, request,
        request_size, response_capacity, &destination);
    if (status) return status;
    uintptr_t source_address = (uintptr_t)request, destination_address = (uintptr_t)destination;
    if (source_address != destination_address &&
        ((source_address < destination_address && destination_address - source_address < request_size) ||
         (destination_address < source_address && source_address - destination_address < request_size))) {
        (void)ph_fs_channel_cancel(pending);
        return -EINVAL;
    }
    if (destination != request) memcpy(destination, request, request_size);
    return ph_fs_channel_publish(pending, notify);
}

int ph_fs_channel_poll(struct ph_fs_channel *channel, struct ph_fs_pending **completed) {
    if (!channel || !completed || channel->lane.side != KB2_VQ_DRIVER ||
        channel->call_chain.state) return -EINVAL;
    *completed = NULL;
    kb2_vq_chain_t *chain = NULL;
    kb2_vq_status_t status = kb2_vq_take_used(&channel->lane,
        channel->config.generation, &chain);
    if (status == KB2_VQ_EMPTY) return -EAGAIN;
    if (status != KB2_VQ_OK) return -EPROTO;
    for (struct ph_fs_pending *p = channel->pending; p; p = p->next) {
        if (&p->chain == chain && !p->completed) {
            p->completed = 1;
            *completed = p;
            return 0;
        }
    }
    kb2_vq_channel_fault(&channel->channel);
    return -EPROTO;
}

int ph_fs_channel_arm(struct ph_fs_channel *channel, int *ready) {
    if (!channel || !ready || channel->lane.side != KB2_VQ_DRIVER ||
        channel->call_chain.state) return -EINVAL;
    if (!kb2_vq_arm(&channel->lane, channel->config.generation, ready)) return 0;
    return -EPROTO;
}

int ph_fs_channel_result_header(struct ph_fs_pending *pending, kb2_fs_response_t *response,
    size_t *used) {
    if (!pending || !pending->channel || !response || !used) return -EINVAL;
    *used = 0;
    if (!pending->completed) return -EAGAIN;
    struct ph_fs_channel *channel = pending->channel;
    kb2_vq_chain_t *chain = &pending->chain;
    uint8_t header[KB2_FILESYSTEM_RESPONSE_SIZE];
    kb2_fs_response_t decoded;
    if (chain->used_length < sizeof(header) || chain->used_length > chain->writable ||
        kb2_vq_copy_response(&channel->lane, channel->config.generation,
            chain, 0, header, sizeof(header)) ||
        kb2_fs_response_decode(header, chain->used_length, &decoded) ||
        !kb2_fs_response_matches(&decoded, &pending->request)) goto fault;
    switch (pending->request.opcode) {
    case KB2_FILESYSTEM_OP_READ:
    case KB2_FILESYSTEM_OP_PREAD:
    case KB2_FILESYSTEM_OP_GETDENTS64:
    case KB2_FILESYSTEM_OP_READLINKAT:
    case KB2_FILESYSTEM_OP_READLINK_HANDLE:
        if (decoded.result >= 0 && (uint64_t)decoded.result != decoded.data_length)
            goto fault;
        break;
    default: break;
    }
    *response = decoded;
    *used = chain->used_length;
    pending->result_read = 1;
    pending->result_data_length = decoded.data_length;
    return 0;
fault:
    kb2_vq_channel_fault(&channel->channel);
    return -EPROTO;
}

int ph_fs_channel_result_data(struct ph_fs_pending *pending, uint64_t offset,
    void *data, size_t bytes) {
    if (!pending || !pending->channel || !pending->result_read || (!data && bytes) ||
        offset > pending->result_data_length || bytes > pending->result_data_length - offset)
        return -EINVAL;
    struct ph_fs_channel *channel = pending->channel;
    if (kb2_vq_copy_response(&channel->lane, channel->config.generation, &pending->chain,
            KB2_FILESYSTEM_RESPONSE_SIZE + offset, data, bytes)) {
        kb2_vq_channel_fault(&channel->channel);
        return -EPROTO;
    }
    return 0;
}

int ph_fs_channel_read_result(struct ph_fs_pending *pending, kb2_fs_response_t *response,
    void *data, size_t data_capacity, size_t *used) {
    if (!used || (data_capacity && !data)) return -EINVAL;
    int status = ph_fs_channel_result_header(pending, response, used);
    if (status) return status;
    if (response->data_length > data_capacity) { *used = 0; return -ENOSPC; }
    return ph_fs_channel_result_data(pending, 0, data, (size_t)response->data_length);
}

int ph_fs_channel_release(struct ph_fs_pending *pending) {
    if (!pending || !pending->channel || !pending->completed || !pending->result_read)
        return -EINVAL;
    struct ph_fs_channel *channel = pending->channel;
    struct ph_fs_pending **cursor = &channel->pending;
    while (*cursor && *cursor != pending) cursor = &(*cursor)->next;
    if (!*cursor || kb2_vq_release(&channel->lane, channel->config.generation, &pending->chain)) {
        kb2_vq_channel_fault(&channel->channel);
        return -EPROTO;
    }
    *cursor = pending->next;
    memset(pending, 0, sizeof(*pending));
    return 0;
}

int ph_fs_channel_collect(struct ph_fs_pending *pending, kb2_fs_response_t *response,
    void *data, size_t data_capacity, size_t *used) {
    int status = ph_fs_channel_read_result(pending, response, data, data_capacity, used);
    return status ? status : ph_fs_channel_release(pending);
}

static int channel_call(struct ph_fs_channel *channel,
                       const void *request, size_t request_size,
                       void *response, size_t response_capacity, size_t *used,
                       const struct ph_fs_channel_wait *wait,
                       void *data, kb2_fs_response_t *decoded_response) {
    if (!channel || !request || !response || !used || !wait || !wait->notify ||
        !wait->wait || channel->lane.side != KB2_VQ_DRIVER || channel->lane.owned ||
        channel->pending ||
        request_size < KB2_FILESYSTEM_REQUEST_SIZE ||
        response_capacity < KB2_FILESYSTEM_RESPONSE_SIZE ||
        request_size > UINT32_MAX || response_capacity > UINT32_MAX ||
        request_size > channel->config.arena_bytes ||
        response_capacity > channel->config.arena_bytes - request_size)
        return -EINVAL;
#ifdef FILED_INTERNAL_MICROBENCH
    uint64_t bench_stage=filed_bench_collect ? filed_bench_tsc() : 0;
#endif
#ifdef PH_FS_CHANNEL_PROFILE
    uint64_t profile_start = ph_fs_channel_profile_tsc(), profile_cycles[4];
    unsigned profile_waits = 0;
#endif
    kb2_fs_request_t decoded;
    if (kb2_fs_request_decode(request, request_size, request_size, &decoded) ||
        decoded.generation != channel->config.generation) {
        FS_DIAG(20, -EPROTO, request_size); return -EPROTO;
    }
    uint64_t offset = channel->arena_offset;
    /* A locked producer may encode directly into its unpublished arena.
     * Skip identical-pointer memcpy, whose overlap contract is undefined. */
    if (request != (unsigned char *)channel->mapping.address + offset)
        memcpy((unsigned char *)channel->mapping.address + offset, request, request_size);
    channel->call_segments[0] = (kb2_vq_segment_t)
        {.address = offset, .length = (uint32_t)request_size, .descriptor = 0};
    channel->call_segments[1] = (kb2_vq_segment_t)
        {.address = offset + request_size, .length = (uint32_t)response_capacity,
         .descriptor = 1, .writable = 1};
    channel->call_chain = (kb2_vq_chain_t){.segments = channel->call_segments,
        .capacity = 2, .count = 2};
    kb2_vq_chain_t *chain = &channel->call_chain;
    int notify, error = 0;
#if defined(PH_FS_CHANNEL_PROFILE) && defined(PH_FS_CHANNEL_TRACE)
    uint64_t trace_publish = ph_fs_channel_profile_tsc();
#endif
    kb2_vq_status_t publish = kb2_vq_publish(&channel->lane, decoded.generation, chain, &notify);
    if (publish) { FS_DIAG(21, publish, decoded.opcode); return -EPROTO; }
#if defined(PH_FS_CHANNEL_PROFILE) && defined(PH_FS_CHANNEL_FORCE_NOTIFY)
    /* Diagnostic control: a conservative extra doorbell distinguishes the
     * EVENT_IDX fast producer path from payload/copy cost. Not production. */
    notify = 1;
#endif
    /* A successful publish may intentionally omit a doorbell. Observing its
     * actual EVENT_IDX decision distinguishes that case from a failed send. */
    FS_DIAG(26, notify, decoded.opcode);
#ifdef FILED_INTERNAL_MICROBENCH
    FILED_BENCH_STAGE(filed_bench_channel_cycles,0,bench_stage);
#endif
#ifdef PH_FS_CHANNEL_PROFILE
    uint64_t profile_end = ph_fs_channel_profile_tsc();
    profile_cycles[0] = profile_end - profile_start;
    profile_start = profile_end;
#endif
    if (notify && (error = wait->notify(wait->context,
            channel->queue.available_notification_id))) goto fault;
#ifdef PH_FS_CHANNEL_PROFILE
    profile_end = ph_fs_channel_profile_tsc();
    profile_cycles[1] = profile_end - profile_start;
    profile_start = profile_end;
#endif
    kb2_vq_chain_t *done = NULL;
    for (;;) {
        kb2_vq_status_t status = kb2_vq_take_used(&channel->lane,
            decoded.generation, &done);
        if (status == KB2_VQ_OK) break;
        if (status != KB2_VQ_EMPTY) {
            FS_DIAG(22, status, decoded.opcode); error = -EPROTO; goto fault;
        }
        int ready;
        if (kb2_vq_arm(&channel->lane, decoded.generation, &ready)) {
            FS_DIAG(23, -EPROTO, decoded.opcode);
            error = -EPROTO; goto fault;
        }
        if (ready) continue;
#ifdef PH_FS_CHANNEL_PROFILE
        ++profile_waits;
#endif
        error = wait->wait(wait->context, channel->queue.used_notification_id);
        if (error && error != -EAGAIN) goto fault;
    }
#ifdef FILED_INTERNAL_MICROBENCH
    FILED_BENCH_STAGE(filed_bench_channel_cycles,1,bench_stage);
#endif
#ifdef PH_FS_CHANNEL_PROFILE
    profile_end = ph_fs_channel_profile_tsc();
    profile_cycles[2] = profile_end - profile_start;
    profile_start = profile_end;
#ifdef PH_FS_CHANNEL_TRACE
    uint64_t trace_completion = profile_end;
#endif
#endif
    if (done != chain || chain->used_length > response_capacity ||
        chain->used_length < KB2_FILESYSTEM_RESPONSE_SIZE ||
        kb2_vq_copy_response(&channel->lane, decoded.generation, chain,
            0, response, KB2_FILESYSTEM_RESPONSE_SIZE)) {
        FS_DIAG(24, -EPROTO, chain->used_length); error = -EPROTO; goto fault;
    }
    kb2_fs_response_t reply;
    /* The decoder reads only the fixed header and checks its payload length
     * against the total transport size. Keep that private header snapshot
     * before copying data; malformed replies must not modify the destination. */
    if (kb2_fs_response_decode(response, chain->used_length, &reply) ||
        !kb2_fs_response_matches(&reply, &decoded) ||
        (decoded.opcode==KB2_FILESYSTEM_OP_PREAD && reply.result>=0 &&
         (uint64_t)reply.result!=reply.data_length)) {
        FS_DIAG(25, -EPROTO, chain->used_length); error = -EPROTO; goto fault;
    }
    if (reply.data_length && kb2_vq_copy_response(&channel->lane,
            decoded.generation, chain, KB2_FILESYSTEM_RESPONSE_SIZE,
            decoded_response ? data : (unsigned char *)response + KB2_FILESYSTEM_RESPONSE_SIZE,
            (size_t)reply.data_length)) {
        error = -EPROTO; goto fault;
    }
    if (decoded_response) *decoded_response = reply;
    *used = chain->used_length;
    if (kb2_vq_release(&channel->lane, decoded.generation, chain)) {
        error = -EPROTO; goto fault;
    }
#ifdef FILED_INTERNAL_MICROBENCH
    FILED_BENCH_STAGE(filed_bench_channel_cycles,2,bench_stage);
#endif
#ifdef PH_FS_CHANNEL_PROFILE
    profile_cycles[3] = ph_fs_channel_profile_tsc() - profile_start;
    ph_fs_channel_profile(decoded.opcode, profile_cycles, !!notify, profile_waits);
#ifdef PH_FS_CHANNEL_TRACE
    ph_fs_channel_trace(channel->config.channel_id, decoded.opcode, decoded.sequence,
        trace_publish, trace_completion);
#endif
#endif
    return 0;
fault:
    kb2_vq_channel_fault(&channel->channel);
    return error;
}

int ph_fs_channel_call(struct ph_fs_channel *channel,
    const void *request, size_t request_size, void *response,
    size_t response_capacity, size_t *used, const struct ph_fs_channel_wait *wait)
{
    return channel_call(channel,request,request_size,response,response_capacity,
        used,wait,NULL,NULL);
}
int ph_fs_channel_call_split(struct ph_fs_channel *channel,
    const void *request,size_t request_size,kb2_fs_response_t *response,
    void *data,size_t capacity,size_t *used,const struct ph_fs_channel_wait *wait)
{
    if(!response || (capacity && !data) ||
        capacity>SIZE_MAX-KB2_FILESYSTEM_RESPONSE_SIZE) return -EINVAL;
    uint8_t header[KB2_FILESYSTEM_RESPONSE_SIZE];
    return channel_call(channel,request,request_size,header,
        KB2_FILESYSTEM_RESPONSE_SIZE+capacity,used,wait,data,response);
}
