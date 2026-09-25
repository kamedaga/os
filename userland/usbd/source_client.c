/* SPDX-License-Identifier: MIT */
#include "source_client.h"

#include <pacha/ipc.h>
#include <errno.h>
#include <string.h>

static int send_page(struct usbd_source_client *source, uint32_t kind,
    const void *payload, uint32_t count, uint64_t sequence,
    uint64_t overwritten) {
    if (!source || !source->page || !source->source_id || !source->generation)
        return -EINVAL;
    size_t bytes = kind == INPUTD_SOURCE_DEVICE ?
        sizeof(struct kobox_linux_input_device_info) :
        kind == INPUTD_SOURCE_EVENTS ?
        (size_t)count * sizeof(struct kobox_linux_input_record) : 0;
    if (bytes > INPUTD_SOURCE_PAGE_BYTES -
        offsetof(struct inputd_source_page, payload) || (bytes && !payload))
        return -EINVAL;
    memset(source->page, 0, INPUTD_SOURCE_PAGE_BYTES);
    *source->page = (struct inputd_source_page){
        .magic = INPUTD_SOURCE_MAGIC, .source_id = source->source_id,
        .generation = source->generation, .sequence = sequence,
        .overwritten = overwritten, .kind = kind, .count = count,
        .pci_segment = source->pci_segment, .pci_bus = source->pci_bus,
        .pci_device = source->pci_device,
        .pci_function = source->pci_function,
    };
    if (bytes) memcpy(source->page->payload, payload, bytes);
    uint64_t correlation = ++source->correlation;
    if (!correlation) return -EOVERFLOW;
    struct pacha_ipc_fd fd = {.fd = (uint64_t)source->page_fd,
        .rights = PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_TRANSFER |
            PACHA_FD_RIGHT_MAP_READ};
    const struct pacha_ipc_msg request = {.word0 = INPUTD_SOURCE_MAGIC,
        .word1 = source->source_id, .word2 = source->generation,
        .word3 = correlation, .fds = &fd, .fd_count = 1};
    int reply_fd = pacha_ipc_call(source->endpoint_fd, &request);
    if (reply_fd < 16) return reply_fd < 0 ? reply_fd : -EIO;
    int result = pacha_timerfd_settime(source->timeout_fd,
        UINT64_C(2000000000), 0, 0);
    if (!result) {
        struct pacha_pollfd events[2] = {
            {.fd = reply_fd, .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP},
            {.fd = source->timeout_fd, .events = PACHA_FD_EVENT_READABLE},
        };
        long waited = pacha_fd_wait_many(events, 2, PACHA_FD_WAIT_FOREVER);
        if (waited <= 0 || events[1].revents ||
            events[0].revents != PACHA_FD_EVENT_READABLE)
            result = -ETIMEDOUT;
        else {
            struct pacha_ipc_msg reply = {0};
            result = pacha_ipc_recv(reply_fd, &reply);
            if (!result)
                result = reply.word0 == INPUTD_SOURCE_MAGIC &&
                    reply.word3 == correlation && !reply.fd_count ?
                    (int)(int64_t)reply.word1 : -EPROTO;
        }
    }
    (void)pacha_timerfd_settime(source->timeout_fd, 0, 0, 0);
    (void)pacha_fd_close(reply_fd);
    return result;
}

int usbd_source_open(struct usbd_source_client *source, int endpoint_fd,
    uint64_t source_id, uint64_t generation,
    uint16_t pci_segment, uint8_t pci_bus, uint8_t pci_device,
    uint8_t pci_function) {
    if (!source || source->page || endpoint_fd < 16 || !source_id ||
        !generation || pci_device > 31 || pci_function > 7)
        return -EINVAL;
    const uint64_t rights = PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE;
    int fd = pacha_vmo_create(INPUTD_SOURCE_PAGE_BYTES, rights, 0);
    if (fd < 16) return fd < 0 ? fd : -ENOMEM;
    void *mapped = pacha_mmap(fd, INPUTD_SOURCE_PAGE_BYTES,
        PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
    if (!mapped) { (void)pacha_fd_close(fd); return -ENOMEM; }
    const uint64_t timer_rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_READ |
        PACHA_FD_RIGHT_WRITE | PACHA_FD_RIGHT_WAIT |
        PACHA_FD_RIGHT_POLL | PACHA_FD_RIGHT_CLOSE;
    int timeout_fd = pacha_timerfd_create(0, 0, timer_rights, 0);
    if (timeout_fd < 16) {
        (void)pacha_munmap(mapped, INPUTD_SOURCE_PAGE_BYTES);
        (void)pacha_fd_close(fd);
        return timeout_fd < 0 ? timeout_fd : -ENOMEM;
    }
    *source = (struct usbd_source_client){.endpoint_fd = endpoint_fd,
        .page_fd = fd, .timeout_fd = timeout_fd, .page = mapped,
        .source_id = source_id, .generation = generation,
        .pci_segment = pci_segment, .pci_bus = pci_bus,
        .pci_device = pci_device, .pci_function = pci_function};
    return 0;
}

int usbd_source_sync(struct usbd_source_client *source,
    struct usbd_input_bridge *bridge, struct ph_ipc *ipc, int process_fd,
    int lost_events) {
    const struct kobox_linux_input_device_info *devices;
    size_t count = 0;
    uint64_t next_sequence = 0;
    int result = usbd_input_bridge_snapshot(bridge, ipc, process_fd,
        &devices, &count, &next_sequence);
    if (result) return result;
    result = send_page(source, INPUTD_SOURCE_SYNC_BEGIN, NULL, 0,
        next_sequence, lost_events ? 1 : 0);
    if (result) return result;
    for (size_t i = 0; i < count; i++) {
        result = send_page(source, INPUTD_SOURCE_DEVICE, &devices[i], 1, 0, 0);
        if (result) return result;
    }
    result = send_page(source, INPUTD_SOURCE_SYNC_END, NULL, 0, 0, 0);
    if (!result) source->next_sequence = next_sequence;
    return result;
}

int usbd_source_pump(struct usbd_source_client *source,
    struct usbd_input_bridge *bridge, struct ph_ipc *ipc, int process_fd) {
    for (unsigned pass = 0; pass < 8; pass++) {
        const struct kobox_linux_input_record *records;
        size_t count = 0;
        uint64_t overwritten = 0;
        int result = usbd_input_bridge_read(bridge, ipc, process_fd,
            &records, &count, &overwritten);
        if (result) return result;
        if (overwritten != source->overwritten) {
            source->overwritten = overwritten;
            result = usbd_source_sync(source, bridge, ipc, process_fd, 1);
            if (result) return result;
            continue;
        }
        if (!count) return 0;
        size_t start = 0;
        while (start < count && records[start].sequence < source->next_sequence)
            start++;
        if (start == count) continue;
        for (size_t i = start; i < count; i++) {
            if (records[i].sequence != source->next_sequence + i - start ||
                records[i].kind != KOBOX_INPUT_EVENT) {
                result = usbd_source_sync(source, bridge, ipc, process_fd,
                    records[i].sequence != source->next_sequence + i - start);
                if (result) return result;
                start = count;
                break;
            }
        }
        if (start == count) continue;
        result = send_page(source, INPUTD_SOURCE_EVENTS, records + start,
            (uint32_t)(count - start), records[start].sequence, overwritten);
        if (result) return result;
        source->next_sequence += count - start;
    }
    return 0;
}

int usbd_source_close(struct usbd_source_client *source) {
    if (!source) return -EINVAL;
    int result = 0;
    if (source->page)
        result = pacha_munmap(source->page, INPUTD_SOURCE_PAGE_BYTES);
    if (source->page_fd >= 16) {
        int closed = pacha_fd_close(source->page_fd);
        if (!result) result = closed;
    }
    if (source->timeout_fd >= 16) {
        int closed = pacha_fd_close(source->timeout_fd);
        if (!result) result = closed;
    }
    *source = (struct usbd_source_client){0};
    return result;
}
