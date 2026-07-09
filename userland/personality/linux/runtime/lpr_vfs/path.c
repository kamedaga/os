#include "../lpr_filed_internal.h"

int64_t lpr_filed_endpoint_ready(void)
{
    if (lpr_filed_endpoint_checked > 0) {
        return 0;
    }
    if (lpr_filed_endpoint_checked < 0) {
        return -LPR_LINUX_ENOSYS;
    }
    struct pacha_fd_info info;
    lpr_memset(&info, 0, sizeof(info));
    const int64_t status = lpr_pacha_syscall2(
        PACHAOS_SYSCALL_FD_GET_INFO,
        LPR_FILED_ENDPOINT_FD,
        (uint64_t)(uintptr_t)&info);
    if (status != 0 ||
        (info.kind != PACHA_FD_KIND_ENDPOINT && info.kind != PACHA_FD_KIND_CHANNEL))
    {
        lpr_filed_endpoint_checked = -1;
        return -LPR_LINUX_ENOSYS;
    }
    lpr_filed_endpoint_checked = 1;
    return 0;
}

int64_t lpr_filed_session_connect(void)
{
    if (lpr_session_fd >= 16 &&
        lpr_session_page_fd >= 16 &&
        lpr_session_page != 0)
    {
        return 0;
    }
    if (lpr_session_checked < 0) {
        return -LPR_LINUX_ENOSYS;
    }
    const int64_t ready = lpr_filed_endpoint_ready();
    if (ready != 0) {
        lpr_session_checked = -1;
        return ready;
    }

    const uint64_t channel_rights =
        PACHA_FD_RIGHT_INSPECT |
        PACHA_FD_RIGHT_WAIT |
        PACHA_FD_RIGHT_POLL |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_SEND |
        PACHA_FD_RIGHT_RECV |
        PACHA_FD_RIGHT_TRANSFER;
    const uint64_t page_rights =
        PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE;
    uint64_t pair[2] = {0, 0};
    int64_t status = lpr_pacha_syscall3(
        PACHAOS_SYSCALL_IPC_CHANNEL_CREATE,
        (uint64_t)(uintptr_t)pair,
        channel_rights,
        PACHA_FD_FLAG_CLOEXEC);
    if (status != 0 || pair[0] < 16 || pair[1] < 16) {
        if (pair[0] >= 16) {
            (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, pair[0]);
        }
        if (pair[1] >= 16) {
            (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, pair[1]);
        }
        lpr_session_checked = -1;
        return status != 0 ? lpr_pacha_status_to_errno(status) : -LPR_LINUX_EINVAL;
    }

    const int64_t page_fd = lpr_pacha_syscall3(
        PACHAOS_SYSCALL_VMO_CREATE,
        FILED_V2_SESSION_PAGE_BYTES,
        page_rights,
        0);
    if (page_fd < 16) {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, pair[0]);
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, pair[1]);
        lpr_session_checked = -1;
        return lpr_pacha_status_to_errno(page_fd);
    }

    const int64_t mapped = lpr_pacha_syscall6(
        PACHAOS_SYSCALL_MMAP,
        (uint64_t)(uint32_t)page_fd,
        0,
        FILED_V2_SESSION_PAGE_BYTES,
        PACHAOS_PROT_READ | PACHAOS_PROT_WRITE,
        PACHAOS_MMAP_SHARED,
        0);
    if (mapped < 4096) {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)page_fd);
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, pair[0]);
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, pair[1]);
        lpr_session_checked = -1;
        return lpr_pacha_status_to_errno(mapped);
    }

    void *page = (void *)(uintptr_t)mapped;
    lpr_zero_bytes(page, FILED_V2_SESSION_PAGE_BYTES);
    filed_v2_fast_header_t *header = (filed_v2_fast_header_t *)page;
    header->magic = FILED_V2_FAST_MAGIC;
    header->version = FILED_V2_FAST_VERSION;
    header->request_capacity = FILED_V2_FAST_REQUEST_CAPACITY;
    header->completion_capacity = FILED_V2_FAST_COMPLETION_CAPACITY;
    header->payload_slot_count = FILED_V2_FAST_PAYLOAD_SLOT_COUNT;
    header->payload_slot_size = FILED_V2_PAGE_BYTES;
    header->payload_offset = FILED_V2_FAST_PAYLOAD_OFFSET;
    header->generation_offset = FILED_V2_FAST_GENERATION_OFFSET;
    header->generation_capacity = FILED_V2_FAST_GENERATION_CAPACITY;

    void *service_page = 0;
    const int service_page_fd = lpr_create_standalone_wire_page(&service_page);
    if (service_page_fd < 16) {
        (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP, (uint64_t)(uintptr_t)page, FILED_V2_SESSION_PAGE_BYTES);
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)page_fd);
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, pair[0]);
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, pair[1]);
        lpr_session_checked = -1;
        return service_page_fd;
    }

    const uint64_t request_id = ++lpr_request_id;
    pacha_service_request_header_t *service_header = (pacha_service_request_header_t *)service_page;
    lpr_zero_bytes(service_header, sizeof(*service_header));
    service_header->magic = PACHA_SERVICE_REQUEST_MAGIC;
    service_header->abi_version = PACHA_SERVICE_ABI_VERSION;
    service_header->service_id = FILED_V2_SERVICE_ID;
    service_header->op = FILED_V2_OP_SESSION_OPEN;
    service_header->flags = 0;
    service_header->request_id = request_id;
    service_header->trace_id = request_id;
    service_header->payload_size = 0;
    service_header->fd_count = 2;

    struct pacha_ipc_fd fds[3];
    struct pacha_ipc_msg request;
    struct pacha_ipc_msg reply;
    lpr_zero_bytes(fds, sizeof(fds));
    lpr_zero_bytes(&request, sizeof(request));
    lpr_zero_bytes(&reply, sizeof(reply));
    fds[0].fd = (uint64_t)(uint32_t)service_page_fd;
    fds[0].rights =
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE;
    fds[1].fd = pair[1];
    fds[1].rights =
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_WAIT |
        PACHA_FD_RIGHT_POLL |
        PACHA_FD_RIGHT_SEND |
        PACHA_FD_RIGHT_RECV;
    fds[2].fd = (uint64_t)(uint32_t)page_fd;
    fds[2].rights = page_rights;
    request.word0 = PACHA_SERVICE_REQUEST_MAGIC;
    request.word1 = 0;
    request.word3 = request_id;
    request.fds = fds;
    request.fd_count = 3;
    const int64_t reply_fd = lpr_pacha_syscall2(
        PACHAOS_SYSCALL_IPC_CALL,
        LPR_FILED_ENDPOINT_FD,
        (uint64_t)(uintptr_t)&request);
    (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, pair[1]);
    if (reply_fd < 16) {
        lpr_destroy_standalone_wire_page(service_page_fd, service_page);
        (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP, (uint64_t)(uintptr_t)page, FILED_V2_SESSION_PAGE_BYTES);
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)page_fd);
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, pair[0]);
        lpr_session_checked = -1;
        return lpr_pacha_status_to_errno(reply_fd);
    }
    status = lpr_pacha_syscall4(
        PACHAOS_SYSCALL_IPC_RECV_WAIT,
        (uint64_t)(uint32_t)reply_fd,
        (uint64_t)(uintptr_t)&reply,
        UINT64_MAX,
        0);
    (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)reply_fd);
    const pacha_service_reply_header_t *service_reply =
        (const pacha_service_reply_header_t *)service_page;
    if (status != 0 ||
        reply.word0 != PACHA_SERVICE_REPLY_MAGIC ||
        reply.word3 != request.word3)
    {
        lpr_destroy_standalone_wire_page(service_page_fd, service_page);
        (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP, (uint64_t)(uintptr_t)page, FILED_V2_SESSION_PAGE_BYTES);
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)page_fd);
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, pair[0]);
        lpr_session_checked = -1;
        return status != 0 ? lpr_pacha_status_to_errno(status) : -LPR_LINUX_EIO;
    }
    if (service_reply->magic != PACHA_SERVICE_REPLY_MAGIC ||
        service_reply->service_id != FILED_V2_SERVICE_ID ||
        service_reply->op != FILED_V2_OP_SESSION_OPEN ||
        service_reply->request_id != request.word3 ||
        service_reply->status != 0)
    {
        const int64_t reply_status = service_reply->status < 0 ? service_reply->status : -LPR_LINUX_EIO;
        lpr_destroy_standalone_wire_page(service_page_fd, service_page);
        (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP, (uint64_t)(uintptr_t)page, FILED_V2_SESSION_PAGE_BYTES);
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)page_fd);
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, pair[0]);
        lpr_session_checked = -1;
        return reply_status;
    }
    lpr_destroy_standalone_wire_page(service_page_fd, service_page);

    lpr_session_fd = (int)(uint32_t)pair[0];
    lpr_session_page_fd = (int)(uint32_t)page_fd;
    lpr_session_page = page;
    lpr_session_checked = 1;
    return 0;
}

int64_t lpr_filed_fast_call(uint32_t op, uint64_t word2, uint64_t *out_result)
{
    if (lpr_filed_session_connect() != 0 ||
        lpr_session_fd < 16 ||
        lpr_session_page == 0)
    {
        return -LPR_LINUX_ENOSYS;
    }
    filed_v2_fast_header_t *header = (filed_v2_fast_header_t *)lpr_session_page;
    if (header->magic != FILED_V2_FAST_MAGIC ||
        header->version != FILED_V2_FAST_VERSION ||
        header->request_capacity != FILED_V2_FAST_REQUEST_CAPACITY ||
        header->completion_capacity != FILED_V2_FAST_COMPLETION_CAPACITY ||
        header->payload_offset != FILED_V2_FAST_PAYLOAD_OFFSET ||
        header->generation_offset != FILED_V2_FAST_GENERATION_OFFSET ||
        header->generation_capacity != FILED_V2_FAST_GENERATION_CAPACITY)
    {
        return -LPR_LINUX_EIO;
    }
    if (header->request_tail - header->request_head >= header->request_capacity) {
        return -LPR_LINUX_EAGAIN;
    }

    filed_v2_fast_request_t *requests =
        (filed_v2_fast_request_t *)((uintptr_t)lpr_session_page + sizeof(*header));
    filed_v2_fast_completion_t *completions =
        (filed_v2_fast_completion_t *)((uintptr_t)requests +
            sizeof(*requests) * FILED_V2_FAST_REQUEST_CAPACITY);
    const uint64_t request_id = ++lpr_request_id;
    const uint64_t tail = header->request_tail;
    filed_v2_fast_request_t *fast_request =
        &requests[tail % header->request_capacity];
    lpr_zero_bytes(fast_request, sizeof(*fast_request));
    fast_request->request_id = request_id;
    fast_request->opcode = op;
    fast_request->word2 = word2;
    fast_request->payload_slot = 0;
    fast_request->payload_length = FILED_V2_PAGE_BYTES;
    __sync_synchronize();
    header->request_tail = tail + 1u;

    struct pacha_ipc_msg request;
    struct pacha_ipc_msg reply;
    lpr_zero_bytes(&request, sizeof(request));
    lpr_zero_bytes(&reply, sizeof(reply));
    request.word0 = PACHA_SERVICE_REQUEST_MAGIC;
    request.word1 = FILED_V2_OP_SESSION_DOORBELL;
    request.word2 = ++header->doorbell_seq;
    request.word3 = request_id;
    int64_t status = lpr_pacha_syscall2(
        PACHAOS_SYSCALL_IPC_SEND,
        (uint64_t)(uint32_t)lpr_session_fd,
        (uint64_t)(uintptr_t)&request);
    if (status != 0) {
        return lpr_pacha_status_to_errno(status);
    }
    status = lpr_pacha_syscall4(
        PACHAOS_SYSCALL_IPC_RECV_WAIT,
        (uint64_t)(uint32_t)lpr_session_fd,
        (uint64_t)(uintptr_t)&reply,
        UINT64_MAX,
        0);
    if (status != 0) {
        return lpr_pacha_status_to_errno(status);
    }
    if (reply.word0 != PACHA_SERVICE_REPLY_MAGIC ||
        reply.word1 != 0 ||
        reply.word3 != request_id)
    {
        return -LPR_LINUX_EIO;
    }
    if (header->completion_head == header->completion_tail) {
        return -LPR_LINUX_EIO;
    }
    __sync_synchronize();
    filed_v2_fast_completion_t *completion =
        &completions[header->completion_head % header->completion_capacity];
    if (completion->request_id != request_id) {
        return -LPR_LINUX_EIO;
    }
    header->completion_head++;
    if (completion->status < 0) {
        return completion->status;
    }
    if (out_result != 0) {
        *out_result = completion->result;
    }
    return 0;
}

int lpr_create_wire_page(void **out_page)
{
    if (out_page == 0) {
        return -LPR_LINUX_EINVAL;
    }
    if (lpr_filed_session_connect() == 0 &&
        lpr_session_page_fd >= 16 &&
        lpr_session_page != 0 &&
        !lpr_session_payload_busy)
    {
        void *payload = lpr_session_payload_slot(0);
        if (payload != 0) {
            lpr_session_payload_busy = 1;
            *out_page = payload;
            return lpr_session_page_fd;
        }
    }
    if (lpr_wire_page_fd >= 16 && lpr_wire_page != 0 && !lpr_wire_page_busy) {
        lpr_wire_page_busy = 1;
        *out_page = lpr_wire_page;
        return lpr_wire_page_fd;
    }
    const uint64_t rights =
        PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE;
    const int64_t fd = lpr_pacha_syscall3(
        PACHAOS_SYSCALL_VMO_CREATE,
        FILED_V2_PAGE_BYTES,
        rights,
        0);
    if (fd < 16) {
        return (int)lpr_pacha_status_to_errno(fd);
    }
    const int64_t mapped = lpr_pacha_syscall6(
        PACHAOS_SYSCALL_MMAP,
        (uint64_t)(uint32_t)fd,
        0,
        FILED_V2_PAGE_BYTES,
        PACHAOS_PROT_READ | PACHAOS_PROT_WRITE,
        PACHAOS_MMAP_SHARED,
        0);
    if (mapped < 4096) {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)fd);
        return (int)lpr_pacha_status_to_errno(mapped);
    }
    *out_page = (void *)(uintptr_t)mapped;
    if (!lpr_wire_page_busy && lpr_wire_page_fd < 16 && lpr_wire_page == 0) {
        lpr_wire_page_fd = (int)fd;
        lpr_wire_page = *out_page;
        lpr_wire_page_busy = 1;
    }
    return (int)fd;
}

void lpr_destroy_wire_page(int fd, void *page)
{
    if (fd == lpr_session_page_fd && page == lpr_session_payload_slot(0)) {
        lpr_session_payload_busy = 0;
        return;
    }
    if (fd == lpr_wire_page_fd && page == lpr_wire_page) {
        lpr_wire_page_busy = 0;
        return;
    }
    if (page != 0) {
        (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP, (uint64_t)(uintptr_t)page, FILED_V2_PAGE_BYTES);
    }
    if (fd >= 16) {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)fd);
    }
}

int lpr_create_tty_wire_page(void **out_page)
{
    if (out_page == 0) {
        return -LPR_LINUX_EINVAL;
    }
    if (lpr_tty_wire_page_fd >= 16 && lpr_tty_wire_page != 0 && !lpr_tty_wire_page_busy) {
        lpr_tty_wire_page_busy = 1;
        *out_page = lpr_tty_wire_page;
        return lpr_tty_wire_page_fd;
    }
    const uint64_t rights =
        PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE;
    const int64_t fd = lpr_pacha_syscall3(
        PACHAOS_SYSCALL_VMO_CREATE,
        TERMD_V2_PAGE_BYTES,
        rights,
        0);
    if (fd < 16) {
        return (int)lpr_pacha_status_to_errno(fd);
    }
    const int64_t mapped = lpr_pacha_syscall6(
        PACHAOS_SYSCALL_MMAP,
        (uint64_t)(uint32_t)fd,
        0,
        TERMD_V2_PAGE_BYTES,
        PACHAOS_PROT_READ | PACHAOS_PROT_WRITE,
        PACHAOS_MMAP_SHARED,
        0);
    if (mapped < 4096) {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)fd);
        return (int)lpr_pacha_status_to_errno(mapped);
    }
    *out_page = (void *)(uintptr_t)mapped;
    if (!lpr_tty_wire_page_busy && lpr_tty_wire_page_fd < 16 && lpr_tty_wire_page == 0) {
        lpr_tty_wire_page_fd = (int)fd;
        lpr_tty_wire_page = *out_page;
        lpr_tty_wire_page_busy = 1;
    }
    return (int)fd;
}

void lpr_destroy_tty_wire_page(int fd, void *page)
{
    if (fd == lpr_tty_wire_page_fd && page == lpr_tty_wire_page) {
        lpr_tty_wire_page_busy = 0;
        return;
    }
    if (page != 0) {
        (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP, (uint64_t)(uintptr_t)page, TERMD_V2_PAGE_BYTES);
    }
    if (fd >= 16) {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)fd);
    }
}

void lpr_reset_fork_child_rpc_state(void)
{
    if (lpr_wire_page != 0) {
        (void)lpr_pacha_syscall2(
            PACHAOS_SYSCALL_MUNMAP,
            (uint64_t)(uintptr_t)lpr_wire_page,
            FILED_V2_PAGE_BYTES);
    }
    if (lpr_wire_page_fd >= 16) {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)lpr_wire_page_fd);
    }
    lpr_wire_page_fd = -1;
    lpr_wire_page = 0;
    lpr_wire_page_busy = 0;

    if (lpr_tty_wire_page != 0) {
        (void)lpr_pacha_syscall2(
            PACHAOS_SYSCALL_MUNMAP,
            (uint64_t)(uintptr_t)lpr_tty_wire_page,
            TERMD_V2_PAGE_BYTES);
    }
    if (lpr_tty_wire_page_fd >= 16) {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)lpr_tty_wire_page_fd);
    }
    lpr_tty_wire_page_fd = -1;
    lpr_tty_wire_page = 0;
    lpr_tty_wire_page_busy = 0;

    if (lpr_session_page != 0) {
        (void)lpr_pacha_syscall2(
            PACHAOS_SYSCALL_MUNMAP,
            (uint64_t)(uintptr_t)lpr_session_page,
            FILED_V2_SESSION_PAGE_BYTES);
    }
    if (lpr_session_page_fd >= 16) {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)lpr_session_page_fd);
    }
    if (lpr_session_fd >= 16) {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)lpr_session_fd);
    }
    lpr_session_fd = -1;
    lpr_session_page_fd = -1;
    lpr_session_page = 0;
    lpr_session_checked = 0;
    lpr_session_payload_busy = 0;

    if (lpr_readv_vmo_map != 0) {
        (void)lpr_pacha_syscall2(
            PACHAOS_SYSCALL_MUNMAP,
            (uint64_t)(uintptr_t)lpr_readv_vmo_map,
            lpr_readv_vmo_len);
    }
    if (lpr_readv_vmo_fd >= 16) {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)lpr_readv_vmo_fd);
    }
    lpr_readv_vmo_fd = -1;
    lpr_readv_vmo_map = 0;
    lpr_readv_vmo_len = 0;

    if (lpr_pread_vmo_page != 0) {
        (void)lpr_pacha_syscall2(
            PACHAOS_SYSCALL_MUNMAP,
            (uint64_t)(uintptr_t)lpr_pread_vmo_page,
            FILED_V2_PAGE_BYTES);
    }
    if (lpr_pread_vmo_page_fd >= 16) {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)lpr_pread_vmo_page_fd);
    }
    lpr_pread_vmo_page_fd = -1;
    lpr_pread_vmo_page = 0;
    lpr_pread_vmo_page_busy = 0;
}

int lpr_create_standalone_wire_page(void **out_page)
{
    if (out_page == 0) {
        return -LPR_LINUX_EINVAL;
    }
    *out_page = 0;
    const uint64_t rights =
        PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE;
    const int64_t fd = lpr_pacha_syscall3(
        PACHAOS_SYSCALL_VMO_CREATE,
        FILED_V2_PAGE_BYTES,
        rights,
        0);
    if (fd < 16) {
        return (int)lpr_pacha_status_to_errno(fd);
    }
    const int64_t mapped = lpr_pacha_syscall6(
        PACHAOS_SYSCALL_MMAP,
        (uint64_t)(uint32_t)fd,
        0,
        FILED_V2_PAGE_BYTES,
        PACHAOS_PROT_READ | PACHAOS_PROT_WRITE,
        PACHAOS_MMAP_SHARED,
        0);
    if (mapped < 4096) {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)fd);
        return (int)lpr_pacha_status_to_errno(mapped);
    }
    *out_page = (void *)(uintptr_t)mapped;
    return (int)fd;
}

void lpr_destroy_standalone_wire_page(int fd, void *page)
{
    if (page != 0) {
        (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP, (uint64_t)(uintptr_t)page, FILED_V2_PAGE_BYTES);
    }
    if (fd >= 16) {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)fd);
    }
}

void *lpr_supervisor_payload(void *page)
{
    return lpr_process_client_payload(page);
}

int64_t lpr_supervisor_call(
    uint32_t op,
    int page_fd,
    void *page,
    uint32_t payload_size,
    int transfer_fd,
    uint64_t *out_result)
{
    return lpr_process_client_call(
        &lpr_request_id,
        lpr_pacha_status_to_errno,
        op,
        page_fd,
        page,
        payload_size,
        transfer_fd,
        out_result);
}

int64_t lpr_supervisor_call_token(
    uint32_t op,
    uint64_t token,
    int transfer_fd,
    uint64_t *out_result)
{
    return lpr_process_client_call_token(
        &lpr_request_id,
        lpr_pacha_status_to_errno,
        lpr_create_standalone_wire_page,
        lpr_destroy_standalone_wire_page,
        op,
        token,
        transfer_fd,
        out_result);
}

int64_t lpr_supervisor_kill_pid(int32_t pid, uint32_t sig, uint64_t *out_delivered)
{
    if (!lpr_supervisor_enabled || lpr_supervisor_token == 0 || sig > LPR_LINUX_SIGNAL_MAX) {
        return -LPR_LINUX_EINVAL;
    }
    void *page = 0;
    const int page_fd = lpr_create_standalone_wire_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }
    lpr_memset(page, 0, PACHA_SERVICE_PAGE_BYTES);
    lprs_v2_kill_t *kill_req = (lprs_v2_kill_t *)lpr_supervisor_payload(page);
    kill_req->token = lpr_supervisor_token;
    kill_req->pid = pid;
    kill_req->signal = sig;
    const int64_t status = lpr_supervisor_call(
        LPRS_V2_OP_SIGNAL_KILL,
        page_fd,
        page,
        sizeof(*kill_req),
        -1,
        0);
    if (status == 0 && out_delivered != 0) {
        *out_delivered = kill_req->delivered;
    }
    lpr_destroy_standalone_wire_page(page_fd, page);
    return status;
}

int lpr_supervisor_get_state(lprs_v2_process_state_t *out_state)
{
    if (out_state == 0 || lpr_supervisor_token == 0) {
        return -LPR_LINUX_EINVAL;
    }
    void *page = 0;
    const int page_fd = lpr_create_standalone_wire_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }
    lpr_memset(page, 0, PACHA_SERVICE_PAGE_BYTES);
    lprs_v2_token_request_t *req = (lprs_v2_token_request_t *)lpr_supervisor_payload(page);
    req->token = lpr_supervisor_token;
    const int64_t status = lpr_supervisor_call(
        LPRS_V2_OP_PROCESS_GET_STATE,
        page_fd,
        page,
        sizeof(*req),
        -1,
        0);
    if (status == 0) {
        lpr_memcpy(out_state, lpr_supervisor_payload(page), sizeof(*out_state));
    }
    lpr_destroy_standalone_wire_page(page_fd, page);
    return status == 0 ? 0 : (int)status;
}

int lpr_create_pread_vmo_wire_page(void **out_page)
{
    if (out_page == 0) {
        return -LPR_LINUX_EINVAL;
    }
    if (lpr_pread_vmo_page_fd >= 16 &&
        lpr_pread_vmo_page != 0 &&
        !lpr_pread_vmo_page_busy)
    {
        lpr_pread_vmo_page_busy = 1;
        *out_page = lpr_pread_vmo_page;
        return lpr_pread_vmo_page_fd;
    }
    if (lpr_pread_vmo_page_busy) {
        return lpr_create_standalone_wire_page(out_page);
    }

    void *page = 0;
    const int fd = lpr_create_standalone_wire_page(&page);
    if (fd < 0) {
        return fd;
    }
    lpr_pread_vmo_page_fd = fd;
    lpr_pread_vmo_page = page;
    lpr_pread_vmo_page_busy = 1;
    *out_page = page;
    return fd;
}

void lpr_destroy_pread_vmo_wire_page(int fd, void *page)
{
    if (fd == lpr_pread_vmo_page_fd && page == lpr_pread_vmo_page) {
        lpr_pread_vmo_page_busy = 0;
        return;
    }
    lpr_destroy_standalone_wire_page(fd, page);
}

uint64_t lpr_page_align_up(uint64_t value)
{
    const uint64_t mask = 4095ull;
    if (value > UINT64_MAX - mask) {
        return 0;
    }
    return (value + mask) & ~mask;
}

int64_t lpr_readv_scratch_vmo(uint64_t requested, int *out_fd, unsigned char **out_map, uint64_t *out_len)
{
    if (out_fd == 0 || out_map == 0 || out_len == 0) {
        return -LPR_LINUX_EINVAL;
    }
    *out_fd = -1;
    *out_map = 0;
    *out_len = 0;

    const uint64_t map_len = lpr_page_align_up(requested);
    if (map_len == 0) {
        return -LPR_LINUX_EINVAL;
    }
    if (lpr_readv_vmo_fd >= 16 &&
        lpr_readv_vmo_map != 0 &&
        lpr_readv_vmo_len >= map_len)
    {
        *out_fd = lpr_readv_vmo_fd;
        *out_map = (unsigned char *)lpr_readv_vmo_map;
        *out_len = lpr_readv_vmo_len;
        return 0;
    }

    if (lpr_readv_vmo_map != 0) {
        (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP, (uint64_t)(uintptr_t)lpr_readv_vmo_map, lpr_readv_vmo_len);
        lpr_readv_vmo_map = 0;
        lpr_readv_vmo_len = 0;
    }
    if (lpr_readv_vmo_fd >= 16) {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)lpr_readv_vmo_fd);
        lpr_readv_vmo_fd = -1;
    }

    const uint64_t rights =
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE;
    const int64_t vmo_fd = lpr_pacha_syscall3(
        PACHAOS_SYSCALL_VMO_CREATE,
        map_len,
        rights,
        0);
    if (vmo_fd < 16) {
        return lpr_pacha_status_to_errno(vmo_fd);
    }
    const int64_t mapped = lpr_pacha_syscall6(
        PACHAOS_SYSCALL_MMAP,
        (uint64_t)(uint32_t)vmo_fd,
        0,
        map_len,
        PACHAOS_PROT_READ,
        PACHAOS_MMAP_SHARED,
        0);
    if (mapped < 4096) {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)vmo_fd);
        return lpr_pacha_status_to_errno(mapped);
    }

    lpr_readv_vmo_fd = (int)(uint32_t)vmo_fd;
    lpr_readv_vmo_map = (void *)(uintptr_t)mapped;
    lpr_readv_vmo_len = map_len;
    *out_fd = lpr_readv_vmo_fd;
    *out_map = (unsigned char *)lpr_readv_vmo_map;
    *out_len = lpr_readv_vmo_len;
    return 0;
}

uint64_t lpr_scatter_iov(
    const lpr_linux_iovec_t *iov,
    uint64_t iov_count,
    const unsigned char *src,
    uint64_t length)
{
    uint64_t copied = 0;
    for (uint64_t i = 0; i < iov_count && copied < length; i += 1) {
        if (iov[i].len == 0) {
            continue;
        }
        uint64_t chunk = iov[i].len;
        if (chunk > length - copied) {
            chunk = length - copied;
        }
        lpr_memcpy((void *)(uintptr_t)iov[i].base, src + copied, (size_t)chunk);
        copied += chunk;
    }
    return copied;
}

lpr_filed_page_cache_entry_t *lpr_page_cache_lookup(
    uint64_t handle,
    uint64_t offset,
    uint64_t requested)
{
    if (handle == 0 || requested == 0 || requested > LPR_FILED_PAGE_CACHE_BYTES) {
        return 0;
    }
    const uint64_t page_start = offset & ~(LPR_FILED_PAGE_CACHE_BYTES - 1ull);
    if (offset < page_start || requested > LPR_FILED_PAGE_CACHE_BYTES - (offset - page_start)) {
        return 0;
    }
    for (uint64_t i = 0; i < LPR_FILED_PAGE_CACHE_ENTRIES; i += 1) {
        lpr_filed_page_cache_entry_t *entry = &lpr_page_cache[i];
        if (!entry->active ||
            entry->handle != handle ||
            entry->page_start != page_start)
        {
            continue;
        }
        if (entry->length == 0) {
            return 0;
        }
        if (offset + requested < offset ||
            offset + requested > entry->page_start + entry->length)
        {
            continue;
        }
        entry->clock = ++lpr_page_cache_clock;
        return entry;
    }
    return 0;
}

lpr_filed_page_cache_entry_t *lpr_page_cache_find_marker(uint64_t handle, uint64_t page_start)
{
    if (handle == 0) {
        return 0;
    }
    for (uint64_t i = 0; i < LPR_FILED_PAGE_CACHE_ENTRIES; i += 1) {
        lpr_filed_page_cache_entry_t *entry = &lpr_page_cache[i];
        if (entry->active &&
            entry->handle == handle &&
            entry->page_start == page_start &&
            entry->length == 0)
        {
            entry->clock = ++lpr_page_cache_clock;
            return entry;
        }
    }
    return 0;
}

lpr_filed_page_cache_entry_t *lpr_page_cache_slot(void)
{
    uint64_t slot = 0;
    uint64_t oldest = UINT64_MAX;
    for (uint64_t i = 0; i < LPR_FILED_PAGE_CACHE_ENTRIES; i += 1) {
        if (!lpr_page_cache[i].active) {
            return &lpr_page_cache[i];
        }
        if (lpr_page_cache[i].clock < oldest) {
            oldest = lpr_page_cache[i].clock;
            slot = i;
        }
    }
    return &lpr_page_cache[slot];
}

int64_t lpr_filed_v2_payload_size(uint32_t op, uint32_t *out_payload_size)
{
    if (out_payload_size == 0) {
        return -LPR_LINUX_EINVAL;
    }
    switch (op) {
    case FILED_V2_OP_VFS_OPENAT:
        *out_payload_size = sizeof(filed_v2_path_request_t);
        return 0;
    case FILED_V2_OP_VFS_STAT:
        *out_payload_size = sizeof(filed_v2_statx_t);
        return 0;
    case FILED_V2_OP_VFS_UTIMENS:
        *out_payload_size = sizeof(filed_v2_utimens_t);
        return 0;
    case FILED_V2_OP_VFS_CHMOD:
        *out_payload_size = sizeof(filed_v2_chmod_t);
        return 0;
    case FILED_V2_OP_VFS_PREAD:
    case FILED_V2_OP_VFS_READ:
    case FILED_V2_OP_VFS_PWRITE:
    case FILED_V2_OP_VFS_WRITE:
        *out_payload_size = sizeof(filed_v2_io_t);
        return 0;
    case FILED_V2_OP_VFS_GETDENTS:
        *out_payload_size = sizeof(filed_v2_getdents_t);
        return 0;
    case FILED_V2_OP_VFS_DUP:
    case FILED_V2_OP_VFS_GET_FLAGS:
    case FILED_V2_OP_VFS_SET_FLAGS:
        *out_payload_size = sizeof(filed_v2_handle_flags_t);
        return 0;
    case FILED_V2_OP_VFS_FSYNC:
        *out_payload_size = sizeof(filed_v2_handle_request_t);
        return 0;
    case FILED_V2_OP_VFS_SYNC_ALL:
        *out_payload_size = 0;
        return 0;
    case FILED_V2_OP_VFS_SEEK:
        *out_payload_size = sizeof(filed_v2_seek_t);
        return 0;
    case FILED_V2_OP_VFS_CLOSE:
        *out_payload_size = sizeof(filed_v2_handle_request_t);
        return 0;
    case FILED_V2_OP_VFS_TRUNCATE:
        *out_payload_size = sizeof(filed_v2_truncate_t);
        return 0;
    case FILED_V2_OP_VFS_UNLINK:
        *out_payload_size = sizeof(filed_v2_unlink_t);
        return 0;
    case FILED_V2_OP_VFS_RENAME:
        *out_payload_size = sizeof(filed_v2_rename_t);
        return 0;
    case FILED_V2_OP_VFS_MKDIR:
        *out_payload_size = sizeof(filed_v2_mkdir_t);
        return 0;
    case FILED_V2_OP_VFS_RMDIR:
        *out_payload_size = sizeof(filed_v2_rmdir_t);
        return 0;
    case FILED_V2_OP_VFS_SYMLINK:
        *out_payload_size = sizeof(filed_v2_symlink_t);
        return 0;
    case FILED_V2_OP_VFS_READLINK:
        *out_payload_size = sizeof(filed_v2_readlink_t);
        return 0;
    case FILED_V2_OP_VFS_LINK:
        *out_payload_size = sizeof(filed_v2_link_t);
        return 0;
    default:
        return -LPR_LINUX_ENOSYS;
    }
}

int64_t lpr_filed_call(uint32_t op, int page_fd, uint64_t word2, uint64_t *out_result)
{
    if (page_fd == lpr_session_page_fd && lpr_session_page != 0) {
        return lpr_filed_fast_call(op, word2, out_result);
    } else if (page_fd < 16 &&
        lpr_session_page != 0 &&
        !lpr_session_payload_busy)
    {
        const int64_t fast_status = lpr_filed_fast_call(op, word2, out_result);
        if (fast_status != -LPR_LINUX_ENOSYS && fast_status != -LPR_LINUX_EAGAIN) {
            return fast_status;
        }
    }

    const int64_t ready = lpr_filed_endpoint_ready();
    if (ready != 0) {
        return ready;
    }

    uint32_t payload_size = 0;
    int64_t status = lpr_filed_v2_payload_size(op, &payload_size);
    if (status != 0) {
        return status;
    }

    int owned_page_fd = -1;
    void *owned_page = 0;
    if (page_fd < 16) {
        owned_page_fd = lpr_create_standalone_wire_page(&owned_page);
        if (owned_page_fd < 16) {
            return owned_page_fd;
        }
        page_fd = owned_page_fd;
    }

    void *page = owned_page;
    if (page == 0) {
        const int64_t mapped = lpr_pacha_syscall6(
            PACHAOS_SYSCALL_MMAP,
            (uint64_t)(uint32_t)page_fd,
            0,
            FILED_V2_PAGE_BYTES,
            PACHAOS_PROT_READ | PACHAOS_PROT_WRITE,
            PACHAOS_MMAP_SHARED,
            0);
        if (mapped < 4096) {
            if (owned_page_fd >= 16) {
                lpr_destroy_standalone_wire_page(owned_page_fd, owned_page);
            }
            return lpr_pacha_status_to_errno(mapped);
        }
        page = (void *)(uintptr_t)mapped;
    }

    if (op == FILED_V2_OP_VFS_OPENAT) {
        filed_v2_openat_t openat_payload;
        lpr_memcpy(&openat_payload, page, sizeof(openat_payload));
        lpr_memset(page, 0, FILED_V2_PAGE_BYTES);
        filed_v2_path_request_t *path =
            (filed_v2_path_request_t *)((uint8_t *)page + PACHA_SERVICE_HEADER_BYTES);
        path->dir_handle = openat_payload.dir_handle;
        path->rights = openat_payload.rights;
        path->flags = openat_payload.open_flags;
        path->mode = 0;
        size_t path_len = lpr_strnlen(openat_payload.name, sizeof(openat_payload.name));
        if (path_len >= sizeof(path->path)) {
            path_len = sizeof(path->path) - 1u;
        }
        lpr_memcpy(path->path, openat_payload.name, path_len);
        path->path[path_len] = '\0';
    } else if (op == FILED_V2_OP_VFS_CLOSE || op == FILED_V2_OP_VFS_FSYNC) {
        lpr_memset(page, 0, FILED_V2_PAGE_BYTES);
        filed_v2_handle_request_t *handle =
            (filed_v2_handle_request_t *)((uint8_t *)page + PACHA_SERVICE_HEADER_BYTES);
        handle->handle = word2;
    } else {
        lpr_memmove((uint8_t *)page + PACHA_SERVICE_HEADER_BYTES, page, payload_size);
        lpr_memset(page, 0, PACHA_SERVICE_HEADER_BYTES);
    }

    pacha_service_request_header_t *header = (pacha_service_request_header_t *)page;
    header->magic = PACHA_SERVICE_REQUEST_MAGIC;
    header->abi_version = PACHA_SERVICE_ABI_VERSION;
    header->service_id = FILED_V2_SERVICE_ID;
    header->op = op;
    header->flags = PACHA_SERVICE_FLAG_PAGE_PAYLOAD;
    header->request_id = ++lpr_request_id;
    header->trace_id = header->request_id;
    header->payload_size = payload_size;

    struct pacha_ipc_fd fd_item;
    lpr_memset(&fd_item, 0, sizeof(fd_item));
    fd_item.fd = (uint64_t)(uint32_t)page_fd;
    fd_item.rights =
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE;

    const uint64_t request_id = header->request_id;
    const struct pacha_ipc_msg request = {
        .word0 = PACHA_SERVICE_REQUEST_MAGIC,
        .word1 = 0,
        .word2 = 0,
        .word3 = request_id,
        .fds = &fd_item,
        .fd_count = 1u,
    };
    const int64_t reply_fd = lpr_pacha_syscall2(
        PACHAOS_SYSCALL_IPC_CALL,
        LPR_FILED_ENDPOINT_FD,
        (uint64_t)(uintptr_t)&request);
    if (reply_fd < 16) {
        const int64_t err = lpr_pacha_status_to_errno(reply_fd);
        lpr_trace_error_record(
            LPR_ERROR_DOMAIN_KERNEL,
            op,
            LPR_ERROR_STAGE_CHILD_RPC_CALL,
            err,
            reply_fd,
            request_id,
            page_fd >= 16 ? 1u : 0u,
            LPR_FILED_ENDPOINT_FD,
            0,
            "filed ipc_call failed");
        if (page != owned_page) {
            (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP, (uint64_t)(uintptr_t)page, FILED_V2_PAGE_BYTES);
        }
        if (owned_page_fd >= 16) {
            lpr_destroy_standalone_wire_page(owned_page_fd, owned_page);
        }
        return err;
    }

    struct pacha_ipc_msg reply;
    lpr_memset(&reply, 0, sizeof(reply));
    const int64_t recv_status = lpr_pacha_syscall4(
        PACHAOS_SYSCALL_IPC_RECV_WAIT,
        (uint64_t)(uint32_t)reply_fd,
        (uint64_t)(uintptr_t)&reply,
        UINT64_MAX,
        0);
    (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)reply_fd);
    if (recv_status != 0) {
        const int64_t err = lpr_pacha_status_to_errno(recv_status);
        lpr_trace_error_record(
            LPR_ERROR_DOMAIN_KERNEL,
            op,
            LPR_ERROR_STAGE_CHILD_RPC_RECV,
            err,
            recv_status,
            request_id,
            page_fd >= 16 ? 1u : 0u,
            (uint64_t)(uint32_t)reply_fd,
            0,
            "filed reply recv failed");
        if (page != owned_page) {
            (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP, (uint64_t)(uintptr_t)page, FILED_V2_PAGE_BYTES);
        }
        if (owned_page_fd >= 16) {
            lpr_destroy_standalone_wire_page(owned_page_fd, owned_page);
        }
        return err;
    }
    const pacha_service_reply_header_t *reply_header = (const pacha_service_reply_header_t *)page;
    if (reply.word0 != PACHA_SERVICE_REPLY_MAGIC ||
        reply.word3 != request_id ||
        reply_header->magic != PACHA_SERVICE_REPLY_MAGIC ||
        reply_header->service_id != FILED_V2_SERVICE_ID ||
        reply_header->op != op ||
        reply_header->request_id != request_id)
    {
        lpr_trace_error_record(
            LPR_ERROR_DOMAIN_FILED,
            op,
            LPR_ERROR_STAGE_REPLY_MAGIC,
            -LPR_LINUX_EIO,
            (int64_t)reply.word0,
            request_id,
            page_fd >= 16 ? 1u : 0u,
            reply.word3,
            reply.word2,
            "filed reply mismatch");
        if (page != owned_page) {
            (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP, (uint64_t)(uintptr_t)page, FILED_V2_PAGE_BYTES);
        }
        if (owned_page_fd >= 16) {
            lpr_destroy_standalone_wire_page(owned_page_fd, owned_page);
        }
        return -LPR_LINUX_EIO;
    }
    if (reply_header->status < 0) {
        lpr_trace_error_record(
            LPR_ERROR_DOMAIN_FILED,
            op,
            LPR_ERROR_STAGE_CHILD_STATUS,
            reply_header->status,
            reply_header->status,
            request_id,
            1u,
            0,
            reply_header->result,
            "filed returned error");
        status = reply_header->status;
        if (page != owned_page) {
            (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP, (uint64_t)(uintptr_t)page, FILED_V2_PAGE_BYTES);
        }
        if (owned_page_fd >= 16) {
            lpr_destroy_standalone_wire_page(owned_page_fd, owned_page);
        }
        return status;
    }
    if (op != FILED_V2_OP_VFS_OPENAT &&
        op != FILED_V2_OP_VFS_CLOSE &&
        op != FILED_V2_OP_VFS_FSYNC &&
        op != FILED_V2_OP_VFS_SYNC_ALL)
    {
        lpr_memmove(page, (uint8_t *)page + PACHA_SERVICE_HEADER_BYTES, payload_size);
    }
    if (out_result != 0) {
        *out_result = reply_header->result;
    }
    if (page != owned_page) {
        (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP, (uint64_t)(uintptr_t)page, FILED_V2_PAGE_BYTES);
    }
    if (owned_page_fd >= 16) {
        lpr_destroy_standalone_wire_page(owned_page_fd, owned_page);
    }
    return 0;
}
