/* SPDX-License-Identifier: MIT */
/* Netlink datagrams use the existing netd transport; never split one message
 * into separate sends, or consume another datagram for a second iovec. */
static int64_t lpr_netlink_iov_size(const lpr_linux_iovec_t *iov,
    uint64_t count, uint64_t *size)
{
    if (count > LPR_LINUX_UIO_MAXIOV) return -LPR_LINUX_EMSGSIZE;
    if (!iov && count) return -LPR_LINUX_EFAULT;
    *size = 0;
    for (uint64_t i = 0; i < count; ++i) {
        if (!iov[i].base && iov[i].len) return -LPR_LINUX_EFAULT;
        if (iov[i].len > UINT64_MAX - *size) return -LPR_LINUX_EINVAL;
        *size += iov[i].len;
    }
    return 0;
}

static void lpr_netlink_name(uint64_t address, uint64_t length_address,
    uint32_t pid, uint32_t groups)
{
    if (!address || !length_address) return;
    uint32_t *length = (void *)(uintptr_t)length_address;
    lpr_linux_sockaddr_nl_t name;
    lpr_memset(&name, 0, sizeof(name));
    name.family = LPR_LINUX_AF_NETLINK;
    name.pid = pid;
    name.groups = groups;
    uint32_t copy = *length < sizeof(name) ? *length : sizeof(name);
    if (copy) lpr_memcpy((void *)(uintptr_t)address, &name, copy);
    *length = sizeof(name);
}

static int64_t lpr_netlink_send(uint64_t fd, const lpr_linux_iovec_t *iov,
    uint64_t count, uint64_t flags, uint64_t destination, uint64_t address_size,
    unsigned *wait_not_ready)
{
    uint64_t size;
    int64_t error = lpr_netlink_iov_size(iov, count, &size);
    if (error) return error;
    if (size > NETD_IO_BYTES) return -LPR_LINUX_EMSGSIZE;
    if (destination) {
        if (address_size < sizeof(lpr_linux_sockaddr_nl_t)) return -LPR_LINUX_EINVAL;
        const lpr_linux_sockaddr_nl_t *address = (void *)(uintptr_t)destination;
        if (address->family != LPR_LINUX_AF_NETLINK) return -LPR_LINUX_EAFNOSUPPORT;
        if (address->pid || address->groups) return -LPR_LINUX_EOPNOTSUPP;
    }
    /* Linux binds a route socket implicitly on its first send. Keep the
     * assigned port available to getsockname, just as for explicit bind. */
    if (!lpr_socket_backend(fd)->local_addr_be &&
        lpr_socket_backend(fd)->protocol == 0) {
        void *bind_page = 0;
        pacha_fd_t bind_fd = lpr_netd_create_page(&bind_page);
        if (bind_fd < 0) return bind_fd;
        lpr_memset(bind_page, 0, sizeof(netd_netlink_bind_t));
        netd_netlink_bind_t *binding = bind_page;
        binding->handle = lpr_socket_backend(fd)->handle;
        error = lpr_netd_call(NETD_OP_BIND, bind_fd, 0, 0);
        if (!error) lpr_socket_backend(fd)->local_addr_be = binding->pid;
        lpr_netd_destroy_page(bind_fd, bind_page);
        if (error) return error;
    }
    void *page = 0;
    pacha_fd_t page_fd = lpr_netd_create_page(&page);
    if (page_fd < 0) return page_fd;
    lpr_memset(page, 0, offsetof(netd_io_t, data));
    netd_io_t *request = page;
    request->handle = lpr_socket_backend(fd)->handle;
    request->length = size;
    request->flags = flags;
    uint64_t cursor = 0;
    for (uint64_t i = 0; i < count; ++i) {
        if (iov[i].len) lpr_memcpy(request->data + cursor,
            (void *)(uintptr_t)iov[i].base, iov[i].len);
        cursor += iov[i].len;
    }
    uint64_t sent = 0;
    lpr_wait_deadline_t deadline;
    error = lpr_wait_deadline_init(&deadline,
        lpr_socket_backend(fd)->sndtimeo_ms ? lpr_socket_backend(fd)->sndtimeo_ms : -1);
    if (error) {
        lpr_netd_destroy_page(page_fd, page);
        return error;
    }
    for (;;) {
        error = lpr_netd_call(NETD_OP_SEND, page_fd, 0, &sent);
        if (error != -LPR_LINUX_EAGAIN || lpr_socket_op_nonblocking(fd, flags)) break;
        error = lpr_linux_socket_wait_events_until(fd, LPR_LINUX_POLLOUT,
            &deadline, wait_not_ready);
        if (error) break;
    }
    lpr_netd_destroy_page(page_fd, page);
    if (error) return error;
    return sent == size ? (int64_t)sent : -LPR_LINUX_EIO;
}

static int64_t lpr_netlink_receive(uint64_t fd, const lpr_linux_iovec_t *iov,
    uint64_t count, uint64_t flags, uint64_t address, uint64_t address_size,
    uint32_t *output_flags, unsigned *wait_not_ready)
{
    uint64_t size;
    int64_t error = lpr_netlink_iov_size(iov, count, &size);
    if (error) return error;
    uint64_t capacity = size < NETD_IO_BYTES ? size : NETD_IO_BYTES;
    lpr_wait_deadline_t deadline;
    error = lpr_wait_deadline_init(&deadline,
        lpr_socket_backend(fd)->rcvtimeo_ms ? lpr_socket_backend(fd)->rcvtimeo_ms : -1);
    if (error) return error;
    void *page = 0;
    pacha_fd_t page_fd = lpr_netd_create_page(&page);
    if (page_fd < 0) return page_fd;
    netd_io_t *request = page;
    uint64_t received = 0;
    for (;;) {
        lpr_memset(page, 0, offsetof(netd_io_t, data));
        request->handle = lpr_socket_backend(fd)->handle;
        request->length = capacity;
        request->flags = flags;
        error = lpr_netd_call(NETD_OP_RECV, page_fd, 0, &received);
        if (error != -LPR_LINUX_EAGAIN || lpr_socket_op_nonblocking(fd, flags)) break;
        error = lpr_linux_socket_wait_events_until(fd, LPR_LINUX_POLLIN,
            &deadline, wait_not_ready);
        if (error) break;
    }
    if (!error) {
        const uint64_t copied = received < capacity ? received : capacity;
        uint64_t cursor = 0;
        for (uint64_t i = 0; i < count && cursor < copied; ++i) {
            uint64_t amount = iov[i].len < copied - cursor ? iov[i].len : copied - cursor;
            if (amount) lpr_memcpy((void *)(uintptr_t)iov[i].base,
                request->data + cursor, amount);
            cursor += amount;
        }
        if (output_flags) *output_flags = (uint32_t)request->flags & LPR_LINUX_MSG_TRUNC;
        lpr_netlink_name(address, address_size, 0,
            lpr_socket_backend(fd)->protocol == LPR_LINUX_NETLINK_KOBJECT_UEVENT ?
                lpr_socket_backend(fd)->peer_addr_be : 0);
        if (flags & LPR_LINUX_MSG_PEEK) lpr_linux_socket_mark_readable(fd);
    }
    lpr_netd_destroy_page(page_fd, page);
    return error ? error : (int64_t)received;
}
