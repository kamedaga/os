#include <assert.h>
#include <string.h>
#include "../userland/inputd/src/inputd_service.c"

static unsigned publications, announcements, snapshots;
static int publication_error;

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
    (void)fd; (void)index; assert(0); return 0;
}
int pacha_ipc_call(pacha_fd_t fd, const struct pacha_ipc_msg *request, struct pacha_ipc_msg *reply, uint64_t timeout, int *sent)
{
    assert(fd == 200 && publications > announcements);
    assert(request->word1 == NETD_OP_UEVENT_PUBLISH);
    announcements++; (void)sent; assert(timeout == PACHA_FD_WAIT_FOREVER);
    reply->word0 = PACHA_SERVICE_REPLY_MAGIC;
    reply->word3 = announcements;
    return 0;
}
int pacha_fd_close(pacha_fd_t fd) { assert(fd == 30); return 0; }

int main(void)
{
    struct inputd_boot_config cfg = {0};
    struct inputd_input_island island = {0};
    inputd_service_t service = {.cfg = &cfg, .input = &island};
    assert(inputd_service_publish_startup_devices(&service) == 0);
    assert(publications == 1 && announcements == 0 && snapshots == 1);
    cfg.netd_endpoint_fd = 200;
    assert(inputd_service_publish_startup_devices(&service) == 0);
    assert(publications == 2 && announcements == 1);
    publication_error = -5;
    assert(inputd_service_publish_startup_devices(&service) == -5);
    assert(publications == 3 && announcements == 1);
    puts("inputd publication: offline namespace, uevent ordering and failure passed");
    return 0;
}
