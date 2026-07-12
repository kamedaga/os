#include "../lpr_filed_internal.h"

static void *lpr_inputd_payload(void *page)
{
    return page == 0 ? 0 : (uint8_t *)page + PACHA_SERVICE_HEADER_BYTES;
}

static int64_t lpr_inputd_call(
    uint32_t op,
    int page_fd,
    void *page,
    uint32_t payload_size,
    uint64_t *out_result)
{
    if (page_fd < 16 || page == 0) return -LPR_LINUX_EINVAL;
    pacha_service_envelope_t *header = page;
    const uint64_t request_id = lpr_next_request_id(&lpr_termd_request_id);
    lpr_memset(header, 0, sizeof(*header));
    header->magic = PACHA_SERVICE_REQUEST_MAGIC;
    header->abi_version = PACHA_SERVICE_ABI_VERSION;
    header->service_id = INPUTD_SERVICE_ID;
    header->op = op;
    header->flags = payload_size != 0 ? PACHA_SERVICE_FLAG_PAGE_PAYLOAD : 0;
    header->request_id = request_id;
    header->trace_id = request_id;
    header->payload_size = payload_size;
    header->fd_count = 0;
    const uint64_t rights = PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE;
    struct pacha_ipc_fd fd = {
        .fd = (uint64_t)(uint32_t)page_fd,
        .rights = rights,
    };
    const struct pacha_ipc_msg request = {
        .word0 = PACHA_SERVICE_REQUEST_MAGIC,
        .word3 = request_id,
        .fds = &fd,
        .fd_count = 1,
    };
    const int reply_fd = (int)lpr_pacha_syscall2(
        PACHAOS_SYSCALL_IPC_CALL,
        LPR_INPUTD_INPUT_ENDPOINT_FD,
        (uint64_t)(uintptr_t)&request);
    if (reply_fd < 16) return lpr_pacha_status_to_errno(reply_fd);
    struct pacha_ipc_msg reply;
    lpr_memset(&reply, 0, sizeof(reply));
    const int64_t recv_status = lpr_pacha_syscall4(
        PACHAOS_SYSCALL_IPC_RECV_WAIT,
        (uint64_t)(uint32_t)reply_fd,
        (uint64_t)(uintptr_t)&reply,
        UINT64_MAX,
        0);
    (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)reply_fd);
    if (recv_status != 0) return lpr_pacha_status_to_errno(recv_status);
    if (reply.word0 != PACHA_SERVICE_REPLY_MAGIC || reply.word3 != request_id ||
        header->magic != PACHA_SERVICE_REPLY_MAGIC ||
        header->service_id != INPUTD_SERVICE_ID || header->op != op ||
        header->request_id != request_id)
        return -LPR_LINUX_EIO;
    const int64_t status = header->status;
    if (status == 0 && out_result != 0) *out_result = header->result;
    return status;
}

static int lpr_input_fd_alloc(uint64_t handle, uint64_t flags, uint32_t event_index)
{
    const int fd = lpr_fd_slot_alloc();
    if (fd < 0) return fd;
    if (lpr_control_install_fd(fd, LPR_FD_TABLE_KIND_INPUT, flags, handle, 0) != 0)
        return -LPR_LINUX_EMFILE;
    lpr_input_fd_t *input = lpr_fd_input_payload(fd);
    if (input == 0) {
        lpr_control_close_fd(fd);
        return -LPR_LINUX_EIO;
    }
    input->active = 1;
    input->reserved0 = (uint8_t)event_index;
    input->flags = (uint32_t)flags;
    input->handle = handle;
    return fd;
}

int64_t lpr_input_open_path(const char *path, uint64_t flags)
{
    static const char prefix[] = "/dev/input/event";
    if (path == 0 || lpr_strncmp(path, prefix, sizeof(prefix) - 1u) != 0)
        return -LPR_LINUX_ENOENT;
    const char *suffix = path + sizeof(prefix) - 1u;
    if (suffix[0] < '0' || suffix[0] > '9' || suffix[1] != 0)
        return -LPR_LINUX_ENOENT;
    const uint32_t event_index = (uint32_t)(suffix[0] - '0');
    void *page = 0;
    const int page_fd = lpr_create_tty_wire_page(&page);
    if (page_fd < 0) return page_fd;
    inputd_open_request_t *open = lpr_inputd_payload(page);
    lpr_memset(open, 0, sizeof(*open));
    open->event_index = event_index;
    open->flags = (uint32_t)flags;
    uint64_t handle = 0;
    const int64_t status = lpr_inputd_call(
        INPUTD_OP_OPEN, page_fd, page, sizeof(*open), &handle);
    lpr_destroy_tty_wire_page(page_fd, page);
    if (status != 0) return status;
    const int fd = lpr_input_fd_alloc(handle, flags, event_index);
    if (fd < 0) (void)lpr_input_close_handle(handle);
    return fd;
}

static int64_t lpr_input_handle_call(uint32_t op, uint64_t handle, uint64_t *out_result)
{
    void *page = 0;
    const int page_fd = lpr_create_tty_wire_page(&page);
    if (page_fd < 0) return page_fd;
    inputd_handle_request_t *request = lpr_inputd_payload(page);
    lpr_memset(request, 0, sizeof(*request));
    request->handle = handle;
    const int64_t status = lpr_inputd_call(op, page_fd, page, sizeof(*request), out_result);
    lpr_destroy_tty_wire_page(page_fd, page);
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

int64_t lpr_input_read_events(uint64_t fd, uint64_t buf, uint64_t count)
{
    lpr_input_fd_t *input = lpr_fd_input_payload(fd);
    if (input == 0) return -LPR_LINUX_EBADF;
    if (count == 0) return 0;
    if (buf == 0) return -LPR_LINUX_EFAULT;
    if (count < sizeof(inputd_input_event_t)) return -LPR_LINUX_EINVAL;
    for (;;) {
        void *page = 0;
        const int page_fd = lpr_create_tty_wire_page(&page);
        if (page_fd < 0) return page_fd;
        inputd_read_request_t *read = lpr_inputd_payload(page);
        lpr_memset(read, 0, sizeof(*read));
        read->handle = input->handle;
        uint64_t capacity = count / sizeof(inputd_input_event_t);
        if (capacity > INPUTD_EVENT_CAPACITY) capacity = INPUTD_EVENT_CAPACITY;
        read->event_capacity = (uint32_t)capacity;
        uint64_t result = 0;
        const int64_t status = lpr_inputd_call(
            INPUTD_OP_READ, page_fd, page, sizeof(*read), &result);
        if (status == 0) {
            uint64_t events = read->event_count < result ? read->event_count : result;
            if (events > capacity) {
                lpr_destroy_tty_wire_page(page_fd, page);
                return -LPR_LINUX_EIO;
            }
            const uint64_t bytes = events * sizeof(inputd_input_event_t);
            lpr_memcpy((void *)(uintptr_t)buf, read->events, (size_t)bytes);
            lpr_destroy_tty_wire_page(page_fd, page);
            return (int64_t)bytes;
        }
        lpr_destroy_tty_wire_page(page_fd, page);
        if (status != -LPR_LINUX_EAGAIN || (input->flags & LPR_LINUX_O_NONBLOCK) != 0)
            return status;
        struct pachaos_timespec delay = { .tv_sec = 0, .tv_nsec = 1000000 };
        const int64_t sleep_status = lpr_pacha_syscall1(
            PACHAOS_SYSCALL_NANOSLEEP, (uint64_t)(uintptr_t)&delay);
        if (sleep_status != 0) return lpr_pacha_status_to_errno(sleep_status);
    }
}

int64_t lpr_input_ioctl(uint64_t fd, uint64_t command, uint64_t arg)
{
    lpr_input_fd_t *input = lpr_fd_input_payload(fd);
    if (input == 0) return -LPR_LINUX_EBADF;
    const uint32_t size = (uint32_t)((command >> 16) & 0x3fffu);
    const uint32_t direction = (uint32_t)((command >> 30) & 0x3u);
    if (size > INPUTD_IOCTL_DATA_BYTES) return -LPR_LINUX_EINVAL;
    if (size != 0 && arg == 0) return -LPR_LINUX_EFAULT;
    void *page = 0;
    const int page_fd = lpr_create_tty_wire_page(&page);
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
    lpr_destroy_tty_wire_page(page_fd, page);
    return status;
}

int64_t lpr_input_poll_events(uint64_t fd, uint32_t events)
{
    lpr_input_fd_t *input = lpr_fd_input_payload(fd);
    if (input == 0) return -LPR_LINUX_EBADF;
    void *page = 0;
    const int page_fd = lpr_create_tty_wire_page(&page);
    if (page_fd < 0) return page_fd;
    inputd_poll_request_t *request = lpr_inputd_payload(page);
    lpr_memset(request, 0, sizeof(*request));
    request->handle = input->handle;
    request->events = events;
    uint64_t result = 0;
    const int64_t status = lpr_inputd_call(
        INPUTD_OP_POLL, page_fd, page, sizeof(*request), &result);
    lpr_destroy_tty_wire_page(page_fd, page);
    return status == 0 ? (int64_t)result : status;
}
