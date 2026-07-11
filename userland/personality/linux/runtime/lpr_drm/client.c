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
    uint64_t *out_result,
    int *out_received_fd)
{
    if (LPR_DRMD_DRM_ENDPOINT_FD < 16 || page_fd < 16 || page == 0) {
        return -LPR_LINUX_ENODEV;
    }
    struct pacha_ipc_fd fd;
    struct pacha_ipc_msg request;
    struct pacha_ipc_msg reply;
    struct pacha_ipc_fd reply_fd_item;
    lpr_memset(&fd, 0, sizeof(fd));
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
    if (out_received_fd != 0) {
        if (reply_header->status == 0 && reply.fd_count == 1 && reply_fd_item.fd >= 16) {
            *out_received_fd = (int)(uint32_t)reply_fd_item.fd;
        } else if (reply.fd_count != 0 && reply_fd_item.fd >= 16) {
            (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, reply_fd_item.fd);
        }
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
    const int64_t status = lpr_drmd_call(DRMD_OP_OPEN_CARD, page_fd, page, sizeof(*open), &handle, 0);
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
    const int64_t status = lpr_drmd_call(DRMD_OP_HANDLE_CLOSE, page_fd, page, sizeof(*close), 0, 0);
    lpr_destroy_tty_wire_page(page_fd, page);
    return status;
}

int64_t lpr_drm_ioctl(uint64_t fd, uint64_t request, uint64_t arg)
{
    lpr_drm_fd_t *drm = lpr_fd_drm_payload(fd);
    const uint32_t command = (uint32_t)request;
    const int no_argument = command == DRMD_IOCTL_SET_MASTER || command == DRMD_IOCTL_DROP_MASTER;
    if (drm == 0 || (arg == 0 && !no_argument)) {
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
    enum {
        LPR_DRM_WIRE_GENERIC,
        LPR_DRM_WIRE_VERSION,
        LPR_DRM_WIRE_RESOURCES,
        LPR_DRM_WIRE_CONNECTOR,
        LPR_DRM_WIRE_CRTC,
        LPR_DRM_WIRE_PLANE_RES,
        LPR_DRM_WIRE_PLANE,
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
        if (user->modes_ptr != 0) lpr_memcpy((void *)(uintptr_t)user->modes_ptr, wire->modes, mode_capacity * sizeof(drmd_modeinfo_t));
        if (user->encoders_ptr != 0) lpr_memcpy((void *)(uintptr_t)user->encoders_ptr, wire->encoders, encoder_capacity * sizeof(uint32_t));
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
    lpr_drm_fd_t *drm = lpr_fd_drm_payload(fd);
    if (drm == 0) return -LPR_LINUX_EBADF;
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
    lpr_drm_fd_t *drm = lpr_fd_drm_payload(fd);
    if (drm == 0) return -LPR_LINUX_EBADF;
    if (count == 0) return 0;
    if (buf == 0) return -LPR_LINUX_EFAULT;
    for (;;) {
        void *page = 0;
        const int page_fd = lpr_create_tty_wire_page(&page);
        if (page_fd < 0) return page_fd;
        drmd_read_request_t *read = (drmd_read_request_t *)lpr_drmd_payload(page);
        lpr_memset(read, 0, sizeof(*read));
        read->handle = drm->handle;
        read->capacity = count < sizeof(read->data) ? count : sizeof(read->data);
        uint64_t result = 0;
        const int64_t status = lpr_drmd_call(
            DRMD_OP_HANDLE_READ, page_fd, page, sizeof(*read), &result, 0);
        if (status == 0) {
            const uint64_t bytes = read->data_size < result ? read->data_size : result;
            if (bytes > count || bytes > sizeof(read->data)) {
                lpr_destroy_tty_wire_page(page_fd, page);
                return -LPR_LINUX_EIO;
            }
            lpr_memcpy((void *)(uintptr_t)buf, read->data, bytes);
            lpr_destroy_tty_wire_page(page_fd, page);
            return (int64_t)bytes;
        }
        lpr_destroy_tty_wire_page(page_fd, page);
        if (status != -LPR_LINUX_EAGAIN ||
            (drm->flags & LPR_LINUX_O_NONBLOCK) != 0) {
            return status;
        }
        struct pachaos_timespec delay = { .tv_sec = 0, .tv_nsec = 1000000 };
        const int64_t sleep_status = lpr_pacha_syscall1(
            PACHAOS_SYSCALL_NANOSLEEP, (uint64_t)(uintptr_t)&delay);
        if (sleep_status != 0) return lpr_pacha_status_to_errno(sleep_status);
    }
}
