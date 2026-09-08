#include "service.h"
#include "broker.h"
#include "escrow.h"
#include "rights.h"
#include "pacha/ipc.h"
#include "pacha/status.h"
#include "filed/unix_path.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UNIX_CAP_COMMON (PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE | \
    PACHA_FD_RIGHT_DUP | PACHA_FD_RIGHT_TRANSFER)
#define UNIX_CAP_MEMORY (UNIX_CAP_COMMON | PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE)
#define UNIX_CAP_CHANNEL (UNIX_CAP_COMMON | PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL | \
    PACHA_FD_RIGHT_SEND | PACHA_FD_RIGHT_RECV | PACHA_FD_RIGHT_CALL)

struct service_session {
    struct service_session *next;
    struct unix_session *session;
    int control_fd;
#if defined(UNIXD_PROFILE) && UNIXD_PROFILE
    unsigned profile_pid;
    struct { uint64_t count, ticks; } profile[UNIX_OP_DIAG + 1][6];
#endif
};

#if defined(UNIXD_PROFILE) && UNIXD_PROFILE
static uint64_t server_ticks(void)
{
    uint32_t lo, hi;
    __asm__ volatile("lfence; rdtsc; lfence" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | lo;
}
static void server_record(struct service_session *s, unsigned op, unsigned stage, uint64_t *start)
{
    uint64_t end = server_ticks();
    if (s && op <= UNIX_OP_DIAG) {
        s->profile[op][stage].count++;
        s->profile[op][stage].ticks += end - *start;
    }
    *start = end;
}
static void server_dump(struct service_session *s)
{
    for (unsigned op = 0; op <= UNIX_OP_DIAG; op++)
        for (unsigned stage = 0; stage < 6; stage++) if (s->profile[op][stage].count)
            printf("UNIX_SERVER_PROFILE pid=%u op=%u stage=%u count=%llu ticks=%llu\n",
                s->profile_pid, op, stage, (unsigned long long)s->profile[op][stage].count,
                (unsigned long long)s->profile[op][stage].ticks);
    fflush(stdout);
}
#define SERVER_START uint64_t profile_start = server_ticks()
#define SERVER_RECORD(stage) server_record(session, (unsigned)message.word1, stage, &profile_start)
#else
#define SERVER_START ((void)0)
#define SERVER_RECORD(stage) ((void)0)
#define server_dump(s) ((void)0)
#endif

struct service_thread {
    struct service_thread *next;
    struct service_session *session;
    uint64_t owner;
    int thread_fd;
};

struct service_watch {
    struct service_watch *next;
    uint64_t socket;
};

struct service_waiter {
    struct service_waiter *next;
    struct service_session *session;
    struct service_watch *watches;
    uint64_t owner;
    uint32_t id;
    int notify_fd;
};

struct service_path { struct service_path *next; uint64_t socket, hold; };
struct service_handoff {
    struct service_handoff *next;
    uint64_t id, socket, target;
    int fd, imported;
};

struct unix_service {
    struct unix_broker *broker;
    struct unix_escrow escrow;
    struct service_session *sessions;
    struct service_thread *threads;
    struct service_waiter *waiters;
    uint64_t next_owner;
    uint32_t next_waiter;
    int admin;
    int filed_path;
    struct service_path *paths;
    struct service_handoff *handoffs;
    struct unix_session *handoff_roots;
    uint64_t next_handoff;
    /* Global bound, not multiplied by number of clients. Mappings retain
     * their VMOs; received FDs still close at the end of every request. */
    struct { struct unix_control *page; uint64_t session, token; } buffers[16];
    uint64_t next_buffer_token;
    unsigned next_buffer_slot;
};

static struct unix_control *find_buffer(struct unix_service *service,
    struct service_session *session, uint64_t token)
{
    if (!session || token < 2) return NULL;
    const uint64_t owner = unix_broker_session_id(session->session);
    for (unsigned i = 0; i < 16; i++)
        if (service->buffers[i].session == owner && service->buffers[i].token == token)
            return service->buffers[i].page;
    return NULL;
}

static int release_buffer(struct unix_service *service, unsigned i)
{
    if (service->buffers[i].page && pacha_munmap(service->buffers[i].page, UNIX_CONTROL_BYTES) != 0)
        return -EIO;
    memset(&service->buffers[i], 0, sizeof(service->buffers[i]));
    return 0;
}

static uint64_t retain_buffer(struct unix_service *service, struct service_session *session,
    struct unix_control *page)
{
    if (!session || service->next_buffer_token == UINT64_MAX) return 0;
    for (unsigned n = 0; n < 16; n++) {
        unsigned i = (service->next_buffer_slot + n) % 16;
        if (release_buffer(service, i)) continue;
        service->next_buffer_slot = (i + 1) % 16;
        if (!service->next_buffer_token) service->next_buffer_token = 1;
        service->buffers[i].page = page;
        service->buffers[i].session = unix_broker_session_id(session->session);
        return service->buffers[i].token = ++service->next_buffer_token;
    }
    return 0; /* No retainable slot: complete using the temporary mapping. */
}

static void release_session_buffers(struct unix_service *service, struct service_session *session)
{
    const uint64_t owner = unix_broker_session_id(session->session);
    for (unsigned i = 0; i < 16; i++) if (service->buffers[i].session == owner) {
        /* Failed unmaps stay tracked for eviction retry, but become
         * inaccessible before the session ID / pointer is retired. */
        service->buffers[i].session = 0;
        service->buffers[i].token = 0;
        (void)release_buffer(service, i);
    }
}

static int filed_path_call(struct unix_service *service, struct filed_unix_path *request)
{
    const uint64_t rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE;
    int page_fd = pacha_vmo_create(4096, rights, 0);
    if (page_fd < 16) return -ENOMEM;
    struct filed_unix_path *page = pacha_mmap(page_fd, 4096,
        PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
    if (!page) { (void)pacha_fd_close(page_fd); return -ENOMEM; }
    request->magic = FILED_UNIX_PATH_MAGIC;
    *page = *request;
    struct pacha_ipc_fd cap = { .fd = (uint64_t)page_fd, .rights = rights & ~PACHA_FD_RIGHT_TRANSFER };
    struct pacha_ipc_msg message = { .word0 = FILED_UNIX_PATH_MAGIC, .fds = &cap, .fd_count = 1 };
    int reply = pacha_ipc_call(service->filed_path, &message);
    int status = -EIO;
    if (reply >= 16) {
        struct pacha_ipc_msg response = {0};
        status = (int)pacha_kernel_status_to_errno(pacha_ipc_recv_wait(reply, &response, PACHA_FD_WAIT_FOREVER));
        if (status == 0) {
            *request = *page;
            const int64_t replied = (int64_t)response.word1;
            status = response.word0 != FILED_UNIX_PATH_MAGIC || replied > 0 || replied < -4095 ? -EPROTO :
                replied != 0 ? (int)replied : request->status == 0 ? 0 : -EPROTO;
        }
        (void)pacha_fd_close(reply);
    }
    (void)pacha_munmap(page, 4096);
    (void)pacha_fd_close(page_fd);
    return status;
}

static void release_path(void *opaque, uint64_t socket)
{
    struct unix_service *service = opaque;
    struct service_path **cursor = &service->paths;
    while (*cursor && (*cursor)->socket != socket) cursor = &(*cursor)->next;
    if (!*cursor) return;
    struct service_path *path = *cursor;
    *cursor = path->next;
    struct filed_unix_path request = { .operation = FILED_UNIX_PATH_RELEASE, .hold = path->hold };
    (void)filed_path_call(service, &request);
    free(path);
}

static int path_address(struct unix_service *service, struct service_session *session,
    struct unix_control *request, int binding)
{
    int status = unix_broker_path_check(service->broker, session->session, request->socket, binding);
    if (status != 0) return status;
    if (!request->address.length || request->address.length > UNIX_PATH_BYTES ||
        memchr(request->address.bytes, 0, request->address.length)) return -EINVAL;
    struct service_path *path = binding ? calloc(1, sizeof(*path)) : NULL;
    if (binding && !path) return -ENOMEM;
    struct filed_unix_path file = { .operation = binding ? FILED_UNIX_PATH_CREATE : FILED_UNIX_PATH_OPEN,
        .directory = request->argument, .mode = request->transaction };
    memcpy(file.path, request->address.bytes, request->address.length);
    status = filed_path_call(service, &file);
    if (status != 0) { free(path); return status; }
    request->address.filesystem = file.filesystem;
    request->address.inode = file.inode;
    /* ROUTE only resolves the authenticated inode. It must not connect or
     * change the socket's default destination as a side effect of sendto. */
    status = binding ? unix_broker_bind(service->broker, session->session, request->socket, &request->address) :
        request->operation == UNIX_OP_DGRAM_ROUTE ? 0 :
        unix_broker_connect(service->broker, session->session, request->socket, &request->address);
    if (binding && status == 0) {
        path->socket = request->socket;
        path->hold = file.hold;
        path->next = service->paths;
        service->paths = path;
    } else {
        file.operation = FILED_UNIX_PATH_RELEASE;
        (void)filed_path_call(service, &file);
        free(path);
    }
    return status;
}

struct response {
    struct pacha_ipc_fd capabilities[PACHA_IPC_MAX_TRANSFER_FDS];
    unsigned count;
};

static int retain_external(void *context, uint64_t reference)
{
    return unix_escrow_retain(&((struct unix_service *)context)->escrow, reference);
}

static void release_external(void *context, uint64_t reference)
{
    unix_escrow_release(&((struct unix_service *)context)->escrow, reference);
}

/* Reserve an entire incoming native message, including its reply capability.
 * Test before persistent allocations; CLOSE/recovery can always be received. */
static int admit(unsigned retained)
{
    struct pacha_fd_table_info info;
    const uint64_t needed = retained + PACHA_IPC_MAX_TRANSFER_FDS;
    if (pacha_fd_table(0, &info) != 0) return -EIO;
    if (info.free_slots >= needed) return 0;
    if (info.capacity < info.maximum) {
        const uint64_t previous = info.capacity;
        uint64_t target = info.capacity + needed - info.free_slots;
        if (target <= info.maximum && pacha_fd_table(target, &info) == 0 &&
            info.free_slots >= needed) {
            fprintf(stderr, "[unixd] fd capacity %llu -> %llu\n",
                (unsigned long long)previous, (unsigned long long)info.capacity);
            return 0;
        }
    }
    fprintf(stderr, "[unixd] fd admission capacity=%llu free=%llu need=%llu\n",
        (unsigned long long)info.capacity, (unsigned long long)info.free_slots,
        (unsigned long long)needed);
    return -EMFILE;
}

static int create_direction(void *context, uint64_t generation, uint32_t type,
    struct unix_direction *direction)
{
    (void)context;
    *direction = (struct unix_direction){ .tx_fd = -1, .rx_fd = -1 };
    int status = admit(2);
    if (status != 0) return status;
    direction->tx_fd = pacha_vmo_create(sizeof(struct unix_tx), UNIX_CAP_MEMORY, 0);
    if (direction->tx_fd < 16) return -ENOMEM;
    direction->rx_fd = pacha_vmo_create(sizeof(struct unix_rx), UNIX_CAP_MEMORY, 0);
    if (direction->rx_fd < 16) { status = -ENOMEM; goto fail; }
    direction->tx = pacha_mmap(direction->tx_fd, sizeof(struct unix_tx),
        PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
    if (direction->tx == NULL) { status = -ENOMEM; goto fail; }
    direction->rx = pacha_mmap(direction->rx_fd, sizeof(struct unix_rx),
        PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
    if (direction->rx == NULL) { status = -ENOMEM; goto fail; }
    status = unix_transport_init(direction->tx, direction->rx, generation, type);
    if (status == 0) return 0;
fail:
    if (direction->tx) (void)pacha_munmap(direction->tx, sizeof(struct unix_tx));
    if (direction->rx) (void)pacha_munmap(direction->rx, sizeof(struct unix_rx));
    if (direction->tx_fd >= 16) (void)pacha_fd_close(direction->tx_fd);
    if (direction->rx_fd >= 16) (void)pacha_fd_close(direction->rx_fd);
    *direction = (struct unix_direction){ .tx_fd = -1, .rx_fd = -1 };
    return status;
}

static void destroy_direction(void *context, struct unix_direction *direction)
{
    (void)context;
    (void)pacha_munmap(direction->tx, sizeof(struct unix_tx));
    (void)pacha_munmap(direction->rx, sizeof(struct unix_rx));
    (void)pacha_fd_close(direction->tx_fd);
    (void)pacha_fd_close(direction->rx_fd);
}

static void destroy_connection(void *context, struct unix_direction directions[2])
{
    (void)context;
    for (unsigned i = 0; i < 2; i++) {
        if (directions[i].tx) (void)pacha_munmap(directions[i].tx, sizeof(struct unix_endpoint));
        if (directions[i].tx_fd >= 16) (void)pacha_fd_close(directions[i].tx_fd);
    }
}

static int create_connection(void *context, uint64_t generation, uint32_t type,
    struct unix_direction directions[2])
{
    for (unsigned i = 0; i < 2; i++)
        directions[i] = (struct unix_direction){ .tx_fd = -1, .rx_fd = -1 };
    int status = admit(2);
    if (status) return status;
    for (unsigned i = 0; i < 2; i++) {
        directions[i].tx_fd = pacha_vmo_create(sizeof(struct unix_endpoint), UNIX_CAP_MEMORY, 0);
        if (directions[i].tx_fd < 16) { status = -ENOMEM; goto fail; }
        directions[i].tx = pacha_mmap(directions[i].tx_fd, sizeof(struct unix_endpoint),
            PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
        if (!directions[i].tx) { status = -ENOMEM; goto fail; }
    }
    for (unsigned i = 0; i < 2; i++) {
        directions[i].rx_fd = directions[1 - i].tx_fd;
        directions[i].rx = &((struct unix_endpoint *)directions[1 - i].tx)->rx;
        status = unix_transport_init(directions[i].tx, directions[i].rx, generation, type);
        if (status) goto fail;
    }
    return 0;
fail:
    destroy_connection(context, directions);
    return status;
}

static void append_cap(struct response *response, int fd, uint64_t rights, int move)
{
    response->capabilities[response->count++] = (struct pacha_ipc_fd){
        .fd = (uint64_t)(uint32_t)fd, .rights = rights,
        .transfer_flags = move ? PACHA_IPC_TRANSFER_MOVE : 0,
    };
}

static struct service_session *find_session(struct unix_service *service, uint64_t id)
{
    for (struct service_session *session = service->sessions; session; session = session->next)
        if (unix_broker_session_id(session->session) == id) return session;
    return NULL;
}

static int owns_thread(struct unix_service *service, struct service_session *session,
    uint64_t owner)
{
    for (struct service_thread *thread = service->threads; thread; thread = thread->next)
        if (thread->owner == owner && thread->session == session) return 1;
    return 0;
}

static int append_rights(struct unix_service *service, struct service_session *session,
    struct unix_control *request, struct pacha_ipc_fd *fds, unsigned count)
{
    const unsigned length = request->rights.count;
    if (!length || length > UNIX_TRANSFER_BATCH) return -EINVAL;
    unsigned cap_index = 0;
    for (unsigned i = 0; i < length; i++) {
        const struct unix_transfer_item *item = &request->rights.items[i];
        if (!item->object || item->capability_first != cap_index ||
            item->capability_count > UNIX_TRANSFER_CAPS_PER_ITEM) return -EINVAL;
        if (item->provider == UNIX_TRANSFER_SOCKET) {
            if (item->capability_count || item->rights || item->flags || item->provider_data) return -EINVAL;
        } else if (!item->capability_count) return -EINVAL;
        cap_index += item->capability_count;
    }
    if (cap_index != count) return -EINVAL;
    struct unix_right_ref refs[UNIX_TRANSFER_BATCH] = {{0}};
    int present = 0;
    int status = unix_broker_rights_appended(service->broker, session->session,
        request->rights.ticket, request->rights.offset, refs, length, &present);
    if (status != 0) return status;
    if (present) {
        /* Retry names the first accepted occurrences, not replacement caps.
         * RPC cleanup closes the duplicate incoming native FDs. */
        for (unsigned i = 0; i < length; i++) {
            const struct unix_transfer_item *item = &request->rights.items[i];
            if (item->provider == UNIX_TRANSFER_SOCKET) {
                if (refs[i].socket != item->object) return -EINVAL;
            } else if (!unix_escrow_matches(&service->escrow, refs[i].external, item)) return -EINVAL;
        }
        return 0;
    }
    status = admit(0);
    if (status != 0) return status;
    for (unsigned i = 0; i < length; i++) {
        const struct unix_transfer_item *item = &request->rights.items[i];
        if (item->provider == UNIX_TRANSFER_SOCKET) refs[i].socket = item->object;
        else {
            status = unix_escrow_capture(&service->escrow, item, fds, count, &refs[i].external);
            if (status != 0) break;
        }
    }
    if (status == 0) status = unix_broker_rights_append(service->broker, session->session,
        request->rights.ticket, request->rights.offset, refs, length);
    /* Successful append retained each external escrow; failed capture or
     * append drops every temporary entry, including its adopted native FDs. */
    for (unsigned i = 0; i < length; i++)
        if (refs[i].external) unix_escrow_release(&service->escrow, refs[i].external);
    return status;
}

static int claim_rights(struct unix_service *service, struct service_session *session,
    struct unix_control *request, const struct unix_read *read, struct response *response)
{
    if (!request->rights.count || request->rights.count > UNIX_TRANSFER_BATCH) return -EINVAL;
    struct unix_right_ref refs[UNIX_RIGHTS_MAX];
    struct unix_rights_info info;
    int status = unix_broker_rights_head(service->broker, session->session,
        request->socket, request->rights.ticket, read, &info, refs, UNIX_RIGHTS_MAX);
    if (status != 0) return status;
    if (request->rights.offset > info.count) return -EINVAL;
    unsigned length = info.count - request->rights.offset;
    if (length > request->rights.count) length = request->rights.count;
    const unsigned original_count = response->count;
    unsigned copied = 0;
    for (; copied < length; copied++) {
        const struct unix_right_ref *ref = &refs[request->rights.offset + copied];
        struct unix_transfer_item *item = &request->rights.items[copied];
        if (ref->socket) {
            if (copied) break; /* Socket attachment has its own segment. */
            struct unix_direction tx, rx;
            status = unix_broker_rights_socket(service->broker, session->session,
                request->socket, request->rights.ticket, read, request->rights.offset,
                &request->attachment, &tx, &rx);
            if (status) return status;
            *item = (struct unix_transfer_item){ .object = ref->socket };
            if (request->attachment.generation) {
                append_cap(response, tx.tx_fd, UNIX_CAP_MEMORY, 0);
                append_cap(response, rx.tx_fd, UNIX_CAP_COMMON | PACHA_FD_RIGHT_MAP_READ, 0);
                item->capability_count = 2;
            }
            copied++;
            break;
        } else {
            status = unix_escrow_export(&service->escrow, ref->external, item,
                response->capabilities + response->count, PACHA_IPC_MAX_TRANSFER_FDS - response->count);
            if (status == -ENOBUFS && copied) break; /* next segment starts here */
            if (status != 0) { response->count = original_count; return status; }
            item->capability_first = (uint16_t)response->count;
            response->count += item->capability_count;
        }
    }
    request->rights.count = copied;
    request->rights.total = info.count;
    request->credentials = info.credentials;
    request->rights.flags = info.credentials_present ? UNIX_RIGHTS_CREDENTIALS : 0;
    request->result = info.generation;
    return 0;
}

static int dispatch_rights(struct unix_service *service, struct service_session *session,
    struct unix_control *request, struct pacha_ipc_fd *fds, unsigned count,
    struct response *response)
{
    if (request->operation != UNIX_OP_RIGHTS_APPEND && count) return -EINVAL;
    uint32_t allowed_flags = 0;
    if (request->operation == UNIX_OP_RIGHTS_FINISH) allowed_flags = UNIX_RIGHTS_PEEK;
    if (request->operation == UNIX_OP_RIGHTS_ACK) allowed_flags = UNIX_RIGHTS_RECEIVING;
    if (request->rights.flags & ~allowed_flags) return -EINVAL;
    const int alive = owns_thread(service, session, request->io.owner);
    if (!alive && request->operation != UNIX_OP_RIGHTS_ACK &&
        request->operation != UNIX_OP_RIGHTS_FINISH) return -EPERM;
    struct unix_read read = { .owner = request->io.owner, .operation = request->io.operation,
        .ticket = request->rights.ticket, .before = request->io.before, .after = request->io.after,
        .length = request->io.length, .message_length = request->io.message_length };
    switch (request->operation) {
    case UNIX_OP_RIGHTS_PREPARE: {
        const struct unix_credentials *claimed = request->credentials.generation ? &request->credentials : NULL;
        if (request->rights.route) return unix_broker_rights_prepare_route(service->broker, session->session,
            request->socket, request->rights.route, request->io.owner, request->rights.operation,
            request->rights.total, claimed, &request->rights.ticket);
        return unix_broker_rights_prepare(service->broker, session->session, request->socket,
            request->io.owner, request->rights.operation, request->rights.total, claimed, &request->rights.ticket);
    }
    case UNIX_OP_RIGHTS_APPEND:
        return append_rights(service, session, request, fds, count);
    case UNIX_OP_RIGHTS_COMMIT: {
        struct unix_write write = { .owner = request->io.owner, .before = request->io.before,
            .after = request->io.after, .length = request->io.length };
        return unix_broker_rights_commit(service->broker, session->session, request->rights.ticket, &write);
    }
    case UNIX_OP_RIGHTS_CLAIM:
        return claim_rights(service, session, request, &read, response);
    case UNIX_OP_RIGHTS_FINISH:
        if (!alive) return unix_broker_rights_received(service->broker, session->session, request->socket,
            request->rights.ticket, request->io.owner, request->rights.operation, request->rights.count,
            !!(request->rights.flags & UNIX_RIGHTS_PEEK));
        return unix_broker_rights_consume(service->broker, session->session, request->socket,
            request->rights.ticket, request->io.owner, request->rights.operation, &read, request->rights.count,
            !!(request->rights.flags & UNIX_RIGHTS_PEEK));
    case UNIX_OP_RIGHTS_ACK:
        return unix_broker_rights_ack(service->broker, session->session, request->rights.ticket,
            request->io.owner, request->rights.operation, !!(request->rights.flags & UNIX_RIGHTS_RECEIVING));
    case UNIX_OP_RIGHTS_CANCEL:
        return unix_broker_rights_cancel(service->broker, session->session, request->rights.ticket);
    default:
        return -EOPNOTSUPP;
    }
}

static struct service_waiter *find_waiter(struct unix_service *service, uint64_t id)
{
    for (struct service_waiter *waiter = service->waiters; waiter; waiter = waiter->next)
        if (waiter->id == id) return waiter;
    return NULL;
}

static void drop_waiter(struct unix_service *service, struct service_waiter *waiter)
{
    while (waiter->watches) {
        struct service_watch *watch = waiter->watches;
        unix_broker_wait_remove(service->broker, watch->socket, waiter->id);
        waiter->watches = watch->next;
        free(watch);
    }
    struct service_waiter **cursor = &service->waiters;
    while (*cursor != waiter) cursor = &(*cursor)->next;
    *cursor = waiter->next;
    (void)pacha_fd_close(waiter->notify_fd);
    free(waiter);
}

static void drop_thread_waiters(struct unix_service *service, uint64_t owner)
{
    struct service_waiter *waiter = service->waiters;
    while (waiter) {
        struct service_waiter *next = waiter->next;
        if (waiter->owner == owner) drop_waiter(service, waiter);
        waiter = next;
    }
}

static void notify_socket(void *context, uint64_t socket)
{
    struct unix_service *service = context;
    for (struct service_waiter *waiter = service->waiters; waiter; waiter = waiter->next) {
        for (struct service_watch *watch = waiter->watches; watch; watch = watch->next) {
            if (watch->socket != socket) continue;
            const struct pacha_ipc_msg message = { .word0 = UNIX_NOTIFY_MAGIC,
                .word1 = waiter->id };
            /* No wait/retry: a full channel already contains a wake. */
            (void)pacha_ipc_send(waiter->notify_fd, &message);
            break;
        }
    }
}

static int register_waiter(struct unix_service *service, struct service_session *session,
    struct unix_control *request, struct response *response)
{
    if (!owns_thread(service, session, request->transaction)) return -EPERM;
    struct unix_socket_diagnostic info;
    int status = unix_broker_diagnose(service->broker, session->session, request->socket, &info);
    if (status != 0) return status;
    struct service_waiter *waiter = find_waiter(service, request->argument);
    if (request->argument && (!waiter || waiter->session != session ||
        waiter->owner != request->transaction)) return -EPERM;
    if (waiter) {
        unsigned registrations = 0;
        for (struct service_watch *watch = waiter->watches; watch; watch = watch->next) {
            if (watch->socket == request->socket) { request->result = waiter->id; return 0; }
            registrations++;
        }
        if (registrations >= UNIX_WAIT_REGISTRATION_LIMIT) return -EAGAIN;
    }
    struct service_watch *watch = calloc(1, sizeof(*watch));
    if (!watch) return -ENOMEM;
    if (!waiter) {
        status = admit(2);
        if (status != 0 || !service->next_waiter) {
            free(watch);
            return status != 0 ? status : -EOVERFLOW;
        }
        waiter = calloc(1, sizeof(*waiter));
        if (!waiter) { free(watch); return -ENOMEM; }
        struct pacha_ipc_channel_pair pair;
        if (pacha_ipc_channel_create(&pair, UNIX_CAP_CHANNEL, 0) != 0) {
            free(watch); free(waiter); return -ENOMEM;
        }
        waiter->id = service->next_waiter++;
        waiter->session = session;
        waiter->owner = request->transaction;
        waiter->notify_fd = pair.a;
        waiter->next = service->waiters;
        service->waiters = waiter;
        /* The receive end must not leak through fork or SCM_RIGHTS and allow
         * another process to drain this thread's wakeups. */
        append_cap(response, pair.b, PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE |
            PACHA_FD_RIGHT_RECV | PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL, 1);
        response->capabilities[response->count - 1].transfer_flags |=
            PACHA_IPC_TRANSFER_PRIVATE | PACHA_IPC_TRANSFER_CLOEXEC;
    }
    watch->socket = request->socket;
    watch->next = waiter->watches;
    waiter->watches = watch;
    request->result = waiter->id;
    /* Covers readiness that existed before this control-plane registration.
     * The shared-memory arm/recheck protocol covers subsequent direct I/O. */
    notify_socket(service, request->socket);
    return 0;
}

static int sync_waiter(struct unix_service *service, struct service_session *session,
    struct unix_control *request, struct response *response)
{
    const struct service_waiter *waiter = find_waiter(service, request->argument);
    if (!waiter) return -ENOENT;
    for (const struct service_watch *watch = waiter->watches; watch; watch = watch->next) {
        if (!unix_broker_can_notify(service->broker, session->session, request->socket, watch->socket))
            continue;
        if (request->operation == UNIX_OP_WAIT_NOTIFY) {
            const struct pacha_ipc_msg message = { .word0 = UNIX_NOTIFY_MAGIC,
                .word1 = waiter->id };
            const int status = pacha_ipc_send(waiter->notify_fd, &message);
            /* With no FD payload, native SEND's ALLOC means the destination
             * queue is full (IpcQueue.push/TableFull), not a cap allocation. */
            return status == 0 || status == PACHA_ERR_ALLOC ? 0 : -EIO;
        }
        append_cap(response, waiter->notify_fd,
            PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_SEND, 0);
        response->capabilities[response->count - 1].transfer_flags =
            PACHA_IPC_TRANSFER_PRIVATE | PACHA_IPC_TRANSFER_CLOEXEC;
        request->result = waiter->id;
        return 0;
    }
    return -EPERM;
}

static int remove_waiter(struct unix_service *service, struct service_session *session,
    const struct unix_control *request)
{
    struct service_waiter *waiter = find_waiter(service, request->argument);
    if (!waiter) return 0; /* Retried removal has no additional effect. */
    if (waiter->session != session) return -EPERM;
    if (!request->socket) { drop_waiter(service, waiter); return 0; }
    struct service_watch **cursor = &waiter->watches;
    while (*cursor) {
        struct service_watch *watch = *cursor;
        if (watch->socket != request->socket) { cursor = &watch->next; continue; }
        unix_broker_wait_remove(service->broker, watch->socket, waiter->id);
        *cursor = watch->next;
        free(watch);
        break;
    }
    return 0;
}

static void prune_socket_watches(struct unix_service *service,
    struct service_session *session, uint64_t socket)
{
    struct unix_socket_diagnostic info;
    if (unix_broker_diagnose(service->broker, session->session, socket, &info) == 0) return;
    /* A poller may still be asleep while another thread closes its last FD.
     * Wake it to recheck ownership before removing the registration. */
    notify_socket(service, socket);
    for (struct service_waiter *waiter = service->waiters; waiter; waiter = waiter->next) {
        if (waiter->session != session) continue;
        const struct unix_control removal = { .socket = socket, .argument = waiter->id };
        (void)remove_waiter(service, session, &removal);
    }
}

static int register_session(struct unix_service *service, struct unix_control *request,
    struct response *response)
{
    int status = admit(2);
    if (status != 0) return status;
    struct service_session *session = calloc(1, sizeof(*session));
    if (session == NULL) return -ENOMEM;
#if defined(UNIXD_PROFILE) && UNIXD_PROFILE
    session->profile_pid = request->credentials.pid;
#endif
    status = unix_broker_session_create(service->broker, &request->credentials, &session->session);
    if (status != 0) { free(session); return status; }
    struct pacha_ipc_channel_pair pair = { .a = -1, .b = -1 };
    status = pacha_ipc_channel_create(&pair, UNIX_CAP_CHANNEL, 0);
    if (status != 0) {
        unix_broker_session_destroy(service->broker, session->session);
        free(session);
        return -ENOMEM;
    }
    session->control_fd = pair.a;
    session->next = service->sessions;
    service->sessions = session;
    request->result = unix_broker_session_id(session->session);
    /* Only the trusted admin receives this transferable bootstrap cap. Its
     * final LPR delivery must remove DUP/TRANSFER and set PRIVATE. */
    append_cap(response, pair.b, UNIX_CAP_CHANNEL, 1);
    return 0;
}

static int authorize_handoff(struct unix_service *service, struct service_handoff *handoff,
    struct unix_control *request, unsigned count)
{
    if (count || request->operation != UNIX_OP_HANDOFF || request->socket != handoff->socket || !request->argument)
        return -EPERM;
    struct service_session *target = service->sessions;
    while (target && unix_broker_session_id(target->session) != request->argument) target = target->next;
    if (!target) return -ESRCH;
    if (handoff->target && handoff->target != request->argument) return -EALREADY;
    handoff->target = request->argument;
    request->result = handoff->id;
    return 0;
}

static int handoff_socket(struct unix_service *service, struct service_session *session,
    struct unix_control *request, struct response *response)
{
    if (request->argument) {
        struct service_handoff *handoff = service->handoffs;
        while (handoff && handoff->id != request->argument) handoff = handoff->next;
        if (!handoff || handoff->socket != request->socket ||
            handoff->target != unix_broker_session_id(session->session)) return -EPERM;
        if (!handoff->imported) {
            int status = unix_broker_retain(service->broker, service->handoff_roots,
                handoff->socket, session->session);
            if (status) return status;
            handoff->imported = 1;
        }
        struct unix_socket_diagnostic info;
        int status = unix_broker_diagnose(service->broker, session->session, handoff->socket, &info);
        if (status) return status;
        request->attachment = (struct unix_attachment){ .socket = handoff->socket,
            .type = info.type, .flags = info.flags, .listening = info.listening };
        struct unix_direction tx, rx;
        status = unix_broker_attach(service->broker, session->session, handoff->socket,
            &request->attachment, &tx, &rx);
        if (status == -ENOTCONN) return 0;
        if (status) return status;
        append_cap(response, tx.tx_fd, UNIX_CAP_MEMORY, 0);
        append_cap(response, rx.tx_fd, UNIX_CAP_COMMON | PACHA_FD_RIGHT_MAP_READ, 0);
        return 0;
    }
    int status = admit(2);
    if (status) return status;
    if (service->next_handoff == UINT64_MAX) return -EOVERFLOW;
    if (!service->handoff_roots) {
        const struct unix_credentials credentials = { .pid = 1, .generation = 1 };
        status = unix_broker_session_create(service->broker, &credentials, &service->handoff_roots);
        if (status) return status;
    }
    struct service_handoff *handoff = calloc(1, sizeof(*handoff));
    if (!handoff) return -ENOMEM;
    struct pacha_ipc_channel_pair pair;
    status = pacha_ipc_channel_create(&pair, UNIX_CAP_CHANNEL, 0);
    if (status) { free(handoff); return -ENOMEM; }
    status = unix_broker_retain(service->broker, session->session, request->socket, service->handoff_roots);
    if (status) { (void)pacha_fd_close(pair.a); (void)pacha_fd_close(pair.b); free(handoff); return status; }
    *handoff = (struct service_handoff){ .next = service->handoffs, .id = ++service->next_handoff,
        .socket = request->socket, .fd = pair.a };
    service->handoffs = handoff;
    request->result = handoff->id;
    append_cap(response, pair.b, UNIX_CAP_COMMON | PACHA_FD_RIGHT_CALL, 1);
    return 0;
}

static int dispatch(struct unix_service *service, struct service_session *session,
    struct unix_control *request, struct pacha_ipc_fd *fds, unsigned count,
    struct response *response)
{
    const uint32_t op = request->operation;
    if (op == UNIX_OP_HELLO) {
        if (count != 0) return -EINVAL;
        request->result = UNIX_SERVICE_VERSION;
        if (session) unix_broker_session_identity(session->session, &request->credentials);
        return 0;
    }
    if (session == NULL) {
        if (count != 0) return -EINVAL;
        if (op == UNIX_OP_PROCESS_REGISTER) return register_session(service, request, response);
        if (op == UNIX_OP_PROCESS_CREDENTIALS) {
            struct service_session *target = find_session(service, request->socket);
            return target ? unix_broker_session_credentials(target->session, &request->credentials) : -ESRCH;
        }
        if (op == UNIX_OP_RETAIN) {
            struct service_session *source = find_session(service, request->argument);
            struct service_session *destination = find_session(service, request->transaction);
            return source && destination ? unix_broker_retain(service->broker,
                source->session, request->socket, destination->session) : -ESRCH;
        }
        return -EPERM;
    }
    if (op == UNIX_OP_PROCESS_REGISTER || op == UNIX_OP_PROCESS_CREDENTIALS)
        return -EPERM;
    if (op == UNIX_OP_THREAD_REGISTER) {
        if (count != 1) return -EINVAL;
        struct pacha_fd_info info;
        if (pacha_fd_get_info((int)fds[0].fd, &info) != 0 ||
            info.kind != PACHA_FD_KIND_THREAD ||
            (info.rights & (PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL)) !=
                (PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL)) return -EBADF;
        const int status = admit(0);
        if (status != 0) return status;
        if (service->next_owner == UINT64_MAX) return -EOVERFLOW;
        struct service_thread *thread = calloc(1, sizeof(*thread));
        if (thread == NULL) return -ENOMEM;
        thread->thread_fd = (int)fds[0].fd;
        fds[0].fd = 0;
        thread->session = session;
        thread->owner = service->next_owner++;
        thread->next = service->threads;
        service->threads = thread;
        request->result = thread->owner;
        return 0;
    }
    if (op >= UNIX_OP_RIGHTS_PREPARE && op <= UNIX_OP_RIGHTS_CANCEL)
        return dispatch_rights(service, session, request, fds, count, response);
    if (count != 0) return -EINVAL;
    switch (op) {
    case UNIX_OP_HANDOFF:
        return handoff_socket(service, session, request, response);
    case UNIX_OP_WAIT_REGISTER:
        return register_waiter(service, session, request, response);
    case UNIX_OP_WAIT_SYNC:
    case UNIX_OP_WAIT_NOTIFY:
        return sync_waiter(service, session, request, response);
    case UNIX_OP_WAIT_REMOVE:
        return remove_waiter(service, session, request);
    case UNIX_OP_SOCKET:
        if (request->argument > UINT32_MAX || request->transaction > UINT32_MAX) return -EINVAL;
        return unix_broker_socket(service->broker, session->session,
            (uint32_t)request->argument, (uint32_t)request->transaction, &request->result);
    case UNIX_OP_SOCKETPAIR: {
        if (request->argument > UINT32_MAX || request->transaction > UINT32_MAX) return -EINVAL;
        uint64_t pair[2];
        const int status = unix_broker_socketpair(service->broker, session->session,
            (uint32_t)request->argument, (uint32_t)request->transaction, pair);
        if (status != 0) return status;
        request->result = pair[0];
        request->argument = pair[1];
        return status;
    }
    case UNIX_OP_BIND:
        if (request->address.kind == UNIX_ADDRESS_PATH) return path_address(service, session, request, 1);
        return unix_broker_bind(service->broker, session->session, request->socket, &request->address);
    case UNIX_OP_LISTEN:
        if (request->argument > UINT32_MAX) return -EINVAL;
        return unix_broker_listen(service->broker, session->session,
            request->socket, (uint32_t)request->argument);
    case UNIX_OP_CONNECT:
        if (request->address.kind == UNIX_ADDRESS_PATH) return path_address(service, session, request, 0);
        return unix_broker_connect(service->broker, session->session, request->socket, &request->address);
    case UNIX_OP_ACCEPT:
        return unix_broker_accept(service->broker, session->session, request->socket, &request->result);
    case UNIX_OP_NAME:
        if (request->argument > 1) return -EINVAL;
        return unix_broker_name(service->broker, session->session, request->socket,
            request->argument != 0, &request->address);
    case UNIX_OP_POLL:
        if (request->argument > UINT32_MAX) return -EINVAL;
        {
            int status = unix_broker_poll_sequence(service->broker, session->session,
                request->socket, &request->poll_sequence);
            if (status) return status;
        }
        return unix_broker_poll(service->broker, session->session, request->socket,
            (uint32_t)request->argument, &request->result);
    case UNIX_OP_PENDING:
        return unix_broker_pending(service->broker, session->session, request->socket, &request->result);
    case UNIX_OP_ATTACH: {
        struct unix_attachment attachment;
        struct unix_direction tx, rx;
        const int status = unix_broker_attach(service->broker, session->session,
            request->socket, &attachment, &tx, &rx);
        if (status != 0) return status;
        request->result = attachment.generation;
        request->argument = attachment.peer;
        request->transaction = (uint64_t)attachment.type | ((uint64_t)attachment.flags << 32);
        request->credentials = attachment.peer_credentials;
        append_cap(response, tx.tx_fd, UNIX_CAP_MEMORY, 0);
        append_cap(response, rx.tx_fd, UNIX_CAP_COMMON | PACHA_FD_RIGHT_MAP_READ, 0);
        return 0;
    }
    case UNIX_OP_RETAIN:
        return unix_broker_retain(service->broker, session->session, request->socket, session->session);
    case UNIX_OP_SHUTDOWN:
        if (request->argument > 2) return -EINVAL;
        return unix_broker_shutdown(service->broker, session->session, request->socket,
            (unsigned)request->argument);
    case UNIX_OP_CLOSE: {
        const int status = unix_broker_close(service->broker, session->session, request->socket);
        if (status == 0) prune_socket_watches(service, session, request->socket);
        return status;
    }
    case UNIX_OP_FLAGS: {
        if (request->transaction > 1 || request->argument > UINT32_MAX) return -EINVAL;
        int status = request->transaction ? unix_broker_set_flags(service->broker, session->session,
            request->socket, (uint32_t)request->argument) : 0;
        return status ? status : unix_broker_get_flags(service->broker, session->session,
            request->socket, &request->result);
    }
    case UNIX_OP_PEERCRED:
        return unix_broker_peercred(service->broker, session->session, request->socket, &request->credentials);
    case UNIX_OP_OPTIONS:
        if (request->argument > UINT32_MAX || request->transaction > UINT32_MAX) return -EINVAL;
        return unix_broker_options(service->broker, session->session, request->socket,
            (uint32_t)request->transaction, (uint32_t)request->argument, &request->result);
    case UNIX_OP_ERROR:
        return unix_broker_error(service->broker, session->session, request->socket, &request->result);
    case UNIX_OP_DGRAM_ROUTE: {
        if (request->address.kind == UNIX_ADDRESS_PATH) {
            const int status = path_address(service, session, request, 0);
            if (status) return status;
        }
        struct unix_direction direction;
        const int status = unix_broker_dgram_route(service->broker, session->session,
            request->socket, &request->address, &request->result,
            &request->argument, &direction);
        if (status != 0) return status;
        if (request->cached_generation != request->argument) {
            const uint64_t common = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_MAP_READ;
            append_cap(response, direction.tx_fd, common | PACHA_FD_RIGHT_MAP_WRITE, 0);
            append_cap(response, direction.rx_fd, common, 0);
            response->capabilities[response->count - 2].transfer_flags = PACHA_IPC_TRANSFER_PRIVATE | PACHA_IPC_TRANSFER_CLOEXEC;
            response->capabilities[response->count - 1].transfer_flags = PACHA_IPC_TRANSFER_PRIVATE | PACHA_IPC_TRANSFER_CLOEXEC;
        }
        return 0;
    }
    case UNIX_OP_DGRAM_COMMIT: {
        if (!owns_thread(service, session, request->io.owner)) return -EPERM;
        struct unix_write write = { .owner = request->io.owner,
            .before = request->io.before, .after = request->io.after,
            .length = request->io.length };
        return unix_broker_dgram_commit(service->broker, session->session,
            request->argument, &write, &request->result);
    }
    case UNIX_OP_DGRAM_HEAD: {
        struct unix_delivery delivery;
        struct unix_direction direction;
        const int status = unix_broker_dgram_head(service->broker, session->session,
            request->socket, &delivery, &direction);
        if (status != 0) return status;
        _Static_assert(sizeof(request->delivery) == sizeof(delivery), "datagram delivery wire");
        memcpy(&request->delivery, &delivery, sizeof(delivery));
        request->result = delivery.id;
        const int options_status = unix_broker_options(service->broker, session->session,
            request->socket, 0, 0, &request->transaction);
        if (options_status) return options_status;
        if (!delivery.id) return 0; /* receive shutdown, empty queue: no VMOs */
        if (request->cached_generation != delivery.generation) {
            const uint64_t common = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_MAP_READ;
            append_cap(response, direction.tx_fd, common, 0);
            append_cap(response, direction.rx_fd, common | PACHA_FD_RIGHT_MAP_WRITE, 0);
            response->capabilities[response->count - 2].transfer_flags = PACHA_IPC_TRANSFER_PRIVATE | PACHA_IPC_TRANSFER_CLOEXEC;
            response->capabilities[response->count - 1].transfer_flags = PACHA_IPC_TRANSFER_PRIVATE | PACHA_IPC_TRANSFER_CLOEXEC;
        }
        return 0;
    }
    case UNIX_OP_DGRAM_CONSUME: {
        if (!owns_thread(service, session, request->io.owner)) return -EPERM;
        if (request->transaction & ~UINT64_C(2)) return -EINVAL;
        struct unix_read read = { .owner = request->io.owner,
            .before = request->io.before, .after = request->io.after,
            .length = request->io.length, .message_length = request->io.message_length };
        return unix_broker_dgram_consume(service->broker, session->session,
            request->socket, request->argument, &read, request->transaction != 0);
    }
    case UNIX_OP_DIAG:
        return unix_broker_diagnose(service->broker, session->session,
            request->socket, &request->diagnostic);
    default:
        return -EOPNOTSUPP;
    }
}

static int receive_one(struct unix_service *service, struct service_session *session,
    struct service_handoff *handoff)
{
    struct pacha_ipc_fd fds[PACHA_IPC_MAX_TRANSFER_FDS] = {{0}};
    struct pacha_ipc_msg message = { .fds = fds, .fd_capacity = PACHA_IPC_MAX_TRANSFER_FDS };
    SERVER_START;
    const int received = pacha_ipc_recv(handoff ? handoff->fd : session ? session->control_fd : service->admin, &message);
    if (received != 0) return received;
    SERVER_RECORD(0);
    struct response response = {0};
    struct unix_control *page = NULL;
    int status = -EPROTO;
    uint64_t result = 0;
    int reply_fd = -1;
    uint64_t buffer_token = 0;
    int buffer_miss = 0;
    if (message.fd_count >= 1 && message.fd_count <= PACHA_IPC_MAX_TRANSFER_FDS) {
        const unsigned index = (unsigned)message.fd_count - 1u;
        struct pacha_fd_info info;
        if (pacha_fd_get_info((int)fds[index].fd, &info) == 0 && info.kind == PACHA_FD_KIND_REPLY)
            reply_fd = (int)fds[index].fd;
    }
    const unsigned page_caps = message.word2 < 2;
    if (reply_fd >= 16 && message.word0 == UNIX_SERVICE_MAGIC && message.fd_count >= page_caps + 1u) {
        struct pacha_fd_info info;
        if (!page_caps) {
            page = find_buffer(service, session, message.word2);
            buffer_miss = page == NULL;
            if (page) buffer_token = message.word2;
        } else if (pacha_fd_get_info((int)fds[0].fd, &info) == 0 &&
            info.kind == PACHA_FD_KIND_VMO && info.size >= UNIX_CONTROL_BYTES)
            page = pacha_mmap((int)fds[0].fd, UNIX_CONTROL_BYTES,
                PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
        if (page) {
            /* Never dispatch from a page the caller can change under us. */
            struct unix_control request = *page;
            SERVER_RECORD(1);
            if (request.magic == UNIX_SERVICE_MAGIC && request.version == UNIX_SERVICE_VERSION &&
                request.request == message.word3 && request.operation == message.word1) {
                if (message.word2 == 1) buffer_token = retain_buffer(service, session, page);
                request.result = 0;
                status = handoff ? authorize_handoff(service, handoff, &request,
                    (unsigned)message.fd_count - page_caps - 1u) : dispatch(service, session, &request, fds + page_caps,
                    (unsigned)message.fd_count - page_caps - 1u, &response);
                request.status = status;
                request.magic = UNIX_REPLY_MAGIC;
                request.buffer_token = buffer_token;
                *page = request;
                result = request.result;
            }
            SERVER_RECORD(2);
        }
    }
    if (page && !buffer_token) (void)pacha_munmap(page, UNIX_CONTROL_BYTES);
    SERVER_RECORD(3);
    if (reply_fd >= 16) {
        const struct pacha_ipc_msg reply = {
            .word0 = buffer_miss ? UNIX_BUFFER_MISS_MAGIC : UNIX_REPLY_MAGIC,
            .word1 = buffer_miss ? UNIX_SERVICE_VERSION : (uint64_t)(int64_t)status,
            .word2 = buffer_miss ? message.word2 : result, .word3 = message.word3,
            .fds = response.capabilities, .fd_count = response.count,
        };
        (void)pacha_ipc_reply(reply_fd, &reply);
    }
    SERVER_RECORD(4);
    for (unsigned i = 0; i < response.count; i++)
        if (response.capabilities[i].transfer_flags & PACHA_IPC_TRANSFER_MOVE)
            (void)pacha_fd_close((int)response.capabilities[i].fd);
    for (uint64_t i = 0; i < message.fd_count && i < PACHA_IPC_MAX_TRANSFER_FDS; i++)
        if (fds[i].fd >= 16) (void)pacha_fd_close((int)fds[i].fd);
    SERVER_RECORD(5);
    return 0;
}

static void reap(struct unix_service *service, const struct pacha_service_wait_set *set)
{
    struct service_handoff **handoff_cursor = &service->handoffs;
    while (*handoff_cursor) {
        struct service_handoff *handoff = *handoff_cursor;
        if (!(pacha_service_wait_revents(set, handoff->fd) & PACHA_FD_EVENT_HANGUP)) {
            handoff_cursor = &handoff->next; continue;
        }
        *handoff_cursor = handoff->next;
        (void)unix_broker_close(service->broker, service->handoff_roots, handoff->socket);
        (void)pacha_fd_close(handoff->fd);
        free(handoff);
    }
    struct service_thread **thread_cursor = &service->threads;
    while (*thread_cursor) {
        struct service_thread *thread = *thread_cursor;
        if (!(pacha_service_wait_revents(set, thread->thread_fd) & PACHA_FD_EVENT_READABLE)) {
            thread_cursor = &thread->next;
            continue;
        }
        drop_thread_waiters(service, thread->owner);
        unix_broker_thread_died(service->broker, thread->owner);
        *thread_cursor = thread->next;
        (void)pacha_fd_close(thread->thread_fd);
        free(thread);
    }
    /* exec closes the private receive end without necessarily ending the
     * native thread. Drop its registrations even while that thread lives. */
    struct service_waiter *waiter = service->waiters;
    while (waiter) {
        struct service_waiter *next = waiter->next;
        if (pacha_service_wait_revents(set, waiter->notify_fd) & PACHA_FD_EVENT_HANGUP)
            drop_waiter(service, waiter);
        waiter = next;
    }
    struct service_session **cursor = &service->sessions;
    while (*cursor) {
        struct service_session *session = *cursor;
        if (!(pacha_service_wait_revents(set, session->control_fd) & PACHA_FD_EVENT_HANGUP)) {
            cursor = &session->next;
            continue;
        }
        thread_cursor = &service->threads;
        while (*thread_cursor) {
            struct service_thread *thread = *thread_cursor;
            if (thread->session != session) { thread_cursor = &thread->next; continue; }
            drop_thread_waiters(service, thread->owner);
            unix_broker_thread_died(service->broker, thread->owner);
            *thread_cursor = thread->next;
            (void)pacha_fd_close(thread->thread_fd);
            free(thread);
        }
        release_session_buffers(service, session);
        unix_broker_session_destroy(service->broker, session->session);
        *cursor = session->next;
        (void)pacha_fd_close(session->control_fd);
        server_dump(session);
        free(session);
    }
}

int unix_service_run(const struct unix_boot_config *config)
{
    struct unix_service service = { .admin = (int)config->admin_endpoint,
        .filed_path = (int)config->filed_path_channel,
        .next_owner = 1, .next_waiter = 1 };
    unix_escrow_init(&service.escrow);
    const struct unix_broker_platform platform = { .context = &service,
        .direction_create = create_direction, .direction_destroy = destroy_direction,
        .connection_create = create_connection, .connection_destroy = destroy_connection,
        .notify = notify_socket, .external_retain = retain_external,
        .external_release = release_external, .socket_destroy = release_path };
    service.broker = unix_broker_create(&platform);
    if (service.broker == NULL) return 3;
    const struct pacha_ipc_msg ready = { .word0 = UNIX_READY_MAGIC };
    if (pacha_ipc_send((int)config->ready_channel, &ready) != 0) return 4;
    (void)pacha_fd_close((int)config->ready_channel);
    puts("[unixd] ready");
    fflush(stdout);
    for (;;) {
        unsigned busy = 0;
            for (unsigned i = 0; i < 8 && receive_one(&service, NULL, NULL) == 0; i++) busy++;
        for (struct service_session *session = service.sessions; session; session = session->next)
            for (unsigned i = 0; i < 8 && receive_one(&service, session, NULL) == 0; i++) busy++;
        for (struct service_handoff *handoff = service.handoffs; handoff; handoff = handoff->next)
            for (unsigned i = 0; i < 8 && receive_one(&service, NULL, handoff) == 0; i++) busy++;
        struct pacha_service_wait_set set;
        int status = pacha_service_wait_init(&set, service.admin);
        for (struct service_session *session = service.sessions; status == 0 && session; session = session->next)
            status = pacha_service_wait_add(&set, session->control_fd,
                PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP);
        for (struct service_thread *thread = service.threads; status == 0 && thread; thread = thread->next)
            status = pacha_service_wait_add(&set, thread->thread_fd, PACHA_FD_EVENT_READABLE);
        for (struct service_handoff *handoff = service.handoffs; status == 0 && handoff; handoff = handoff->next)
            status = pacha_service_wait_add(&set, handoff->fd, PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP);
        for (struct service_waiter *waiter = service.waiters; status == 0 && waiter; waiter = waiter->next)
            status = pacha_service_wait_add(&set, waiter->notify_fd, PACHA_FD_EVENT_HANGUP);
        if (status != 0) {
            fprintf(stderr, "[unixd] wait capacity exhausted count=%llu\n", (unsigned long long)set.count);
            return 5;
        }
        (void)pacha_service_wait(&set, busy ? 0 : PACHA_FD_WAIT_FOREVER);
        reap(&service, &set);
    }
}
