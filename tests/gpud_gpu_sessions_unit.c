/* SPDX-License-Identifier: MIT */
#include "../userland/gpud/gpu_sessions.h"
#include <kobox2/gpu_session.h>

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static void ownership(void) {
    struct gpud_gpu_sessions table = {0};
    uint64_t a = 0, b = 0, c = 0, cookie = 99;
    assert(ph_gpu_sessions_init(&table, 0, 2) == -EINVAL);
    assert(!ph_gpu_sessions_init(&table, 9, 2));
    assert(ph_gpu_sessions_init(&table, 10, 2) == -EINVAL);
    assert(ph_gpu_session_open_begin(&table, 8, 7, KB2_GPU_NODE_RENDER, &a) ==
           KB2_GPU_STATUS_STALE_GENERATION);
    assert(!ph_gpu_session_open_begin(&table, 9, 7, KB2_GPU_NODE_PRIMARY, &a));
    assert(!ph_gpu_session_open_begin(&table, 9, 8, KB2_GPU_NODE_RENDER, &b));
    assert(a && b && a != b && table.occupied == 2);
    assert(ph_gpu_session_open_begin(&table, 9, 7, KB2_GPU_NODE_RENDER, &c) ==
           KB2_GPU_STATUS_LIMIT);
    assert(ph_gpu_session_acquire(&table, 9, 7, a, &cookie) == KB2_GPU_STATUS_SESSION_LOST &&
           cookie == 99);
    assert(!ph_gpu_session_open_finish(&table, 9, 7, a, 101, KB2_GPU_STATUS_OK));
    assert(ph_gpu_session_open_finish(&table, 9, 8, b, 101, KB2_GPU_STATUS_OK) ==
           KB2_GPU_STATUS_DEVICE_LOST);
    assert(!ph_gpu_session_open_finish(&table, 9, 8, b, 102, KB2_GPU_STATUS_OK));
    assert(ph_gpu_session_acquire(&table, 9, 8, a, &cookie) == KB2_GPU_STATUS_DENIED &&
           cookie == 99);
    assert(ph_gpu_session_close_begin(&table, 9, 8, a) == KB2_GPU_STATUS_DENIED);
    assert(ph_gpu_session_acquire(&table, 10, 7, a, &cookie) == KB2_GPU_STATUS_STALE_GENERATION);
    assert(!ph_gpu_session_acquire(&table, 9, 7, a, &cookie) && cookie == 101);
    assert(!ph_gpu_session_close_begin(&table, 9, 7, a));
    assert(ph_gpu_session_close_ready(&table, 9, 7, a, &cookie) == KB2_GPU_STATUS_BUSY);
    assert(ph_gpu_session_close_finish(&table, 9, 7, a, KB2_GPU_STATUS_OK) ==
           KB2_GPU_STATUS_INVALID);
    assert(ph_gpu_session_acquire(&table, 9, 7, a, &cookie) == KB2_GPU_STATUS_SESSION_LOST);
    assert(!ph_gpu_session_acquire(&table, 9, 8, b, &cookie) && cookie == 102);
    assert(!ph_gpu_session_release(&table, 9, 8, b));
    assert(!ph_gpu_session_release(&table, 9, 7, a));
    assert(!ph_gpu_session_close_ready(&table, 9, 7, a, &cookie) && cookie == 101);
    assert(!ph_gpu_session_close_finish(&table, 9, 7, a, KB2_GPU_STATUS_OK));
    assert(ph_gpu_session_acquire(&table, 9, 7, a, &cookie) == KB2_GPU_STATUS_NOT_FOUND);
    assert(!ph_gpu_session_open_begin(&table, 9, 7, KB2_GPU_NODE_RENDER, &c) && c > b);
    assert(!ph_gpu_session_open_finish(&table, 9, 7, c, 0, KB2_GPU_STATUS_NO_MEMORY));
    uint64_t failed = c;
    assert(!ph_gpu_session_open_begin(&table, 9, 7, KB2_GPU_NODE_RENDER, &c) && c > failed);
    assert(!ph_gpu_session_open_finish(&table, 9, 7, c, 103, KB2_GPU_STATUS_OK));
    assert(!ph_gpu_session_close_begin(&table, 9, 7, c));
    assert(!ph_gpu_session_close_finish(&table, 9, 7, c, KB2_GPU_STATUS_DEVICE_LOST));
    assert(table.occupied == 2);
    assert(ph_gpu_session_open_begin(&table, 9, 7, KB2_GPU_NODE_RENDER, &a) ==
           KB2_GPU_STATUS_LIMIT);
    assert(ph_gpu_session_close_begin(&table, 9, 7, c) == KB2_GPU_STATUS_SESSION_LOST);
    assert(!ph_gpu_session_close_begin(&table, 9, 8, b));
    assert(!ph_gpu_session_close_finish(&table, 9, 8, b, KB2_GPU_STATUS_OK));
    table.sequence = UINT64_MAX;
    assert(ph_gpu_session_open_begin(&table, 9, 7, KB2_GPU_NODE_RENDER, &a) ==
           KB2_GPU_STATUS_LIMIT);
}

static void desktop_capacity_reuse(void) {
    struct gpud_gpu_sessions table = {0};
    uint64_t ids[GPUD_GPU_SESSION_CAPACITY], extra, cookie;
    assert(GPUD_GPU_SESSION_CAPACITY >= 32);
    assert(!ph_gpu_sessions_init(&table, 1, GPUD_GPU_SESSION_CAPACITY));
    for (unsigned round = 0; round < 2; ++round) {
        for (size_t i = 0; i < GPUD_GPU_SESSION_CAPACITY; ++i) {
            assert(!ph_gpu_session_open_begin(&table, 1, 7, KB2_GPU_NODE_RENDER, &ids[i]));
            assert(!ph_gpu_session_open_finish(&table, 1, 7, ids[i], ids[i], KB2_GPU_STATUS_OK));
        }
        assert(table.occupied == GPUD_GPU_SESSION_CAPACITY);
        assert(ph_gpu_session_open_begin(&table, 1, 7, KB2_GPU_NODE_RENDER, &extra) == KB2_GPU_STATUS_LIMIT);
        for (size_t i = 0; i < GPUD_GPU_SESSION_CAPACITY; ++i) {
            assert(!ph_gpu_session_close_begin(&table, 1, 7, ids[i]));
            assert(!ph_gpu_session_close_ready(&table, 1, 7, ids[i], &cookie));
            assert(cookie == ids[i]);
            assert(!ph_gpu_session_close_finish(&table, 1, 7, ids[i], KB2_GPU_STATUS_OK));
        }
        assert(!table.occupied);
    }
}

static void request_codec(void) {
    unsigned char bytes[64];
    kb2_gpu_session_open_t open = {.node_type = KB2_GPU_NODE_RENDER, .client_id = UINT64_MAX};
    kb2_gpu_session_open_t saved = {.node_type = 77, .client_id = 88}, decoded = saved;
    size_t size = KB2_GPU_SESSION_OPEN_REQUEST_SIZE;
    assert(!kb2_gpu_session_open_encode(bytes, size, &open));
    for (size_t i = 0; i < size; ++i) {
        assert(kb2_gpu_session_open_decode(bytes, i, &decoded));
        assert(!memcmp(&saved, &decoded, sizeof(saved)));
    }
    bytes[KB2_GPU_SESSION_OPEN_REQUEST_FLAGS_OFFSET] = 1;
    assert(kb2_gpu_session_open_decode(bytes, size, &decoded));
    bytes[KB2_GPU_SESSION_OPEN_REQUEST_FLAGS_OFFSET] = 0;
    bytes[KB2_GPU_SESSION_OPEN_REQUEST_RESERVED_OFFSET] = 1;
    assert(kb2_gpu_session_open_decode(bytes, size, &decoded));
    bytes[KB2_GPU_SESSION_OPEN_REQUEST_RESERVED_OFFSET] = 0;
    assert(!kb2_gpu_session_open_decode(bytes, size, &decoded));
    assert(decoded.node_type == open.node_type && decoded.client_id == open.client_id);
    assert(kb2_gpu_session_open_decode(bytes, size + 1, &decoded));
    size = KB2_GPU_SESSION_CLOSE_REQUEST_SIZE;
    uint64_t id = 77;
    assert(!kb2_gpu_session_close_encode(bytes, size, UINT64_MAX));
    for (size_t i = 0; i < size; ++i) {
        assert(kb2_gpu_session_close_decode(bytes, i, &id));
        assert(id == 77);
    }
    bytes[KB2_GPU_SESSION_CLOSE_REQUEST_FLAGS_OFFSET] = 1;
    assert(kb2_gpu_session_close_decode(bytes, size, &id) && id == 77);
    bytes[KB2_GPU_SESSION_CLOSE_REQUEST_FLAGS_OFFSET] = 0;
    bytes[KB2_GPU_SESSION_CLOSE_REQUEST_RESERVED_OFFSET] = 1;
    assert(kb2_gpu_session_close_decode(bytes, size, &id) && id == 77);
    bytes[KB2_GPU_SESSION_CLOSE_REQUEST_RESERVED_OFFSET] = 0;
    assert(!kb2_gpu_session_close_decode(bytes, size, &id) && id == UINT64_MAX);
    memset(bytes, 0, size);
    assert(kb2_gpu_session_close_decode(bytes, size, &id));
}

static void completion_codec(void) {
    const uint32_t opcodes[] = {KB2_GPU_OPCODE_SESSION_OPEN, KB2_GPU_OPCODE_SESSION_CLOSE};
    unsigned char bytes[256], mutated[256];
    const size_t offsets[] = {KB2_GPU_COMPLETION_HEADER_TOTAL_SIZE_OFFSET,
                              KB2_GPU_COMPLETION_HEADER_ABI_IDENTITY_OFFSET,
                              KB2_GPU_COMPLETION_HEADER_SCHEMA_DIGEST_OFFSET,
                              KB2_GPU_COMPLETION_HEADER_DISPOSITION_OFFSET,
                              KB2_GPU_COMPLETION_HEADER_ARGUMENT_COUNT_OFFSET,
                              KB2_GPU_COMPLETION_HEADER_SPAN_COUNT_OFFSET,
                              KB2_GPU_COMPLETION_HEADER_ATTACHMENT_COUNT_OFFSET,
                              KB2_GPU_COMPLETION_HEADER_INLINE_LENGTH_OFFSET,
                              KB2_GPU_COMPLETION_HEADER_ARGUMENT_TABLE_OFFSET_OFFSET,
                              KB2_GPU_COMPLETION_HEADER_SPAN_TABLE_OFFSET_OFFSET,
                              KB2_GPU_COMPLETION_HEADER_ATTACHMENT_TABLE_OFFSET_OFFSET,
                              KB2_GPU_COMPLETION_HEADER_INLINE_OFFSET_OFFSET,
                              KB2_GPU_COMPLETION_HEADER_DETAIL_SET_ID_OFFSET,
                              KB2_GPU_COMPLETION_HEADER_DETAIL_CODE_OFFSET,
                              KB2_GPU_COMPLETION_HEADER_RESERVED_OFFSET};
    for (size_t op = 0; op < 2; ++op) {
        for (uint32_t status = 0; status <= KB2_GPU_STATUS_DEVICE_LOST; ++status) {
            kb2_gpu_session_completion_t input = {.opcode = opcodes[op],
                                                  .status = status,
                                                  .session_id = op || !status ? UINT64_MAX : 0,
                                                  .topology_epoch = !op && !status ? 1 : 0};
            kb2_gpu_session_completion_t saved = {.opcode = 999}, output = saved;
            size_t size = 99;
            assert(kb2_gpu_session_completion_encode(bytes, 1, &size, &input) ==
                       KB2_PROTOCOL_BUFFER_TOO_SMALL &&
                   size == 99);
            assert(!kb2_gpu_session_completion_encode(bytes, sizeof(bytes), &size, &input));
            for (size_t length = 0; length < size; ++length) {
                assert(kb2_gpu_session_completion_decode(bytes, length, input.opcode, &output));
                assert(!memcmp(&saved, &output, sizeof(saved)));
            }
            for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
                memcpy(mutated, bytes, size);
                mutated[offsets[i]] ^= 1;
                assert(kb2_gpu_session_completion_decode(mutated, size, input.opcode, &output));
                assert(!memcmp(&saved, &output, sizeof(saved)));
            }
            assert(!kb2_gpu_session_completion_decode(bytes, size, input.opcode, &output));
            assert(output.opcode == input.opcode && output.status == input.status &&
                   output.session_id == input.session_id &&
                   output.topology_epoch == input.topology_epoch &&
                   output.capabilities == input.capabilities);
            if (!op && !status) {
                bytes[KB2_GPU_COMPLETION_HEADER_SIZE +
                      KB2_GPU_SESSION_OPEN_RESPONSE_SESSION_ID_OFFSET] ^= 1;
                assert(kb2_gpu_session_completion_decode(bytes, size, input.opcode, &output));
            }
        }
    }
}

int main(void) {
    ownership();
    desktop_capacity_reuse();
    request_codec();
    completion_codec();
    puts("GPUD_GPU_SESSIONS_UNIT=PASS authenticated-owner generation quota drain retained-close "
         "codec");
    return 0;
}
