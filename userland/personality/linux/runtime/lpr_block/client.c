/* SPDX-License-Identifier: MIT */
#include "../lpr_filed_internal.h"
#include "../lpr_fd/allocate.h"
#include "client.h"
#include "../../../../installer/disk_port.h"

struct lpr_install_device_state {
    struct installer_disk_port_page *page;
    pacha_fd_t page_fd;
    uint64_t sequence, bytes, writable_first, writable_end;
};

static volatile uint32_t lpr_block_lock_word;
static struct lpr_install_device_state
    lpr_install_devices[LPR_INSTALL_DEVICE_COUNT];

static uint64_t device_endpoint(unsigned kind) {
    static const uint16_t endpoints[LPR_INSTALL_DEVICE_COUNT] = {
        LPR_INSTALL_DISK_ENDPOINT_FD, LPR_INSTALL_MEDIA_ENDPOINT_FD,
        LPR_INSTALL_ROOT_ENDPOINT_FD,
    };
    return kind < LPR_INSTALL_DEVICE_COUNT ? endpoints[kind] : 0;
}

static void block_lock(void) {
    if (__atomic_exchange_n(&lpr_block_lock_word, 1u,
            __ATOMIC_ACQUIRE) == 0u) return;
    while (__atomic_exchange_n(&lpr_block_lock_word, 2u,
            __ATOMIC_ACQUIRE) != 0u)
        (void)lpr_pacha_syscall4(PACHA_RUNTIME_SYSCALL_FUTEX_WAIT,
            (uint64_t)(uintptr_t)&lpr_block_lock_word, 2u, 0, PACHA_WAIT_UNINTERRUPTIBLE);
}

static void block_unlock(void) {
    if (__atomic_exchange_n(&lpr_block_lock_word, 0u,
            __ATOMIC_RELEASE) == 2u)
        (void)lpr_pacha_syscall2(PACHA_RUNTIME_SYSCALL_FUTEX_WAKE,
            (uint64_t)(uintptr_t)&lpr_block_lock_word, 1u);
}

static int64_t send_request(unsigned kind, uint32_t operation) {
    struct lpr_install_device_state *state = &lpr_install_devices[kind];
    struct installer_disk_port_page *page = state->page;
    const uint64_t endpoint = device_endpoint(kind);
    if (!page || !endpoint) return -LPR_LINUX_ENODEV;
    const uint64_t sequence = ++state->sequence;
    page->magic = INSTALLER_DISK_PORT_MAGIC;
    page->sequence = sequence;
    page->operation = operation;
    const struct pacha_ipc_msg request = {.word0 = operation,
        .word1 = sequence};
    int64_t status = lpr_ipc_message(PACHAOS_SYSCALL_IPC_SEND, endpoint, (uint64_t)(uintptr_t)((uint64_t)(uintptr_t)&request), 0, 0, 0, 0);
    if (status) return lpr_pacha_status_to_errno(status);
    struct pacha_ipc_fd received[1] = {0};
    struct pacha_ipc_msg reply = {.fds = received, .fd_capacity = 1};
    status = lpr_native_ipc_recv_wait(endpoint, &reply);
    if (status) return lpr_pacha_status_to_errno(status);
    for (uint32_t i = 0; i < reply.fd_count; ++i)
        if (received[i].fd >= 16)
            (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE,
                received[i].fd);
    if (reply.fd_count || reply.word0 != operation ||
        reply.word1 != sequence || reply.word3 ||
        (int64_t)reply.word2 != page->status)
        return -LPR_LINUX_EIO;
    return (int64_t)reply.word2;
}

static int64_t ensure_attached(unsigned kind) {
    if (kind >= LPR_INSTALL_DEVICE_COUNT) return -LPR_LINUX_ENODEV;
    struct lpr_install_device_state *state = &lpr_install_devices[kind];
    if (state->page) return 0;
    const uint64_t endpoint = device_endpoint(kind);
    struct pacha_fd_info info;
    if (!lpr_native_fd_info(endpoint, &info) ||
        info.kind != PACHA_FD_KIND_CHANNEL)
        return -LPR_LINUX_ENOENT;
    const uint64_t rights = PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE;
    const int64_t fd = lpr_pacha_syscall3(PACHAOS_SYSCALL_VMO_CREATE,
        INSTALLER_DISK_PORT_BYTES, rights,
        PACHA_FD_FLAG_PRIVATE | PACHA_FD_FLAG_CLOEXEC);
    if (fd < 16) return fd ? lpr_pacha_status_to_errno(fd) :
        -LPR_LINUX_EIO;
    const int64_t mapped = lpr_pacha_syscall6(PACHAOS_SYSCALL_MMAP,
        (uint64_t)fd, 0, INSTALLER_DISK_PORT_BYTES,
        PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
    if (mapped < 4096) {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)fd);
        return -LPR_LINUX_ENOMEM;
    }
    struct installer_disk_port_page *page = (void *)(uintptr_t)mapped;
    lpr_memset(page, 0, sizeof(*page));
    const uint64_t sequence = ++state->sequence;
    struct pacha_ipc_fd sent = {.fd = (uint64_t)fd, .rights = rights};
    const struct pacha_ipc_msg request = {
        .word0 = INSTALLER_DISK_PORT_ATTACH, .word1 = sequence,
        .fds = &sent, .fd_count = 1};
    int64_t status = lpr_ipc_message(PACHAOS_SYSCALL_IPC_SEND, endpoint, (uint64_t)(uintptr_t)((uint64_t)(uintptr_t)&request), 0, 0, 0, 0);
    if (!status) {
        struct pacha_ipc_msg reply = {0};
        status = lpr_native_ipc_recv_wait(endpoint, &reply);
        if (!status && (reply.fd_count ||
            reply.word0 != INSTALLER_DISK_PORT_ATTACH ||
            reply.word1 != sequence || reply.word3))
            status = -LPR_LINUX_EIO;
        else if (!status) status = (int64_t)reply.word2;
    } else status = lpr_pacha_status_to_errno(status);
    if (status) {
        (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP,
            (uint64_t)(uintptr_t)page, INSTALLER_DISK_PORT_BYTES);
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)fd);
        return status;
    }
    state->page = page;
    state->page_fd = (pacha_fd_t)fd;
    page->offset = 0;
    page->length = 0;
    status = send_request(kind, INSTALLER_DISK_PORT_INFO);
    if (!status) {
        state->bytes = page->disk_bytes;
        state->writable_first = page->writable_first;
        state->writable_end = page->writable_end;
        if (!state->bytes || state->writable_first > state->writable_end ||
            state->writable_end > state->bytes)
            status = -LPR_LINUX_EIO;
    }
    if (status) {
        lpr_memset(state, 0, sizeof(*state));
        (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP,
            (uint64_t)(uintptr_t)page, INSTALLER_DISK_PORT_BYTES);
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)fd);
    }
    return status;
}

int64_t lpr_block_open_path(const char *path, uint64_t flags) {
    unsigned kind;
    if (!path) return -LPR_LINUX_ENOENT;
    if (!lpr_strcmp(path, "/dev/pacha/install-target"))
        kind = LPR_INSTALL_DEVICE_TARGET;
    else if (!lpr_strcmp(path, "/dev/pacha/install-media"))
        kind = LPR_INSTALL_DEVICE_MEDIA;
    else if (!lpr_strcmp(path, "/dev/pacha/install-root"))
        kind = LPR_INSTALL_DEVICE_ROOT;
    else return -LPR_LINUX_ENOENT;
    if (flags & (LPR_LINUX_O_CREAT | LPR_LINUX_O_TRUNC |
            LPR_LINUX_O_DIRECTORY | LPR_LINUX_O_APPEND))
        return -LPR_LINUX_EINVAL;
    block_lock();
    int64_t status = ensure_attached(kind);
    if (!status && (flags & LPR_LINUX_O_ACCMODE) != LPR_LINUX_O_RDONLY &&
        lpr_install_devices[kind].writable_first ==
            lpr_install_devices[kind].writable_end)
        status = -LPR_LINUX_EACCES;
    block_unlock();
    if (status) return status;
    const lpr_block_backend_t backend = {.active = 1,
        .device_kind = (uint8_t)kind, .flags = (uint32_t)flags,
        .size = lpr_install_devices[kind].bytes};
    return lpr_fd_alloc_state(LPR_FD_OPS_INSTALL_DISK, flags, 0,
        &backend, sizeof(backend));
}

int64_t lpr_block_io(uint64_t fd, uint64_t buffer, uint64_t count,
    uint64_t offset, int writing) {
    const lpr_block_backend_t *backend = lpr_block_backend(fd);
    if (!backend || backend->device_kind >= LPR_INSTALL_DEVICE_COUNT)
        return -LPR_LINUX_EBADF;
    const unsigned kind = backend->device_kind;
    if (writing && (backend->flags & LPR_LINUX_O_ACCMODE) ==
        LPR_LINUX_O_RDONLY) return -LPR_LINUX_EBADF;
    if (!writing && (backend->flags & LPR_LINUX_O_ACCMODE) ==
        LPR_LINUX_O_WRONLY) return -LPR_LINUX_EBADF;
    if ((!buffer && count) ||
        (count && buffer > UINT64_MAX - (count - 1u)) ||
        offset > INT64_MAX || count > INT64_MAX - offset)
        return -LPR_LINUX_EFAULT;
    block_lock();
    int64_t status = ensure_attached(kind);
    uint64_t done = 0;
    while (!status && done < count) {
        const uint32_t chunk = (uint32_t)((count - done) >
            KB2_BLOCK_DEVICE_MAX_TRANSFER ?
            KB2_BLOCK_DEVICE_MAX_TRANSFER : count - done);
        struct installer_disk_port_page *page =
            lpr_install_devices[kind].page;
        page->offset = offset + done;
        page->length = chunk;
        if (writing)
            lpr_memcpy(page->data,
                (const void *)(uintptr_t)(buffer + done), chunk);
        status = send_request(kind, writing ? INSTALLER_DISK_PORT_WRITE :
            INSTALLER_DISK_PORT_READ);
        if (status) break;
        if (page->transferred > chunk) { status = -LPR_LINUX_EIO; break; }
        if (!writing && page->transferred)
            lpr_memcpy((void *)(uintptr_t)(buffer + done),
                page->data, page->transferred);
        done += page->transferred;
        if (page->transferred < chunk) break;
    }
    block_unlock();
    return done ? (int64_t)done : status;
}

static int64_t flush_backend(const lpr_block_backend_t *backend) {
    if (!backend || backend->device_kind >= LPR_INSTALL_DEVICE_COUNT)
        return -LPR_LINUX_EBADF;
    const unsigned kind = backend->device_kind;
    block_lock();
    int64_t status = ensure_attached(kind);
    if (!status) {
        lpr_install_devices[kind].page->offset = 0;
        lpr_install_devices[kind].page->length = 0;
        status = send_request(kind, INSTALLER_DISK_PORT_FLUSH);
    }
    block_unlock();
    return status;
}

int64_t lpr_block_flush(uint64_t fd) {
    return flush_backend(lpr_block_backend(fd));
}

int64_t lpr_block_ioctl(uint64_t fd, uint64_t request, uint64_t arg) {
    const lpr_block_backend_t *backend = lpr_block_backend(fd);
    if (!backend) return -LPR_LINUX_EBADF;
    if (request != INSTALLER_PROGRESS_IOCTL)
        return -LPR_LINUX_ENOTTY;
    if (backend->device_kind != LPR_INSTALL_DEVICE_MEDIA)
        return -LPR_LINUX_EACCES;
    if (!arg) return -LPR_LINUX_EFAULT;
    const struct installer_progress_wire *wire =
        (const void *)(uintptr_t)arg;
    if (wire->magic != INSTALLER_PROGRESS_MAGIC || wire->reserved ||
        wire->phase < INSTALLER_PROGRESS_PREFLIGHT_FILES ||
        wire->phase > INSTALLER_PROGRESS_COMPLETE ||
        (wire->total_bytes && wire->completed_bytes > wire->total_bytes))
        return -LPR_LINUX_EINVAL;
    block_lock();
    int64_t status = ensure_attached(backend->device_kind);
    if (!status) {
        struct installer_disk_port_page *page =
            lpr_install_devices[backend->device_kind].page;
        page->offset = 0;
        page->length = sizeof(*wire);
        lpr_memcpy(page->data, wire, sizeof(*wire));
        status = send_request(backend->device_kind,
            INSTALLER_DISK_PORT_PROGRESS);
    }
    block_unlock();
    return status;
}

int64_t lpr_block_close_state(const void *state) {
    const lpr_block_backend_t *backend = state;
    if (!backend || !backend->active) return 0;
    /* Linux block-device users are allowed to rely on the final close to
     * submit buffered metadata.  Our virtual descriptor does not own the
     * Linux struct file used for individual I/O calls, so reproduce that
     * close-time durability explicitly before dropping the last LPR state. */
    return flush_backend(backend);
}

int64_t lpr_block_size(uint64_t fd) {
    const lpr_block_backend_t *backend = lpr_block_backend(fd);
    return backend && backend->size <= INT64_MAX ?
        (int64_t)backend->size : -LPR_LINUX_EBADF;
}
