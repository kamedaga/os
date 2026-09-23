#include "socket.h"
#include "diagnostic.h"
#include "../lpr_filed_internal.h"
#include <errno.h>

struct unix_linux_message {
    uint64_t name;
    uint32_t name_length, pad0;
    uint64_t vectors, vector_count, control, control_length;
    uint32_t flags, pad1;
};
_Static_assert(sizeof(struct unix_linux_message) == 56, "Linux x86-64 msghdr");

int64_t lpr_unix_socket_message(uint64_t fd, uint64_t raw, uint64_t flags, int writing)
{
    if (!lpr_user_range_plausible(raw, sizeof(struct unix_linux_message))) return -EFAULT;
    struct unix_linux_message message;
    lpr_memcpy(&message, (const void *)(uintptr_t)raw, sizeof(message));
    if (message.control_length && !lpr_user_range_plausible(message.control, message.control_length)) return -EFAULT;
    struct lpr_unix_ancillary ancillary = { .control = message.control, .capacity = message.control_length };
    if (writing && message.control_length) {
        int status = lpr_unix_rights_parse(&ancillary);
        if (status) return status;
    }
    lpr_fd_pin_t pin;
    if (fd > LPR_LINUX_FD_MAX || lpr_fd_table_pin(&lpr_control_fd_table, (uint32_t)fd, &pin) != 0) return -EBADF;
    int64_t result = -ENOTSOCK;
    if (pin.ops_id != LPR_FD_OPS_UNIX) goto done;
    const int datagram = ((struct lpr_unix_socket *)pin.state)->type == UNIX_TRANSPORT_DGRAM;
    if (message.name && message.name_length &&
        !lpr_user_range_plausible(message.name, message.name_length)) { result = -EFAULT; goto done; }
    if (writing && datagram && message.name) {
        result = lpr_unix_address_encode(message.name, message.name_length, &ancillary.address);
        if (result) goto done;
    }
    uint32_t returned_flags = 0;
    result = lpr_unix_socket_message_iov(&pin, message.vectors, message.vector_count,
        writing, flags, &returned_flags, &ancillary);
    /* Include credential truncation and identify the Linux FD, even when
     * recvmsg succeeds. No logging/counters on the normal receive path. */
    if (!writing && result >= 0 && (returned_flags & 8u))
        lpr_unix_diag('E', fd, 110, ancillary.capacity, ancillary.used);
    if (!writing && result >= 0) {
        struct unix_linux_message *out = (void *)(uintptr_t)raw;
        if (datagram && message.name && ancillary.address.kind != UNIX_ADDRESS_UNNAMED) {
            const int status = lpr_unix_address_copy(&ancillary.address, message.name,
                (uint64_t)(uintptr_t)&out->name_length);
            if (status) result = status;
        } else out->name_length = 0; /* STREAM does not supply a source address. */
        out->control_length = ancillary.used;
        out->flags = returned_flags;
    }
done:
    lpr_fd_unpin(&pin);
    return result;
}

int64_t lpr_unix_socket_address_io(uint64_t fd, uint64_t buffer, uint64_t length,
    int writing, uint64_t flags, uint64_t address, uint64_t address_length)
{
    uint32_t capacity = 0;
    if (address && !writing) {
        if (!lpr_user_range_plausible(address_length, sizeof(uint32_t))) return -EFAULT;
        lpr_memcpy(&capacity, (const void *)(uintptr_t)address_length, sizeof(capacity));
    } else if (address) {
        if (address_length > UINT32_MAX) return -EINVAL;
        capacity = (uint32_t)address_length;
    }
    const lpr_linux_iovec_t vector = { .base = buffer, .len = length };
    struct unix_linux_message message = { .name = address, .name_length = capacity,
        .vectors = (uint64_t)(uintptr_t)&vector, .vector_count = 1 };
    int64_t result = lpr_unix_socket_message(fd, (uint64_t)(uintptr_t)&message, flags, writing);
    if (result >= 0 && !writing && address)
        lpr_memcpy((void *)(uintptr_t)address_length, &message.name_length, sizeof(capacity));
    return result;
}
