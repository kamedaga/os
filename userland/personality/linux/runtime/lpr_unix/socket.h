#pragma once
#include "mapping.h"
#include "rights.h"
#include "../lpr_fd/table.h"

struct lpr_unix_socket {
    uint64_t process_token, socket, wait_identity;
    uint32_t type, flags;
    int32_t error;
    uint32_t mapped;
    uint64_t send_timeout_ns, receive_timeout_ns;
    uint32_t listening;
    int32_t handoff_fd;
    uint32_t adopt_lock;
    struct lpr_unix_mapping mapping;
};

int lpr_unix_socket_active(uint64_t fd);
int lpr_unix_socket_adopt(struct lpr_unix_socket *socket);
int lpr_unix_socket_map(struct lpr_unix_socket *socket);
int lpr_unix_socket_handoff(struct lpr_unix_socket *socket, struct lpr_unix_socket *record);
int64_t lpr_unix_socket_create(uint64_t type, uint64_t protocol);
int64_t lpr_unix_socket_pair(uint64_t type, uint64_t protocol, uint64_t output);
int64_t lpr_unix_socket_close(void *state);
int64_t lpr_unix_socket_io(const lpr_fd_pin_t *pin, uint64_t buffer, uint64_t length,
    int writing, uint64_t flags);
int64_t lpr_unix_socket_iov(const lpr_fd_pin_t *pin, uint64_t vectors, uint64_t count,
    int writing, uint64_t flags, uint32_t *message_flags);
int64_t lpr_unix_socket_message_iov(const lpr_fd_pin_t *pin, uint64_t vectors, uint64_t count,
    int writing, uint64_t flags, uint32_t *message_flags, struct lpr_unix_ancillary *ancillary);
int64_t lpr_unix_socket_message(uint64_t fd, uint64_t message, uint64_t flags, int writing);
int lpr_unix_address_encode(uint64_t raw, uint64_t length, struct unix_address *out);
int lpr_unix_address_copy(const struct unix_address *name, uint64_t address, uint64_t length);
struct lpr_linux_iovec;
void lpr_unix_copy_iov(const struct lpr_linux_iovec *vectors, unsigned count,
    const struct unix_const_span spans[2], int writing);
int64_t lpr_unix_dgram_io(const lpr_fd_pin_t *pin, const struct lpr_linux_iovec *vectors,
    unsigned count, uint64_t length, int writing, uint64_t flags,
    uint32_t *message_flags, struct lpr_unix_ancillary *ancillary);
int64_t lpr_unix_socket_address_io(uint64_t fd, uint64_t buffer, uint64_t length,
    int writing, uint64_t flags, uint64_t address, uint64_t address_length);
int64_t lpr_unix_socket_option(uint64_t fd, uint64_t level, uint64_t option,
    uint64_t value, uint64_t length, int setting);
int64_t lpr_unix_socket_ioctl(const lpr_fd_pin_t *pin, uint64_t request, uint64_t value);
int lpr_unix_socket_flags(const lpr_fd_pin_t *pin, uint32_t *flags, int setting);
int64_t lpr_unix_socket_sendrecv(uint64_t fd, uint64_t buffer, uint64_t length,
    int writing, uint64_t flags);
int64_t lpr_unix_socket_stat(const lpr_fd_pin_t *pin, uint64_t output);
int64_t lpr_unix_socket_shutdown(uint64_t fd, uint64_t how);
int64_t lpr_unix_socket_address(uint64_t fd, uint64_t operation, uint64_t address, uint64_t length);
int64_t lpr_unix_socket_listen(uint64_t fd, uint64_t backlog);
int64_t lpr_unix_socket_accept(uint64_t fd, uint64_t address, uint64_t length, uint64_t flags);
int64_t lpr_unix_socket_name(uint64_t fd, uint64_t address, uint64_t length, int peer);
