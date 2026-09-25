#include <assert.h>
#include "../userland/gpud/drm_service.c"

static int sends, closes, send_error, close_error;
static unsigned char incoming[PH_GPU_DRM_EVENT_MESSAGE_BYTES];
static size_t incoming_size;
static int incoming_ready, waits, wait_failure, expected_sends;
int gpud_gpu_rpc_next_event(struct gpud_gpu_rpc *rpc, unsigned char *out,
    size_t capacity, size_t *size)
{
    assert(rpc && capacity >= incoming_size);
    if (!incoming_ready) { *size = 0; return 0; }
    memcpy(out, incoming, incoming_size);
    *size = incoming_size;
    incoming_ready = 0;
    return 1;
}
int gpud_drm_files_fault(struct gpud_drm_files *files, uint64_t generation, int error)
{
    assert(files->generation == generation);
    return files->terminal_error = error;
}
long pacha_syscall4(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{
    assert(nr == PACHA_FD_SYSCALL_WAIT_MANY && a1 == 2 && a2 == UINT64_MAX && !a3);
    struct pacha_pollfd *fds = (void *)(uintptr_t)a0;
    assert(fds[0].fd == 41 && fds[1].fd == 42);
    assert(fds[0].events & PACHA_FD_EVENT_HANGUP);
    assert(fds[1].events & PACHA_FD_EVENT_HANGUP);
    if (!waits++) {
        incoming_ready = 1;
        fds[1].revents = PACHA_FD_EVENT_READABLE;
    } else {
        /* The service must forward the producer's event before it can
         * observe its own imported fence as ready; otherwise it deadlocks. */
        assert(waits == 2 && !incoming_ready && sends == expected_sends);
        fds[0].revents = wait_failure ? PACHA_FD_EVENT_HANGUP :
            PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP;
    }
    return 1;
}
int pacha_ipc_send(int fd, const struct pacha_ipc_msg *message)
{
    assert(fd >= 16 && message && !message->word0 && !message->fd_count);
    ++sends;
    return send_error;
}
int pacha_fd_close(int fd) { assert(fd >= 16); ++closes; return close_error; }

static void pending(struct gpud_drm_service *service, uint64_t session,
    uint64_t correlation, int fd)
{
    struct gpud_drm_pending_fence *entry = calloc(1, sizeof(*entry));
    assert(entry);
    *entry = (struct gpud_drm_pending_fence) {
        .next = service->fences, .session = session, .correlation = correlation, .fd = fd,
    };
    service->fences = entry;
}
static void record(unsigned char *out, uint64_t session, uint64_t correlation, int status)
{
    ph_gpu_event_store_u64(out, session);
    ph_gpu_event_store_u64(out + 8, correlation);
    ph_gpu_event_store_u32(out + 16, (uint32_t)status);
    ph_gpu_event_store_u32(out + 20, 0);
}

int main(void)
{
    static struct gpud_drm_service service;
    unsigned char bytes[PH_GPU_FENCE_RECORD_BYTES * 2];
    pending(&service, 11, 21, 16);
    pending(&service, 12, 22, 17);
    assert(!sends && !closes); /* No still-open DRM file/session is required. */
    record(bytes, 13, 21, 1);
    assert(complete_fences(&service, bytes, 24) == -EPROTO);
    record(bytes, 11, 21, 0);
    assert(complete_fences(&service, bytes, 24) == -EPROTO);
    record(bytes, 11, 21, 1); bytes[20] = 1;
    assert(complete_fences(&service, bytes, 24) == -EPROTO);
    record(bytes, 11, 21, 1);
    assert(complete_fences(&service, bytes, 23) == -EPROTO);
    assert(!sends && !closes);
    record(bytes, 12, 22, -EIO);
    record(bytes + 24, 11, 21, 1);
    assert(!complete_fences(&service, bytes, sizeof(bytes)));
    assert(sends == 1 && closes == 2 && !service.fences);
    assert(complete_fences(&service, bytes, 24) == -EPROTO); /* No replay. */
    assert(!complete_fences(&service, NULL, 0));

    pending(&service, 12, 23, 18);
    record(bytes, 12, 23, 1);
    send_error = -PACHA_SYSCALL_ERR_CLOSED;
    assert(!complete_fences(&service, bytes, 24) && !service.fences);
    pending(&service, 12, 24, 19);
    record(bytes, 12, 24, 1);
    send_error = -PACHA_SYSCALL_ERR_NOT_READY;
    assert(complete_fences(&service, bytes, 24) == -EIO && service.fences);
    int before = sends;
    close_error = -1;
    assert(retire_fence(&service, service.fences, 0) == -EIO && service.fences);
    close_error = 0;
    assert(!retire_fence(&service, service.fences, 0) && !service.fences && sends == before);

    unsigned char message[PH_GPU_DRM_EVENT_MESSAGE_BYTES];
    struct ph_gpu_drm_event_message decoded;
    size_t size;
    record(bytes, 7, 99, 1);
    assert(!ph_gpu_event_encode(message, sizeof(message), &size, 9, 0, 3, bytes, 24, 1));
    assert(!ph_gpu_drm_event_decode(message, size, 9, &decoded));
    assert(decoded.fences && !decoded.session_id && decoded.data_size == 24);
    assert(ph_gpu_drm_event_decode(message, size, 10, &decoded));
    assert(ph_gpu_event_encode(message, sizeof(message), &size, 9, 7, 3, bytes, 24, 1));
    assert(ph_gpu_event_encode(message, sizeof(message), &size, 9, 0, 3, bytes, 23, 1));
    assert(!ph_gpu_drm_event_encode(message, sizeof(message), &size, 9, 7, 3, NULL, 0));
    assert(!ph_gpu_drm_event_decode(message, size, 9, &decoded) && !decoded.fences);
    service.files.generation = 9;
    struct ph_ipc ipc = {.fd = 42, .generation = 9};
    service.gpu.ipc = &ipc;
    send_error = 0;
    for (int failure = 0; failure < 2; ++failure) {
        pending(&service, 7, 100 + failure, 40);
        record(bytes, 7, 100 + failure, failure ? -EIO : 1);
        assert(!ph_gpu_event_encode(incoming, sizeof(incoming), &incoming_size,
            9, 0, 4 + failure, bytes, 24, 1));
        waits = 0;
        wait_failure = failure;
        expected_sends = sends + !failure;
        assert(wait_fence(&service, 41) == (failure ? -EPIPE : 0));
        assert(waits == 2 && !service.fences && !service.error);
    }
    puts("GPUD asynchronous fence completion: identity, close/error, replay, lifetime, event codec PASS");
}
