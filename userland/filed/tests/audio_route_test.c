/* SPDX-License-Identifier: MIT */
#include "../src/relay.c"
#include <assert.h>

static unsigned controls, creates, closed[64], storage_closes;
static unsigned major_number = 14;
static int publication = 1, call_error, reply_error;
int filed_storage_device_route(struct filed_runtime *rt, kb2_fs_request_t *r,
    const void *tail, size_t size, filed_storage_stat_t *stat, uint64_t *handle, uint8_t *wire)
{
    (void)rt; (void)tail; (void)size; (void)wire;
    assert(r->opcode == KB2_FILESYSTEM_OP_DEVICE_ROUTE);
    *stat = (filed_storage_stat_t){.mode = 0020660, .rdev_major = major_number, .rdev_minor = 3};
    *handle = 123; return 0;
}
int filed_storage_close(filed_runtime_t *rt, uint64_t handle)
{ (void)rt; assert(handle == 123); ++storage_closes; return 0; }
int pacha_fd_close(pacha_fd_t fd)
{ assert(fd >= 0 && fd < 64); ++closed[fd]; return 0; }
int pacha_ipc_channel_create(struct pacha_ipc_channel_pair *pair, uint64_t rights, uint32_t flags)
{
    assert(!flags && rights == AUDIOD_CLIENT_RIGHTS && (rights & PACHA_FD_RIGHT_CALL));
    ++creates; *pair = (struct pacha_ipc_channel_pair){.a = 30, .b = 31}; return 0;
}
int pacha_ipc_call(pacha_fd_t fd, const struct pacha_ipc_msg *request,
    struct pacha_ipc_msg *reply, uint64_t timeout, int *published)
{
    assert(fd == 20 && timeout == PACHA_FD_WAIT_FOREVER);
    assert(request->word0 == AUDIOD_MAGIC && request->word1 == AUDIO_OPEN);
    assert(request->word2 == 1 && request->fd_count == 1 && request->fds[0].fd == 31);
    assert(request->fds[0].transfer_flags == PACHA_IPC_TRANSFER_MOVE);
    assert(request->fds[0].rights == AUDIOD_SERVER_RIGHTS);
    assert(!(request->fds[0].rights & PACHA_FD_RIGHT_CALL));
    ++controls; *published = publication;
    *reply = (struct pacha_ipc_msg){.word0 = reply_error ? 0 : AUDIOD_MAGIC, .word3 = 1};
    return call_error;
}
static void reset(void)
{
    controls = creates = storage_closes = 0; memset(closed, 0, sizeof(closed));
    publication = 1; call_error = reply_error = 0; major_number = 14;
}
int main(void)
{
    filed_runtime_t rt = {.audiod_pcm_endpoint_fd = 20};
    kb2_fs_request_t request = {.flags = 1};
    kb2_fs_response_t response = {0};
    uint8_t bytes[KB2_FILESYSTEM_REQUEST_SIZE] = {0};
    pacha_fd_t pcm = -1;
    reset();
    int status = route_device(&rt, &request, bytes, sizeof(bytes), &response, NULL, &pcm);
    assert(status == 0 && pcm == 30 && response.service == FILED_SERVICE_AUDIOD_PCM);
    assert(controls == 1 && creates == 1 && !closed[30] && !closed[31] && !storage_closes);
    reset(); request.flags = 0; pcm = -1;
    assert(route_device(&rt, &request, bytes, sizeof(bytes), &response, NULL, &pcm) == -EACCES);
    assert(!controls && !creates && storage_closes == 1 && pcm == -1);
    request.flags = 1;
    for (int committed = 0; committed <= 1; ++committed) {
        reset(); publication = committed; call_error = -5; pcm = -1;
        assert(route_device(&rt, &request, bytes, sizeof(bytes), &response, NULL, &pcm) == -EIO);
        assert(closed[30] == 1 && closed[31] == (unsigned)!committed && storage_closes == 1);
    }
    reset(); reply_error = 1; pcm = -1;
    assert(route_device(&rt, &request, bytes, sizeof(bytes), &response, NULL, &pcm) == -EIO);
    assert(closed[30] == 1 && !closed[31] && storage_closes == 1);
    reset(); major_number = 42;
    assert(route_device(&rt, &request, bytes, sizeof(bytes), &response, NULL, &pcm) == -ENODEV);
    assert(!controls && storage_closes == 1);
    puts("filed PCM routing: checked node/access, per-open capability, committed MOVE cleanup PASS");
    return 0;
}
