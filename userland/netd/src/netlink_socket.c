#include "netlink_socket.h"

#include "pacha/abi.h"
#include "pacha/ipc.h"

#include <stdio.h>
#include <string.h>

#define NETD_NETLINK_HANDLE_BIT (1ull << 62)
#define NETD_NETLINK_MAX 16u
#define NETD_NETLINK_QUEUE_CAPACITY 16u
#define NETD_NETLINK_MESSAGE_BYTES 2048u

typedef struct netd_netlink_message {
    uint16_t length;
    uint8_t data[NETD_NETLINK_MESSAGE_BYTES];
} netd_netlink_message_t;

typedef struct netd_netlink_socket_state {
    uint64_t handle;
    uint32_t refs;
    uint32_t pid;
    uint32_t groups;
    uint8_t bound;
    uint8_t notify_pending;
    uint16_t queue_head;
    uint16_t queue_len;
    int notify_fd;
    netd_netlink_message_t queue[NETD_NETLINK_QUEUE_CAPACITY];
} netd_netlink_socket_state_t;

static netd_netlink_socket_state_t sockets[NETD_NETLINK_MAX];
static uint64_t next_handle;

int netd_netlink_socket_is_handle(uint64_t handle)
{
    return (handle & NETD_NETLINK_HANDLE_BIT) != 0 && (handle >> 63) == 0;
}

static netd_netlink_socket_state_t *find_socket(uint64_t handle)
{
    for (unsigned i = 0; i < NETD_NETLINK_MAX; i++)
        if (sockets[i].handle == handle) return &sockets[i];
    return NULL;
}

static void destroy_socket(netd_netlink_socket_state_t *socket)
{
    if (socket == NULL) return;
    if (socket->notify_fd >= 16) (void)pacha_fd_close(socket->notify_fd);
    memset(socket, 0, sizeof(*socket));
}

int netd_netlink_socket_collect_wait_sources(struct pacha_service_wait_set *wait_set)
{
    if (wait_set == NULL) return -22;
    for (unsigned i = 0; i < NETD_NETLINK_MAX; i++) {
        const netd_netlink_socket_state_t *socket = &sockets[i];
        if (socket->handle == 0 || socket->notify_fd < 16) continue;
        if (pacha_service_wait_add(
                wait_set, socket->notify_fd, PACHA_FD_EVENT_HANGUP) != 0)
            return -24;
    }
    return 0;
}

void netd_netlink_socket_reap_hangups(
    const struct pacha_service_wait_set *wait_set)
{
    for (unsigned i = 0; i < NETD_NETLINK_MAX; i++) {
        netd_netlink_socket_state_t *socket = &sockets[i];
        if (socket->handle == 0 || socket->notify_fd < 16) continue;
        if ((pacha_service_wait_revents(wait_set, socket->notify_fd) &
             PACHA_FD_EVENT_HANGUP) == 0) continue;
        const uint64_t handle = socket->handle;
        destroy_socket(socket);
        printf("[netd] netlink_orphan_reap handle=%llu\n",
            (unsigned long long)handle);
    }
}

static int notify(netd_netlink_socket_state_t *socket)
{
    if (socket == NULL || socket->notify_fd < 16 || socket->notify_pending || socket->queue_len == 0)
        return 0;
    const struct pacha_ipc_msg message = {
        .word0 = 0x554556454e545244ull,
        .word1 = socket->queue_len,
    };
    const int status = pacha_ipc_send(socket->notify_fd, &message);
    if (status == 0) socket->notify_pending = 1;
    return status;
}

int netd_netlink_socket_open(uint64_t type, uint64_t protocol, int notify_fd, uint64_t *out_handle)
{
    if (out_handle == NULL || notify_fd < 16 ||
        (type != NETD_SOCK_DGRAM && type != NETD_SOCK_RAW) ||
        protocol != NETD_NETLINK_KOBJECT_UEVENT) return -93;
    for (unsigned i = 0; i < NETD_NETLINK_MAX; i++) {
        if (sockets[i].handle != 0) continue;
        memset(&sockets[i], 0, sizeof(sockets[i]));
        sockets[i].handle = NETD_NETLINK_HANDLE_BIT | ++next_handle;
        sockets[i].refs = 1;
        sockets[i].notify_fd = notify_fd;
        *out_handle = sockets[i].handle;
        return 0;
    }
    return -24;
}

int netd_netlink_socket_dup(uint64_t handle)
{
    netd_netlink_socket_state_t *socket = find_socket(handle);
    if (socket == NULL) return -9;
    if (socket->refs == UINT32_MAX) return -75;
    socket->refs++;
    return 0;
}

int netd_netlink_socket_bind(const netd_netlink_bind_t *request)
{
    netd_netlink_socket_state_t *socket = request ? find_socket(request->handle) : NULL;
    if (socket == NULL) return -9;
    socket->pid = request->pid;
    socket->groups = request->groups;
    socket->bound = 1;
    return 0;
}

int netd_netlink_socket_recv(netd_io_t *request, size_t capacity, size_t *out_received)
{
    if (request == NULL || out_received == NULL) return -9;
    netd_netlink_socket_state_t *socket = find_socket(request->handle);
    if (socket == NULL) return -9;
    if (socket->queue_len == 0) {
        *out_received = 0;
        return -11;
    }
    netd_netlink_message_t *message = &socket->queue[socket->queue_head];
    if (capacity < message->length) return -90;
    memcpy(request->data, message->data, message->length);
    *out_received = message->length;
    socket->queue_head = (uint16_t)((socket->queue_head + 1u) % NETD_NETLINK_QUEUE_CAPACITY);
    socket->queue_len--;
    socket->notify_pending = 0;
    (void)notify(socket);
    return 0;
}

int netd_netlink_socket_poll(uint64_t handle, uint32_t events, uint32_t *out_revents, int32_t *out_error)
{
    netd_netlink_socket_state_t *socket = find_socket(handle);
    if (socket == NULL || out_revents == NULL || out_error == NULL) return -9;
    *out_revents = socket->queue_len != 0 && (events & NETD_POLLIN) != 0 ? NETD_POLLIN : 0;
    *out_error = 0;
    return 0;
}

int netd_netlink_socket_close(uint64_t handle)
{
    netd_netlink_socket_state_t *socket = find_socket(handle);
    if (socket == NULL) return -9;
    if (socket->refs > 1) {
        socket->refs--;
        return 0;
    }
    destroy_socket(socket);
    return 0;
}

typedef struct netd_device_fields {
    char path[256];
    char subsystem[32];
    char devname[128];
    char pci_slot_name[64];
    uint8_t input_capabilities;
    uint8_t input;
} netd_device_fields_t;

static int format_fits(int written, size_t capacity)
{
    return written >= 0 && (size_t)written < capacity;
}

static int device_fields(uint64_t device, netd_device_fields_t *out)
{
    if (out == NULL) return -22;
    memset(out, 0, sizeof(*out));
    if (device == NETD_UEVENT_DRM_CARD0) {
        if (!format_fits(snprintf(out->path, sizeof(out->path),
                "/devices/pci0000:00/0000:00:03.0/virtio1/drm/card0"),
                sizeof(out->path)) ||
            !format_fits(snprintf(out->subsystem, sizeof(out->subsystem), "drm"),
                sizeof(out->subsystem)) ||
            !format_fits(snprintf(out->devname, sizeof(out->devname), "dri/card0"),
                sizeof(out->devname)))
            return -22;
        return 0;
    }

    netd_input_uevent_descriptor_t descriptor;
    if (!netd_input_uevent_decode(device, &descriptor)) return -22;
    if (!format_fits(snprintf(out->path, sizeof(out->path),
            "/devices/virtual/input/input%u/event%u",
            (unsigned)descriptor.event_index, (unsigned)descriptor.event_index),
            sizeof(out->path)) ||
        !format_fits(snprintf(out->subsystem, sizeof(out->subsystem), "input"),
            sizeof(out->subsystem)) ||
        !format_fits(snprintf(out->devname, sizeof(out->devname),
            "input/event%u", (unsigned)descriptor.event_index),
            sizeof(out->devname)) ||
        !format_fits(snprintf(out->pci_slot_name, sizeof(out->pci_slot_name),
            "PACHA_PCI_SLOT_NAME=%04x:%02x:%02x.%u",
            descriptor.segment, descriptor.bus, descriptor.device,
            descriptor.function), sizeof(out->pci_slot_name)))
        return -22;
    out->input_capabilities = descriptor.capabilities;
    out->input = 1;
    return 0;
}

static size_t append_field(uint8_t *dst, size_t capacity, size_t offset, const char *field)
{
    const size_t length = strlen(field) + 1u;
    if (offset > capacity || length > capacity - offset) return capacity + 1u;
    memcpy(dst + offset, field, length);
    return offset + length;
}

int netd_netlink_publish_device(uint64_t device)
{
    static uint64_t sequence;
    netd_device_fields_t fields;
    int status = device_fields(device, &fields);
    if (status != 0) return status;
    char header[256], action[32], devpath[320], subsystem_field[64], devname_field[128], seqnum[64];
    snprintf(header, sizeof(header), "change@%s", fields.path);
    snprintf(action, sizeof(action), "ACTION=change");
    snprintf(devpath, sizeof(devpath), "DEVPATH=%s", fields.path);
    snprintf(subsystem_field, sizeof(subsystem_field), "SUBSYSTEM=%s", fields.subsystem);
    snprintf(devname_field, sizeof(devname_field), "DEVNAME=%s", fields.devname);
    snprintf(seqnum, sizeof(seqnum), "SEQNUM=%llu", (unsigned long long)++sequence);

    unsigned delivered = 0;
    for (unsigned i = 0; i < NETD_NETLINK_MAX; i++) {
        netd_netlink_socket_state_t *socket = &sockets[i];
        if (socket->handle == 0 || !socket->bound || socket->groups == 0 ||
            socket->queue_len >= NETD_NETLINK_QUEUE_CAPACITY) continue;
        const unsigned tail = (socket->queue_head + socket->queue_len) % NETD_NETLINK_QUEUE_CAPACITY;
        netd_netlink_message_t *message = &socket->queue[tail];
        size_t length = 0;
        length = append_field(message->data, sizeof(message->data), length, header);
        length = append_field(message->data, sizeof(message->data), length, action);
        length = append_field(message->data, sizeof(message->data), length, devpath);
        length = append_field(message->data, sizeof(message->data), length, subsystem_field);
        length = append_field(message->data, sizeof(message->data), length, devname_field);
        if (fields.input) {
            length = append_field(message->data, sizeof(message->data), length, "ID_INPUT=1");
            if ((fields.input_capabilities & NETD_INPUT_CAP_KEYBOARD) != 0)
                length = append_field(message->data, sizeof(message->data), length,
                    "ID_INPUT_KEYBOARD=1");
            if ((fields.input_capabilities &
                    (NETD_INPUT_CAP_RELATIVE | NETD_INPUT_CAP_ABSOLUTE)) != 0)
                length = append_field(message->data, sizeof(message->data), length,
                    "ID_INPUT_MOUSE=1");
            length = append_field(message->data, sizeof(message->data), length,
                fields.pci_slot_name);
        }
        length = append_field(message->data, sizeof(message->data), length, seqnum);
        if (length >= sizeof(message->data)) continue;
        message->data[length++] = '\0';
        message->length = (uint16_t)length;
        socket->queue_len++;
        (void)notify(socket);
        delivered++;
    }
    printf("[netd] uevent device=%llu action=change subscribers=%u\n",
        (unsigned long long)device, delivered);
    return 0;
}
