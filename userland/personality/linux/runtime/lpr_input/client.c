#include "../lpr_filed_internal.h"
#include "../lpr_fd/allocate.h"
#include "wire.h"
#include <inputd/event_ring.h>

static void *lpr_inputd_payload(void *page)
{
    return page == 0 ? 0 : (uint8_t *)page + PACHA_SERVICE_HEADER_BYTES;
}

static int64_t lpr_inputd_call_transfer(
    uint32_t op,
    pacha_fd_t page_fd,
    void *page,
    uint32_t payload_size,
    uint64_t *out_result,
    pacha_fd_t transfer_fd, pacha_fd_t ring_fd)
{
    if (page_fd < 16 || page == 0) return -LPR_LINUX_EINVAL;
    pacha_service_envelope_t *header = page;
    const uint64_t request_id = lpr_next_request_id(&lpr_termd_request_id);
    lpr_memset(header, 0, sizeof(*header));
    header->magic = PACHA_SERVICE_REQUEST_MAGIC;
    header->header_size = sizeof(*header);
    header->service_id = INPUTD_SERVICE_ID;
    header->op = op;
    header->flags = payload_size != 0 ? PACHA_SERVICE_FLAG_PAGE_PAYLOAD : 0;
    header->request_id = request_id;
    header->trace_id = request_id;
    header->payload_size = payload_size;
    header->fd_count = 0;
    const uint64_t rights = PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE;
    struct pacha_ipc_fd fds[3] = {{
        .fd = (uint64_t)(uint32_t)page_fd,
        .rights = rights,
    }};
    uint64_t fd_count = 1;
    if (transfer_fd >= 16) {
        struct pacha_fd_info info;
        if (!lpr_native_fd_info((uint64_t)transfer_fd, &info) ||
            info.kind != PACHA_FD_KIND_CHANNEL) return -LPR_LINUX_EBADF;
        fds[1].fd = (uint64_t)transfer_fd;
        fds[1].rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_TRANSFER |
            PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_SEND | PACHA_FD_RIGHT_RECV |
            PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL;
        fds[1].transfer_flags = PACHA_IPC_TRANSFER_MOVE;
        fd_count = 2;
    }
    if (ring_fd >= 16) {
        fds[fd_count++] = (struct pacha_ipc_fd){.fd = (uint64_t)ring_fd,
            .rights = PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_MAP_READ |
                PACHA_FD_RIGHT_MAP_WRITE};
    }
    const struct pacha_ipc_msg request = {
        .word0 = PACHA_SERVICE_REQUEST_MAGIC,
        .word3 = request_id,
        .fds = fds,
        .fd_count = fd_count,
    };
    struct pacha_ipc_msg reply = {0};
    const int64_t recv_status = lpr_native_ipc_call_wait(
        LPR_INPUTD_INPUT_ENDPOINT_FD, &request, &reply, NULL);
    if (recv_status != 0) return lpr_pacha_status_to_errno(recv_status);
    if (reply.word0 != PACHA_SERVICE_REPLY_MAGIC || reply.word3 != request_id ||
        header->magic != PACHA_SERVICE_REPLY_MAGIC ||
        header->header_size != sizeof(*header) ||
        header->service_id != INPUTD_SERVICE_ID || header->op != op ||
        header->request_id != request_id)
        return -LPR_LINUX_EIO;
    const int64_t status = header->status;
    if (status == 0 && out_result != 0) *out_result = header->result;
    return status;
}

static int64_t lpr_inputd_call(
    uint32_t op,
    pacha_fd_t page_fd,
    void *page,
    uint32_t payload_size,
    uint64_t *out_result)
{
    return lpr_inputd_call_transfer(op, page_fd, page, payload_size, out_result, -1, -1);
}

static int lpr_input_parse_event_index(const char *suffix, uint32_t *out_event_index)
{
    if (suffix == 0 || out_event_index == 0 || suffix[0] == 0)
        return 0;
    if (suffix[0] == '0' && suffix[1] != 0)
        return 0;
    uint32_t value = 0;
    for (const char *cursor = suffix; *cursor != 0; cursor++) {
        if (*cursor < '0' || *cursor > '9')
            return 0;
        const uint32_t digit = (uint32_t)(*cursor - '0');
        if (value > (255u - digit) / 10u)
            return 0;
        value = value * 10u + digit;
    }
    *out_event_index = value;
    return 1;
}

static int lpr_input_fd_alloc(uint64_t handle, uint64_t flags, uint32_t event_index, pacha_fd_t native_wait_fd, pacha_fd_t ring_fd, void *ring)
{
    const lpr_input_backend_t record = {
        .active = 1, .event_index = (uint8_t)event_index,
        .flags = (uint32_t)flags, .handle = handle,
        .wait_fd.raw = native_wait_fd, .lease_fd.raw = -1,
        .ring_fd.raw = ring_fd, .ring_address = (uint64_t)(uintptr_t)ring,
    };
    return lpr_fd_alloc_state(LPR_FD_OPS_INPUT, flags, 0, &record, sizeof(record));
}

int64_t lpr_input_open_path(const char *path, uint64_t flags)
{
    static const char prefix[] = "/dev/input/event";
    if (path == 0 || lpr_strncmp(path, prefix, sizeof(prefix) - 1u) != 0)
        return -LPR_LINUX_ENOENT;
    const char *suffix = path + sizeof(prefix) - 1u;
    uint32_t event_index = 0;
    if (!lpr_input_parse_event_index(suffix, &event_index))
        return -LPR_LINUX_ENOENT;
    void *page = 0;
    const pacha_fd_t page_fd = lpr_input_wire_acquire(&page);
    if (page_fd < 0) return page_fd;
    inputd_open_request_t *open = lpr_inputd_payload(page);
    lpr_memset(open, 0, sizeof(*open));
    open->event_index = event_index;
    open->flags = (uint32_t)flags;
    pacha_fd_t native_wait_fd = -1;
    pacha_fd_t remote_wait_fd = -1;
    const int pair_status = lpr_native_wait_pair(&native_wait_fd, &remote_wait_fd);
    if (pair_status != 0) {
        lpr_input_wire_release();
        return pair_status;
    }
    const uint64_t rights = PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_DUP |
        PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE;
    pacha_fd_t ring_fd = (pacha_fd_t)lpr_pacha_syscall3(PACHAOS_SYSCALL_VMO_CREATE,
        INPUTD_RING_BYTES, rights, PACHA_FD_FLAG_INHERIT);
    int64_t mapped = ring_fd >= 16 ? lpr_pacha_syscall6(PACHAOS_SYSCALL_MMAP,
        ring_fd, 0, INPUTD_RING_BYTES, PACHAOS_PROT_READ | PACHAOS_PROT_WRITE,
        PACHAOS_MMAP_SHARED, 0) : -1;
    if (mapped < 4096) {
        if (ring_fd >= 16) (void)lpr_close_native_fd_if_open(ring_fd);
        (void)lpr_close_native_fd_if_open(native_wait_fd);
        (void)lpr_close_native_fd_if_open(remote_wait_fd);
        lpr_input_wire_release();
        return -LPR_LINUX_ENOMEM;
    }
    uint64_t handle = 0;
    int64_t status = lpr_inputd_call_transfer(
        INPUTD_OP_OPEN, page_fd, page, sizeof(*open), &handle, remote_wait_fd, ring_fd);
    lpr_input_wire_release();
    if (status == 0 && __atomic_load_n(&((struct inputd_event_ring *)(uintptr_t)mapped)->handle,
            __ATOMIC_ACQUIRE) != handle) {
        (void)lpr_input_close_handle(handle);
        status = -LPR_LINUX_EIO; /* old inputd must fail closed, never hang */
    }
    if (status != 0) {
        (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP, mapped, INPUTD_RING_BYTES);
        (void)lpr_close_native_fd_if_open(ring_fd);
        (void)lpr_close_native_fd_if_open((uint64_t)native_wait_fd);
        (void)lpr_close_native_fd_if_open((uint64_t)remote_wait_fd);
        return status;
    }
    const int fd = lpr_input_fd_alloc(handle, flags, event_index, native_wait_fd, ring_fd, (void *)(uintptr_t)mapped);
    if (fd < 0) {
        (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP, mapped, INPUTD_RING_BYTES);
        (void)lpr_close_native_fd_if_open(ring_fd);
        (void)lpr_input_close_handle(handle);
        (void)lpr_close_native_fd_if_open((uint64_t)native_wait_fd);
    }
    return fd;
}

static int64_t lpr_input_handle_call(uint32_t op, uint64_t handle, uint64_t *out_result)
{
    void *page = 0;
    const pacha_fd_t page_fd = lpr_input_wire_acquire(&page);
    if (page_fd < 0) return page_fd;
    inputd_handle_request_t *request = lpr_inputd_payload(page);
    lpr_memset(request, 0, sizeof(*request));
    request->handle = handle;
    const int64_t status = lpr_inputd_call(op, page_fd, page, sizeof(*request), out_result);
    lpr_input_wire_release();
    return status;
}

int64_t lpr_input_close_handle(uint64_t handle)
{
    return lpr_input_handle_call(INPUTD_OP_CLOSE, handle, 0);
}

int64_t lpr_input_dup_handle(uint64_t handle)
{
    uint64_t result = 0;
    const int64_t status = lpr_input_handle_call(INPUTD_OP_DUP, handle, &result);
    return status == 0 && result == handle ? 0 : (status != 0 ? status : -LPR_LINUX_EIO);
}

int64_t lpr_input_transfer_dup_handle(
    uint64_t handle,
    pacha_fd_t lease_fd,
    uint64_t *out_handle)
{
    if (out_handle == 0 || lease_fd < 16) return -LPR_LINUX_EINVAL;
    *out_handle = 0;
    void *page = 0;
    const pacha_fd_t page_fd = lpr_input_wire_acquire(&page);
    if (page_fd < 0) return page_fd;
    inputd_handle_request_t *request = lpr_inputd_payload(page);
    lpr_memset(request, 0, sizeof(*request));
    request->handle = handle;
    const int64_t status = lpr_inputd_call_transfer(
        INPUTD_OP_DUP,
        page_fd,
        page,
        sizeof(*request),
        out_handle,
        lease_fd, -1);
    lpr_input_wire_release();
    return status == 0 && *out_handle == handle ? 0 :
        (status != 0 ? status : -LPR_LINUX_EIO);
}

static struct inputd_event_ring *lpr_input_ring(lpr_input_backend_t *input)
{
    uint64_t address = __atomic_load_n(&input->ring_address, __ATOMIC_ACQUIRE);
    if (address) return (void *)(uintptr_t)address;
    if (input->ring_fd.raw < 16) return 0;
    int64_t mapped = lpr_pacha_syscall6(PACHAOS_SYSCALL_MMAP,
        input->ring_fd.raw, 0, INPUTD_RING_BYTES,
        PACHAOS_PROT_READ | PACHAOS_PROT_WRITE, PACHAOS_MMAP_SHARED, 0);
    if (mapped < 4096) return 0;
    if (__atomic_load_n(&((struct inputd_event_ring *)(uintptr_t)mapped)->handle,
            __ATOMIC_ACQUIRE) != input->handle) {
        (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP, mapped, INPUTD_RING_BYTES);
        return 0;
    }
    if (!__atomic_compare_exchange_n(&input->ring_address, &address, (uint64_t)mapped,
            0, __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)) {
        (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP, mapped, INPUTD_RING_BYTES);
        return (void *)(uintptr_t)address;
    }
    return (void *)(uintptr_t)mapped;
}

int64_t lpr_input_read_events(uint64_t fd, uint64_t buf, uint64_t count, unsigned *wait_not_ready)
{
    lpr_input_backend_t *input = lpr_input_backend(fd);
    if (!input) return -LPR_LINUX_EBADF;
    if (count && count < sizeof(inputd_input_event_t)) return -LPR_LINUX_EINVAL;
    struct inputd_event_ring *ring = lpr_input_ring(input);
    if (!ring) return -LPR_LINUX_EIO;
    if (!count) {
        int error = inputd_ring_error(ring);
        if (error) return error;
        if ((input->flags & LPR_LINUX_O_NONBLOCK) && !inputd_ring_ready(ring))
            return -LPR_LINUX_EAGAIN;
        return 0;
    }
    for (;;) {
        int events = inputd_ring_read(ring, (void *)(uintptr_t)buf,
            count / sizeof(inputd_input_event_t));
        if (events >= 0) return events * (int64_t)sizeof(inputd_input_event_t);
        if (events != -LPR_LINUX_EAGAIN || (input->flags & LPR_LINUX_O_NONBLOCK))
            return events;
        lpr_wait_graph_t graph;
        lpr_wait_deadline_t deadline;
        lpr_wait_graph_init(&graph);
        int64_t status = lpr_wait_graph_add_fd(&graph, fd, INPUTD_POLLIN);
        if (status) return status;
        status = lpr_wait_deadline_init(&deadline, -1);
        if (status) return status;
        status = lpr_wait_graph_block(&graph, &deadline, wait_not_ready);
        if (status) return status;
    }
}

int64_t lpr_input_ioctl(uint64_t fd, uint64_t command, uint64_t arg)
{
    lpr_input_backend_t *input = lpr_input_backend(fd);
    if (input == 0) return -LPR_LINUX_EBADF;
    const uint32_t size = (uint32_t)((command >> 16) & 0x3fffu);
    const uint32_t direction = (uint32_t)((command >> 30) & 0x3u);
    if (size > INPUTD_IOCTL_DATA_BYTES) return -LPR_LINUX_EINVAL;
    if (size != 0 && arg == 0) return -LPR_LINUX_EFAULT;
    void *page = 0;
    const pacha_fd_t page_fd = lpr_input_wire_acquire(&page);
    if (page_fd < 0) return page_fd;
    inputd_ioctl_request_t *request = lpr_inputd_payload(page);
    lpr_memset(request, 0, sizeof(*request));
    request->handle = input->handle;
    request->request = command;
    request->data_size = size;
    if ((direction & 1u) != 0 && size != 0)
        lpr_memcpy(request->data, (const void *)(uintptr_t)arg, size);
    uint64_t result = 0;
    const int64_t status = lpr_inputd_call(
        INPUTD_OP_IOCTL, page_fd, page, sizeof(*request), &result);
    if (status == 0 && (direction & 2u) != 0 && result != 0) {
        uint64_t bytes = request->result_size < result ? request->result_size : result;
        if (bytes > size) bytes = size;
        lpr_memcpy((void *)(uintptr_t)arg, request->data, (size_t)bytes);
    }
    lpr_input_wire_release();
    return status;
}

int64_t lpr_input_poll_sequence(uint64_t fd, uint32_t events, struct unix_poll_sequence *sequence)
{
    lpr_input_backend_t *input = lpr_input_backend(fd);
    if (!input) return -LPR_LINUX_EBADF;
    struct inputd_event_ring *ring = lpr_input_ring(input);
    if (!ring) return -LPR_LINUX_EIO;
    uint64_t tail = inputd_ring_tail(ring);
    int error = inputd_ring_error(ring);
    if (sequence) *sequence = (struct unix_poll_sequence){.source=input->handle,
        .read=tail,.error=(uint64_t)(int64_t)error};
    if (error) return INPUTD_POLLHUP | 0x0008u;
    return inputd_ring_head(ring) != tail ? events & INPUTD_POLLIN : 0;
}

int64_t lpr_input_poll_events(uint64_t fd, uint32_t events)
{
    return lpr_input_poll_sequence(fd, events, 0);
}

/* Only blocking waits drain IPC tokens. Arm after draining and recheck both
 * progress stamps and level: producer publishes tail before exchanging armed.
 * ET suppresses previously reported level but never a subsequent SYN_REPORT. */
int lpr_input_prepare_wait_sequence(uint64_t fd, uint32_t events, uint32_t ignore,
    const struct unix_poll_sequence *expected)
{
    lpr_input_backend_t *input = lpr_input_backend(fd);
    if (!input) return -LPR_LINUX_EBADF;
    struct inputd_event_ring *ring = lpr_input_ring(input);
    if (!ring) return -LPR_LINUX_EIO;
    struct unix_poll_sequence observed;
    int64_t ready = lpr_input_poll_sequence(fd, events | 8u | 16u, &observed);
    if (ready < 0) return (int)ready;
    if (expected) ignore &= ~unix_poll_sequence_changed(expected, &observed);
    if ((uint32_t)ready & ~ignore) return 1;
    lpr_native_wait_drain(input->wait_fd.raw);
    __atomic_store_n(&ring->armed, 1, __ATOMIC_SEQ_CST);
    ready = lpr_input_poll_sequence(fd, events | 8u | 16u, &observed);
    if (ready < 0) return (int)ready;
    if (expected) ignore &= ~unix_poll_sequence_changed(expected, &observed);
    return ((uint32_t)ready & ~ignore) != 0;
}

int lpr_input_prepare_wait(uint64_t fd)
{
    return lpr_input_prepare_wait_sequence(fd, 1u, 0, 0);
}

pacha_fd_t lpr_input_native_wait_fd(uint64_t fd)
{
    lpr_input_backend_t *input = lpr_input_backend(fd);
    return input ? input->wait_fd.raw : -1;
}

void lpr_input_mark_hangup(uint64_t fd)
{
    lpr_input_backend_t *input = lpr_input_backend(fd);
    struct inputd_event_ring *ring = input ? lpr_input_ring(input) : 0;
    if (ring) __atomic_store_n(&ring->error, -LPR_LINUX_ENODEV, __ATOMIC_RELEASE);
}
