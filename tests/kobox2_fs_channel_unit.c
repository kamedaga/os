/* SPDX-License-Identifier: MIT */
#include "../userland/kobox2_owner/fs_channel.h"
#include <kobox2/virtqueue_x86_64.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef PH_FS_CHANNEL_PROFILE
static unsigned profile_calls;
#ifdef PH_FS_CHANNEL_TRACE
static unsigned trace_calls;
void ph_fs_channel_trace(uint64_t channel, uint32_t opcode, uint64_t sequence,
    uint64_t publish, uint64_t completion)
{
    assert(channel == 7 && opcode == KB2_FILESYSTEM_OP_WRITE && sequence > 0);
    assert(completion >= publish);
    ++trace_calls;
}
#endif
void ph_fs_channel_profile(uint32_t opcode, const uint64_t cycles[4],
    unsigned notifications, unsigned waits)
{
    assert(opcode == KB2_FILESYSTEM_OP_WRITE);
    for (unsigned i = 0; i < 4; ++i) assert(cycles[i] > 0);
    assert(notifications <= 1 && waits == 1);
    ++profile_calls;
}
#endif

struct fixture {
    struct ph_fs_channel driver, device;
    void *mapping;
    kb2_vq_chain_t **driver_slots, **device_slots;
    uint16_t *driver_owners, *device_owners;
    uint64_t generation;
    unsigned notifications, waits;
    unsigned malformed;
};

static void setup(struct fixture *f, unsigned depth, size_t arena) {
    memset(f, 0, sizeof(*f));
    f->generation = 123;
    struct ph_fs_channel_config config = {.generation = f->generation,
        .channel_id = 7, .queue_size = depth, .arena_bytes = arena};
    size_t bytes, offset;
    assert(!ph_fs_channel_layout(&config, &bytes, &offset));
    assert(bytes >= offset + arena && !(bytes % 4096));
    f->mapping = aligned_alloc(4096, bytes);
    assert(f->mapping);
    memset(f->mapping, 0, bytes);
    f->driver_slots = calloc(depth, sizeof(*f->driver_slots));
    f->device_slots = calloc(depth, sizeof(*f->device_slots));
    f->driver_owners = calloc(depth, sizeof(*f->driver_owners));
    f->device_owners = calloc(depth, sizeof(*f->device_owners));
    assert(f->driver_slots && f->device_slots && f->driver_owners && f->device_owners);
    assert(!ph_fs_channel_bind(&f->driver, f->mapping, bytes, &config,
        KB2_VQ_DRIVER, &kb2_vq_x86_64_atomics, f->driver_slots, f->driver_owners));
    assert(!ph_fs_channel_bind(&f->device, f->mapping, bytes, &config,
        KB2_VQ_DEVICE, &kb2_vq_x86_64_atomics, f->device_slots, f->device_owners));
}

static void teardown(struct fixture *f) {
    free(f->driver_slots); free(f->device_slots);
    free(f->driver_owners); free(f->device_owners); free(f->mapping);
}

static int notify(void *context, uint32_t id) {
    struct fixture *f = context;
    assert(id == 1);
    f->notifications++;
    return 0;
}

static int service(void *context, uint32_t id) {
    struct fixture *f = context;
    assert(id == 2);
    f->waits++;
    size_t depth = f->device.config.queue_size;
    kb2_vq_segment_t *segments = calloc(depth, sizeof(*segments));
    assert(segments);
    kb2_vq_chain_t chain = {.segments = segments, .capacity = depth};
    assert(!kb2_vq_take_available(&f->device.lane, f->generation, &chain));
    unsigned char *input = malloc((size_t)chain.readable);
    assert(input);
    assert(!kb2_vq_copy_request(&f->device.lane, f->generation, &chain,
        0, input, (size_t)chain.readable));
    kb2_fs_request_t request;
    assert(!kb2_fs_request_decode(input, (size_t)chain.readable, chain.readable, &request));
    unsigned char header[KB2_FILESYSTEM_RESPONSE_SIZE];
    kb2_fs_response_t response = {.opcode = request.opcode,
        .generation = request.generation, .sequence = request.sequence,
        .result = (int64_t)request.data.length, .data_length = request.data.length};
    if(f->malformed==1) ++response.sequence;
    if(f->malformed==2) ++response.data_length;
    if(f->malformed==3) {response.result=-EIO;response.data_length=0;}
    if(f->malformed==4) ++response.result;
    assert(!kb2_fs_response_encode(header, sizeof(header), &response));
    assert(!kb2_vq_write_response(&f->device.lane, f->generation, &chain,
        header, sizeof(header)));
    if(f->malformed!=3)
        assert(!kb2_vq_write_response(&f->device.lane, f->generation, &chain,
            input + request.data.offset, (size_t)request.data.length));
    int wake;
    assert(!kb2_vq_complete(&f->device.lane, f->generation, &chain, &wake));
    free(input);
    free(segments);
    return 0;
}

static void roundtrip(unsigned depth, size_t payload) {
    struct fixture f;
    size_t request_size = KB2_FILESYSTEM_REQUEST_SIZE + payload;
    size_t reply_capacity = KB2_FILESYSTEM_RESPONSE_SIZE + payload;
    setup(&f, depth, request_size + reply_capacity + 97);
    unsigned char *request = calloc(1, request_size);
    unsigned char *response = malloc(reply_capacity);
    assert(request && response);
    for (size_t i = 0; i < payload; ++i)
        request[KB2_FILESYSTEM_REQUEST_SIZE + i] = (unsigned char)(i * 71);
    kb2_fs_request_t decoded = {.opcode = KB2_FILESYSTEM_OP_WRITE,
        .generation = f.generation, .sequence = 1, .client = 4,
        .credential_generation = 1, .handle = 6,
        .data = {KB2_FILESYSTEM_REQUEST_SIZE, payload}, .length = payload};
    assert(!kb2_fs_request_encode(request, request_size, request_size, &decoded));
    struct ph_fs_channel_wait wait = {.context = &f, .notify = notify, .wait = service};
    size_t used;
    assert(!ph_fs_channel_call(&f.driver, request, request_size,
        response, reply_capacity, &used, &wait));
    assert(used == reply_capacity && f.waits == 1);
    assert(!memcmp(response + KB2_FILESYSTEM_RESPONSE_SIZE,
        request + KB2_FILESYSTEM_REQUEST_SIZE, payload));
    assert(!f.driver.lane.owned && !f.device.lane.owned);
    decoded.sequence++;
    assert(!kb2_fs_request_encode(request, request_size, request_size, &decoded));
    assert(!ph_fs_channel_call(&f.driver, request, request_size,
        response, reply_capacity, &used, &wait));
    assert(f.waits == 2);
    unsigned char *destination=malloc(payload+2);assert(destination);
    memset(destination,0xa5,payload+2);kb2_fs_response_t split;
    decoded.sequence++;
    assert(!kb2_fs_request_encode(request,request_size,request_size,&decoded));
    assert(!ph_fs_channel_call_split(&f.driver,request,request_size,&split,
        destination+1,payload,&used,&wait));
    assert(split.result==(int64_t)payload && split.data_length==payload && used==reply_capacity);
    assert(destination[0]==0xa5 && destination[payload+1]==0xa5);
    assert(!memcmp(destination+1,request+KB2_FILESYSTEM_REQUEST_SIZE,payload));
    assert(f.waits==3 && !f.driver.lane.owned && !f.device.lane.owned);
    decoded.sequence++;
    assert(!kb2_fs_request_encode(request,request_size,request_size,&decoded));
    unsigned char *arena=(unsigned char *)f.driver.mapping.address+f.driver.arena_offset;
    memcpy(arena,request,request_size);
    assert(!ph_fs_channel_call_split(&f.driver,arena,request_size,&split,
        destination+1,payload,&used,&wait));
    assert(split.sequence==decoded.sequence && used==reply_capacity);
    assert(!memcmp(destination+1,request+KB2_FILESYSTEM_REQUEST_SIZE,payload));
    assert(f.waits==4 && !f.driver.lane.owned && !f.device.lane.owned);
    assert(ph_fs_channel_call_split(&f.driver,request,request_size,&split,
        NULL,payload,&used,&wait)==-EINVAL);
    assert(ph_fs_channel_call_split(&f.driver,request,request_size,&split,
        destination,SIZE_MAX,&used,&wait)==-EINVAL);
    free(destination);
    free(request); free(response); teardown(&f);
}

static void split_invalid_reply(void)
{
    for(unsigned bad=1;bad<=4;++bad) {
        struct fixture f;setup(&f,2,4096);f.malformed=bad;
        uint8_t request[KB2_FILESYSTEM_REQUEST_SIZE+8]={0},data[10];memset(data,0xa5,sizeof(data));
        kb2_fs_request_t r={.opcode=bad==4 ? KB2_FILESYSTEM_OP_PREAD : KB2_FILESYSTEM_OP_WRITE,.generation=f.generation,
            .sequence=1,.data={KB2_FILESYSTEM_REQUEST_SIZE,8},.length=8};
        assert(!kb2_fs_request_encode(request,sizeof(request),sizeof(request),&r));
        struct ph_fs_channel_wait wait={.context=&f,.notify=notify,.wait=service};
        kb2_fs_response_t response;size_t used=0;
        int status=ph_fs_channel_call_split(&f.driver,request,sizeof(request),
            &response,data+1,8,&used,&wait);
        assert(status==(bad==3 ? 0 : -EPROTO));
        for(size_t i=0;i<sizeof(data);++i) assert(data[i]==0xa5);
        if(bad==3) assert(response.result==-EIO && !response.data_length && used==64 && !f.driver.lane.owned);
        else assert(f.driver.channel.faulted && f.driver.lane.owned);
        teardown(&f);
    }
}

static void multiple_outstanding(void) {
    struct fixture f;
    setup(&f, 8, 65536);
    kb2_vq_segment_t driver_segments[2][2], device_segments[2][8];
    kb2_vq_chain_t driver_chains[2] = {{0}}, device_chains[2] = {{0}};
    for (unsigned i = 0; i < 2; ++i) {
        size_t offset = f.driver.arena_offset + i * 4096;
        driver_segments[i][0] = (kb2_vq_segment_t){offset, 192, (uint16_t)(2*i), 0};
        driver_segments[i][1] = (kb2_vq_segment_t){offset + 192, 64, (uint16_t)(2*i+1), 1};
        driver_chains[i] = (kb2_vq_chain_t){.segments = driver_segments[i],
            .capacity = 2, .count = 2, .head = (uint16_t)(2*i)};
        kb2_fs_request_t request = {.opcode = KB2_FILESYSTEM_OP_CLOSE,
            .generation = f.generation, .sequence = i + 1, .client = 4,
            .credential_generation = 1, .handle = i + 1};
        assert(!kb2_fs_request_encode((unsigned char *)f.mapping + offset, 192, 192, &request));
        int wake;
        assert(!kb2_vq_publish(&f.driver.lane, f.generation, &driver_chains[i], &wake));
    }
    assert(f.driver.lane.owned == 2);
    for (unsigned i = 0; i < 2; ++i) {
        device_chains[i] = (kb2_vq_chain_t){.segments = device_segments[i], .capacity = 8};
        assert(!kb2_vq_take_available(&f.device.lane, f.generation, &device_chains[i]));
    }
    for (unsigned i = 2; i--;) {
        unsigned char header[64];
        kb2_fs_response_t response = {.opcode = KB2_FILESYSTEM_OP_CLOSE,
            .generation = f.generation, .sequence = i + 1};
        assert(!kb2_fs_response_encode(header, sizeof(header), &response));
        assert(!kb2_vq_write_response(&f.device.lane, f.generation, &device_chains[i], header, 64));
        int wake;
        assert(!kb2_vq_complete(&f.device.lane, f.generation, &device_chains[i], &wake));
    }
    for (unsigned i = 2; i--;) {
        kb2_vq_chain_t *completed;
        assert(!kb2_vq_take_used(&f.driver.lane, f.generation, &completed));
        assert(completed == &driver_chains[i]);
        assert(!kb2_vq_release(&f.driver.lane, f.generation, completed));
    }
    teardown(&f);
}

static void pending_take(struct fixture *f, kb2_vq_chain_t *chain,
                         kb2_vq_segment_t *segments) {
    *chain = (kb2_vq_chain_t){.segments = segments, .capacity = f->device.config.queue_size};
    assert(!kb2_vq_take_available(&f->device.lane, f->generation, chain));
}

static void pending_complete(struct fixture *f, kb2_vq_chain_t *chain, int malformed) {
    uint8_t request[KB2_FILESYSTEM_REQUEST_SIZE], header[KB2_FILESYSTEM_RESPONSE_SIZE];
    assert(!kb2_vq_copy_request(&f->device.lane, f->generation, chain,
        0, request, sizeof(request)));
    kb2_fs_request_t decoded;
    assert(!kb2_fs_request_decode(request, sizeof(request), chain->readable, &decoded));
    kb2_fs_response_t response = {.opcode = decoded.opcode,
        .generation = decoded.generation, .sequence = decoded.sequence,
        .result = 4, .data_length = 4};
    if (malformed == 1) ++response.sequence;
    if (malformed == 2) ++response.result;
    assert(!kb2_fs_response_encode(header, sizeof(header), &response));
    uint8_t payload[4]; memset(payload, (int)decoded.sequence, sizeof(payload));
    assert(!kb2_vq_write_response(&f->device.lane, f->generation, chain,
        header, sizeof(header)));
    assert(!kb2_vq_write_response(&f->device.lane, f->generation, chain,
        payload, sizeof(payload)));
    int wake;
    assert(!kb2_vq_complete(&f->device.lane, f->generation, chain, &wake));
}

static int pending_submit(struct fixture *f, struct ph_fs_pending *pending, uint64_t sequence) {
    uint8_t bytes[KB2_FILESYSTEM_REQUEST_SIZE];
    kb2_fs_request_t r = {.opcode = KB2_FILESYSTEM_OP_PREAD,
        .generation = f->generation, .sequence = sequence, .length = 4};
    assert(!kb2_fs_request_encode(bytes, sizeof(bytes), sizeof(bytes), &r));
    int wake = -1;
    int status = ph_fs_channel_submit(&f->driver, pending, bytes, sizeof(bytes),
        KB2_FILESYSTEM_RESPONSE_SIZE + 4, &wake);
    assert(wake == 0 || wake == 1);
    return status;
}

static void pending_collect(struct ph_fs_pending *pending, uint64_t sequence) {
    kb2_fs_response_t response;
    uint8_t data[4]; size_t used = 0;
    assert(!ph_fs_channel_collect(pending, &response, data, sizeof(data), &used));
    assert(response.sequence == sequence && response.result == 4 && used == 68);
    for (unsigned i = 0; i < sizeof(data); ++i) assert(data[i] == (uint8_t)sequence);
    assert(!pending->channel && !pending->chain.state);
}

static void pending_multiplexed(void) {
    struct fixture f; setup(&f, 8, 3 * 264);
    struct ph_fs_pending pending[4] = {{0}}, *done = NULL;
    kb2_vq_chain_t chains[4]; kb2_vq_segment_t segments[4][8];
    for (unsigned i = 0; i < 3; ++i) {
        assert(!pending_submit(&f, &pending[i], i + 1));
        pending_take(&f, &chains[i], segments[i]);
    }
    assert(pending_submit(&f, &pending[3], 4) == -EAGAIN);
    assert(!pending[3].channel && !f.driver.channel.faulted);
    assert(ph_fs_channel_poll(&f.driver, &done) == -EAGAIN && !done);
    int ready;
    assert(!ph_fs_channel_arm(&f.driver, &ready) && !ready);
    /* Request 1 remains executing. Requests 3 and 2 can still complete and
     * be collected by their own callers, without releasing request 1's arena. */
    pending_complete(&f, &chains[2], 0);
    pending_complete(&f, &chains[1], 0);
    assert(!ph_fs_channel_arm(&f.driver, &ready) && ready);
    assert(!ph_fs_channel_poll(&f.driver, &done) && done == &pending[2]);
    uint8_t untouched[4]; memset(untouched, 0xa5, sizeof(untouched));
    kb2_fs_response_t response; size_t used;
    assert(ph_fs_channel_collect(done, &response, untouched, 3, &used) == -ENOSPC);
    assert(done->channel && used == 0 && untouched[0] == 0xa5);
    pending_collect(done, 3);
    assert(!ph_fs_channel_poll(&f.driver, &done) && done == &pending[1]);
    assert(pending_submit(&f, &pending[3], 2) == -EEXIST);
    assert(!pending_submit(&f, &pending[3], 4));
    assert(pending[3].arena_begin == 528 && pending[0].arena_begin == 0);
    pending_take(&f, &chains[3], segments[3]);
    pending_complete(&f, &chains[3], 0);
    pending_collect(&pending[1], 2);
    assert(!ph_fs_channel_poll(&f.driver, &done) && done == &pending[3]);
    pending_collect(done, 4);
    assert(pending[0].channel && !pending[0].completed);
    assert(!f.driver.channel.faulted);
    pending_complete(&f, &chains[0], 0);
    assert(!ph_fs_channel_poll(&f.driver, &done) && done == &pending[0]);
    pending_collect(done, 1);
    assert(!f.driver.pending && !f.driver.lane.owned && !f.device.lane.owned);
    /* Descriptor and arena reuse across the 16-bit ring-index wrap must not
     * misattribute an old head to a new request sequence. */
    for (uint64_t sequence = 5; sequence < 65542; ++sequence) {
        assert(!pending_submit(&f, &pending[0], sequence));
        pending_take(&f, &chains[0], segments[0]);
        pending_complete(&f, &chains[0], 0);
        assert(!ph_fs_channel_poll(&f.driver, &done) && done == &pending[0]);
        pending_collect(done, sequence);
    }
    teardown(&f);
}

static void pending_malformed(void) {
    for (int malformed = 1; malformed <= 2; ++malformed) {
        struct fixture f; setup(&f, 2, 1024);
        struct ph_fs_pending pending = {0}, another = {0}, *done = NULL;
        kb2_vq_chain_t chain; kb2_vq_segment_t segments[2];
        assert(!pending_submit(&f, &pending, 1));
        assert(pending_submit(&f, &another, 2) == -EAGAIN);
        pending_take(&f, &chain, segments);
        pending_complete(&f, &chain, malformed);
        assert(!ph_fs_channel_poll(&f.driver, &done) && done == &pending);
        uint8_t data[4]; memset(data, 0xa5, sizeof(data));
        kb2_fs_response_t response; size_t used;
        assert(ph_fs_channel_collect(done, &response, data, sizeof(data), &used) == -EPROTO);
        assert(f.driver.channel.faulted && f.driver.pending == &pending);
        assert(pending.channel && f.driver.lane.owned && used == 0);
        for (unsigned i = 0; i < sizeof(data); ++i) assert(data[i] == 0xa5);
        assert(pending_submit(&f, &another, 2) == -EIO);
        teardown(&f);
    }
}

static int (*pending_original_store)(void *, uint64_t, uint16_t);
static int pending_fail_after_store(void *context, uint64_t address, uint16_t value) {
    int status = pending_original_store(context, address, value);
    return status ? status : -1;
}

static void pending_ambiguous_publish(void) {
    struct fixture f; setup(&f, 4, 1024);
    struct ph_fs_pending pending = {0}, another = {0};
    pending_original_store = f.driver.lane.memory.store_release;
    f.driver.lane.memory.store_release = pending_fail_after_store;
    /* The available index becomes visible before the callback reports an
     * error. Local failure must not release/replay this potentially live I/O. */
    assert(pending_submit(&f, &pending, 1) == -EPROTO);
    assert(f.driver.channel.faulted && pending.channel == &f.driver);
    assert(f.driver.pending == &pending && f.driver.lane.owned == 1);
    kb2_vq_chain_t chain; kb2_vq_segment_t segments[4];
    pending_take(&f, &chain, segments);
    assert(pending_submit(&f, &another, 2) == -EIO);
    pending_complete(&f, &chain, 0);
    assert(pending.channel && pending.chain.state && f.driver.pending == &pending);
    teardown(&f);
}

static void pending_source_overlap(void) {
    struct fixture f; setup(&f, 4, 1024);
    struct ph_fs_pending pending = {0}, *done;
    uint8_t *arena = (uint8_t *)f.mapping + f.driver.arena_offset;
    kb2_fs_request_t request = {.opcode = KB2_FILESYSTEM_OP_PREAD,
        .generation = f.generation, .sequence = 1, .length = 4};
    assert(!kb2_fs_request_encode(arena + 1, 192, 192, &request));
    int wake;
    assert(ph_fs_channel_submit(&f.driver, &pending, arena + 1, 192, 68, &wake) == -EINVAL);
    assert(!pending.channel && !f.driver.lane.owned && !f.driver.channel.faulted);
    assert(!kb2_fs_request_encode(arena, 192, 192, &request));
    assert(!ph_fs_channel_submit(&f.driver, &pending, arena, 192, 68, &wake));
    kb2_vq_chain_t chain; kb2_vq_segment_t segments[4];
    pending_take(&f, &chain, segments);
    pending_complete(&f, &chain, 0);
    assert(!ph_fs_channel_poll(&f.driver, &done) && done == &pending);
    pending_collect(done, 1);
    teardown(&f);
}

static void pending_unlocked_copies(void) {
    struct fixture f; setup(&f, 4, 1024);
    struct ph_fs_pending pending[2] = {{0}}, excess = {0}, *done;
    uint8_t wire[2][192]; void *destination[2];
    for (unsigned i = 0; i < 2; ++i) {
        kb2_fs_request_t r = {.opcode = KB2_FILESYSTEM_OP_PREAD,
            .generation = f.generation, .sequence = i + 1, .length = 4};
        assert(!kb2_fs_request_encode(wire[i], 192, 192, &r));
        assert(!ph_fs_channel_reserve(&f.driver, &pending[i], wire[i], 192, 68, &destination[i]));
    }
    assert(pending[0].chain.head != pending[1].chain.head && !f.driver.lane.owned);
    void *unused;
    assert(ph_fs_channel_reserve(&f.driver, &excess, wire[0], 192, 68, &unused) == -EAGAIN);
    /* A slow first copy has only a private reservation. A second caller may
     * publish and collect while that copy is still outstanding. */
    memcpy(destination[1], wire[1], 192);
    int notify; assert(!ph_fs_channel_publish(&pending[1], &notify));
    kb2_vq_chain_t chain; kb2_vq_segment_t segments[4];
    pending_take(&f, &chain, segments); pending_complete(&f, &chain, 0);
    assert(!ph_fs_channel_poll(&f.driver, &done) && done == &pending[1]);
    uint8_t data[4]; kb2_fs_response_t response; size_t used;
    assert(!ph_fs_channel_read_result(done, &response, data, 4, &used));
    assert(done->channel && done->result_read && f.driver.lane.owned == 1);
    assert(!ph_fs_channel_release(done));
    memcpy(destination[0], wire[0], 192);
    assert(!ph_fs_channel_publish(&pending[0], &notify));
    pending_take(&f, &chain, segments); pending_complete(&f, &chain, 0);
    assert(!ph_fs_channel_poll(&f.driver, &done) && done == &pending[0]);
    pending_collect(done, 1);
    assert(!f.driver.pending && !f.driver.lane.owned);
    teardown(&f);
}

int main(void) {
    roundtrip(2, 1);
    roundtrip(16, 16384);
    roundtrip(1024, 1048576);
    split_invalid_reply();
    multiple_outstanding();
    pending_multiplexed();
    pending_malformed();
    pending_ambiguous_publish();
    pending_source_overlap();
    pending_unlocked_copies();
    struct ph_fs_channel_config invalid = {.generation = 1, .channel_id = 1,
        .queue_size = 3, .arena_bytes = 4096};
    size_t bytes, offset;
    assert(ph_fs_channel_layout(&invalid, &bytes, &offset) == -EINVAL);
    invalid.queue_size = 2; invalid.arena_bytes = UINT64_MAX;
    assert(ph_fs_channel_layout(&invalid, &bytes, &offset) == -EINVAL);
#ifdef PH_FS_CHANNEL_PROFILE
    /* Twelve ordinary/split successes plus one Linux error response. Invalid
     * replies must fault without being reported as a completed partition. */
    assert(profile_calls == 13);
#ifdef PH_FS_CHANNEL_TRACE
    assert(trace_calls == profile_calls);
#endif
#endif
    puts("kobox2 FS channel: negotiated depth/arena, 1 MiB roundtrip, multiple outstanding PASS");
    return 0;
}
