#include "../lpr_filed_internal.h"

int64_t lpr_filed_io(uint32_t op, uint64_t fd, uint64_t buf, uint64_t count, uint64_t offset)
{
    if (!lpr_fd_is_filed(fd)) {
        return -LPR_LINUX_EBADF;
    }
    if (buf == 0 && count != 0) {
        return -LPR_LINUX_EFAULT;
    }
    void *page = 0;
    const int page_fd = lpr_create_wire_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }
    filed_io_t *io = (filed_io_t *)page;
    lpr_memset(io, 0, sizeof(*io));
    io->handle = lpr_filed_backend(fd)->handle;
    io->offset = offset;
    io->length = count > FILED_IO_BYTES ? FILED_IO_BYTES : count;
    if (op == FILED_OP_VFS_WRITE && io->length != 0) {
        lpr_memcpy(io->data, (const void *)(uintptr_t)buf, (size_t)io->length);
    }
    uint64_t result = 0;
    const int64_t status = lpr_filed_call(op, page_fd, 0, &result);
    if (status == 0 && result > io->length) {
        result = io->length;
    }
    if (status == 0 && op != FILED_OP_VFS_WRITE && result != 0) {
        lpr_memcpy((void *)(uintptr_t)buf, io->data, (size_t)result);
    }
    lpr_destroy_wire_page(page_fd, page);
    return status == 0 ? (int64_t)result : status;
}

static int64_t lpr_filed_io_chunked(uint32_t op, uint64_t fd, uint64_t buf, uint64_t count, uint64_t offset)
{
    if (buf == 0 && count != 0) {
        return -LPR_LINUX_EFAULT;
    }
    if (count != 0 && buf > UINT64_MAX - (count - 1u)) {
        return -LPR_LINUX_EFAULT;
    }
    if (op == FILED_OP_VFS_PREAD && count != 0 && offset > UINT64_MAX - (count - 1u)) {
        return -LPR_LINUX_EINVAL;
    }
    uint64_t total = 0;
    while (total < count) {
        const uint64_t remaining = count - total;
        const uint64_t chunk = remaining > FILED_IO_BYTES ? FILED_IO_BYTES : remaining;
        const uint64_t chunk_offset = op == FILED_OP_VFS_PREAD ? offset + total : 0;
        const int64_t n = lpr_filed_io(op, fd, buf + total, chunk, chunk_offset);
        if (n < 0) {
            return total != 0 ? (int64_t)total : n;
        }
        if (n == 0) {
            break;
        }
        total += (uint64_t)n;
        if ((uint64_t)n < chunk) {
            break;
        }
    }
    return (int64_t)total;
}

static lpr_filed_page_cache_entry_t *lpr_page_cache_fill(uint64_t fd, uint64_t offset, uint64_t requested)
{
    if (lpr_shared_file_mapping_active ||
        !lpr_fd_shadow_offset_eligible(fd) ||
        requested == 0 ||
        requested > LPR_FILED_PAGE_CACHE_BYTES)
    {
        return 0;
    }
    const uint64_t page_start = offset & ~(LPR_FILED_PAGE_CACHE_BYTES - 1ull);
    if (requested > LPR_FILED_PAGE_CACHE_BYTES - (offset - page_start)) {
        return 0;
    }
    lpr_filed_page_cache_entry_t *entry =
        lpr_page_cache_find_marker(lpr_filed_backend(fd)->handle, page_start);
    if (entry == 0) {
        entry = lpr_page_cache_slot();
    }
    const int64_t n = lpr_filed_io(
        FILED_OP_VFS_PREAD,
        fd,
        (uint64_t)(uintptr_t)entry->data,
        LPR_FILED_PAGE_CACHE_BYTES,
        page_start);
    if (n <= 0) {
        return 0;
    }
    lpr_memset(entry, 0, offsetof(lpr_filed_page_cache_entry_t, data));
    entry->active = 1;
    entry->handle = lpr_filed_backend(fd)->handle;
    entry->page_start = page_start;
    entry->length = (uint64_t)n;
    entry->clock = ++lpr_page_cache_clock;
    if (offset + requested < offset ||
        offset + requested > entry->page_start + entry->length)
    {
        return 0;
    }
    return entry;
}

static lpr_filed_page_cache_entry_t *lpr_page_cache_get(uint64_t fd, uint64_t offset, uint64_t requested, int *out_hit)
{
    lpr_filed_page_cache_entry_t *entry =
        lpr_page_cache_lookup(lpr_filed_backend(fd)->handle, offset, requested);
    if (entry != 0) {
        if (out_hit != 0) {
            *out_hit = 1;
        }
        return entry;
    }
    if (out_hit != 0) {
        *out_hit = 0;
    }
    return lpr_page_cache_fill(fd, offset, requested);
}

int64_t lpr_read_from_page_cache(uint64_t fd, uint64_t buf, uint64_t requested, uint64_t offset)
{
    if (requested == 0) {
        return 0;
    }
    if (buf == 0 || requested > LPR_FILED_PAGE_CACHE_BYTES) {
        return -1;
    }
    const uint64_t page_start = offset & ~(LPR_FILED_PAGE_CACHE_BYTES - 1ull);
    const uint64_t page_offset = offset - page_start;
    if (requested <= LPR_FILED_PAGE_CACHE_BYTES - page_offset) {
        lpr_filed_page_cache_entry_t *entry = lpr_page_cache_get(fd, offset, requested, 0);
        if (entry == 0) {
            return -1;
        }
        lpr_memcpy((void *)(uintptr_t)buf, entry->data + page_offset, (size_t)requested);
        return (int64_t)requested;
    }

    const uint64_t first_len = LPR_FILED_PAGE_CACHE_BYTES - page_offset;
    const uint64_t second_len = requested - first_len;
    lpr_filed_page_cache_entry_t *first = lpr_page_cache_get(fd, offset, first_len, 0);
    lpr_filed_page_cache_entry_t *second =
        first != 0 ? lpr_page_cache_get(fd, offset + first_len, second_len, 0) : 0;
    if (first == 0 || second == 0) {
        return -1;
    }
    unsigned char *dst = (unsigned char *)(uintptr_t)buf;
    lpr_memcpy(dst, first->data + page_offset, (size_t)first_len);
    lpr_memcpy(dst + first_len, second->data, (size_t)second_len);
    return (int64_t)requested;
}

int64_t lpr_backend_read(uint64_t fd, uint64_t buf, uint64_t count)
{
    if (lpr_linux_device_fd_active(fd)) {
        if ((lpr_device_backend(fd)->flags & LPR_LINUX_O_ACCMODE) == LPR_LINUX_O_WRONLY) {
            return -LPR_LINUX_EBADF;
        }
        (void)buf;
        (void)count;
        return 0;
    }
    if (lpr_linux_input_fd_active(fd)) {
        return lpr_input_read_events(fd, buf, count);
    }
    if (lpr_linux_drm_fd_active(fd)) {
        return lpr_drm_read_events(fd, buf, count);
    }
    if (lpr_linux_tty_fd_active(fd)) {
        return lpr_tty_io(TERMD_OP_HANDLE_READ, fd, buf, count);
    }
    if (lpr_linux_timerfd_active(fd)) {
        return lpr_linux_timerfd_read(fd, buf, count);
    }
    if (lpr_linux_eventfd_active(fd)) {
        if (count < sizeof(uint64_t)) {
            return -LPR_LINUX_EINVAL;
        }
        if (buf == 0) {
            return -LPR_LINUX_EFAULT;
        }
        lpr_event_backend_t *event = lpr_event_backend(fd);
        while (event->counter == 0) {
            if ((event->flags & LPR_LINUX_O_NONBLOCK) != 0)
                return -LPR_LINUX_EAGAIN;
            lpr_wait_graph_t graph;
            lpr_wait_deadline_t deadline;
            lpr_wait_graph_init(&graph);
            int64_t wait_status = lpr_wait_graph_add_fd(
                &graph, fd, 0x0001u);
            if (wait_status == 0)
                wait_status = lpr_wait_deadline_init(&deadline, -1);
            if (wait_status == 0)
                wait_status = lpr_wait_graph_block(&graph, &deadline);
            if (wait_status != 0) return wait_status;
        }
        if ((event->reserved1 & LPR_LINUX_EFD_SEMAPHORE) != 0) {
            *(uint64_t *)(uintptr_t)buf = 1;
            event->counter--;
        } else {
            *(uint64_t *)(uintptr_t)buf = event->counter;
            event->counter = 0;
        }
        lpr_event_backend_notify(event);
        return (int64_t)sizeof(uint64_t);
    }
    if (lpr_pipe_fd_is_active(fd)) {
        lpr_pipe_backend_t *pipe = lpr_pipe_backend(fd);
        if (pipe == 0 || !pipe->readable || pipe->native.raw < 0) {
            return -LPR_LINUX_EBADF;
        }
        if (count == 0) {
            return 0;
        }
        if (buf == 0) {
            return -LPR_LINUX_EFAULT;
        }
        for (;;) {
            const int64_t n = lpr_pacha_syscall3(
                PACHAOS_SYSCALL_FD_READ,
                (uint64_t)(uint32_t)pipe->native.raw,
                buf,
                count);
            if (n >= 0) {
                return n;
            }
            const int64_t err = lpr_pacha_status_to_errno(n);
            if (err != -LPR_LINUX_EAGAIN ||
                (pipe->flags & LPR_LINUX_O_NONBLOCK) != 0)
            {
                return err;
            }
            const int64_t wait_status = lpr_pipe_wait(fd, 0x0001u, 0);
            if (wait_status != 0) {
                return wait_status;
            }
        }
    }
    if (lpr_fd_is_filed(fd)) {
        if (count == 0) {
            return 0;
        }
        if (lpr_fd_shadow_offset_eligible(fd) &&
            lpr_filed_backend(fd)->offset_valid &&
            count <= LPR_FILED_PAGE_CACHE_BYTES)
        {
            const uint64_t offset = lpr_filed_control_offset(fd);
            const int64_t cached = lpr_read_from_page_cache(fd, buf, count, offset);
            if (cached >= 0) {
                lpr_filed_control_advance_offset(fd, offset, (uint64_t)cached);
                lpr_filed_backend(fd)->pread_active = 1;
                return cached;
            }
        }
        if (lpr_fd_shadow_offset_eligible(fd) &&
            lpr_filed_backend(fd)->offset_valid &&
            lpr_filed_backend(fd)->pread_active)
        {
            const uint64_t offset = lpr_filed_control_offset(fd);
            const int64_t n = count > FILED_IO_BYTES ?
                lpr_filed_io_chunked(FILED_OP_VFS_PREAD, fd, buf, count, offset) :
                lpr_filed_io(FILED_OP_VFS_PREAD, fd, buf, count, offset);
            if (n > 0) {
                lpr_filed_control_advance_offset(fd, offset, (uint64_t)n);
            }
            return n;
        }
        const int64_t n = count > FILED_IO_BYTES ?
            lpr_filed_io_chunked(FILED_OP_VFS_READ, fd, buf, count, 0) :
            lpr_filed_io(FILED_OP_VFS_READ, fd, buf, count, 0);
        if (n >= 0 && lpr_fd_shadow_offset_eligible(fd)) {
            const uint64_t old_offset = lpr_filed_control_offset(fd);
            lpr_filed_control_advance_offset(fd, old_offset, (uint64_t)n);
        }
        return n;
    }
    return -LPR_LINUX_EBADF;
}

int64_t lpr_backend_readv(uint64_t fd, uint64_t iov_raw, uint64_t iov_count)
{
    lpr_readv_cache_total++;
    if (iov_raw == 0 && iov_count != 0) {
        return -LPR_LINUX_EFAULT;
    }
    if (lpr_linux_device_fd_active(fd)) {
        return (lpr_device_backend(fd)->flags & LPR_LINUX_O_ACCMODE) == LPR_LINUX_O_WRONLY ?
            -LPR_LINUX_EBADF : 0;
    }
    if (lpr_pipe_fd_is_active(fd)) {
        lpr_pipe_backend_t *pipe = lpr_pipe_backend(fd);
        if (pipe == 0 || !pipe->readable || pipe->native.raw < 0) {
            return -LPR_LINUX_EBADF;
        }
        for (;;) {
            const int64_t n = lpr_pacha_syscall3(
                PACHAOS_SYSCALL_FD_READV,
                (uint64_t)(uint32_t)pipe->native.raw,
                iov_raw,
                iov_count);
            if (n >= 0) {
                return n;
            }
            const int64_t err = lpr_pacha_status_to_errno(n);
            if (err != -LPR_LINUX_EAGAIN ||
                (pipe->flags & LPR_LINUX_O_NONBLOCK) != 0)
            {
                return err;
            }
            const int64_t wait_status = lpr_pipe_wait(fd, 0x0001u, 0);
            if (wait_status != 0) {
                return wait_status;
            }
        }
    }
    if (lpr_linux_tty_fd_active(fd)) {
        return lpr_iov_scalar_io(fd, iov_raw, iov_count, 0);
    }
    if (lpr_linux_drm_fd_active(fd)) {
        return lpr_iov_scalar_io(fd, iov_raw, iov_count, 0);
    }
    if (lpr_linux_input_fd_active(fd)) {
        return lpr_iov_scalar_io(fd, iov_raw, iov_count, 0);
    }
    if (lpr_linux_eventfd_active(fd) || lpr_linux_timerfd_active(fd)) {
        int64_t total = 0;
        const lpr_linux_iovec_t *iov = (const lpr_linux_iovec_t *)(uintptr_t)iov_raw;
        for (uint64_t i = 0; i < iov_count; i += 1) {
            const int64_t n = lpr_linux_read(fd, iov[i].base, iov[i].len);
            if (n < 0) {
                return total != 0 ? total : n;
            }
            total += n;
            if ((uint64_t)n < iov[i].len) {
                break;
            }
        }
        return total;
    }
    if (!lpr_fd_is_filed(fd)) {
        return -LPR_LINUX_EBADF;
    }
    const lpr_linux_iovec_t *iov = (const lpr_linux_iovec_t *)(uintptr_t)iov_raw;
    uint64_t trace_requested = 0;
    for (uint64_t i = 0; i < iov_count; i += 1) {
        if (iov[i].len != 0 && trace_requested <= UINT64_MAX - iov[i].len) {
            trace_requested += iov[i].len;
        }
    }
    if (iov_count > 1 &&
        lpr_fd_shadow_offset_eligible(fd) &&
        lpr_filed_backend(fd)->offset_valid)
    {
        uint64_t requested = 0;
        for (uint64_t i = 0; i < iov_count; i += 1) {
            if (iov[i].len == 0) {
                continue;
            }
            if (iov[i].base == 0 || requested > UINT64_MAX - iov[i].len) {
                return -LPR_LINUX_EFAULT;
            }
            requested += iov[i].len;
        }
        if (requested != 0 && requested <= FILED_IO_BYTES) {
            const uint64_t offset = lpr_filed_control_offset(fd);
            lpr_readv_cache_coalesced++;
            lpr_readv_cache_bytes += requested;
            lpr_trace_readv_size(fd, iov_count, requested, 1, offset);
            if (requested <= LPR_FILED_PAGE_CACHE_BYTES) {
                const uint64_t page_start = offset & ~(LPR_FILED_PAGE_CACHE_BYTES - 1ull);
                const uint64_t page_offset = offset - page_start;
                if (requested <= LPR_FILED_PAGE_CACHE_BYTES - page_offset) {
                    int cache_hit = 0;
                    lpr_filed_page_cache_entry_t *entry =
                        lpr_page_cache_get(fd, offset, requested, &cache_hit);
                    if (entry != 0) {
                        if (cache_hit) {
                            lpr_readv_cache_hit++;
                        } else {
                            lpr_readv_cache_fill++;
                        }
                        (void)lpr_scatter_iov(iov, iov_count, entry->data + page_offset, requested);
                        lpr_filed_control_advance_offset(fd, offset, requested);
                        lpr_filed_backend(fd)->pread_active = 1;
                        return (int64_t)requested;
                    }
                } else {
                    lpr_readv_cache_cross_page++;
                    const uint64_t first_len = LPR_FILED_PAGE_CACHE_BYTES - page_offset;
                    const uint64_t second_len = requested - first_len;
                    int first_hit = 0;
                    int second_hit = 0;
                    lpr_filed_page_cache_entry_t *first =
                        lpr_page_cache_get(fd, offset, first_len, &first_hit);
                    lpr_filed_page_cache_entry_t *second =
                        first != 0 ? lpr_page_cache_get(fd, offset + first_len, second_len, &second_hit) : 0;
                    if (first != 0 && second != 0) {
                        uint8_t cache_scratch[LPR_FILED_PAGE_CACHE_BYTES];
                        lpr_memcpy(cache_scratch, first->data + page_offset, (size_t)first_len);
                        lpr_memcpy(cache_scratch + first_len, second->data, (size_t)second_len);
                        if (first_hit && second_hit) {
                            lpr_readv_cache_hit++;
                        } else {
                            lpr_readv_cache_fill++;
                        }
                        (void)lpr_scatter_iov(iov, iov_count, cache_scratch, requested);
                        lpr_filed_control_advance_offset(fd, offset, requested);
                        lpr_filed_backend(fd)->pread_active = 1;
                        return (int64_t)requested;
                    }
                }
            }
            lpr_readv_cache_fallback++;
            uint8_t scratch[FILED_IO_BYTES];
            const int64_t n = lpr_filed_io(FILED_OP_VFS_PREAD, fd, (uint64_t)(uintptr_t)scratch, requested, offset);
            if (n < 0) {
                return n;
            }
            const uint64_t got = (uint64_t)n;
            (void)lpr_scatter_iov(iov, iov_count, scratch, got);
            lpr_filed_control_advance_offset(fd, offset, got);
            lpr_filed_backend(fd)->pread_active = 1;
            return (int64_t)got;
        }
        if (requested >= LPR_FILED_READV_TO_VMO_MIN && requested <= LPR_FILED_READV_TO_VMO_MAX) {
            int vmo_fd = -1;
            unsigned char *mapped = 0;
            uint64_t map_len = 0;
            lpr_state_lock(&lpr_state.filed_rpc.readv_lock_word);
            if (lpr_readv_scratch_vmo(requested, &vmo_fd, &mapped, &map_len) == 0) {
                lpr_readv_cache_to_vmo++;
                lpr_trace_readv_size(fd, iov_count, requested, 2, lpr_filed_control_offset(fd));
                const uint64_t offset = lpr_filed_control_offset(fd);
                const int64_t n = lpr_linux_pread_to_vmo(
                    fd,
                    (uint64_t)(uint32_t)vmo_fd,
                    0,
                    requested,
                    offset);
                if (n >= 0) {
                    const uint64_t got = (uint64_t)n;
                    const unsigned char *src = mapped;
                    (void)lpr_scatter_iov(iov, iov_count, src, got);
                    if (lpr_fd_shadow_offset_eligible(fd)) {
                        lpr_filed_control_advance_offset(fd, offset, got);
                        lpr_filed_backend(fd)->pread_active = 1;
                    }
                    lpr_state_unlock(&lpr_state.filed_rpc.readv_lock_word);
                    return (int64_t)got;
                }
                lpr_trace_readv_to_vmo_status(fd, requested, n);
            }
            lpr_state_unlock(&lpr_state.filed_rpc.readv_lock_word);
        }
    }
    lpr_trace_readv_size(
        fd,
        iov_count,
        trace_requested,
        0,
        lpr_fd_is_filed(fd) ? lpr_filed_control_offset(fd) : 0);
    int64_t total = 0;
    for (uint64_t i = 0; i < iov_count; i += 1) {
        if (iov[i].len == 0) {
            continue;
        }
        const int64_t n = lpr_linux_read(fd, iov[i].base, iov[i].len);
        if (n < 0) {
            return total != 0 ? total : n;
        }
        total += n;
        if ((uint64_t)n < iov[i].len) {
            break;
        }
    }
    return total;
}

int64_t lpr_linux_pread64(uint64_t fd, uint64_t buf, uint64_t count, uint64_t offset)
{
    if (count > FILED_IO_BYTES) {
        return lpr_filed_io_chunked(FILED_OP_VFS_PREAD, fd, buf, count, offset);
    }
    return lpr_filed_io(FILED_OP_VFS_PREAD, fd, buf, count, offset);
}

int64_t lpr_linux_pread_to_vmo(
    uint64_t fd,
    uint64_t vmo_fd,
    uint64_t vmo_offset,
    uint64_t count,
    uint64_t file_offset)
{
    if (!lpr_fd_is_filed(fd)) {
        return -LPR_LINUX_EBADF;
    }
    if (vmo_fd < 16) {
        return -LPR_LINUX_EINVAL;
    }
    if (vmo_offset + count < vmo_offset) {
        return -LPR_LINUX_EINVAL;
    }

    void *page = 0;
    const int page_fd = lpr_create_pread_vmo_wire_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }

    filed_pread_vmo_t *pread_vmo = (filed_pread_vmo_t *)page;
    lpr_memset(pread_vmo, 0, sizeof(*pread_vmo));
    pread_vmo->handle = lpr_filed_backend(fd)->handle;
    pread_vmo->file_offset = file_offset;
    pread_vmo->vmo_offset = vmo_offset;
    pread_vmo->length = count;

    const int64_t ready = lpr_filed_endpoint_ready();
    if (ready != 0) {
        lpr_destroy_pread_vmo_wire_page(page_fd, page);
        return ready;
    }

    struct pacha_ipc_fd fds[2];
    struct pacha_ipc_msg request;
    struct pacha_ipc_msg reply;
    lpr_zero_bytes(fds, sizeof(fds));
    lpr_zero_bytes(&request, sizeof(request));
    lpr_zero_bytes(&reply, sizeof(reply));

    fds[0].fd = (uint64_t)(uint32_t)page_fd;
    fds[0].rights =
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE;
    fds[1].fd = vmo_fd;
    fds[1].rights =
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE;

    lpr_memmove((uint8_t *)page + PACHA_SERVICE_HEADER_BYTES, page, sizeof(*pread_vmo));
    lpr_memset(page, 0, PACHA_SERVICE_HEADER_BYTES);
    const uint64_t request_id = lpr_next_request_id(&lpr_request_id);
    pacha_service_envelope_t *header = (pacha_service_envelope_t *)page;
    header->magic = PACHA_SERVICE_REQUEST_MAGIC;
    header->abi_version = PACHA_SERVICE_ABI_VERSION;
    header->service_id = FILED_SERVICE_ID;
    header->op = FILED_OP_VFS_PREAD_TO_VMO;
    header->flags = PACHA_SERVICE_FLAG_PAGE_PAYLOAD;
    header->request_id = request_id;
    header->trace_id = request_id;
    header->payload_size = sizeof(*pread_vmo);
    header->fd_count = 1;
    request.word0 = PACHA_SERVICE_REQUEST_MAGIC;
    request.word1 = 0;
    request.word3 = request_id;
    request.fds = fds;
    request.fd_count = 2;

    const int64_t reply_fd = lpr_pacha_syscall2(
        PACHAOS_SYSCALL_IPC_CALL,
        LPR_FILED_ENDPOINT_FD,
        (uint64_t)(uintptr_t)&request);
    if (reply_fd < 16) {
        lpr_destroy_pread_vmo_wire_page(page_fd, page);
        return lpr_pacha_status_to_errno(reply_fd);
    }

    const int64_t recv_status = lpr_native_ipc_recv_wait(
        (uint64_t)(uint32_t)reply_fd,
        &reply);
    (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)reply_fd);
    if (recv_status != 0) {
        lpr_destroy_pread_vmo_wire_page(page_fd, page);
        return lpr_pacha_status_to_errno(recv_status);
    }
    const pacha_service_envelope_t *reply_header = (const pacha_service_envelope_t *)page;
    if (reply.word0 != PACHA_SERVICE_REPLY_MAGIC ||
        reply.word3 != request_id ||
        reply_header->magic != PACHA_SERVICE_REPLY_MAGIC ||
        reply_header->service_id != FILED_SERVICE_ID ||
        reply_header->op != FILED_OP_VFS_PREAD_TO_VMO ||
        reply_header->request_id != request_id)
    {
        lpr_destroy_pread_vmo_wire_page(page_fd, page);
        return -LPR_LINUX_EIO;
    }
    if (reply_header->status < 0) {
        const int64_t status = reply_header->status;
        lpr_destroy_pread_vmo_wire_page(page_fd, page);
        return status;
    }
    const int64_t result = (int64_t)reply_header->result;
    lpr_destroy_pread_vmo_wire_page(page_fd, page);
    return result;
}
