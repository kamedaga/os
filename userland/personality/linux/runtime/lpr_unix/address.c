#include "socket.h"
#include <unixd/profile.h>
#include "diagnostic.h"
#include "../lpr_filed_internal.h"
#include <errno.h>

enum { UX_NONBLOCK = 0x800, UX_CLOEXEC = 0x80000 };
struct linux_unix_address { uint16_t family; unsigned char path[UNIX_PATH_BYTES]; };

static int pin_socket(uint64_t fd, lpr_fd_pin_t *pin, struct lpr_unix_context **context)
{
    lpr_fd_arrays_init();
    if (fd > LPR_LINUX_FD_MAX || lpr_fd_table_pin(&lpr_control_fd_table, (uint32_t)fd, pin) != 0) return -EBADF;
    if (pin->ops_id != LPR_FD_OPS_UNIX) { lpr_fd_unpin(pin); return -ENOTSOCK; }
    int status = lpr_unix_socket_adopt(pin->state);
    if (!status) status = lpr_unix_context_current(context);
    if (status != 0) lpr_fd_unpin(pin);
    return status;
}

int lpr_unix_address_encode(uint64_t raw, uint64_t length, struct unix_address *out)
{
    if (length <= sizeof(uint16_t) || length > sizeof(struct linux_unix_address)) return -EINVAL;
    if (!lpr_user_range_plausible(raw, length)) return -EFAULT;
    struct linux_unix_address local = {0};
    lpr_memcpy(&local, (const void *)(uintptr_t)raw, length);
    if (local.family != 1) return -EAFNOSUPPORT;
    const uint32_t bytes = (uint32_t)length - sizeof(uint16_t);
    *out = (struct unix_address){0};
    if (!local.path[0]) {
        out->kind = UNIX_ADDRESS_ABSTRACT;
        out->length = bytes - 1;
        lpr_memcpy(out->bytes, local.path + 1, out->length);
    } else {
        out->kind = UNIX_ADDRESS_PATH;
        out->length = (uint32_t)lpr_strnlen((const char *)local.path, bytes);
        lpr_memcpy(out->bytes, local.path, out->length);
    }
    return 0;
}

static int name_output_valid(uint64_t address, uint64_t length)
{
    if (!address || !lpr_user_range_plausible(length, sizeof(uint32_t))) return -EFAULT;
    return lpr_user_range_plausible(address, *(uint32_t *)(uintptr_t)length) ? 0 : -EFAULT;
}

int lpr_unix_address_copy(const struct unix_address *name, uint64_t address, uint64_t length)
{
    int status = name_output_valid(address, length);
    if (status != 0) return status;
    if (name->kind > UNIX_ADDRESS_ABSTRACT || name->length > UNIX_PATH_BYTES ||
        (name->kind == UNIX_ADDRESS_ABSTRACT && name->length >= UNIX_PATH_BYTES)) return -EPROTO;
    struct linux_unix_address local = { .family = 1 };
    uint32_t bytes = sizeof(local.family);
    if (name->kind == UNIX_ADDRESS_ABSTRACT) {
        lpr_memcpy(local.path + 1, name->bytes, name->length);
        bytes += 1 + name->length;
    } else if (name->kind == UNIX_ADDRESS_PATH) {
        lpr_memcpy(local.path, name->bytes, name->length);
        bytes += name->length + (name->length < UNIX_PATH_BYTES);
    }
    uint32_t *capacity = (void *)(uintptr_t)length;
    uint32_t copied = *capacity < bytes ? *capacity : bytes;
    if (copied) lpr_memcpy((void *)(uintptr_t)address, &local, copied);
    *capacity = bytes;
    return 0;
}

/* The broker owns this state, so register before the retry and keep the
 * queue intact between the last RPC and WAIT_MANY. Shared transport slots
 * are needed for direct I/O, not for serialized listen/connect operations. */
static int64_t call_wait(struct lpr_unix_context *context, const lpr_fd_pin_t *pin,
    struct unix_control *request)
{
    struct unix_control original;
    lpr_memcpy(&original, request, sizeof(original));
    lpr_wait_deadline_t deadline;
    struct lpr_unix_socket *socket = pin->state;
    const uint64_t timeout = __atomic_load_n(original.operation == UNIX_OP_ACCEPT ?
        &socket->receive_timeout_ns : &socket->send_timeout_ns, __ATOMIC_ACQUIRE);
    int64_t status = timeout ? lpr_wait_deadline_init_ns(&deadline, timeout) : lpr_wait_deadline_init(&deadline, -1);
    if (status != 0) return status;
    int watched = 0;
    for (;;) {
        if (watched) {
            status = lpr_unix_waiter_drain(&context->waiter);
            if (status != 0 && status != -EAGAIN) break;
        }
        lpr_memcpy(request, &original, sizeof(*request));
        unsigned count = 0;
        status = lpr_unix_context_call(context, request, NULL, 0, NULL, 0, &count);
        if (status != -EAGAIN) break;
        uint32_t socket_flags;
        int flag_status = lpr_unix_socket_flags(pin, &socket_flags, 0);
        if (flag_status) { status = flag_status; break; }
        if (socket_flags & UX_NONBLOCK) break;
        int expired = 0;
        status = lpr_wait_deadline_expired(&deadline, &expired);
        if (status != 0 || expired) { if (expired) status = -EAGAIN; break; }
        if (!watched) {
            status = lpr_unix_waiter_watch(&context->waiter, original.socket, &socket->wait_identity);
            if (status != 0) break;
            watched = 1;
            continue;
        }
        lpr_wait_graph_t graph;
        lpr_wait_graph_init(&graph);
        status = lpr_unix_waiter_add_graph(&context->waiter, &graph);
        if (status == 0) status = lpr_wait_graph_block(&graph, &deadline);
        if (status != 0) break;
    }
    if (watched) (void)lpr_unix_waiter_unwatch(&context->waiter, original.socket);
    return status;
}

static int attach(struct lpr_unix_context *context, struct lpr_unix_socket *socket)
{
    (void)context;
    if (socket->type == UNIX_TRANSPORT_DGRAM) return 0;
    return lpr_unix_socket_map(socket);
}

int64_t lpr_unix_socket_address(uint64_t fd, uint64_t operation, uint64_t address, uint64_t length)
{
    UP_BEGIN(total, UP_CONTROL, operation, UP_TOTAL);
    if (operation != UNIX_OP_BIND && operation != UNIX_OP_CONNECT) return -EINVAL;
    struct unix_control request = { .operation = (uint32_t)operation };
    int disconnect = 0;
    if (operation == UNIX_OP_CONNECT && length >= sizeof(uint16_t) &&
        length <= sizeof(struct linux_unix_address)) {
        if (!lpr_user_range_plausible(address, length)) return -EFAULT;
        uint16_t family;
        lpr_memcpy(&family, (const void *)(uintptr_t)address, sizeof(family));
        disconnect = family == 0; /* AF_UNSPEC, only connect may accept it. */
    }
    int64_t status = disconnect ? 0 : lpr_unix_address_encode(address, length, &request.address);
    if (status != 0) return status;
    lpr_fd_pin_t pin;
    struct lpr_unix_context *context;
    status = pin_socket(fd, &pin, &context);
    if (status != 0) return status;
    struct lpr_unix_socket *socket = pin.state;
    request.socket = socket->socket;
    if (request.address.kind == UNIX_ADDRESS_PATH) {
        lpr_cwd_init();
        request.argument = request.address.bytes[0] == '/' ? 0 : lpr_cwd_handle;
        request.transaction = 0777u & ~lpr_linux_umask_value;
    }
    status = call_wait(context, &pin, &request);
    lpr_unix_diag(operation, socket->socket, status, fd, request.address.kind);
    if (status == 0 && operation == UNIX_OP_CONNECT) status = attach(context, socket);
    lpr_fd_unpin(&pin);
    return status;
}

int64_t lpr_unix_socket_listen(uint64_t fd, uint64_t backlog)
{
    UP_BEGIN(total, UP_CONTROL, UNIX_OP_LISTEN, UP_TOTAL);
    lpr_fd_pin_t pin;
    struct lpr_unix_context *context;
    int status = pin_socket(fd, &pin, &context);
    if (status != 0) return status;
    struct unix_control request = { .operation = UNIX_OP_LISTEN,
        .socket = ((struct lpr_unix_socket *)pin.state)->socket,
        .argument = (int32_t)backlog < 0 ? UINT32_MAX : (uint32_t)backlog };
    unsigned count;
    status = lpr_unix_context_call(context, &request, NULL, 0, NULL, 0, &count);
    if (status == 0) __atomic_store_n(&((struct lpr_unix_socket *)pin.state)->listening, 1, __ATOMIC_RELEASE);
    lpr_fd_unpin(&pin);
    return status;
}

int64_t lpr_unix_socket_name(uint64_t fd, uint64_t address, uint64_t length, int peer)
{
    int status = name_output_valid(address, length);
    if (status != 0) return status;
    lpr_fd_pin_t pin;
    struct lpr_unix_context *context;
    status = pin_socket(fd, &pin, &context);
    if (status != 0) return status;
    struct unix_control request = { .operation = UNIX_OP_NAME,
        .socket = ((struct lpr_unix_socket *)pin.state)->socket, .argument = peer != 0 };
    unsigned count;
    status = lpr_unix_context_call(context, &request, NULL, 0, NULL, 0, &count);
    if (status == 0) status = lpr_unix_address_copy(&request.address, address, length);
    lpr_fd_unpin(&pin);
    return status;
}

int64_t lpr_unix_socket_accept(uint64_t fd, uint64_t address, uint64_t length, uint64_t flags)
{
    UP_BEGIN(total, UP_CONTROL, UNIX_OP_ACCEPT, UP_TOTAL);
    if (flags & ~(uint64_t)(UX_NONBLOCK | UX_CLOEXEC)) return -EINVAL;
    if (address) { int status = name_output_valid(address, length); if (status != 0) return status; }
    lpr_fd_pin_t pin;
    struct lpr_unix_context *context;
    int64_t status = pin_socket(fd, &pin, &context);
    if (status != 0) return status;
    struct lpr_unix_socket *socket = lpr_backend_state_alloc(sizeof(*socket));
    if (!socket) { lpr_fd_unpin(&pin); return -ENOMEM; }
    *socket = (struct lpr_unix_socket){ .process_token = lpr_supervisor_token,
        .type = ((struct lpr_unix_socket *)pin.state)->type,
        .flags = LPR_LINUX_O_RDWR | (uint32_t)(flags & UX_NONBLOCK),
        .mapping = { .fds = {-1, -1} } };
    const struct lpr_unix_socket *listener = pin.state;
    socket->send_timeout_ns = __atomic_load_n(&listener->send_timeout_ns, __ATOMIC_ACQUIRE);
    socket->receive_timeout_ns = __atomic_load_n(&listener->receive_timeout_ns, __ATOMIC_ACQUIRE);
    struct unix_control request = { .operation = UNIX_OP_ACCEPT,
        .socket = ((struct lpr_unix_socket *)pin.state)->socket };
    status = call_wait(context, &pin, &request);
    lpr_fd_unpin(&pin);
    if (status != 0) goto fail;
    socket->socket = request.result;
    if (!socket->socket) { status = -EPROTO; goto fail; }
    status = attach(context, socket);
    if (status != 0) goto fail;
    {
        const lpr_fd_pin_t accepted_pin = { .state = socket };
        uint32_t socket_flags = socket->flags;
        status = lpr_unix_socket_flags(&accepted_pin, &socket_flags, 1);
        if (status) goto fail;
    }
    if (address) {
        request = (struct unix_control){ .operation = UNIX_OP_NAME, .socket = socket->socket, .argument = 1 };
        unsigned count;
        status = lpr_unix_context_call(context, &request, NULL, 0, NULL, 0, &count);
        if (status == 0) status = lpr_unix_address_copy(&request.address, address, length);
        if (status != 0) goto fail;
    }
    lpr_fd_install_t install = { .ops_id = LPR_FD_OPS_UNIX,
        .fd_flags = flags & UX_CLOEXEC ? LPR_FD_ENTRY_CLOEXEC : 0,
        .access_mode = LPR_LINUX_O_RDWR, .status_flags = flags & UX_NONBLOCK ? LPR_OFD_NONBLOCK : 0,
        .rights = LPR_FD_RIGHT_READ | LPR_FD_RIGHT_WRITE | LPR_FD_RIGHT_DUP | LPR_FD_RIGHT_STAT | LPR_FD_RIGHT_IOCTL,
        .backend_state = socket, .backend_state_bytes = sizeof(*socket) };
    lpr_linux_fd_t result;
    while (lpr_fd_table_alloc(&lpr_control_fd_table, 0, &install, &result) != 0) {
        if (lpr_fd_table_capacity >= LPR_FD_TABLE_MAX_SIZE ||
            lpr_fd_table_ensure_capacity(lpr_fd_table_capacity + 1) != 0) { status = -EMFILE; goto fail; }
    }
    return result;
fail:
    (void)lpr_unix_socket_close(socket);
    (void)lpr_backend_state_free(socket, sizeof(*socket));
    return status;
}
