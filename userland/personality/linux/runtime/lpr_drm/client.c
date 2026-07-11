#include "../lpr_filed_internal.h"

enum {
    LPR_DRM_IOCTL_VERSION = 0xc0406400u,
};

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

static int64_t lpr_drmd_call(
    uint32_t op,
    int page_fd,
    void *page,
    uint32_t payload_size,
    uint64_t *out_result)
{
    if (LPR_DRMD_DRM_ENDPOINT_FD < 16 || page_fd < 16 || page == 0) {
        return -LPR_LINUX_ENODEV;
    }
    struct pacha_ipc_fd fd;
    struct pacha_ipc_msg request;
    struct pacha_ipc_msg reply;
    lpr_memset(&fd, 0, sizeof(fd));
    lpr_memset(&request, 0, sizeof(request));
    lpr_memset(&reply, 0, sizeof(reply));
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
    fd.fd = (uint64_t)(uint32_t)page_fd;
    fd.rights = PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE;
    request.word0 = PACHA_SERVICE_REQUEST_MAGIC;
    request.word3 = request_id;
    request.fds = &fd;
    request.fd_count = 1;
    const int64_t reply_fd = lpr_pacha_syscall2(
        PACHAOS_SYSCALL_IPC_CALL,
        LPR_DRMD_DRM_ENDPOINT_FD,
        (uint64_t)(uintptr_t)&request);
    if (reply_fd < 16) {
        return lpr_pacha_status_to_errno(reply_fd);
    }
    const int64_t recv_status = lpr_pacha_syscall4(
        PACHAOS_SYSCALL_IPC_RECV_WAIT,
        (uint64_t)(uint32_t)reply_fd,
        (uint64_t)(uintptr_t)&reply,
        UINT64_MAX,
        0);
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
    return reply_header->status;
}

int lpr_drm_fd_alloc(uint64_t handle, uint64_t flags)
{
    const int fd = lpr_fd_slot_alloc();
    if (fd < 0) {
        return fd;
    }
    const int status = lpr_control_install_fd(fd, LPR_FD_TABLE_KIND_DRM, flags, handle, 0);
    if (status != 0) {
        return status;
    }
    lpr_drm_fd_t *drm = lpr_fd_drm_payload(fd);
    if (drm == 0) {
        lpr_control_close_fd(fd);
        return -LPR_LINUX_EIO;
    }
    drm->active = 1;
    drm->flags = (uint32_t)flags;
    drm->handle = handle;
    return fd;
}

int64_t lpr_drm_open_path(const char *path, uint64_t flags)
{
    if (path == 0 || lpr_strcmp(path, "/dev/dri/card0") != 0) {
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
    uint64_t handle = 0;
    const int64_t status = lpr_drmd_call(DRMD_OP_OPEN_CARD, page_fd, page, sizeof(*open), &handle);
    lpr_destroy_tty_wire_page(page_fd, page);
    if (status != 0) {
        return status;
    }
    const int fd = lpr_drm_fd_alloc(handle, flags);
    if (fd < 0) {
        (void)lpr_drm_close_handle(handle);
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
    const int64_t status = lpr_drmd_call(DRMD_OP_HANDLE_CLOSE, page_fd, page, sizeof(*close), 0);
    lpr_destroy_tty_wire_page(page_fd, page);
    return status;
}

int64_t lpr_drm_ioctl(uint64_t fd, uint64_t request, uint64_t arg)
{
    lpr_drm_fd_t *drm = lpr_fd_drm_payload(fd);
    if (drm == 0 || arg == 0) {
        return drm == 0 ? -LPR_LINUX_EBADF : -LPR_LINUX_EFAULT;
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
    if ((uint32_t)request == LPR_DRM_IOCTL_VERSION) {
        lpr_drm_version_t *version = (lpr_drm_version_t *)(uintptr_t)arg;
        drmd_version_wire_t *wire = (drmd_version_wire_t *)ioctl->data;
        wire->name_capacity = version->name_len;
        wire->date_capacity = version->date_len;
        wire->desc_capacity = version->desc_len;
        ioctl->arg_size = sizeof(*version);
        ioctl->data_size = sizeof(*wire);
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
    const int64_t status = lpr_drmd_call(DRMD_OP_HANDLE_IOCTL, page_fd, page, sizeof(*ioctl), 0);
    if (status == 0 && (uint32_t)request == LPR_DRM_IOCTL_VERSION) {
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
    } else if (status == 0) {
        lpr_memcpy((void *)(uintptr_t)arg, ioctl->data, ioctl->data_size);
    }
    lpr_destroy_tty_wire_page(page_fd, page);
    return status;
}

int64_t lpr_drm_mmap(uint64_t fd, uint64_t length, uint64_t prot, uint64_t flags, uint64_t offset)
{
    lpr_drm_fd_t *drm = lpr_fd_drm_payload(fd);
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
    mmap->prot = prot;
    mmap->flags = flags;
    mmap->offset = offset;
    const int64_t status = lpr_drmd_call(DRMD_OP_HANDLE_MMAP, page_fd, page, sizeof(*mmap), 0);
    lpr_destroy_tty_wire_page(page_fd, page);
    return status;
}
