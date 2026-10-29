#include <assert.h>
#include <stdlib.h>
#include <string.h>
static int fail_allocation;
static void *event_malloc(size_t size)
{
    return fail_allocation ? NULL : malloc(size);
}
#define malloc event_malloc
#include "../userland/inputd/src/inputd_service.c"
#undef malloc

static unsigned publications, announcements, snapshots, removals, attempts;
static int publication_error, send_status, source_events;
static uint64_t delivered[256];

size_t inputd_input_public_device_count(const struct inputd_input_island *island)
{
    assert(island); return 1;
}
int inputd_input_public_device(const struct inputd_input_island *island,
    size_t ordinal, struct inputd_public_device *out)
{
    assert(island && !ordinal);
    *out = (struct inputd_public_device){.event_index = 3,
        .capabilities = INPUTD_INPUT_CAP_KEYBOARD};
    return 0;
}
int inputd_input_device_snapshot(uint32_t index, struct kb_input_device_snapshot *out)
{
    assert(index == 3); snapshots++;
    memset(out, 0, sizeof(*out)); strcpy(out->name, "Actual device");
    return 0;
}
int inputd_namespace_publish(pacha_fd_t fd, const struct inputd_public_device *d,
    const struct kb_input_device_snapshot *s)
{
    assert(fd == FILED_CLIENT_ENDPOINT_FD && d->event_index == 3);
    assert(!strcmp(s->name, "Actual device"));
    publications++; return publication_error;
}
int inputd_namespace_remove(pacha_fd_t fd, uint32_t index)
{
    assert(fd == FILED_CLIENT_ENDPOINT_FD && index == 3);
    ++removals;
    return publication_error;
}
int pacha_ipc_send(pacha_fd_t fd, const struct pacha_ipc_msg *request)
{
    assert(fd == 200 && publications + removals > announcements);
    assert(request->word1 == NETD_OP_UEVENT_PUBLISH);
    assert(request->word0 == PACHA_SERVICE_REQUEST_MAGIC && !request->word3);
    assert(!request->fd_count && !request->fds);
    ++attempts;
    if (send_status) return send_status;
    assert(announcements < sizeof(delivered) / sizeof(delivered[0]));
    delivered[announcements++] = request->word2;
    return 0;
}
int pacha_fd_close(pacha_fd_t fd) { assert(fd == 30); return 0; }

int inputd_input_source_begin(struct inputd_input_island *island,
    uint64_t id, uint64_t generation, int lost)
{
    assert(island && id == 1 && generation == 1 && !lost);
    return 0;
}
int inputd_input_source_device(struct inputd_input_island *island,
    uint64_t id, uint64_t generation, uint32_t kind, uint64_t platform_id,
    uint16_t segment, uint8_t bus, uint8_t device, uint8_t function,
    const struct kobox_linux_input_device_info *info, struct inputd_public_device *out)
{
    assert(island && id == 1 && generation == 1 && kind == INPUTD_SOURCE_PLATFORM);
    assert(platform_id == 9 && !segment && !bus && !device && !function && info->id == 2);
    *out = (struct inputd_public_device){.event_index = 3,
        .capabilities = INPUTD_INPUT_CAP_KEYBOARD, .source_kind = kind,
        .platform_id = platform_id};
    return 1;
}
int inputd_input_source_mark_published(struct inputd_input_island *island,
    uint64_t id, uint64_t generation, uint32_t local)
{
    assert(island && id == 1 && generation == 1 && local == 2);
    return 0;
}
int inputd_input_source_end(struct inputd_input_island *island,
    uint64_t id, uint64_t generation,
    int (*removed)(const struct inputd_public_device *, void *), void *context)
{
    assert(island && id == 1 && generation == 1 && removed && context);
    return 0;
}
int inputd_input_source_events(struct inputd_input_island *island,
    uint64_t id, uint64_t generation, const struct kobox_linux_input_record *records,
    size_t count)
{
    assert(island && id == 1 && generation == 1 && count == 1);
    assert(records[0].sequence == 20 + (unsigned)source_events);
    ++source_events;
    return 0;
}

int main(void)
{
    netd_input_uevent_descriptor_t platform = {.event_index = 3,
        .capabilities = NETD_INPUT_CAP_KEYBOARD, .action = NETD_UEVENT_ADD, .platform = 1};
    netd_input_uevent_descriptor_t decoded;
    const uint64_t encoded = netd_input_uevent_encode(platform);
    assert(netd_input_uevent_decode(encoded, &decoded) && decoded.platform &&
        decoded.event_index == 3 && decoded.action == NETD_UEVENT_ADD);
    assert(!netd_input_uevent_decode(encoded | 1, &decoded));
    struct inputd_boot_config cfg = {0};
    struct inputd_input_island island = {0};
    inputd_service_t service = {.cfg = &cfg, .input = &island};
    assert(inputd_service_publish_startup_devices(&service) == 0);
    assert(publications == 1 && announcements == 0 && snapshots == 1);
    cfg.netd_endpoint_fd = 200;
    assert(inputd_service_publish_startup_devices(&service) == 0);
    assert(publications == 2 && announcements == 0);
    assert(inputd_service_uevents_pending(&service));
    assert(inputd_service_flush_uevents(&service) == 0);
    assert(publications == 2 && announcements == 1);
    assert(!inputd_service_uevents_pending(&service));
    publication_error = -5;
    assert(inputd_service_publish_startup_devices(&service) == -5);
    assert(publications == 3 && announcements == 1);
    assert(!service.uevent_head);
    publication_error = 0;

    /* A full/unserviced netd endpoint cannot delay source registration or
     * subsequent keyboard events. Retry only on a later service turn/wake. */
    send_status = PACHA_ERR_NOT_READY;
    union {
        struct inputd_source_page page;
        unsigned char bytes[INPUTD_SOURCE_PAGE_BYTES];
    } source = {.page = {.magic = INPUTD_SOURCE_MAGIC,
        .source_id = 1, .generation = 1, .source_kind = INPUTD_SOURCE_PLATFORM,
        .platform_id = 9, .kind = INPUTD_SOURCE_SYNC_BEGIN, .sequence = 20}};
    struct inputd_source_page *page = &source.page;
    assert(dispatch_source_page(&service, page) == 0);
    page->kind = INPUTD_SOURCE_DEVICE;
    page->count = 1;
    ((struct kobox_linux_input_device_info *)(void *)page->payload)->id = 2;
    assert(dispatch_source_page(&service, page) == 0);
    unsigned before = attempts;
    assert(inputd_service_flush_uevents(&service) == 0 && attempts == before + 1);
    assert(inputd_service_uevents_pending(&service) && announcements == 1);
    page->kind = INPUTD_SOURCE_SYNC_END;
    page->count = 0;
    assert(dispatch_source_page(&service, page) == 0);
    page->kind = INPUTD_SOURCE_EVENTS;
    page->count = 1;
    memset(page->payload, 0, INPUTD_SOURCE_PAGE_BYTES - offsetof(struct inputd_source_page, payload));
    ((struct kobox_linux_input_record *)(void *)page->payload)->sequence = 20;
    assert(dispatch_source_page(&service, page) == 0 && source_events == 1);

    /* Preserve ADD/REMOVE/ADD, including platform identity, across backpressure. */
    struct inputd_public_device device = {.event_index = 3,
        .capabilities = INPUTD_INPUT_CAP_KEYBOARD, .source_kind = INPUTD_SOURCE_PLATFORM};
    assert(publish_device(&service, &device, NETD_UEVENT_REMOVE) == 0);
    assert(publish_device(&service, &device, NETD_UEVENT_ADD) == 0);
    send_status = 0;
    assert(inputd_service_flush_uevents(&service) == 0);
    assert(announcements == 4 && !inputd_service_uevents_pending(&service));
    for (unsigned i = 1; i < 4; ++i) {
        assert(netd_input_uevent_decode(delivered[i], &decoded) && decoded.platform);
        assert(decoded.action == (i == 2 ? NETD_UEVENT_REMOVE : NETD_UEVENT_ADD));
    }

    /* More than one turn's budget retains FIFO work for WRITABLE wakeup. */
    for (unsigned i = 0; i < 65; ++i)
        assert(publish_device(&service, &device, NETD_UEVENT_CHANGE) == 0);
    before = announcements;
    assert(inputd_service_flush_uevents(&service) == 0 && announcements == before + 32);
    assert(inputd_service_uevents_pending(&service));
    assert(inputd_service_flush_uevents(&service) == 0 && announcements == before + 64);
    assert(inputd_service_flush_uevents(&service) == 0 && announcements == before + 65);
    assert(!service.uevent_head && !service.uevent_tail);

    fail_allocation = 1;
    before = publications;
    assert(publish_device(&service, &device, NETD_UEVENT_ADD) == -12);
    assert(publications == before && !service.uevent_head);
    fail_allocation = 0;

    /* Terminal transport failure retains the event without hot-loop retrying,
     * and later source keyboard traffic still completes. */
    assert(publish_device(&service, &device, NETD_UEVENT_ADD) == 0);
    struct inputd_uevent *saved = service.uevent_head;
    send_status = PACHA_ERR_CLOSED;
    assert(inputd_service_flush_uevents(&service) == PACHA_ERR_CLOSED);
    assert(service.uevent_head == saved && !inputd_service_uevents_pending(&service));
    before = attempts;
    assert(inputd_service_flush_uevents(&service) == 0 && attempts == before);
    ((struct kobox_linux_input_record *)(void *)page->payload)->sequence = 21;
    assert(dispatch_source_page(&service, page) == 0 && source_events == 2);
    free(saved);
    free(service.sources);

    inputd_service_t hungup = {.cfg = &cfg};
    assert(publish_device(&hungup, &device, NETD_UEVENT_REMOVE) == 0);
    saved = hungup.uevent_head;
    inputd_service_uevent_hangup(&hungup);
    assert(!inputd_service_uevents_pending(&hungup) && hungup.uevent_head == saved);
    assert(inputd_service_flush_uevents(&hungup) == 0 && attempts == before);
    free(saved);
    puts("inputd publication: queued ordering, source input during backpressure, fairness, OOM and disconnect PASS");
    return 0;
}
