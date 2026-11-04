#include "netlink_socket.h"
#include "libuinet_backend.h"
#include <pacha/readiness.h>

#include "pacha/abi.h"
#include "pacha/ipc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NETD_NETLINK_HANDLE_BIT (1ull << 62)
#define NETD_NETLINK_MAX 16u
#define NETD_NETLINK_QUEUE_CAPACITY 16u
#define NETD_NETLINK_MESSAGE_BYTES 2048u

typedef struct netd_netlink_message {
    uint16_t length;
    uint8_t data[NETD_NETLINK_MESSAGE_BYTES];
} netd_netlink_message_t;

struct netd_netlink_watch {
    struct netd_netlink_watch *next;
    pacha_fd_t fd;
};
typedef struct netd_netlink_socket_state {
    uint64_t handle;
    uint32_t refs;
    uint32_t protocol;
    uint32_t pid;
    uint32_t groups;
    uint8_t bound;
    uint8_t notify_pending;
    uint16_t queue_head;
    uint16_t queue_len;
    pacha_fd_t notify_fd;
    pacha_fd_t readiness_fd;
    struct pacha_readiness *readiness;
    struct netd_netlink_watch *watches;
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
    while (socket->watches) {
        struct netd_netlink_watch *watch=socket->watches;
        socket->watches=watch->next;
        (void)pacha_fd_close(watch->fd);free(watch);
    }
    if (socket->readiness) (void)pacha_munmap(socket->readiness, PACHA_READINESS_BYTES);
    if (socket->readiness_fd >= 16) (void)pacha_fd_close(socket->readiness_fd);
    if (socket->notify_fd >= 16) (void)pacha_fd_close(socket->notify_fd);
    memset(socket, 0, sizeof(*socket));
}

int netd_netlink_socket_collect_wait_sources(struct pacha_service_wait_set *wait_set)
{
    if (wait_set == NULL) return -22;
    for (unsigned i = 0; i < NETD_NETLINK_MAX; i++) {
        const netd_netlink_socket_state_t *socket = &sockets[i];
        if (socket->handle == 0 || socket->notify_fd < 16) continue;
        for (const struct netd_netlink_watch *w=socket->watches;w;w=w->next)
            if (pacha_service_wait_add(wait_set,w->fd,PACHA_FD_EVENT_HANGUP)) return -24;
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
        struct netd_netlink_watch **cursor=&socket->watches;
        while (*cursor) {
            struct netd_netlink_watch *watch=*cursor;
            if (!(pacha_service_wait_revents(wait_set,watch->fd)&PACHA_FD_EVENT_HANGUP)) {
                cursor=&watch->next;continue;
            }
            *cursor=watch->next;(void)pacha_fd_close(watch->fd);free(watch);
        }
        if ((pacha_service_wait_revents(wait_set, socket->notify_fd) &
             PACHA_FD_EVENT_HANGUP) == 0) continue;
        const uint64_t handle = socket->handle;
        destroy_socket(socket);
        printf("[netd] netlink_orphan_reap handle=%llu\n",
            (unsigned long long)handle);
    }
}

static int notify(netd_netlink_socket_state_t *socket, uint32_t events)
{
    if (socket && socket->readiness) {
        __atomic_store_n(&socket->readiness->names,
            socket->pid | ((uint64_t)socket->groups<<32),__ATOMIC_RELEASE);
        uint32_t ready = socket->queue_len ? NETD_POLLIN : 0;
        if (!socket->protocol && socket->queue_len < NETD_NETLINK_QUEUE_CAPACITY) ready |= NETD_POLLOUT;
        /* A queue operation can introduce a new edge while the level stays
         * readable. Publish before the sticky notification, even if coalesced. */
        pacha_readiness_publish_events(socket->readiness, ready, events);
    }
    if (socket) for (struct netd_netlink_watch *w=socket->watches;w;w=w->next) {
        const struct pacha_ipc_msg wake={.word0=UINT64_C(0x554556454e545244)};
        (void)pacha_ipc_send(w->fd,&wake);
    }
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

int netd_netlink_socket_open(uint64_t type, uint64_t protocol, pacha_fd_t notify_fd, uint64_t *out_handle)
{
    if (out_handle == NULL || notify_fd < 16 ||
        (type != NETD_SOCK_DGRAM && type != NETD_SOCK_RAW) ||
        (protocol != 0 && protocol != NETD_NETLINK_KOBJECT_UEVENT)) return -93;
    for (unsigned i = 0; i < NETD_NETLINK_MAX; i++) {
        if (sockets[i].handle != 0) continue;
        memset(&sockets[i], 0, sizeof(sockets[i]));
        sockets[i].handle = NETD_NETLINK_HANDLE_BIT | ++next_handle;
        sockets[i].refs = 1;
        sockets[i].protocol = (uint32_t)protocol;
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

int netd_netlink_socket_bind(netd_netlink_bind_t *request)
{
    netd_netlink_socket_state_t *socket = request ? find_socket(request->handle) : NULL;
    if (socket == NULL) return -9;
    if (socket->bound) return -22;
    uint32_t pid = request->pid;
    if (socket->protocol == 0) {
        /* Port IDs occupy a namespace per protocol, not per process. */
        for (;;) {
            if (!pid) pid = 1;
            unsigned i;
            for (i = 0; i < NETD_NETLINK_MAX; ++i)
                if (sockets[i].handle && sockets[i].protocol == 0 &&
                    sockets[i].bound && sockets[i].pid == pid) break;
            if (i == NETD_NETLINK_MAX) break;
            if (request->pid) return -98;
            ++pid;
        }
    }
    socket->pid = request->pid = pid;
    socket->groups = request->groups;
    socket->bound = 1;
    if(socket->readiness) __atomic_store_n(&socket->readiness->names,
        socket->pid | ((uint64_t)socket->groups<<32),__ATOMIC_RELEASE);
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
    const uint64_t flags = request->flags;
    if (capacity > NETD_IO_BYTES) capacity = NETD_IO_BYTES;
    size_t copied = capacity < message->length ? capacity : message->length;
    memcpy(request->data, message->data, copied);
    request->flags = copied < message->length ? 0x20 : 0;
    memset(&request->addr, 0, sizeof(request->addr));
    *out_received = (flags & 0x20) ? message->length : copied;
    if (!(flags & 2)) {
        socket->queue_head = (uint16_t)((socket->queue_head + 1u) % NETD_NETLINK_QUEUE_CAPACITY);
        socket->queue_len--;
        socket->notify_pending = 0;
        (void)notify(socket, 0);
    }
    return 0;
}

int netd_netlink_socket_poll(uint64_t handle, uint32_t events, uint32_t *out_revents, int32_t *out_error)
{
    netd_netlink_socket_state_t *socket = find_socket(handle);
    if (socket == NULL || out_revents == NULL || out_error == NULL) return -9;
    *out_revents = socket->queue_len != 0 && (events & NETD_POLLIN) != 0 ? NETD_POLLIN : 0;
    if (socket->protocol == 0 && socket->queue_len < NETD_NETLINK_QUEUE_CAPACITY)
        *out_revents |= events & NETD_POLLOUT;
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
    uint8_t action;
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
    if (descriptor.platform)
        out->pci_slot_name[0] = 0;
    out->input_capabilities = descriptor.capabilities;
    out->input = 1;
    out->action = descriptor.action;
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
    const char *event_action = fields.action == NETD_UEVENT_ADD ? "add" :
        fields.action == NETD_UEVENT_REMOVE ? "remove" : "change";
    snprintf(header, sizeof(header), "%s@%s", event_action, fields.path);
    snprintf(action, sizeof(action), "ACTION=%s", event_action);
    snprintf(devpath, sizeof(devpath), "DEVPATH=%s", fields.path);
    snprintf(subsystem_field, sizeof(subsystem_field), "SUBSYSTEM=%s", fields.subsystem);
    snprintf(devname_field, sizeof(devname_field), "DEVNAME=%s", fields.devname);
    snprintf(seqnum, sizeof(seqnum), "SEQNUM=%llu", (unsigned long long)++sequence);

    unsigned delivered = 0;
    for (unsigned i = 0; i < NETD_NETLINK_MAX; i++) {
        netd_netlink_socket_state_t *socket = &sockets[i];
        if (socket->handle == 0 || socket->protocol != NETD_NETLINK_KOBJECT_UEVENT ||
            !socket->bound || socket->groups == 0 ||
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
            if (fields.pci_slot_name[0])
                length = append_field(message->data, sizeof(message->data), length,
                    fields.pci_slot_name);
        }
        length = append_field(message->data, sizeof(message->data), length, seqnum);
        if (length >= sizeof(message->data)) continue;
        message->data[length++] = '\0';
        message->length = (uint16_t)length;
        socket->queue_len++;
        (void)notify(socket, NETD_POLLIN);
        delivered++;
    }
    printf("[netd] uevent device=%llu action=%s subscribers=%u\n",
        (unsigned long long)device, event_action, delivered);
    return 0;
}

/* Linux netlink wire records, independent of the host's headers. */
struct route_header {
    uint32_t length;
    uint16_t type, flags;
    uint32_t sequence, pid;
};
struct route_link {
    uint8_t family, pad;
    uint16_t type;
    int32_t index;
    uint32_t flags, change;
};
struct route_address {
    uint8_t family, prefix, flags, scope;
    uint32_t index;
};

static void route_attribute(netd_netlink_message_t *message, uint16_t type,
    const void *data, uint16_t size)
{
    uint16_t header[2] = {(uint16_t)(4 + size), type};
    size_t length = (4u + size + 3u) & ~3u;
    /* All callers use fixed, small attributes; the packet starts zeroed. */
    memcpy(message->data + message->length, header, 4);
    memcpy(message->data + message->length + 4, data, size);
    message->length += (uint16_t)length;
}

static void route_record(netd_netlink_message_t *message, uint16_t type,
    const struct route_header *request, uint32_t pid,
    const void *body, size_t size)
{
    memset(message, 0, sizeof(*message));
    struct route_header header = {16 + (uint32_t)size, type, 2,
        request->sequence, pid};
    memcpy(message->data, &header, 16);
    if (size) memcpy(message->data + 16, body, size);
    message->length = (uint16_t)header.length;
}

int netd_netlink_socket_send(const netd_io_t *request, size_t *out_sent)
{
    if (!request || !out_sent) return -22;
    *out_sent = 0;
    netd_netlink_socket_state_t *socket = find_socket(request->handle);
    if (!socket) return -9;
    if (socket->protocol != 0) return -95;
    if (!request->length || request->length > NETD_IO_BYTES) return -22;
    if (request->addr.addr_be || request->addr.port_be) return -95;

    /* Stage everything before binding or publishing: malformed multi-record
     * sends and queue exhaustion must never expose partial dumps. */
    netd_netlink_message_t pending[1];
    unsigned count = 0;
    struct netd_interface_snapshot snapshot;
    int present = netd_libuinet_interface_snapshot(&snapshot);
    if (present < 0) return present;
    for (size_t offset = 0; offset < request->length;) {
        struct route_header header;
        size_t remaining = request->length - offset;
        if (remaining < sizeof(header)) return -22;
        memcpy(&header, request->data + offset, sizeof(header));
        if (header.length < 17 || header.length > remaining) return -22;
        size_t aligned = ((size_t)header.length + 3u) & ~3u;
        if (aligned > remaining && header.length != remaining) return -22;
        if (header.type != 18 && header.type != 22) return -95;
        if ((header.flags & 0x301) != 0x301 ||
            (header.flags & ~0x305u)) return -95;
        uint8_t family = request->data[offset + 16];
        /* glibc sends a one-byte rtgenmsg for both dumps. Accept the
         * corresponding complete fixed header too, validating attributes. */
        size_t fixed = header.type == 18 ? 16 : 8;
        size_t payload = header.length - 16;
        if (payload <= 4) {
            for (size_t pad = 1; pad < payload; ++pad)
                if (request->data[offset + 16 + pad]) return -22;
        } else {
            if (payload < fixed) return -22;
            size_t at = 16 + fixed;
            while (at < header.length) {
                uint16_t length;
                if (header.length - at < 4) return -22;
                memcpy(&length, request->data + offset + at, 2);
                if (length < 4 || length > header.length - at) return -22;
                at += (length + 3u) & ~3u;
                if (at > header.length) return -22;
            }
        }
        int emit = present && (family == 0 || family == (header.type == 18 ? 17 : 2));
        if (header.type == 22 && !snapshot.has_address) emit = 0;
        /* This read-only glibc path accepts one dump per SEND. Batches
         * would require more queue credit than POLLOUT promises. */
        if (count != 0) return -95;
        netd_netlink_message_t *message = &pending[count++];
        memset(message, 0, sizeof(*message));
        if (emit) {
            if (header.type == 18) {
                uint32_t flags = 2 | 0x1000; /* broadcast, multicast */
                if (snapshot.admin_up) flags |= 1;
                if (snapshot.carrier) flags |= 0x40 | 0x10000;
                struct route_link link = {0, 0, 1, 1, flags, 0};
                route_record(message, 16, &header, 0, &link, sizeof(link));
                route_attribute(message, 3, "net0", 5);
                route_attribute(message, 1, snapshot.mac, 6);
                const uint8_t broadcast[6] = {255,255,255,255,255,255};
                route_attribute(message, 2, broadcast, 6);
                route_attribute(message, 4, &snapshot.mtu, 4);
                route_attribute(message, 33, &snapshot.carrier, 1);
                uint8_t state = snapshot.admin_up ? (snapshot.carrier ? 6 : 2) : 2;
                route_attribute(message, 16, &state, 1);
            } else {
                uint8_t prefix = 0;
                int zero = 0;
                for (unsigned i = 0; i < 32; ++i) {
                    int bit = snapshot.netmask[i / 8] & (0x80u >> (i % 8));
                    if (!bit) zero = 1;
                    else if (zero) return -22;
                    else ++prefix;
                }
                struct route_address address = {2, prefix, 0x80, 0, 1};
                route_record(message, 20, &header, 0, &address, sizeof(address));
                route_attribute(message, 1, snapshot.address, 4);
                route_attribute(message, 2, snapshot.address, 4);
                route_attribute(message, 4, snapshot.broadcast, 4);
                route_attribute(message, 3, "net0", 5);
            }
            uint32_t length = message->length;
            memcpy(message->data, &length, 4);
        }
        /* One dump occupies one queue slot, matching POLLOUT admission. */
        struct route_header done = {20, 3, 2, header.sequence, 0};
        memcpy(message->data + message->length, &done, sizeof(done));
        message->length += 20; /* zeroed int32 success follows the header */
        offset += aligned > remaining ? remaining : aligned;
    }
    if (count > NETD_NETLINK_QUEUE_CAPACITY - socket->queue_len) return -11;
    if (!socket->bound) {
        netd_netlink_bind_t binding = {.handle = socket->handle};
        int status = netd_netlink_socket_bind(&binding);
        if (status) return status;
    }
    for (unsigned i = 0; i < count; ++i) {
        for (size_t at = 0; at < pending[i].length;) {
            uint32_t length;
            memcpy(&length, pending[i].data + at, 4);
            memcpy(pending[i].data + at + 12, &socket->pid, 4);
            at += (length + 3u) & ~3u;
        }
        unsigned tail = (socket->queue_head + socket->queue_len) % NETD_NETLINK_QUEUE_CAPACITY;
        socket->queue[tail] = pending[i];
        ++socket->queue_len;
    }
    (void)notify(socket, NETD_POLLIN);
    *out_sent = request->length;
    return 0;
}

int netd_netlink_socket_readiness(uint64_t handle, pacha_fd_t *out)
{
    netd_netlink_socket_state_t *socket = find_socket(handle);
    if (!socket || !out) return -9;
    if (!socket->readiness) {
        const uint64_t rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE |
            PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE | PACHA_FD_RIGHT_TRANSFER;
        pacha_fd_t fd = pacha_vmo_create(PACHA_READINESS_BYTES, rights, 0);
        if (fd < 16) return -12;
        struct pacha_readiness *page = pacha_mmap(fd, PACHA_READINESS_BYTES,
            PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
        if (!page) { (void)pacha_fd_close(fd); return -12; }
        page->identity = handle;
        socket->readiness_fd = fd;
        socket->readiness = page;
        (void)notify(socket, 0);
    }
    *out = socket->readiness_fd;
    return 0;
}

int netd_netlink_socket_arm(uint64_t handle, pacha_fd_t fd)
{
    netd_netlink_socket_state_t *socket=find_socket(handle);
    if (!socket) return -9;
    struct pacha_fd_info info;
    const uint64_t required=PACHA_FD_RIGHT_SEND | PACHA_FD_RIGHT_WAIT |
        PACHA_FD_RIGHT_POLL | PACHA_FD_RIGHT_CLOSE;
    if (fd < 16 || pacha_fd_get_info(fd,&info) || info.kind!=PACHA_FD_KIND_CHANNEL ||
        (info.rights&required)!=required) return -9;
    /* Each blocking invocation owns a distinct doorbell. A sibling reader
     * cannot consume its wake between registration and the local recheck. */
    unsigned count=0;
    for(unsigned i=0;i<NETD_NETLINK_MAX;i++)
        for(struct netd_netlink_watch*w=sockets[i].watches;w;w=w->next)count++;
    if(count>=256) return -24;
    struct netd_netlink_watch *watch=calloc(1,sizeof(*watch));
    if(!watch)return -12;
    *watch=(struct netd_netlink_watch){.next=socket->watches,.fd=fd};
    socket->watches=watch;
    return 0;
}
