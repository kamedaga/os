/* SPDX-License-Identifier: MIT */
#pragma once
#include "../../kobox2_owner/fs_channel_profile.h"
#include <stdio.h>

static uint64_t filed_ring_storage_cycles[4];
static uint64_t filed_ring_storage_notifications, filed_ring_storage_waits;
void ph_fs_channel_profile(uint32_t opcode, const uint64_t cycles[4],
    unsigned notifications, unsigned waits)
{
    (void)opcode;
    /* filed's event loop serializes the owner lane. Attribute the nested
     * storage hop to the current outer request, not to a separate average. */
    for (unsigned i = 0; i < 4; ++i) filed_ring_storage_cycles[i] += cycles[i];
    filed_ring_storage_notifications += notifications;
    filed_ring_storage_waits += waits;
}
#ifdef PH_FS_CHANNEL_TRACE
void ph_fs_channel_trace(uint64_t channel, uint32_t opcode, uint64_t sequence,
    uint64_t publish, uint64_t completion)
{
    /* The inner storage sequence belongs to filed, not the LPR request. Its
     * partition is captured above; only the outer request logs correlation. */
    (void)channel; (void)opcode; (void)sequence; (void)publish; (void)completion;
}
#endif
struct filed_ring_trace_record {
    struct filed_ring_trace_record *next;
    uint64_t values[18];
};
static int filed_ring_trace_save(struct filed_ring_trace_record **records,
    uint64_t channel, int32_t pid, const kb2_fs_request_t *request,
    const uint64_t stamps[8], const uint64_t storage[6])
{
    if ((request->sequence & 255) || (request->opcode != KB2_FILESYSTEM_OP_STATX &&
        request->opcode != KB2_FILESYSTEM_OP_PREAD &&
        request->opcode != KB2_FILESYSTEM_OP_PWRITE)) return 0;
    struct filed_ring_trace_record *record = malloc(sizeof(*record));
    if (!record) return -ENOMEM;
    uint64_t *values = record->values;
    values[0] = channel; values[1] = (uint64_t)(int64_t)pid;
    values[2] = request->opcode; values[3] = request->sequence;
    for (unsigned i = 0; i < 8; ++i) values[i + 4] = stamps[i];
    for (unsigned i = 0; i < 6; ++i) values[i + 12] = storage[i];
    record->next = *records;
    *records = record;
    return 0;
}
static int filed_ring_trace_flush(filed_runtime_t *rt,
    struct filed_ring_trace_record **records)
{
    if (!*records) return 0;
    /* Even deferred console output can interleave with sandbox timer warnings.
     * Save private diagnostic records after timing, in this run's COW only;
     * the offline checker exports them without native console concurrency. */
    char path[80];
    snprintf(path, sizeof(path), "/root/filed-ring-trace-%llu.bin",
        (unsigned long long)(*records)->values[0]);
    uint64_t handle = 0;
    int status = filed_storage_open(rt, 0, path, 01|0100|0200, 0600, 0077, &handle);
    if (status) return status;
    while (*records) {
        struct filed_ring_trace_record *record = *records;
        kb2_fs_request_t request = {.opcode = KB2_FILESYSTEM_OP_WRITE,
            .handle = handle, .length = sizeof(record->values),
            .data = {KB2_FILESYSTEM_REQUEST_SIZE, sizeof(record->values)}};
        kb2_fs_response_t response;
        status = filed_storage_request(rt, &request, record->values,
            sizeof(record->values), &response, NULL, 0);
        if (!status && response.result != (int64_t)sizeof(record->values)) status = -EIO;
        if (status) break;
        *records = record->next;
        free(record);
    }
    int closed = filed_storage_close(rt, handle);
    return status ? status : closed;
}
static void filed_ring_trace_discard(struct filed_ring_trace_record **records)
{
    while (*records) {
        struct filed_ring_trace_record *record = *records;
        *records = record->next;
        free(record);
    }
}
