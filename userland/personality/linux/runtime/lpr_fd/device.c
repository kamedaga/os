#include "../lpr_filed_internal.h"
#include "device.h"

static uint64_t description_rights(void)
{
    return PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_DUP | PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE;
}

int lpr_device_description_capability_valid(pacha_fd_t fd)
{
    struct pacha_fd_info info = {0};
    return fd >= 16 && lpr_native_fd_info((uint64_t)(uint32_t)fd, &info) &&
        info.kind == PACHA_FD_KIND_VMO && info.size == LPR_DEVICE_DESCRIPTION_BYTES &&
        info.rights == description_rights();
}

int lpr_device_description_map(lpr_device_backend_t *device)
{
    if (device->description_map) return 0;
    if (!lpr_device_description_capability_valid(device->description_fd.raw))
        return -LPR_LINUX_EBADF;
    const int64_t mapping = lpr_pacha_syscall6(PACHAOS_SYSCALL_MMAP,
        (uint64_t)(uint32_t)device->description_fd.raw, 0, LPR_DEVICE_DESCRIPTION_BYTES,
        PACHAOS_PROT_READ | PACHAOS_PROT_WRITE, PACHAOS_MMAP_SHARED, 0);
    if (mapping < 4096) return -LPR_LINUX_ENOMEM;
    device->description_map = (uint64_t)mapping;
    return 0;
}

int lpr_device_description_ensure_locked(lpr_device_backend_t *device)
{
    int status = 0;
    if (device->description_fd.raw < 16) {
        const int64_t fd = lpr_pacha_syscall3(PACHAOS_SYSCALL_VMO_CREATE,
            LPR_DEVICE_DESCRIPTION_BYTES, description_rights(), 0);
        if (fd < 16) status = fd ? (int)lpr_pacha_status_to_errno(fd) : -LPR_LINUX_EIO;
        else {
            device->description_fd.raw = (pacha_fd_t)fd;
            status = lpr_device_description_map(device);
            if (status) {
                (void)lpr_close_native_fd_if_open((uint64_t)fd);
                device->description_fd.raw = -1;
            } else lpr_device_description_set_flags(device,
                lpr_control_status_flags_from_linux(device->flags));
        }
    } else status = lpr_device_description_map(device);
    return status;
}

int lpr_device_description_ensure(lpr_device_backend_t *device)
{
    /* A pinned backend cannot disappear here. Serialize first publication
     * with F_SETFL so simultaneous SCM_RIGHTS sends share one description. */
    lpr_fd_table_lock(&lpr_control_fd_table);
    const int status = lpr_device_description_ensure_locked(device);
    lpr_fd_table_unlock(&lpr_control_fd_table);
    return status;
}

uint32_t lpr_device_description_flags(const lpr_device_backend_t *device)
{
    const uint32_t flags = device->description_map ?
        __atomic_load_n((const uint32_t *)(uintptr_t)device->description_map, __ATOMIC_ACQUIRE) :
        lpr_control_status_flags_from_linux(device->flags);
    /* The sender can write its shared page. No device identity, pointer or
     * access rights are read from that page, only these unprivileged flags. */
    return flags & (LPR_OFD_NONBLOCK | LPR_OFD_APPEND);
}

void lpr_device_description_set_flags(lpr_device_backend_t *device, uint32_t flags)
{
    if (device->description_map)
        __atomic_store_n((uint32_t *)(uintptr_t)device->description_map,
            flags & (LPR_OFD_NONBLOCK | LPR_OFD_APPEND), __ATOMIC_RELEASE);
}

int64_t lpr_device_description_unmap(lpr_device_backend_t *device)
{
    int64_t status = 0;
    if (device->description_map)
        status = lpr_pacha_status_to_errno(lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP,
            device->description_map, LPR_DEVICE_DESCRIPTION_BYTES));
    device->description_map = 0;
    return status;
}

int64_t lpr_device_description_close(lpr_device_backend_t *device)
{
    int64_t status = lpr_device_description_unmap(device);
    if (device->description_fd.raw >= 16) {
        const int64_t closed = lpr_close_native_fd_if_open(
            (uint64_t)(uint32_t)device->description_fd.raw);
        if (!status) status = closed;
    }
    device->description_fd.raw = -1;
    return status;
}
