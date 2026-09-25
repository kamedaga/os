#include "socket.h"
#include "../lpr_filed_internal.h"
#include <errno.h>

struct unix_timeval { int64_t seconds, microseconds; };

int lpr_unix_socket_flags(const lpr_fd_pin_t *pin, uint32_t *flags, int setting)
{
    struct lpr_unix_socket *socket = pin->state;
    int adoption = lpr_unix_socket_adopt(socket);
    if (adoption) return adoption;
    if (!setting && __atomic_load_n(&socket->mapped, __ATOMIC_ACQUIRE)) {
        *flags = LPR_LINUX_O_RDWR | (__atomic_load_n(&socket->mapping.outgoing_tx->flags, __ATOMIC_ACQUIRE) & 0x800);
        return 0;
    }
    struct lpr_unix_context *context;
    int status = lpr_unix_context_current(&context);
    if (status) return status;
    struct unix_control request = { .operation = UNIX_OP_FLAGS, .socket = socket->socket,
        .transaction = setting != 0, .argument = setting ? *flags & 0x800u : 0 };
    unsigned count;
    status = lpr_unix_context_call(context, &request, NULL, 0, NULL, 0, &count);
    if (!status) {
        if (request.result & ~UINT64_C(0x800)) return -EPROTO;
        *flags = LPR_LINUX_O_RDWR | (uint32_t)request.result;
        __atomic_store_n(&socket->flags, *flags, __ATOMIC_RELEASE);
    }
    return status;
}

static int copy_option(uint64_t output, uint64_t length, const void *value, uint32_t bytes)
{
    if (!lpr_user_range_plausible(length, sizeof(uint32_t))) return -EFAULT;
    uint32_t *capacity = (void *)(uintptr_t)length;
    if ((int32_t)*capacity < 0) return -EINVAL;
    uint32_t copied = *capacity < bytes ? *capacity : bytes;
    if (copied && !lpr_user_range_plausible(output, copied)) return -EFAULT;
    if (copied) lpr_memcpy((void *)(uintptr_t)output, value, copied);
    *capacity = copied;
    return 0;
}

int64_t lpr_unix_socket_option(uint64_t fd, uint64_t level, uint64_t option,
    uint64_t value, uint64_t length, int setting)
{
    if (level != 1) return -ENOPROTOOPT;
    lpr_fd_pin_t pin;
    if (fd > LPR_LINUX_FD_MAX || lpr_fd_table_pin(&lpr_control_fd_table, (uint32_t)fd, &pin) != 0) return -EBADF;
    int result = -ENOPROTOOPT;
    if (pin.ops_id != LPR_FD_OPS_UNIX) { result = -ENOTSOCK; goto done; }
    struct lpr_unix_socket *socket = pin.state;
    result = lpr_unix_socket_adopt(socket);
    if (result) goto done;
    result = -ENOPROTOOPT;
    if (option == 20 || option == 21) {
        uint64_t *timeout = option == 20 ? &socket->receive_timeout_ns : &socket->send_timeout_ns;
        struct unix_timeval tv;
        if (setting) {
            if (length < sizeof(tv)) { result = -EINVAL; goto done; }
            if (!lpr_user_range_plausible(value, sizeof(tv))) { result = -EFAULT; goto done; }
            lpr_memcpy(&tv, (const void *)(uintptr_t)value, sizeof(tv));
            if (tv.seconds < 0 || tv.microseconds < 0 || tv.microseconds >= 1000000) { result = -EDOM; goto done; }
            uint64_t ns = (uint64_t)tv.seconds > (UINT64_MAX - (uint64_t)tv.microseconds * 1000) / 1000000000 ?
                UINT64_MAX : (uint64_t)tv.seconds * 1000000000 + (uint64_t)tv.microseconds * 1000;
            __atomic_store_n(timeout, ns, __ATOMIC_RELEASE);
            result = 0;
        } else {
            const uint64_t ns = __atomic_load_n(timeout, __ATOMIC_ACQUIRE);
            tv.seconds = (int64_t)(ns / 1000000000); tv.microseconds = (int64_t)(ns % 1000000000 / 1000);
            result = copy_option(value, length, &tv, sizeof(tv));
        }
        goto done;
    }
    if (option == 17 && !setting) {
        struct lpr_unix_context *context;
        result = lpr_unix_context_current(&context);
        if (result) goto done;
        struct unix_control request = { .operation = UNIX_OP_PEERCRED, .socket = socket->socket };
        unsigned count;
        result = lpr_unix_context_call(context, &request, NULL, 0, NULL, 0, &count);
        if (result) goto done;
        struct { int32_t pid; uint32_t uid, gid; } peer = { .pid = request.credentials.pid,
            .uid = request.credentials.euid, .gid = request.credentials.egid };
        result = copy_option(value, length, &peer, sizeof(peer));
        goto done;
    }
    int32_t integer = 0;
    if (option == 2 || option == 9 || option == 16) {
        if (setting) {
            if (length < sizeof(integer)) { result = -EINVAL; goto done; }
            if (!lpr_user_range_plausible(value, sizeof(integer))) { result = -EFAULT; goto done; }
            lpr_memcpy(&integer, (const void *)(uintptr_t)value, sizeof(integer));
        }
        const uint32_t bit = option == 2 ? UNIX_SOCKET_REUSEADDR :
            option == 9 ? UNIX_SOCKET_KEEPALIVE : UNIX_SOCKET_PASSCRED;
        struct lpr_unix_context *context;
        result = lpr_unix_context_current(&context);
        if (result) goto done;
        struct unix_control request = { .operation = UNIX_OP_OPTIONS, .socket = socket->socket,
            .transaction = setting ? bit : 0, .argument = setting && integer ? bit : 0 };
        unsigned count;
        result = lpr_unix_context_call(context, &request, NULL, 0, NULL, 0, &count);
        if (!result && !setting) {
            integer = (request.result & bit) != 0;
            result = copy_option(value, length, &integer, sizeof(integer));
        }
        goto done;
    }
    if (setting) {
        if (length < sizeof(integer)) { result = -EINVAL; goto done; }
        if (!lpr_user_range_plausible(value, sizeof(integer))) { result = -EFAULT; goto done; }
        lpr_memcpy(&integer, (const void *)(uintptr_t)value, sizeof(integer));
        /* Buffer resizing/linger are not acknowledged without the
         * corresponding transport behavior. Unknown options stay errors. */
        goto done;
    }
    switch (option) {
    case 3: integer = (int32_t)socket->type; break;
    case 4: {
        struct lpr_unix_context *context;
        result = lpr_unix_context_current(&context);
        if (result) goto done;
        struct unix_control error = { .operation = UNIX_OP_ERROR, .socket = socket->socket };
        unsigned count;
        result = lpr_unix_context_call(context, &error, NULL, 0, NULL, 0, &count);
        if (result) goto done;
        if (error.result > 4095) { result = -EPROTO; goto done; }
        integer = error.result ? (int32_t)error.result :
            __atomic_exchange_n(&socket->error, 0, __ATOMIC_ACQ_REL);
        break;
    }
    case 7: case 8: integer = UNIX_TRANSPORT_BYTES; break;
    case 30: integer = (int32_t)__atomic_load_n(&socket->listening, __ATOMIC_ACQUIRE); break;
    case 38: integer = 0; break;
    case 39: integer = 1; break;
    default: goto done;
    }
    result = copy_option(value, length, &integer, sizeof(integer));
done:
    lpr_fd_unpin(&pin);
    return result;
}

int64_t lpr_unix_socket_ioctl(const lpr_fd_pin_t *pin, uint64_t request, uint64_t value)
{
    if (!pin || pin->ops_id != LPR_FD_OPS_UNIX) return -ENOTSOCK;
    struct lpr_unix_socket *socket = pin->state;
    int adoption = lpr_unix_socket_adopt(socket);
    if (adoption) return adoption;
    if (request != 0x5421 && request != 0x541b) return -ENOTTY;
    if (!lpr_user_range_plausible(value, sizeof(int32_t))) return -EFAULT;
    if (request == 0x5421) {
        int32_t enabled;
        lpr_memcpy(&enabled, (const void *)(uintptr_t)value, sizeof(enabled));
        uint32_t flags = LPR_LINUX_O_RDWR | (enabled ? LPR_LINUX_O_NONBLOCK : 0);
        return lpr_unix_socket_flags(pin, &flags, 1);
    }
    if (socket->type == UNIX_TRANSPORT_DGRAM) {
        struct lpr_unix_context *context;
        int status = lpr_unix_context_current(&context);
        if (status) return status;
        struct unix_control pending = { .operation = UNIX_OP_PENDING, .socket = socket->socket };
        unsigned count;
        status = lpr_unix_context_call(context, &pending, NULL, 0, NULL, 0, &count);
        if (!status && pending.result > UNIX_TRANSPORT_BYTES) return -EPROTO;
        if (!status) *(int32_t *)(uintptr_t)value = (int32_t)pending.result;
        return status;
    }
    if (!__atomic_load_n(&socket->mapped, __ATOMIC_ACQUIRE)) return -EINVAL;
    uint32_t bytes = 0;
    int status = unix_transport_pending(socket->mapping.incoming_tx, socket->mapping.incoming_rx,
        socket->mapping.generation, &bytes);
    if (status == 0) *(int32_t *)(uintptr_t)value = (int32_t)bytes;
    return status;
}
