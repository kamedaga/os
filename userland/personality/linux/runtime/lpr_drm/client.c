#include "../lpr_filed_internal.h"

enum {
    LPR_DRM_IOCTL_VERSION = 0xc0406400u,
    LPR_UDMABUF_CREATE = 0x40187542u,
};

typedef struct lpr_udmabuf_create {
    uint32_t memfd;
    uint32_t flags;
    uint64_t offset;
    uint64_t size;
} lpr_udmabuf_create_t;

typedef struct lpr_dma_buf_sync_file {
    uint32_t flags;
    int32_t fd;
} lpr_dma_buf_sync_file_t;

_Static_assert(sizeof(lpr_dma_buf_sync_file_t) == 8,
    "Linux dma-buf sync-file ioctl layout");

typedef struct lpr_drm_version {
    int32_t major;
    int32_t minor;
    int32_t patchlevel;
    uint32_t reserved0;
    uint64_t name_len;
    uint64_t name;
    uint64_t date_len;
    uint64_t date;
    uint64_t desc_len;
    uint64_t desc;
} lpr_drm_version_t;

static void *lpr_drmd_payload(void *page)
{
    return page == 0 ? 0 : (uint8_t *)page + PACHA_SERVICE_HEADER_BYTES;
}

static int64_t lpr_drmd_call_transfer(
    uint32_t op,
    int page_fd,
    void *page,
    uint32_t payload_size,
    uint64_t *out_result,
    int *out_received_fd,
    int transfer_fd)
{
    if (LPR_DRMD_DRM_ENDPOINT_FD < 16 || page_fd < 16 || page == 0) {
        return -LPR_LINUX_ENODEV;
    }
    struct pacha_ipc_fd fds[2];
    struct pacha_ipc_msg request;
    struct pacha_ipc_msg reply;
    struct pacha_ipc_fd reply_fd_item;
    lpr_memset(fds, 0, sizeof(fds));
    lpr_memset(&request, 0, sizeof(request));
    lpr_memset(&reply, 0, sizeof(reply));
    lpr_memset(&reply_fd_item, 0, sizeof(reply_fd_item));
    if (out_received_fd != 0) {
        *out_received_fd = -1;
        reply.fds = &reply_fd_item;
        reply.fd_capacity = 1;
    }
    const uint64_t request_id = lpr_next_request_id(&lpr_termd_request_id);
    pacha_service_envelope_t *header = (pacha_service_envelope_t *)page;
    lpr_memset(header, 0, sizeof(*header));
    header->magic = PACHA_SERVICE_REQUEST_MAGIC;
    header->abi_version = PACHA_SERVICE_ABI_VERSION;
    header->service_id = DRMD_SERVICE_ID;
    header->op = op;
    header->flags = payload_size != 0 ? PACHA_SERVICE_FLAG_PAGE_PAYLOAD : 0;
    header->request_id = request_id;
    header->trace_id = request_id;
    header->payload_size = payload_size;
    fds[0].fd = (uint64_t)(uint32_t)page_fd;
    fds[0].rights = PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE;
    uint64_t fd_count = 1;
    if (transfer_fd >= 16) {
        struct pacha_fd_info info;
        if (!lpr_native_fd_info((uint64_t)(uint32_t)transfer_fd, &info) ||
            (info.kind != PACHA_FD_KIND_VMO && info.kind != PACHA_FD_KIND_CHANNEL) ||
            (info.rights & (PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_DUP |
                PACHA_FD_RIGHT_SET_FLAGS)) !=
                (PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_DUP | PACHA_FD_RIGHT_SET_FLAGS)) {
            return -LPR_LINUX_EBADF;
        }
        fds[1].fd = (uint64_t)(uint32_t)transfer_fd;
        fds[1].rights = info.kind == PACHA_FD_KIND_VMO ?
            PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_DUP |
                PACHA_FD_RIGHT_SET_FLAGS | PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE :
            PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_CLOSE |
                PACHA_FD_RIGHT_SEND | PACHA_FD_RIGHT_RECV | PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL;
        fds[1].transfer_flags = info.kind == PACHA_FD_KIND_CHANNEL ? PACHA_IPC_TRANSFER_MOVE : 0;
        fd_count = 2;
    }
    request.word0 = PACHA_SERVICE_REQUEST_MAGIC;
    request.word3 = request_id;
    request.fds = fds;
    request.fd_count = fd_count;
    const int64_t reply_fd = lpr_pacha_syscall2(
        PACHAOS_SYSCALL_IPC_CALL,
        LPR_DRMD_DRM_ENDPOINT_FD,
        (uint64_t)(uintptr_t)&request);
    if (reply_fd < 16) {
        return lpr_pacha_status_to_errno(reply_fd);
    }
    const int64_t recv_status = lpr_native_ipc_recv_wait(
        (uint64_t)(uint32_t)reply_fd,
        &reply);
    (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)reply_fd);
    if (recv_status != 0) {
        return lpr_pacha_status_to_errno(recv_status);
    }
    const pacha_service_envelope_t *reply_header = (const pacha_service_envelope_t *)page;
    if (reply.word0 != PACHA_SERVICE_REPLY_MAGIC || reply.word3 != request_id ||
        reply_header->magic != PACHA_SERVICE_REPLY_MAGIC ||
        reply_header->service_id != DRMD_SERVICE_ID ||
        reply_header->op != op || reply_header->request_id != request_id) {
        return -LPR_LINUX_EIO;
    }
    if (out_result != 0) {
        *out_result = reply_header->result;
    }
    if (out_received_fd != 0) {
        if (reply_header->status == 0 && reply.fd_count == 1 && reply_fd_item.fd >= 16) {
            *out_received_fd = (int)(uint32_t)reply_fd_item.fd;
        } else if (reply.fd_count != 0 && reply_fd_item.fd >= 16) {
            (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, reply_fd_item.fd);
        }
    }
    return reply_header->status;
}

static int64_t lpr_drmd_call(
    uint32_t op,
    int page_fd,
    void *page,
    uint32_t payload_size,
    uint64_t *out_result,
    int *out_received_fd)
{
    return lpr_drmd_call_transfer(
        op, page_fd, page, payload_size, out_result, out_received_fd, -1);
}

int lpr_drm_fd_alloc(uint64_t handle, uint64_t flags, int native_wait_fd)
{
    const int fd = lpr_fd_slot_alloc();
    if (fd < 0) {
        return fd;
    }
    const int status = lpr_control_install_fd(fd, LPR_FD_OPS_DRM, flags, handle, 0);
    if (status != 0) {
        return status;
    }
    lpr_drm_backend_t *drm = lpr_drm_backend(fd);
    if (drm == 0) {
        lpr_control_close_fd(fd);
        return -LPR_LINUX_EIO;
    }
    drm->active = 1;
    drm->flags = (uint32_t)flags;
    drm->handle = handle;
    drm->wait_fd.raw = native_wait_fd;
    return fd;
}

int64_t lpr_drm_open_path(const char *path, uint64_t flags)
{
    const int udmabuf = path != 0 && lpr_strcmp(path, "/dev/udmabuf") == 0;
    if (path == 0 || (!udmabuf && lpr_strcmp(path, "/dev/dri/card0") != 0)) {
        return -LPR_LINUX_ENOENT;
    }
    void *page = 0;
    const int page_fd = lpr_create_tty_wire_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }
    drmd_open_request_t *open = (drmd_open_request_t *)lpr_drmd_payload(page);
    lpr_memset(open, 0, sizeof(*open));
    open->card_index = 0;
    open->flags = flags;
    int native_wait_fd = -1;
    int remote_wait_fd = -1;
    const int pair_status = lpr_native_wait_pair(&native_wait_fd, &remote_wait_fd);
    if (pair_status != 0) {
        lpr_destroy_tty_wire_page(page_fd, page);
        return pair_status;
    }
    uint64_t handle = 0;
    const int64_t status = lpr_drmd_call_transfer(
        DRMD_OP_OPEN_CARD, page_fd, page, sizeof(*open), &handle, 0, remote_wait_fd);
    lpr_destroy_tty_wire_page(page_fd, page);
    if (status != 0) {
        (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)native_wait_fd);
        (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)remote_wait_fd);
        return status;
    }
    const int fd = lpr_drm_fd_alloc(handle, flags, native_wait_fd);
    if (fd < 0) {
        (void)lpr_drm_close_handle(handle);
        (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)native_wait_fd);
    } else if (udmabuf) {
        lpr_drm_backend_t *drm = lpr_drm_backend((uint64_t)(uint32_t)fd);
        if (drm != 0) drm->reserved0 = 1;
    }
    return fd;
}

int64_t lpr_drm_close_handle(uint64_t handle)
{
    void *page = 0;
    const int page_fd = lpr_create_tty_wire_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }
    drmd_handle_request_t *close = (drmd_handle_request_t *)lpr_drmd_payload(page);
    lpr_memset(close, 0, sizeof(*close));
    close->handle = handle;
    const int64_t status = lpr_drmd_call(DRMD_OP_HANDLE_CLOSE, page_fd, page, sizeof(*close), 0, 0);
    lpr_destroy_tty_wire_page(page_fd, page);
    return status;
}

int64_t lpr_drm_dup_handle(uint64_t handle)
{
    void *page = 0;
    const int page_fd = lpr_create_tty_wire_page(&page);
    if (page_fd < 0) return page_fd;
    drmd_handle_request_t *dup = (drmd_handle_request_t *)lpr_drmd_payload(page);
    lpr_memset(dup, 0, sizeof(*dup));
    dup->handle = handle;
    uint64_t result = 0;
    const int64_t status = lpr_drmd_call(DRMD_OP_HANDLE_DUP, page_fd, page, sizeof(*dup), &result, 0);
    lpr_destroy_tty_wire_page(page_fd, page);
    return status == 0 && result == handle ? 0 : (status != 0 ? status : -LPR_LINUX_EIO);
}

int64_t lpr_drm_transfer_dup_handle(
    uint64_t handle,
    int lease_fd,
    uint64_t *out_handle)
{
    if (out_handle == 0 || lease_fd < 16) return -LPR_LINUX_EINVAL;
    *out_handle = 0;
    void *page = 0;
    const int page_fd = lpr_create_tty_wire_page(&page);
    if (page_fd < 0) return page_fd;
    drmd_handle_request_t *dup = (drmd_handle_request_t *)lpr_drmd_payload(page);
    lpr_memset(dup, 0, sizeof(*dup));
    dup->handle = handle;
    const int64_t status = lpr_drmd_call_transfer(
        DRMD_OP_HANDLE_DUP,
        page_fd,
        page,
        sizeof(*dup),
        out_handle,
        0,
        lease_fd);
    lpr_destroy_tty_wire_page(page_fd, page);
    return status == 0 && *out_handle == handle ? 0 :
        (status != 0 ? status : -LPR_LINUX_EIO);
}

int64_t lpr_drm_prime_ref(uint32_t op, uint64_t token)
{
    void *page = 0;
    const int page_fd = lpr_create_tty_wire_page(&page);
    if (page_fd < 0) return page_fd;
    drmd_prime_token_request_t *request = (drmd_prime_token_request_t *)lpr_drmd_payload(page);
    lpr_memset(request, 0, sizeof(*request));
    request->token = token;
    const int64_t status = lpr_drmd_call(op, page_fd, page, sizeof(*request), 0, 0);
    lpr_destroy_tty_wire_page(page_fd, page);
    return status;
}

int64_t lpr_drm_prime_transfer_acquire(uint64_t token, int lease_fd)
{
    if (token == 0 || lease_fd < 16) return -LPR_LINUX_EINVAL;
    void *page = 0;
    const int page_fd = lpr_create_tty_wire_page(&page);
    if (page_fd < 0) return page_fd;
    drmd_prime_token_request_t *request =
        (drmd_prime_token_request_t *)lpr_drmd_payload(page);
    lpr_memset(request, 0, sizeof(*request));
    request->token = token;
    const int64_t status = lpr_drmd_call_transfer(
        DRMD_OP_PRIME_ACQUIRE,
        page_fd,
        page,
        sizeof(*request),
        0,
        0,
        lease_fd);
    lpr_destroy_tty_wire_page(page_fd, page);
    return status;
}

static int lpr_dma_buf_sync_flags_valid(uint32_t flags)
{
    return (flags & LPR_LINUX_DMA_BUF_SYNC_RW) != 0 &&
        (flags & ~LPR_LINUX_DMA_BUF_SYNC_RW) == 0;
}

int64_t lpr_dmabuf_ioctl(uint64_t fd, uint64_t request, uint64_t arg)
{
    lpr_dmabuf_backend_t *dmabuf = lpr_dmabuf_backend(fd);
    if (dmabuf == 0) return -LPR_LINUX_EBADF;
    if (arg == 0) return -LPR_LINUX_EFAULT;
    lpr_dma_buf_sync_file_t *sync =
        (lpr_dma_buf_sync_file_t *)(uintptr_t)arg;
    if (!lpr_dma_buf_sync_flags_valid(sync->flags))
        return -LPR_LINUX_EINVAL;

    if (request == LPR_LINUX_DMA_BUF_IOCTL_EXPORT_SYNC_FILE) {
        const int64_t sync_fd = lpr_sync_file_create_signaled();
        if (sync_fd < 0) return sync_fd;
        sync->fd = (int32_t)sync_fd;
        return 0;
    }
    if (request != LPR_LINUX_DMA_BUF_IOCTL_IMPORT_SYNC_FILE)
        return -LPR_LINUX_ENOTTY;
    if (sync->fd < 0 ||
        !lpr_linux_sync_file_fd_active((uint64_t)(uint32_t)sync->fd))
        return -LPR_LINUX_EBADF;

    const int wait_fd = lpr_sync_file_duplicate_wait(
        (uint64_t)(uint32_t)sync->fd);
    if (wait_fd < 0) return wait_fd;
    void *page = 0;
    const int page_fd = lpr_create_tty_wire_page(&page);
    if (page_fd < 0) {
        (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)wait_fd);
        return page_fd;
    }
    drmd_prime_token_request_t *import =
        (drmd_prime_token_request_t *)lpr_drmd_payload(page);
    lpr_memset(import, 0, sizeof(*import));
    import->token = dmabuf->token;
    const int64_t status = lpr_drmd_call_transfer(
        DRMD_OP_PRIME_IMPORT_SYNC_FILE,
        page_fd,
        page,
        sizeof(*import),
        0,
        0,
        wait_fd);
    lpr_destroy_tty_wire_page(page_fd, page);
    (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)wait_fd);
    return status;
}

static int64_t lpr_drm_prime_export(uint64_t drm_handle, uint32_t gem_handle, uint32_t flags)
{
    void *page = 0;
    const int page_fd = lpr_create_tty_wire_page(&page);
    if (page_fd < 0) return page_fd;
    drmd_prime_export_request_t *request = (drmd_prime_export_request_t *)lpr_drmd_payload(page);
    lpr_memset(request, 0, sizeof(*request));
    request->handle = drm_handle;
    request->gem_handle = gem_handle;
    request->flags = flags;
    uint64_t token = 0;
    int native_fd = -1;
    int64_t status = lpr_drmd_call(
        DRMD_OP_PRIME_EXPORT, page_fd, page, sizeof(*request), &token, &native_fd);
    lpr_destroy_tty_wire_page(page_fd, page);
    if (status != 0) return status;
    struct pacha_fd_info info;
    if (native_fd < 0 || token == 0 || !lpr_native_fd_info((uint64_t)(uint32_t)native_fd, &info) ||
        info.kind != PACHA_FD_KIND_VMO ||
        (info.rights & (PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE)) !=
            (PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE)) {
        if (native_fd >= 0) (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)native_fd);
        (void)lpr_drm_prime_ref(DRMD_OP_PRIME_RELEASE, token);
        return -LPR_LINUX_EIO;
    }
    const int linux_fd = lpr_fd_slot_alloc();
    if (linux_fd < 0) {
        (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)native_fd);
        (void)lpr_drm_prime_ref(DRMD_OP_PRIME_RELEASE, token);
        return linux_fd;
    }
    const uint64_t linux_flags = LPR_LINUX_O_RDWR |
        ((flags & DRMD_CLOEXEC) != 0 ? LPR_LINUX_O_CLOEXEC : 0);
    status = lpr_control_install_fd(
        (uint64_t)(uint32_t)linux_fd,
        LPR_FD_OPS_DMABUF,
        linux_flags,
        token,
        info.size);
    if (status != 0) {
        (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)native_fd);
        (void)lpr_drm_prime_ref(DRMD_OP_PRIME_RELEASE, token);
        return status;
    }
    lpr_dmabuf_backend_t *dmabuf = lpr_dmabuf_backend((uint64_t)(uint32_t)linux_fd);
    if (dmabuf == 0) {
        lpr_control_close_fd((uint64_t)(uint32_t)linux_fd);
        (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)native_fd);
        return -LPR_LINUX_EIO;
    }
    dmabuf->active = 1;
    dmabuf->writable = 1;
    dmabuf->flags = (uint32_t)linux_flags;
    dmabuf->token = token;
    dmabuf->size = info.size;
    dmabuf->native.raw = native_fd;
    return linux_fd;
}

static int64_t lpr_drm_prime_import(uint64_t drm_handle, uint64_t prime_fd, uint32_t flags)
{
    lpr_dmabuf_backend_t *dmabuf = lpr_dmabuf_backend(prime_fd);
    if (flags != 0) return -LPR_LINUX_EINVAL;
    int import_vmo_fd = -1;
    uint64_t import_size = 0;
    if (dmabuf == 0) {
        if (!lpr_linux_filed_fd_active(prime_fd)) return -LPR_LINUX_EBADF;
        lpr_linux_stat_t st;
        lpr_memset(&st, 0, sizeof(st));
        const int64_t stat_status = lpr_linux_fstat(
            prime_fd, (uint64_t)(uintptr_t)&st);
        if (stat_status != 0) return stat_status;
        if (st.st_size <= 0 || (uint64_t)st.st_size > 256u * 1024u * 1024u) {
            return -LPR_LINUX_EINVAL;
        }
        import_size = ((uint64_t)st.st_size + 4095u) & ~4095ull;
        uint64_t file_size = 0;
        const int64_t shared_vmo_fd = lpr_linux_shared_file_vmo(
            prime_fd, 0, import_size, 1, 0, &file_size);
        if (shared_vmo_fd < 16) return shared_vmo_fd;
        if (file_size < (uint64_t)st.st_size) {
            (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)shared_vmo_fd);
            return -LPR_LINUX_EIO;
        }
        import_vmo_fd = (int)(uint32_t)shared_vmo_fd;
    }
    void *page = 0;
    const int page_fd = lpr_create_tty_wire_page(&page);
    if (page_fd < 0) {
        if (import_vmo_fd >= 16) (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)import_vmo_fd);
        return page_fd;
    }
    drmd_prime_import_request_t *request = (drmd_prime_import_request_t *)lpr_drmd_payload(page);
    lpr_memset(request, 0, sizeof(*request));
    request->handle = drm_handle;
    request->token = dmabuf != 0 ? dmabuf->token : 0;
    request->size = import_size;
    uint64_t gem_handle = 0;
    const int64_t status = lpr_drmd_call_transfer(
        DRMD_OP_PRIME_IMPORT, page_fd, page, sizeof(*request), &gem_handle, 0, import_vmo_fd);
    lpr_destroy_tty_wire_page(page_fd, page);
    if (import_vmo_fd >= 16) {
        (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)import_vmo_fd);
    }
    return status == 0 && gem_handle <= UINT32_MAX ? (int64_t)gem_handle :
        (status != 0 ? status : -LPR_LINUX_EIO);
}

static int64_t lpr_drm_close_gem(uint64_t drm_handle, uint32_t gem_handle)
{
    void *page = 0;
    const int page_fd = lpr_create_tty_wire_page(&page);
    if (page_fd < 0) return page_fd;
    drmd_ioctl_request_t *ioctl = (drmd_ioctl_request_t *)lpr_drmd_payload(page);
    lpr_memset(ioctl, 0, sizeof(*ioctl));
    ioctl->handle = drm_handle;
    ioctl->request = DRMD_IOCTL_GEM_CLOSE;
    ioctl->arg_size = sizeof(drmd_gem_close_t);
    ioctl->data_size = sizeof(drmd_gem_close_t);
    ((drmd_gem_close_t *)ioctl->data)->handle = gem_handle;
    const int64_t status = lpr_drmd_call(
        DRMD_OP_HANDLE_IOCTL, page_fd, page, sizeof(*ioctl), 0, 0);
    lpr_destroy_tty_wire_page(page_fd, page);
    return status;
}

static int64_t lpr_udmabuf_create(uint64_t drm_handle, uint64_t arg)
{
    if (arg == 0) return -LPR_LINUX_EFAULT;
    const lpr_udmabuf_create_t *create = (const void *)(uintptr_t)arg;
    if ((create->flags & ~1u) != 0 || create->offset != 0 || create->size == 0 ||
        (create->size & 4095u) != 0 || !lpr_linux_filed_fd_active(create->memfd)) {
        return -LPR_LINUX_EINVAL;
    }
    lpr_linux_stat_t st;
    lpr_memset(&st, 0, sizeof(st));
    const int64_t stat_status = lpr_linux_fstat(
        create->memfd, (uint64_t)(uintptr_t)&st);
    if (stat_status != 0) return stat_status;
    if (st.st_size <= 0 || create->size > (uint64_t)st.st_size) return -LPR_LINUX_EINVAL;
    const int64_t gem_handle = lpr_drm_prime_import(drm_handle, create->memfd, 0);
    if (gem_handle < 0) return gem_handle;
    const int64_t dmabuf_fd = lpr_drm_prime_export(
        drm_handle,
        (uint32_t)gem_handle,
        DRMD_RDWR | ((create->flags & 1u) != 0 ? DRMD_CLOEXEC : 0));
    const int64_t close_status = lpr_drm_close_gem(drm_handle, (uint32_t)gem_handle);
    if (dmabuf_fd < 0) return dmabuf_fd;
    if (close_status != 0) {
        (void)lpr_linux_close((uint64_t)dmabuf_fd);
        return close_status;
    }
    return dmabuf_fd;
}

int64_t lpr_drm_ioctl(uint64_t fd, uint64_t request, uint64_t arg)
{
    lpr_drm_backend_t *drm = lpr_drm_backend(fd);
    const uint32_t command = (uint32_t)request;
    const int no_argument = command == DRMD_IOCTL_SET_MASTER || command == DRMD_IOCTL_DROP_MASTER;
    if (drm == 0 || (arg == 0 && !no_argument)) {
        return drm == 0 ? -LPR_LINUX_EBADF : -LPR_LINUX_EFAULT;
    }
    if (drm->reserved0 != 0) {
        return command == LPR_UDMABUF_CREATE ?
            lpr_udmabuf_create(drm->handle, arg) : -LPR_LINUX_ENOTTY;
    }
    if (command == DRMD_IOCTL_PRIME_HANDLE_TO_FD) {
        drmd_prime_handle_t *prime = (drmd_prime_handle_t *)(uintptr_t)arg;
        const int64_t prime_fd = lpr_drm_prime_export(drm->handle, prime->handle, prime->flags);
        if (prime_fd >= 0) prime->fd = (int32_t)prime_fd;
        return prime_fd >= 0 ? 0 : prime_fd;
    }
    if (command == DRMD_IOCTL_PRIME_FD_TO_HANDLE) {
        drmd_prime_handle_t *prime = (drmd_prime_handle_t *)(uintptr_t)arg;
        if (prime->fd < 0) return -LPR_LINUX_EBADF;
        const int64_t gem_handle = lpr_drm_prime_import(
            drm->handle, (uint64_t)(uint32_t)prime->fd, prime->flags);
        if (gem_handle >= 0) prime->handle = (uint32_t)gem_handle;
        return gem_handle >= 0 ? 0 : gem_handle;
    }
    void *page = 0;
    const int page_fd = lpr_create_tty_wire_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }
    drmd_ioctl_request_t *ioctl = (drmd_ioctl_request_t *)lpr_drmd_payload(page);
    lpr_memset(ioctl, 0, sizeof(*ioctl));
    ioctl->handle = drm->handle;
    ioctl->request = request;
    enum {
        LPR_DRM_WIRE_GENERIC,
        LPR_DRM_WIRE_VERSION,
        LPR_DRM_WIRE_RESOURCES,
        LPR_DRM_WIRE_CONNECTOR,
        LPR_DRM_WIRE_CRTC,
        LPR_DRM_WIRE_PLANE_RES,
        LPR_DRM_WIRE_PLANE,
        LPR_DRM_WIRE_OBJECT_PROPERTIES,
        LPR_DRM_WIRE_PROPERTY,
        LPR_DRM_WIRE_PROPERTY_BLOB,
        LPR_DRM_WIRE_NO_ARGUMENT,
    } wire_kind = LPR_DRM_WIRE_GENERIC;
    if (command == LPR_DRM_IOCTL_VERSION) {
        lpr_drm_version_t *version = (lpr_drm_version_t *)(uintptr_t)arg;
        drmd_version_wire_t *wire = (drmd_version_wire_t *)ioctl->data;
        wire->name_capacity = version->name_len;
        wire->date_capacity = version->date_len;
        wire->desc_capacity = version->desc_len;
        ioctl->arg_size = sizeof(*version);
        ioctl->data_size = sizeof(*wire);
        wire_kind = LPR_DRM_WIRE_VERSION;
    } else if (command == DRMD_IOCTL_MODE_GETRESOURCES) {
        drmd_kms_resources_wire_t *wire = (drmd_kms_resources_wire_t *)ioctl->data;
        lpr_memcpy(&wire->value, (const void *)(uintptr_t)arg, sizeof(wire->value));
        ioctl->arg_size = sizeof(wire->value);
        ioctl->data_size = sizeof(*wire);
        wire_kind = LPR_DRM_WIRE_RESOURCES;
    } else if (command == DRMD_IOCTL_MODE_GETCONNECTOR) {
        drmd_kms_connector_wire_t *wire = (drmd_kms_connector_wire_t *)ioctl->data;
        lpr_memcpy(&wire->value, (const void *)(uintptr_t)arg, sizeof(wire->value));
        ioctl->arg_size = sizeof(wire->value);
        ioctl->data_size = sizeof(*wire);
        wire_kind = LPR_DRM_WIRE_CONNECTOR;
    } else if (command == DRMD_IOCTL_MODE_GETCRTC || command == DRMD_IOCTL_MODE_SETCRTC) {
        drmd_kms_crtc_wire_t *wire = (drmd_kms_crtc_wire_t *)ioctl->data;
        lpr_memcpy(&wire->value, (const void *)(uintptr_t)arg, sizeof(wire->value));
        if (command == DRMD_IOCTL_MODE_SETCRTC && wire->value.set_connectors_ptr != 0) {
            uint32_t count = wire->value.count_connectors;
            if (count > DRMD_KMS_CONNECTOR_CAPACITY) count = DRMD_KMS_CONNECTOR_CAPACITY;
            lpr_memcpy(wire->connectors, (const void *)(uintptr_t)wire->value.set_connectors_ptr, count * sizeof(uint32_t));
        }
        ioctl->arg_size = sizeof(wire->value);
        ioctl->data_size = sizeof(*wire);
        wire_kind = LPR_DRM_WIRE_CRTC;
    } else if (command == DRMD_IOCTL_MODE_GETPLANERESOURCES) {
        drmd_kms_plane_res_wire_t *wire = (drmd_kms_plane_res_wire_t *)ioctl->data;
        lpr_memcpy(&wire->value, (const void *)(uintptr_t)arg, sizeof(wire->value));
        ioctl->arg_size = sizeof(wire->value);
        ioctl->data_size = sizeof(*wire);
        wire_kind = LPR_DRM_WIRE_PLANE_RES;
    } else if (command == DRMD_IOCTL_MODE_GETPLANE) {
        drmd_kms_plane_wire_t *wire = (drmd_kms_plane_wire_t *)ioctl->data;
        lpr_memcpy(&wire->value, (const void *)(uintptr_t)arg, sizeof(wire->value));
        ioctl->arg_size = sizeof(wire->value);
        ioctl->data_size = sizeof(*wire);
        wire_kind = LPR_DRM_WIRE_PLANE;
    } else if (command == DRMD_IOCTL_MODE_OBJ_GETPROPERTIES) {
        drmd_kms_object_properties_wire_t *wire =
            (drmd_kms_object_properties_wire_t *)ioctl->data;
        lpr_memcpy(&wire->value, (const void *)(uintptr_t)arg, sizeof(wire->value));
        ioctl->arg_size = sizeof(wire->value);
        ioctl->data_size = sizeof(*wire);
        wire_kind = LPR_DRM_WIRE_OBJECT_PROPERTIES;
    } else if (command == DRMD_IOCTL_MODE_GETPROPERTY) {
        drmd_kms_property_wire_t *wire = (drmd_kms_property_wire_t *)ioctl->data;
        lpr_memcpy(&wire->value, (const void *)(uintptr_t)arg, sizeof(wire->value));
        ioctl->arg_size = sizeof(wire->value);
        ioctl->data_size = sizeof(*wire);
        wire_kind = LPR_DRM_WIRE_PROPERTY;
    } else if (command == DRMD_IOCTL_MODE_GETPROPBLOB) {
        drmd_kms_property_blob_wire_t *wire =
            (drmd_kms_property_blob_wire_t *)ioctl->data;
        lpr_memcpy(&wire->value, (const void *)(uintptr_t)arg, sizeof(wire->value));
        ioctl->arg_size = sizeof(wire->value);
        ioctl->data_size = sizeof(*wire);
        wire_kind = LPR_DRM_WIRE_PROPERTY_BLOB;
    } else if (no_argument) {
        ioctl->arg_size = 0;
        ioctl->data_size = 0;
        wire_kind = LPR_DRM_WIRE_NO_ARGUMENT;
    } else {
        const uint64_t size = (request >> 16u) & 0x3fffu;
        if (size == 0 || size > DRMD_IOCTL_DATA_BYTES) {
            lpr_destroy_tty_wire_page(page_fd, page);
            return -LPR_LINUX_EINVAL;
        }
        ioctl->arg_size = size;
        ioctl->data_size = size;
        lpr_memcpy(ioctl->data, (const void *)(uintptr_t)arg, size);
    }
    const int64_t status = lpr_drmd_call(DRMD_OP_HANDLE_IOCTL, page_fd, page, sizeof(*ioctl), 0, 0);
    if (status == 0 && wire_kind == LPR_DRM_WIRE_VERSION) {
        lpr_drm_version_t *version = (lpr_drm_version_t *)(uintptr_t)arg;
        const drmd_version_wire_t *wire = (const drmd_version_wire_t *)ioctl->data;
        version->major = wire->major;
        version->minor = wire->minor;
        version->patchlevel = wire->patchlevel;
        version->name_len = wire->name_length;
        version->date_len = wire->date_length;
        version->desc_len = wire->desc_length;
        if (version->name != 0 && wire->name_capacity != 0) {
            uint64_t length = wire->name_length < wire->name_capacity ? wire->name_length : wire->name_capacity;
            if (length > sizeof(wire->name)) length = sizeof(wire->name);
            lpr_memcpy((void *)(uintptr_t)version->name, wire->name, length);
        }
        if (version->date != 0 && wire->date_capacity != 0) {
            uint64_t length = wire->date_length < wire->date_capacity ? wire->date_length : wire->date_capacity;
            if (length > sizeof(wire->date)) length = sizeof(wire->date);
            lpr_memcpy((void *)(uintptr_t)version->date, wire->date, length);
        }
        if (version->desc != 0 && wire->desc_capacity != 0) {
            uint64_t length = wire->desc_length < wire->desc_capacity ? wire->desc_length : wire->desc_capacity;
            if (length > sizeof(wire->desc)) length = sizeof(wire->desc);
            lpr_memcpy((void *)(uintptr_t)version->desc, wire->desc, length);
        }
    } else if (status == 0 && wire_kind == LPR_DRM_WIRE_RESOURCES) {
        drmd_kms_resources_wire_t *wire = (drmd_kms_resources_wire_t *)ioctl->data;
        const drmd_mode_card_res_t *user = (const drmd_mode_card_res_t *)(uintptr_t)arg;
        const uint32_t fb_capacity = user->count_fbs < DRMD_KMS_FB_CAPACITY ? user->count_fbs : DRMD_KMS_FB_CAPACITY;
        const uint32_t crtc_capacity = user->count_crtcs < DRMD_KMS_CRTC_CAPACITY ? user->count_crtcs : DRMD_KMS_CRTC_CAPACITY;
        const uint32_t connector_capacity = user->count_connectors < DRMD_KMS_CONNECTOR_CAPACITY ? user->count_connectors : DRMD_KMS_CONNECTOR_CAPACITY;
        const uint32_t encoder_capacity = user->count_encoders < DRMD_KMS_ENCODER_CAPACITY ? user->count_encoders : DRMD_KMS_ENCODER_CAPACITY;
        if (user->fb_id_ptr != 0) lpr_memcpy((void *)(uintptr_t)user->fb_id_ptr, wire->fbs, fb_capacity * sizeof(uint32_t));
        if (user->crtc_id_ptr != 0) lpr_memcpy((void *)(uintptr_t)user->crtc_id_ptr, wire->crtcs, crtc_capacity * sizeof(uint32_t));
        if (user->connector_id_ptr != 0) lpr_memcpy((void *)(uintptr_t)user->connector_id_ptr, wire->connectors, connector_capacity * sizeof(uint32_t));
        if (user->encoder_id_ptr != 0) lpr_memcpy((void *)(uintptr_t)user->encoder_id_ptr, wire->encoders, encoder_capacity * sizeof(uint32_t));
        lpr_memcpy((void *)(uintptr_t)arg, &wire->value, sizeof(wire->value));
    } else if (status == 0 && wire_kind == LPR_DRM_WIRE_CONNECTOR) {
        drmd_kms_connector_wire_t *wire = (drmd_kms_connector_wire_t *)ioctl->data;
        const drmd_mode_get_connector_t *user = (const drmd_mode_get_connector_t *)(uintptr_t)arg;
        const uint32_t mode_capacity = user->count_modes < DRMD_KMS_MODE_CAPACITY ? user->count_modes : DRMD_KMS_MODE_CAPACITY;
        const uint32_t encoder_capacity = user->count_encoders < DRMD_KMS_ENCODER_CAPACITY ? user->count_encoders : DRMD_KMS_ENCODER_CAPACITY;
        const uint32_t prop_capacity = user->count_props < DRMD_KMS_PROPERTY_CAPACITY ?
            user->count_props : DRMD_KMS_PROPERTY_CAPACITY;
        if (user->modes_ptr != 0) lpr_memcpy((void *)(uintptr_t)user->modes_ptr, wire->modes, mode_capacity * sizeof(drmd_modeinfo_t));
        if (user->encoders_ptr != 0) lpr_memcpy((void *)(uintptr_t)user->encoders_ptr, wire->encoders, encoder_capacity * sizeof(uint32_t));
        if (user->props_ptr != 0) lpr_memcpy((void *)(uintptr_t)user->props_ptr, wire->props, prop_capacity * sizeof(uint32_t));
        if (user->prop_values_ptr != 0) lpr_memcpy((void *)(uintptr_t)user->prop_values_ptr, wire->prop_values, prop_capacity * sizeof(uint64_t));
        lpr_memcpy((void *)(uintptr_t)arg, &wire->value, sizeof(wire->value));
    } else if (status == 0 && wire_kind == LPR_DRM_WIRE_CRTC) {
        drmd_kms_crtc_wire_t *wire = (drmd_kms_crtc_wire_t *)ioctl->data;
        lpr_memcpy((void *)(uintptr_t)arg, &wire->value, sizeof(wire->value));
    } else if (status == 0 && wire_kind == LPR_DRM_WIRE_PLANE_RES) {
        drmd_kms_plane_res_wire_t *wire = (drmd_kms_plane_res_wire_t *)ioctl->data;
        const drmd_mode_get_plane_res_t *user = (const drmd_mode_get_plane_res_t *)(uintptr_t)arg;
        const uint32_t capacity = user->count_planes < DRMD_KMS_PLANE_CAPACITY ? user->count_planes : DRMD_KMS_PLANE_CAPACITY;
        if (user->plane_id_ptr != 0) lpr_memcpy((void *)(uintptr_t)user->plane_id_ptr, wire->planes, capacity * sizeof(uint32_t));
        lpr_memcpy((void *)(uintptr_t)arg, &wire->value, sizeof(wire->value));
    } else if (status == 0 && wire_kind == LPR_DRM_WIRE_PLANE) {
        drmd_kms_plane_wire_t *wire = (drmd_kms_plane_wire_t *)ioctl->data;
        const drmd_mode_get_plane_t *user = (const drmd_mode_get_plane_t *)(uintptr_t)arg;
        const uint32_t capacity = user->count_format_types < DRMD_KMS_FORMAT_CAPACITY ? user->count_format_types : DRMD_KMS_FORMAT_CAPACITY;
        if (user->format_type_ptr != 0) lpr_memcpy((void *)(uintptr_t)user->format_type_ptr, wire->formats, capacity * sizeof(uint32_t));
        lpr_memcpy((void *)(uintptr_t)arg, &wire->value, sizeof(wire->value));
    } else if (status == 0 && wire_kind == LPR_DRM_WIRE_OBJECT_PROPERTIES) {
        drmd_kms_object_properties_wire_t *wire =
            (drmd_kms_object_properties_wire_t *)ioctl->data;
        const drmd_mode_obj_get_properties_t *user =
            (const drmd_mode_obj_get_properties_t *)(uintptr_t)arg;
        const uint32_t capacity = user->count_props < DRMD_KMS_PROPERTY_CAPACITY ?
            user->count_props : DRMD_KMS_PROPERTY_CAPACITY;
        if (user->props_ptr != 0) lpr_memcpy((void *)(uintptr_t)user->props_ptr,
            wire->props, capacity * sizeof(uint32_t));
        if (user->prop_values_ptr != 0) lpr_memcpy((void *)(uintptr_t)user->prop_values_ptr,
            wire->prop_values, capacity * sizeof(uint64_t));
        lpr_memcpy((void *)(uintptr_t)arg, &wire->value, sizeof(wire->value));
    } else if (status == 0 && wire_kind == LPR_DRM_WIRE_PROPERTY) {
        drmd_kms_property_wire_t *wire = (drmd_kms_property_wire_t *)ioctl->data;
        const drmd_mode_get_property_t *user =
            (const drmd_mode_get_property_t *)(uintptr_t)arg;
        const uint32_t value_capacity = user->count_values < DRMD_KMS_PROPERTY_VALUE_CAPACITY ?
            user->count_values : DRMD_KMS_PROPERTY_VALUE_CAPACITY;
        const uint32_t enum_capacity = user->count_enum_blobs < DRMD_KMS_PROPERTY_ENUM_CAPACITY ?
            user->count_enum_blobs : DRMD_KMS_PROPERTY_ENUM_CAPACITY;
        if (user->values_ptr != 0) lpr_memcpy((void *)(uintptr_t)user->values_ptr,
            wire->values, value_capacity * sizeof(uint64_t));
        if (user->enum_blob_ptr != 0) lpr_memcpy((void *)(uintptr_t)user->enum_blob_ptr,
            wire->enums, enum_capacity * sizeof(drmd_mode_property_enum_t));
        lpr_memcpy((void *)(uintptr_t)arg, &wire->value, sizeof(wire->value));
    } else if (status == 0 && wire_kind == LPR_DRM_WIRE_PROPERTY_BLOB) {
        drmd_kms_property_blob_wire_t *wire =
            (drmd_kms_property_blob_wire_t *)ioctl->data;
        const drmd_mode_get_blob_t *user = (const drmd_mode_get_blob_t *)(uintptr_t)arg;
        uint32_t length = user->length < wire->value.length ? user->length : wire->value.length;
        if (length > DRMD_KMS_PROPERTY_BLOB_BYTES) length = DRMD_KMS_PROPERTY_BLOB_BYTES;
        if (user->data != 0) lpr_memcpy((void *)(uintptr_t)user->data, wire->data, length);
        lpr_memcpy((void *)(uintptr_t)arg, &wire->value, sizeof(wire->value));
    } else if (status == 0 && wire_kind == LPR_DRM_WIRE_GENERIC) {
        lpr_memcpy((void *)(uintptr_t)arg, ioctl->data, ioctl->data_size);
    }
    lpr_destroy_tty_wire_page(page_fd, page);
    return status;
}

int64_t lpr_drm_mmap(
    uint64_t fd,
    uint64_t address,
    uint64_t length,
    uint64_t pacha_prot,
    uint64_t pacha_flags,
    uint64_t offset)
{
    lpr_drm_backend_t *drm = lpr_drm_backend(fd);
    if (drm == 0) {
        return -LPR_LINUX_EBADF;
    }
    void *page = 0;
    const int page_fd = lpr_create_tty_wire_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }
    drmd_mmap_request_t *mmap = (drmd_mmap_request_t *)lpr_drmd_payload(page);
    lpr_memset(mmap, 0, sizeof(*mmap));
    mmap->handle = drm->handle;
    mmap->length = length;
    mmap->prot = pacha_prot;
    mmap->flags = pacha_flags;
    mmap->offset = offset;
    int vmo_fd = -1;
    const int64_t status = lpr_drmd_call(DRMD_OP_HANDLE_MMAP, page_fd, page, sizeof(*mmap), 0, &vmo_fd);
    lpr_destroy_tty_wire_page(page_fd, page);
    if (status != 0) {
        return status;
    }
    if (vmo_fd < 16) {
        return -LPR_LINUX_EIO;
    }
    const int64_t mapped = lpr_pacha_syscall6(
        PACHAOS_SYSCALL_MMAP,
        (uint64_t)(uint32_t)vmo_fd,
        address,
        length,
        pacha_prot,
        pacha_flags,
        0);
    (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)vmo_fd);
    return mapped < 4096 ? lpr_pacha_status_to_errno(mapped) : mapped;
}

int64_t lpr_drm_poll_events(uint64_t fd, uint32_t events)
{
    lpr_drm_backend_t *drm = lpr_drm_backend(fd);
    if (drm == 0) return -LPR_LINUX_EBADF;
    if (drm->wait_fd.raw >= 16) {
        struct pacha_pollfd pollfd = {
            .fd = drm->wait_fd.raw,
            .events = PACHA_FD_EVENT_READABLE,
        };
        const int64_t status = lpr_pacha_syscall2(
            PACHAOS_SYSCALL_FD_POLL,
            (uint64_t)(uintptr_t)&pollfd,
            1);
        if (status < 0) return lpr_pacha_status_to_errno(status);
        return (pollfd.revents & PACHA_FD_EVENT_READABLE) != 0 ? events & 0x0001u : 0;
    }
    void *page = 0;
    const int page_fd = lpr_create_tty_wire_page(&page);
    if (page_fd < 0) return page_fd;
    drmd_handle_request_t *poll = (drmd_handle_request_t *)lpr_drmd_payload(page);
    lpr_memset(poll, 0, sizeof(*poll));
    poll->handle = drm->handle;
    poll->arg0 = events;
    uint64_t revents = 0;
    const int64_t status = lpr_drmd_call(
        DRMD_OP_HANDLE_POLL, page_fd, page, sizeof(*poll), &revents, 0);
    lpr_destroy_tty_wire_page(page_fd, page);
    return status == 0 ? (int64_t)revents : status;
}

int64_t lpr_drm_read_events(uint64_t fd, uint64_t buf, uint64_t count)
{
    lpr_drm_backend_t *drm = lpr_drm_backend(fd);
    if (drm == 0) return -LPR_LINUX_EBADF;
    if (count == 0) return 0;
    if (buf == 0) return -LPR_LINUX_EFAULT;
    if (drm->wait_fd.raw < 16) return -LPR_LINUX_EIO;
    if (count < 4u * sizeof(uint64_t)) return -LPR_LINUX_EINVAL;
    for (;;) {
        struct pacha_ipc_msg event;
        lpr_memset(&event, 0, sizeof(event));
        const int64_t recv_status = lpr_pacha_syscall2(
            PACHAOS_SYSCALL_IPC_RECV,
            (uint64_t)(uint32_t)drm->wait_fd.raw,
            (uint64_t)(uintptr_t)&event);
        if (recv_status == 0) {
            lpr_memcpy((void *)(uintptr_t)buf, &event.word0, 4u * sizeof(uint64_t));
            return 4u * sizeof(uint64_t);
        }
        if (recv_status != PACHA_ERR_EMPTY && recv_status != PACHA_ERR_NOT_READY) {
            return lpr_pacha_status_to_errno(recv_status);
        }
        if ((drm->flags & LPR_LINUX_O_NONBLOCK) != 0) return -LPR_LINUX_EAGAIN;
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
}

int lpr_drm_native_wait_fd(uint64_t fd)
{
    lpr_drm_backend_t *drm = lpr_drm_backend(fd);
    return drm != 0 ? drm->wait_fd.raw : -1;
}
