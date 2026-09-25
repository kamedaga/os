/* SPDX-License-Identifier: MIT */
/* Test-image-only probe of the public inputd IPC. QMP supplies HID events. */
#include <inputd/ipc_protocol.h>
#include <pacha/ipc.h>
#include <pacha/service_abi.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct probe {
    int endpoint, page_fd, timer_fd;
    void *page;
    uint64_t request_id;
};

static int invoke(struct probe *probe, uint32_t op, uint32_t size,
    int notify_fd, uint64_t *out) {
    pacha_service_envelope_t *header = probe->page;
    const uint64_t id = ++probe->request_id;
    *header = (pacha_service_envelope_t){.magic = PACHA_SERVICE_REQUEST_MAGIC,
        .abi_version = PACHA_SERVICE_ABI_VERSION, .service_id = INPUTD_SERVICE_ID,
        .op = op, .flags = PACHA_SERVICE_FLAG_PAGE_PAYLOAD,
        .request_id = id, .trace_id = id, .payload_size = size};
    struct pacha_ipc_fd fds[2] = {{.fd = (uint64_t)probe->page_fd,
        .rights = PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_TRANSFER |
            PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE}};
    unsigned count = 1;
    if (notify_fd >= 16) {
        fds[1] = (struct pacha_ipc_fd){.fd = (uint64_t)notify_fd,
            .rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE |
                PACHA_FD_RIGHT_SEND | PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL};
        count = 2;
    }
    const struct pacha_ipc_msg request = {.word0 = PACHA_SERVICE_REQUEST_MAGIC,
        .word3 = id, .fds = fds, .fd_count = count};
    int reply_fd = pacha_ipc_call(probe->endpoint, &request);
    if (reply_fd < 16) return reply_fd < 0 ? reply_fd : -5;
    struct pacha_ipc_msg reply = {0};
    int result = pacha_ipc_recv_wait(reply_fd, &reply, PACHA_FD_WAIT_FOREVER);
    (void)pacha_fd_close(reply_fd);
    if (result) return result;
    if (reply.word0 != PACHA_SERVICE_REPLY_MAGIC || reply.word3 != id ||
        header->magic != PACHA_SERVICE_REPLY_MAGIC ||
        header->service_id != INPUTD_SERVICE_ID || header->op != op ||
        header->request_id != id) return -5;
    *out = header->result;
    return (int)header->status;
}

static int tick(struct probe *probe) {
    struct pacha_pollfd event = {.fd = probe->timer_fd,
        .events = PACHA_FD_EVENT_READABLE};
    uint64_t expirations = 0;
    return pacha_fd_wait_many(&event, 1, PACHA_FD_WAIT_FOREVER) == 1 &&
        pacha_fd_read(probe->timer_fd, &expirations, sizeof(expirations)) ==
            (long)sizeof(expirations) ? 0 : -5;
}

static int open_event(struct probe *probe, unsigned index,
    uint64_t *handle, int *notify) {
    const uint64_t rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_SEND | PACHA_FD_RIGHT_RECV |
        PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL;
    struct pacha_ipc_channel_pair pair = {.a = -1, .b = -1};
    if (pacha_ipc_channel_create(&pair, rights, 0)) return -5;
    inputd_open_request_t *request = (void *)((uint8_t *)probe->page +
        PACHA_SERVICE_HEADER_BYTES);
    *request = (inputd_open_request_t){.event_index = index};
    int result = invoke(probe, INPUTD_OP_OPEN, sizeof(*request), pair.b, handle);
    (void)pacha_fd_close(pair.b);
    if (result) { (void)pacha_fd_close(pair.a); return result; }
    *notify = pair.a;
    return 0;
}

static int read_event(struct probe *probe, uint64_t handle,
    int *key, int *relative) {
    inputd_read_request_t *request = (void *)((uint8_t *)probe->page +
        PACHA_SERVICE_HEADER_BYTES);
    memset(request, 0, sizeof(*request));
    request->handle = handle;
    request->event_capacity = INPUTD_EVENT_CAPACITY;
    uint64_t count = 0;
    int result = invoke(probe, INPUTD_OP_READ, sizeof(*request), -1, &count);
    if (result == -11) return 0;
    if (result) return result;
    if (count != request->event_count || count > INPUTD_EVENT_CAPACITY) return -5;
    for (size_t i = 0; i < count; i++) {
        const inputd_input_event_t *event = &request->events[i];
        if (event->type == 1 && event->code == 30 && event->value == 1)
            *key = 1;
        if (event->type == 2 && event->code <= 1 && event->value != 0)
            *relative = 1;
    }
    return 0;
}

int seed0root_usb_input_smoke(int endpoint) {
    const uint64_t page_rights = PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE;
    int page_fd = pacha_vmo_create(PACHA_SERVICE_PAGE_BYTES, page_rights, 0);
    if (page_fd < 16) return -5;
    void *page = pacha_mmap(page_fd, PACHA_SERVICE_PAGE_BYTES,
        PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
    if (!page) { (void)pacha_fd_close(page_fd); return -5; }
    const uint64_t timer_rights = PACHA_FD_RIGHT_READ | PACHA_FD_RIGHT_WAIT |
        PACHA_FD_RIGHT_POLL | PACHA_FD_RIGHT_CLOSE;
    int timer_fd = pacha_timerfd_create(UINT64_C(20000000),
        UINT64_C(20000000), timer_rights, 0);
    if (timer_fd < 16) {
        (void)pacha_munmap(page, PACHA_SERVICE_PAGE_BYTES);
        (void)pacha_fd_close(page_fd);
        return -5;
    }
    struct probe probe = {.endpoint = endpoint, .page_fd = page_fd,
        .timer_fd = timer_fd, .page = page};
    uint64_t handle[3] = {0};
    int notify[3] = {-1, -1, -1};
    int status = 0;
    for (unsigned pass = 0; pass < 500 && (!handle[0] || !handle[1]); pass++) {
        for (unsigned i = 0; i < 2; i++) {
            if (handle[i]) continue;
            status = open_event(&probe, i, &handle[i], &notify[i]);
            if (status && status != -19) goto done;
        }
        if (!handle[0] || !handle[1])
            if ((status = tick(&probe))) goto done;
    }
    if (!handle[0] || !handle[1]) { status = -110; goto done; }
    printf("INPUTD_USB_HID_READY event0 event1\n");
    fflush(stdout);
    int key = 0, relative = 0;
    for (unsigned pass = 0; pass < 1500 && (!key || !relative); pass++) {
        for (unsigned i = 0; i < 2; i++)
            if ((status = read_event(&probe, handle[i], &key, &relative))) goto done;
        if (!key || !relative)
            if ((status = tick(&probe))) goto done;
    }
    if (!key || !relative) { status = -110; goto done; }
    printf("INPUTD_USB_HID_EVENTS=PASS key=30 relative=1\n");
    fflush(stdout);
    for (unsigned pass = 0; pass < 1500 && !handle[2]; pass++) {
        status = open_event(&probe, 2, &handle[2], &notify[2]);
        if (status && status != -19) goto done;
        if (!handle[2] && (status = tick(&probe))) goto done;
    }
    if (!handle[2]) { status = -110; goto done; }
    printf("INPUTD_USB_HID_REPLUG_READY event2\n");
    fflush(stdout);
    key = 0;
    for (unsigned pass = 0; pass < 1500 && !key; pass++) {
        if ((status = read_event(&probe, handle[2], &key, &relative))) goto done;
        if (!key && (status = tick(&probe))) goto done;
    }
    if (!key) status = -110;
    if (!status) {
        printf("INPUTD_USB_HID_HOTPLUG=PASS event2 key=30\n");
        fflush(stdout);
    }
done:
    for (unsigned i = 0; i < 3; i++)
        if (notify[i] >= 16) (void)pacha_fd_close(notify[i]);
    (void)pacha_fd_close(timer_fd);
    (void)pacha_munmap(page, PACHA_SERVICE_PAGE_BYTES);
    (void)pacha_fd_close(page_fd);
    if (status) printf("INPUTD_USB_HID_SMOKE=FAIL status=%d\n", status);
    return status;
}
