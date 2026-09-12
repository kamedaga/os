/* SPDX-License-Identifier: MIT */
#include "gpu_sessions.h"

#include <errno.h>
#include <string.h>

int ph_gpu_sessions_init(struct gpud_gpu_sessions *sessions, uint64_t generation, size_t limit) {
    if (!sessions || sessions->generation || !generation || !limit ||
        limit > GPUD_GPU_SESSION_CAPACITY)
        return -EINVAL;
    *sessions = (struct gpud_gpu_sessions){.generation = generation, .limit = limit};
    return 0;
}

static uint32_t lookup(struct gpud_gpu_sessions *sessions,
                       uint64_t generation,
                       uint64_t client,
                       uint64_t id,
                       struct gpud_gpu_session **out) {
    if (!sessions || !sessions->generation || !client || !id)
        return KB2_GPU_STATUS_INVALID;
    if (generation != sessions->generation)
        return KB2_GPU_STATUS_STALE_GENERATION;
    for (size_t i = 0; i < sessions->limit; ++i) {
        struct gpud_gpu_session *entry = &sessions->entries[i];
        if (entry->state == GPUD_GPU_SESSION_FREE || entry->id != id)
            continue;
        if (entry->client != client)
            return KB2_GPU_STATUS_DENIED;
        *out = entry;
        return KB2_GPU_STATUS_OK;
    }
    return KB2_GPU_STATUS_NOT_FOUND;
}

uint32_t ph_gpu_session_open_begin(struct gpud_gpu_sessions *sessions,
                                   uint64_t generation,
                                   uint64_t client,
                                   uint32_t node,
                                   uint64_t *id_out) {
    if (!sessions || !sessions->generation || !client || !id_out)
        return KB2_GPU_STATUS_INVALID;
    if (generation != sessions->generation)
        return KB2_GPU_STATUS_STALE_GENERATION;
    if (node != KB2_GPU_NODE_RENDER)
        return KB2_GPU_STATUS_UNSUPPORTED;
    if (sessions->occupied == sessions->limit || sessions->sequence == UINT64_MAX)
        return KB2_GPU_STATUS_LIMIT;
    for (size_t i = 0; i < sessions->limit; ++i) {
        struct gpud_gpu_session *entry = &sessions->entries[i];
        if (entry->state != GPUD_GPU_SESSION_FREE)
            continue;
        *entry = (struct gpud_gpu_session){
            .id = ++sessions->sequence, .client = client, .state = GPUD_GPU_SESSION_OPENING};
        ++sessions->occupied;
        *id_out = entry->id;
        return KB2_GPU_STATUS_OK;
    }
    return KB2_GPU_STATUS_DEVICE_LOST;
}

uint32_t ph_gpu_session_open_finish(struct gpud_gpu_sessions *sessions,
                                    uint64_t generation,
                                    uint64_t client,
                                    uint64_t id,
                                    uint64_t cookie,
                                    uint32_t status) {
    struct gpud_gpu_session *entry;
    uint32_t result = lookup(sessions, generation, client, id, &entry);
    if (result)
        return result;
    if (entry->state != GPUD_GPU_SESSION_OPENING || status > KB2_GPU_STATUS_DEVICE_LOST ||
        (status == KB2_GPU_STATUS_OK ? !cookie : cookie != 0))
        return KB2_GPU_STATUS_INVALID;
    if (status == KB2_GPU_STATUS_OK) {
        for (size_t i = 0; i < sessions->limit; ++i)
            if (&sessions->entries[i] != entry && sessions->entries[i].file_cookie == cookie)
                return KB2_GPU_STATUS_DEVICE_LOST;
        entry->file_cookie = cookie;
        entry->state = GPUD_GPU_SESSION_OPEN;
    } else {
        memset(entry, 0, sizeof(*entry));
        --sessions->occupied;
    }
    return KB2_GPU_STATUS_OK;
}

uint32_t ph_gpu_session_acquire(struct gpud_gpu_sessions *sessions,
                                uint64_t generation,
                                uint64_t client,
                                uint64_t id,
                                uint64_t *cookie_out) {
    struct gpud_gpu_session *entry;
    uint32_t result;
    if (!cookie_out)
        return KB2_GPU_STATUS_INVALID;
    result = lookup(sessions, generation, client, id, &entry);
    if (result)
        return result;
    if (entry->state != GPUD_GPU_SESSION_OPEN)
        return KB2_GPU_STATUS_SESSION_LOST;
    if (entry->in_flight == UINT32_MAX)
        return KB2_GPU_STATUS_LIMIT;
    ++entry->in_flight;
    *cookie_out = entry->file_cookie;
    return KB2_GPU_STATUS_OK;
}

uint32_t ph_gpu_session_release(struct gpud_gpu_sessions *sessions,
                                uint64_t generation,
                                uint64_t client,
                                uint64_t id) {
    struct gpud_gpu_session *entry;
    uint32_t result = lookup(sessions, generation, client, id, &entry);
    if (result)
        return result;
    if (!entry->in_flight ||
        (entry->state != GPUD_GPU_SESSION_OPEN && entry->state != GPUD_GPU_SESSION_CLOSING))
        return KB2_GPU_STATUS_INVALID;
    --entry->in_flight;
    return KB2_GPU_STATUS_OK;
}

uint32_t ph_gpu_session_close_begin(struct gpud_gpu_sessions *sessions,
                                    uint64_t generation,
                                    uint64_t client,
                                    uint64_t id) {
    struct gpud_gpu_session *entry;
    uint32_t result = lookup(sessions, generation, client, id, &entry);
    if (result)
        return result;
    if (entry->state != GPUD_GPU_SESSION_OPEN && entry->state != GPUD_GPU_SESSION_CLOSING)
        return KB2_GPU_STATUS_SESSION_LOST;
    entry->state = GPUD_GPU_SESSION_CLOSING;
    return KB2_GPU_STATUS_OK;
}

uint32_t ph_gpu_session_close_ready(struct gpud_gpu_sessions *sessions,
                                    uint64_t generation,
                                    uint64_t client,
                                    uint64_t id,
                                    uint64_t *cookie_out) {
    struct gpud_gpu_session *entry;
    uint32_t result;
    if (!cookie_out)
        return KB2_GPU_STATUS_INVALID;
    result = lookup(sessions, generation, client, id, &entry);
    if (result)
        return result;
    if (entry->state != GPUD_GPU_SESSION_CLOSING)
        return KB2_GPU_STATUS_INVALID;
    if (entry->in_flight)
        return KB2_GPU_STATUS_BUSY;
    *cookie_out = entry->file_cookie;
    return KB2_GPU_STATUS_OK;
}

uint32_t ph_gpu_session_close_finish(struct gpud_gpu_sessions *sessions,
                                     uint64_t generation,
                                     uint64_t client,
                                     uint64_t id,
                                     uint32_t status) {
    struct gpud_gpu_session *entry;
    uint32_t result = lookup(sessions, generation, client, id, &entry);
    if (result)
        return result;
    if (entry->state != GPUD_GPU_SESSION_CLOSING || entry->in_flight ||
        status > KB2_GPU_STATUS_DEVICE_LOST)
        return KB2_GPU_STATUS_INVALID;
    if (status == KB2_GPU_STATUS_OK) {
        memset(entry, 0, sizeof(*entry));
        --sessions->occupied;
    } else {
        entry->state = GPUD_GPU_SESSION_FAILED;
    }
    return KB2_GPU_STATUS_OK;
}
