#include "../lpr_filed_internal.h"
#include "../lpr_fd/allocate.h"

uint64_t lpr_open_rights(uint64_t flags)
{
    lpr_linux_process_state_init();
    /*
     * Linux does not require O_DIRECTORY when opening a directory that will
     * later be used as an *at() dirfd.  The object kind is not known until
     * filed completes this open, so request the directory-only capabilities
     * up front.  Filed still rejects lookup/create/remove/rename when the
     * resulting vnode is not a directory.
     */
    uint64_t optional = FILED_RIGHT_SETATTR |
        FILED_RIGHT_LOOKUP |
        FILED_RIGHT_CREATE |
        FILED_RIGHT_REMOVE |
        FILED_RIGHT_RENAME | FILED_RIGHT_GETDENTS;
    /* Optional directory/metadata rights may only come from this launch's
     * namespace grant. Required read/write/create rights remain explicit, so
     * an unauthorized access mode fails at open instead of being downgraded. */
    uint64_t rights = FILED_RIGHT_STAT | (optional & lpr_state.process.filed_rights);
    const uint64_t accmode = flags & LPR_LINUX_O_ACCMODE;
    if (accmode != LPR_LINUX_O_WRONLY) {
        if ((flags & LPR_LINUX_O_DIRECTORY) == 0) {
            rights |= FILED_RIGHT_READ;
        }
    }
    if (accmode == LPR_LINUX_O_WRONLY || accmode == LPR_LINUX_O_RDWR) {
        rights |= FILED_RIGHT_WRITE;
    }
    if ((flags & LPR_LINUX_O_DIRECTORY) != 0)
        rights |= FILED_RIGHT_GETDENTS;
    if ((flags & LPR_LINUX_O_CREAT) != 0) {
        rights |= FILED_RIGHT_CREATE | FILED_RIGHT_WRITE;
    }
    return rights;
}

uint64_t lpr_open_flags(uint64_t flags) { return flags; }


int64_t lpr_copy_path(char *out, uint64_t capacity, const char *path)
{
    if (out == 0 || path == 0) {
        return -LPR_LINUX_EFAULT;
    }
    if (capacity == 0) {
        return -LPR_LINUX_EINVAL;
    }
    const size_t len = lpr_strnlen(path, capacity);
    if (len == capacity) {
        return -LPR_LINUX_ENAMETOOLONG;
    }
    lpr_memset(out, 0, capacity);
    lpr_memcpy(out, path, len + 1u);
    return 0;
}

int64_t lpr_dir_handle_for(uint64_t dirfd, const char *path, uint64_t *out)
{
    if (out == 0 || path == 0) {
        return -LPR_LINUX_EFAULT;
    }
    if (path[0] == '/') {
        *out = 0;
        return 0;
    }
    /* A fork/exec lease names an escrow handle until its connection adopts
     * it. Binding inside lpr_fs_call is too late after this value is copied. */
    const int64_t binding_status = lpr_filed_endpoint_ready();
    if (binding_status) return binding_status;
    if ((int64_t)dirfd == LPR_LINUX_AT_FDCWD) {
        lpr_cwd_init();
        *out = lpr_cwd_handle;
        return 0;
    }
    if (!lpr_fd_is_filed(dirfd)) {
        return -LPR_LINUX_EBADF;
    }
    *out = lpr_filed_backend(dirfd)->handle;
    return 0;
}

int64_t lpr_filed_close_handle(uint64_t handle)
{
    kb2_fs_request_t request = {.opcode = KB2_FILESYSTEM_OP_CLOSE, .handle = handle};
    kb2_fs_response_t response;
    return lpr_fs_call(&request, 0, 0, 0, 0, 0, 0, &response, -1, 0);
}


int64_t lpr_filed_dup_handle(uint64_t handle, uint64_t fd_flags, uint64_t *out_handle)
{
    if (!out_handle) return -LPR_LINUX_EFAULT;
    *out_handle = 0;
    if (!handle) return 0;
    kb2_fs_request_t request = {.opcode = KB2_FILESYSTEM_OP_DUP,
        .handle = handle, .flags = fd_flags};
    kb2_fs_response_t response;
    const int64_t status = lpr_fs_call(&request, 0, 0, 0, 0, 0, 0, &response, -1, 0);
    if (!status) *out_handle = response.handle;
    return status;
}


int64_t lpr_filed_transfer_dup_handle(
    uint64_t handle, uint64_t fd_flags, pacha_fd_t lease_fd, uint64_t *out_handle)
{
    if (!out_handle || !handle || lease_fd < 16) return -LPR_LINUX_EINVAL;
    *out_handle = 0;
    kb2_fs_request_t request = {.opcode = KB2_FILESYSTEM_OP_TRANSFER_DUP,
        .handle = handle, .flags = fd_flags};
    kb2_fs_response_t response;
    const int64_t status = lpr_fs_call(&request, 0, 0, 0, 0, 0, 0,
        &response, lease_fd, 0);
    if (!status) *out_handle = response.handle;
    return status;
}


int64_t lpr_linux_fsync(uint64_t fd)
{
    const int64_t binding_status = lpr_filed_fd_binding(fd);
    if (binding_status) return binding_status;
    const lpr_filed_backend_t *file = lpr_filed_backend(fd);
    if (!file) return lpr_block_backend(fd) ? lpr_block_flush(fd) : -LPR_LINUX_EINVAL;
    kb2_fs_request_t request = {.opcode = KB2_FILESYSTEM_OP_FSYNC, .handle = file->handle};
    kb2_fs_response_t response;
    return lpr_fs_call(&request, 0, 0, 0, 0, 0, 0, &response, -1, 0);
}


int64_t lpr_linux_ftruncate(uint64_t fd, uint64_t length)
{
    if (lpr_block_backend(fd)) return length == (uint64_t)lpr_block_size(fd) ?
        0 : -LPR_LINUX_EPERM;
    const int64_t binding_status = lpr_filed_fd_binding(fd);
    if (binding_status) return binding_status;
    const lpr_filed_backend_t *file = lpr_filed_backend(fd);
    if (!file) return -LPR_LINUX_EINVAL;
    kb2_fs_request_t request = {.opcode = KB2_FILESYSTEM_OP_FTRUNCATE,
        .handle = file->handle, .offset = (int64_t)length};
    kb2_fs_response_t response;
    return lpr_fs_call(&request, 0, 0, 0, 0, 0, 0, &response, -1, 0);
}


int64_t lpr_linux_memfd_create(uint64_t name_raw, uint64_t flags)
{
    if (!name_raw) return -LPR_LINUX_EFAULT;
    kb2_fs_request_t request = {.opcode = KB2_FILESYSTEM_OP_MEMFD_CREATE, .flags = flags};
    kb2_fs_response_t response;
    int64_t status = lpr_fs_call(&request, (const char *)(uintptr_t)name_raw,
        0, 0, 0, 0, 0, &response, -1, 0);
    if (status) return status;
    const uint64_t open_flags = LPR_LINUX_O_RDWR |
        ((flags & LPR_LINUX_MFD_CLOEXEC) ? LPR_LINUX_O_CLOEXEC : 0);
    const int fd = lpr_fd_alloc(response.handle, open_flags);
    if (fd < 0) (void)lpr_filed_close_handle(response.handle);
    return fd;
}


int64_t lpr_linux_sync(void)
{
    kb2_fs_request_t request = {.opcode = KB2_FILESYSTEM_OP_SYNC};
    kb2_fs_response_t response;
    return lpr_fs_call(&request, 0, 0, 0, 0, 0, 0, &response, -1, 0);
}


int64_t lpr_linux_syncfs(uint64_t fd)
{
    const int64_t binding_status = lpr_filed_fd_binding(fd);
    if (binding_status) return binding_status;
    const lpr_filed_backend_t *file = lpr_filed_backend(fd);
    if (!file) return -LPR_LINUX_EBADF;
    kb2_fs_request_t request = {.opcode = KB2_FILESYSTEM_OP_SYNCFS, .handle = file->handle};
    kb2_fs_response_t response;
    return lpr_fs_call(&request, 0, 0, 0, 0, 0, 0, &response, -1, 0);
}


int64_t lpr_linux_mkdirat(uint64_t dirfd, uint64_t path_raw, uint64_t mode)
{
    const char *path = (const char *)(uintptr_t)path_raw;
    uint64_t handle;
    const int64_t status = lpr_dir_handle_for(dirfd, path, &handle);
    if (status) return status;
    kb2_fs_request_t request = {.opcode = KB2_FILESYSTEM_OP_MKDIRAT, .handle = handle, .mode = mode, .umask = lpr_linux_umask_value};
    kb2_fs_response_t response;
    return lpr_fs_call(&request, path, 0, 0, 0, 0, 0, &response, -1, 0);
}


int64_t lpr_linux_unlinkat(uint64_t dirfd, uint64_t path_raw, uint64_t flags)
{
    const char *path = (const char *)(uintptr_t)path_raw;
    uint64_t handle;
    const int64_t status = lpr_dir_handle_for(dirfd, path, &handle);
    if (status) return status;
    kb2_fs_request_t request = {.opcode = KB2_FILESYSTEM_OP_UNLINKAT, .handle = handle, .flags = flags};
    kb2_fs_response_t response;
    return lpr_fs_call(&request, path, 0, 0, 0, 0, 0, &response, -1, 0);
}


int64_t lpr_linux_renameat(uint64_t old_dirfd, uint64_t old_path_raw,
    uint64_t new_dirfd, uint64_t new_path_raw)
{
    const char *from = (const char *)(uintptr_t)old_path_raw;
    const char *to = (const char *)(uintptr_t)new_path_raw;
    uint64_t first, second;
    int64_t status = lpr_dir_handle_for(old_dirfd, from, &first);
    if (!status) status = lpr_dir_handle_for(new_dirfd, to, &second);
    if (status) return status;
    kb2_fs_request_t request = {.opcode = KB2_FILESYSTEM_OP_RENAMEAT2,
        .handle = first, .second_handle = second, .flags = 0};
    kb2_fs_response_t response;
    return lpr_fs_call(&request, from, to, 0, 0, 0, 0, &response, -1, 0);
}


int64_t lpr_linux_fchownat(uint64_t dirfd, uint64_t path_raw, uint64_t owner, uint64_t group, uint64_t flags)
{
    const char *path = (const char *)(uintptr_t)path_raw;
    uint64_t handle;
    const int64_t status = lpr_dir_handle_for(dirfd, path, &handle);
    if (status) return status;
    kb2_fs_request_t request = {.opcode = KB2_FILESYSTEM_OP_FCHOWNAT, .handle = handle, .uid = (uint32_t)owner, .gid = (uint32_t)group, .flags = flags};
    kb2_fs_response_t response;
    return lpr_fs_call(&request, path, 0, 0, 0, 0, 0, &response, -1, 0);
}


int64_t lpr_linux_mknodat(uint64_t dirfd, uint64_t path_raw, uint64_t mode, uint64_t dev)
{
    const char *path = (const char *)(uintptr_t)path_raw;
    uint64_t handle;
    const int64_t status = lpr_dir_handle_for(dirfd, path, &handle);
    if (status) return status;
    kb2_fs_request_t request = {.opcode = KB2_FILESYSTEM_OP_MKNODAT, .handle = handle, .mode = mode, .length = dev, .umask = lpr_linux_umask_value};
    kb2_fs_response_t response;
    return lpr_fs_call(&request, path, 0, 0, 0, 0, 0, &response, -1, 0);
}


int64_t lpr_linux_readlinkat_to_buffer(uint64_t dirfd, uint64_t path_raw, char *target, uint64_t capacity);
int lpr_resolve_final_symlink_path(const char *path, const char *target, char *out, uint64_t capacity);

int64_t lpr_linux_symlinkat(uint64_t target_raw, uint64_t dirfd, uint64_t path_raw)
{
    const char *path = (const char *)(uintptr_t)path_raw;
    if (!target_raw) return -LPR_LINUX_EFAULT;
    uint64_t handle;
    const int64_t status = lpr_dir_handle_for(dirfd, path, &handle);
    if (status) return status;
    kb2_fs_request_t request = {.opcode = KB2_FILESYSTEM_OP_SYMLINKAT, .handle = handle};
    kb2_fs_response_t response;
    /* Linux interprets the target only when following the created symlink;
     * the ABI keeps those literal bytes first and the new link name second. */
    return lpr_fs_call(&request, (const char *)(uintptr_t)target_raw, path,
        0, 0, 0, 0, &response, -1, 0);
}


int64_t lpr_linux_linkat(uint64_t old_dirfd, uint64_t old_path_raw,
    uint64_t new_dirfd, uint64_t new_path_raw, uint64_t flags)
{
    const char *from = (const char *)(uintptr_t)old_path_raw;
    const char *to = (const char *)(uintptr_t)new_path_raw;
    uint64_t first, second;
    int64_t status = lpr_dir_handle_for(old_dirfd, from, &first);
    if (!status) status = lpr_dir_handle_for(new_dirfd, to, &second);
    if (status) return status;
    kb2_fs_request_t request = {.opcode = KB2_FILESYSTEM_OP_LINKAT,
        .handle = first, .second_handle = second, .flags = flags};
    kb2_fs_response_t response;
    return lpr_fs_call(&request, from, to, 0, 0, 0, 0, &response, -1, 0);
}


static int64_t filed_open_handle_at(uint64_t dirfd, const char *path,
    uint64_t flags, uint64_t mode, uint64_t *out_handle, uint64_t *out_kind,
    uint8_t *route_stat)
{
    if (!out_handle) return -LPR_LINUX_EFAULT;
    *out_handle = 0;
    if (out_kind) *out_kind = 0;
    uint64_t dir_handle;
    const int64_t status = lpr_dir_handle_for(dirfd, path, &dir_handle);
    if (status) return status;
    kb2_fs_request_t request = {.opcode = KB2_FILESYSTEM_OP_OPENAT2,
        .handle = dir_handle, .flags = flags,
        .mode = (flags & (LPR_LINUX_O_CREAT | 020000000ull)) ? mode : 0,
        .mask = route_stat ? 0x7ffu : 0, /* STATX_BASIC_STATS */
        .umask = lpr_linux_umask_value};
    kb2_fs_response_t response;
    int64_t opened = lpr_fs_call(&request, path, 0, 0, 0, route_stat,
        route_stat ? KB2_FILESYSTEM_STATX_SIZE : 0, &response, -1, 0);
    if (out_kind && route_stat &&
        (opened == -LPR_LINUX_ENXIO || opened == -LPR_LINUX_ENODEV)) {
        /* Only a side-effect-free device/service lookup may follow a direct
         * open failure. Never resend ordinary OPENAT2 through the control plane. */
        request.opcode = KB2_FILESYSTEM_OP_DEVICE_ROUTE;
        int64_t routed = lpr_fs_call(&request, path, 0, 0, 0, route_stat,
            KB2_FILESYSTEM_STATX_SIZE, &response, -1, 0);
        if (!routed) {
            if (!response.handle || !response.service ||
                response.data_length != KB2_FILESYSTEM_STATX_SIZE) {
                if (response.handle) (void)lpr_filed_close_handle(response.handle);
                return -LPR_LINUX_EIO;
            }
            opened = 0;
        }
    }
    if (opened) return opened;
    if (route_stat && response.data_length != KB2_FILESYSTEM_STATX_SIZE) {
        if (response.handle) (void)lpr_filed_close_handle(response.handle);
        return -LPR_LINUX_EIO;
    }
    *out_handle = response.handle;
    if (out_kind) *out_kind = response.service;
    return 0;
}

int64_t lpr_filed_open_handle_at(uint64_t dirfd, const char *path,
    uint64_t flags, uint64_t mode, uint64_t *out_handle, uint64_t *out_kind)
{
    uint8_t route_stat[KB2_FILESYSTEM_STATX_SIZE];
    return filed_open_handle_at(dirfd,path,flags,mode,out_handle,out_kind,route_stat);
}

int64_t lpr_filed_device_open_route(uint32_t service, const uint8_t *statx, uint64_t flags)
{
    uint32_t major = 0, minor = 0;
    for (unsigned i = 0; i < 4; ++i) {
        major |= (uint32_t)statx[KB2_FILESYSTEM_STATX_RDEV_MAJOR_OFFSET + i] << (8 * i);
        minor |= (uint32_t)statx[KB2_FILESYSTEM_STATX_RDEV_MINOR_OFFSET + i] << (8 * i);
    }
    uint32_t mode = statx[KB2_FILESYSTEM_STATX_MODE_OFFSET] |
        (uint32_t)statx[KB2_FILESYSTEM_STATX_MODE_OFFSET + 1] << 8;
    if ((mode & LPR_LINUX_S_IFMT) != LPR_LINUX_S_IFCHR) return -LPR_LINUX_ENODEV;
    /* Classify the exact pinned device, not a re-resolved alias pathname.
     * Only device numbers supported by the existing service APIs are routed. */
    if (service == FILED_SERVICE_GPUD_DRM && major == 226) {
        if (minor == 0) return lpr_drm_open_path("/dev/dri/card0",flags);
        if (minor == 128) return lpr_drm_open_path("/dev/dri/renderD128",flags);
    } else if (service == FILED_SERVICE_INPUTD_INPUT && major == 13 && minor >= 64) {
        char path[32] = "/dev/input/event";
        size_t at = sizeof("/dev/input/event") - 1;
        /* inputd publishes eventN at minor 64+N in the shared FS namespace;
         * do not impose evdev's old 32-node legacy range on host devices. */
        uint32_t event = minor - 64;
        char digits[10]; unsigned count = 0;
        do { digits[count++] = (char)('0' + event % 10); event /= 10; } while (event);
        while (count) path[at++] = digits[--count];
        path[at] = 0;
        return lpr_input_open_path(path,flags);
    } else if (service == FILED_SERVICE_TERMD_TTY && major == 5) {
        if (minor == 0) return lpr_tty_open_path("/dev/tty",flags);
        if (minor == 1) return lpr_tty_open_path("/dev/console",flags);
        if (minor == 2) return lpr_tty_open_path("/dev/ptmx",flags);
    }
    return -LPR_LINUX_ENODEV;
}


static int lpr_linux_proc_self_fd_number(const char *path, uint64_t *out_fd)
{
    static const char prefix[] = "/proc/self/fd/";
    if (path == 0 || out_fd == 0) return 0;
    for (size_t i = 0; i + 1u < sizeof(prefix); i += 1u) {
        if (path[i] != prefix[i]) {
            return 0;
        }
    }
    const char *digits = path + sizeof(prefix) - 1u;
    if (*digits < '0' || *digits > '9') {
        return 0;
    }
    uint64_t source_fd = 0;
    for (const char *cursor = digits; *cursor != '\0'; cursor += 1) {
        if (*cursor < '0' || *cursor > '9') {
            return 0;
        }
        const uint64_t digit = (uint64_t)(*cursor - '0');
        if (source_fd > (LPR_LINUX_FD_MAX - digit) / 10u) {
            return 0;
        }
        source_fd = source_fd * 10u + digit;
    }
    *out_fd = source_fd;
    return 1;
}

static int64_t lpr_linux_proc_self_fd_open(const char *path, uint64_t flags)
{
    const uint64_t known_flags =
        LPR_LINUX_O_ACCMODE |
        LPR_LINUX_O_APPEND |
        LPR_LINUX_O_NONBLOCK |
        LPR_LINUX_O_LARGEFILE |
        LPR_LINUX_O_CLOEXEC;
    if (path == 0) {
        return -LPR_LINUX_EFAULT;
    }
    uint64_t source_fd = 0;
    if (!lpr_linux_proc_self_fd_number(path, &source_fd)) {
        return -LPR_LINUX_ENOENT;
    }
    if ((flags & ~known_flags) != 0 ||
        (flags & LPR_LINUX_O_ACCMODE) == LPR_LINUX_O_ACCMODE)
    {
        return -LPR_LINUX_EINVAL;
    }
    if (!lpr_fd_is_filed(source_fd)) {
        return -LPR_LINUX_ENOENT;
    }

    const int64_t binding_status = lpr_filed_endpoint_ready();
    if (binding_status) return binding_status;

    const lpr_filed_backend_t *source = lpr_filed_backend(source_fd);
    const uint64_t access = flags & LPR_LINUX_O_ACCMODE;
    if (lpr_memfd_write_is_sealed(source->reserved1) &&
        access != LPR_LINUX_O_RDONLY)
    {
        return -LPR_LINUX_EPERM;
    }

    uint64_t handle = 0;
    const uint64_t filed_flags =
        (flags & LPR_LINUX_O_CLOEXEC) != 0 ? FILED_FD_CLOEXEC : 0;
    const int64_t status = lpr_filed_dup_handle(
        source->handle, filed_flags, &handle);
    if (status != 0) {
        return status;
    }
    const int fd = lpr_fd_alloc(handle, flags);
    if (fd < 0) {
        (void)lpr_filed_close_handle(handle);
        return fd;
    }
    lpr_filed_backend_t *reopened = lpr_filed_backend((uint64_t)(uint32_t)fd);
    if (reopened == 0) {
        (void)lpr_linux_close((uint64_t)(uint32_t)fd);
        return -LPR_LINUX_EIO;
    }
    reopened->reserved1 = source->reserved1;
    return fd;
}

int64_t lpr_linux_openat_once(
    uint64_t dirfd,
    uint64_t path_raw,
    uint64_t flags,
    uint64_t mode,
    uint64_t *out_kind, unsigned *wait_not_ready __attribute__((unused)))
{
    if (out_kind != 0) {
        *out_kind = 0;
    }
    const char *path = (const char *)(uintptr_t)path_raw;
    const int64_t proc_snapshot_fd = lpr_linux_proc_snapshot_open(path, flags, wait_not_ready);
    if (proc_snapshot_fd != -LPR_LINUX_ENOENT) {
        return proc_snapshot_fd;
    }
    const int64_t proc_fd = lpr_linux_proc_self_fd_open(path, flags);
    if (proc_fd != -LPR_LINUX_ENOENT) {
        return proc_fd;
    }
    const int64_t drm_fd = lpr_drm_open_path(path, flags);
    if (drm_fd != -LPR_LINUX_ENOENT) {
        return drm_fd;
    }
    const int64_t input_fd = lpr_input_open_path(path, flags);
    if (input_fd != -LPR_LINUX_ENOENT) {
        return input_fd;
    }
    const int64_t tty_fd = lpr_tty_open_path(path, flags);
    if (tty_fd != -LPR_LINUX_ENOENT) {
        return tty_fd;
    }
    const int64_t block_fd = lpr_block_open_path(path, flags);
    if (block_fd != -LPR_LINUX_ENOENT) return block_fd;
    uint8_t device_minor = 0;
    if (path != 0) {
        if (lpr_strcmp(path, "/dev/null") == 0) {
            device_minor = 3;
        } else if (lpr_strcmp(path, "/dev/zero") == 0) {
            device_minor = 5;
        } else if (lpr_strcmp(path, "/dev/random") == 0) {
            device_minor = 8;
        } else if (lpr_strcmp(path, "/dev/urandom") == 0) {
            device_minor = 9;
        }
    }
    if (device_minor != 0) {
        if ((flags & LPR_LINUX_O_DIRECTORY) != 0) {
            return -LPR_LINUX_ENOTDIR;
        }
        if ((flags & (LPR_LINUX_O_CREAT | LPR_LINUX_O_EXCL)) ==
            (LPR_LINUX_O_CREAT | LPR_LINUX_O_EXCL))
        {
            return -LPR_LINUX_EEXIST;
        }
        const lpr_device_backend_t device = {
            .active = 1, .major = 1, .minor = device_minor,
            .flags = (uint32_t)flags,
            .description_fd = { .raw = -1 },
        };
        return lpr_fd_alloc_state(LPR_FD_OPS_DEVICE, flags, 0,
            &device, sizeof(device));
    }
    uint64_t handle = 0;
    uint64_t opened_kind = 0;
    uint8_t route_stat[KB2_FILESYSTEM_STATX_SIZE];
    const int64_t status = filed_open_handle_at(
        dirfd, path, flags, mode, &handle, &opened_kind,route_stat);
    if (status != 0) {
        return status;
    }
    if (opened_kind) {
        int64_t routed = lpr_filed_device_open_route((uint32_t)opened_kind,route_stat,flags);
        const int64_t closed = lpr_filed_close_handle(handle);
        if (closed) {
            if (routed >= 0) (void)lpr_linux_close((uint64_t)routed);
            return closed;
        }
        if (routed >= 0 && out_kind) *out_kind = opened_kind;
        return routed;
    }
    const int fd = lpr_fd_alloc(handle, flags);
    if (fd < 0) {
        (void)lpr_filed_close_handle(handle);
        return fd;
    }
    /* Classify the opened inode, including aliases and symlinks, using the
     * OPEN reply. A pathname guess is insufficient, while a separate FSTAT
     * imposed another complete IPC/scheduling round trip on every open. */
    uint32_t opened_mode = route_stat[KB2_FILESYSTEM_STATX_MODE_OFFSET] |
        (uint32_t)route_stat[KB2_FILESYSTEM_STATX_MODE_OFFSET + 1] << 8;
    uint32_t major = 0, minor = 0;
    for (unsigned i = 0; i < 4; ++i) {
        major |= (uint32_t)route_stat[KB2_FILESYSTEM_STATX_RDEV_MAJOR_OFFSET + i] << (8 * i);
        minor |= (uint32_t)route_stat[KB2_FILESYSTEM_STATX_RDEV_MINOR_OFFSET + i] << (8 * i);
    }
    if ((opened_mode & LPR_LINUX_S_IFMT) == LPR_LINUX_S_IFCHR && major == 1 &&
        (minor == 3 || minor == 5 || minor == 8 || minor == 9))
    {
        const int64_t close_status = lpr_linux_close((uint64_t)(uint32_t)fd);
        if (close_status != 0) return close_status;
        const uint64_t device_id = (1ull << 32u) | minor;
        const int install = lpr_control_install_fd(
            (uint64_t)(uint32_t)fd, LPR_FD_OPS_DEVICE, flags, device_id, 0);
        if (install != 0) return install;
        lpr_device_backend_t *device = lpr_device_backend((uint64_t)(uint32_t)fd);
        if (device == 0) {
            lpr_control_close_fd((uint64_t)(uint32_t)fd);
            return -LPR_LINUX_EIO;
        }
        device->active = 1;
        device->major = 1;
        device->minor = (uint8_t)minor;
        device->flags = (uint32_t)flags;
    }
    if (out_kind != 0) {
        *out_kind = opened_kind;
    }
    return fd;
}

int64_t lpr_linux_readlinkat_to_buffer(uint64_t dirfd, uint64_t path_raw, char *target, uint64_t capacity)
{
    if (path_raw == 0 || target == 0) {
        return -LPR_LINUX_EFAULT;
    }
    if (capacity == 0) return -LPR_LINUX_EINVAL;
    const char *path = (const char *)(uintptr_t)path_raw;
    int64_t proc_status = 0;
    if (lpr_linux_proc_readlink(path, target, capacity, &proc_status))
        return proc_status;
    uint64_t source_fd = 0;
    if (lpr_linux_proc_self_fd_number(path, &source_fd)) {
        const int64_t binding_status = lpr_filed_fd_binding(source_fd);
        if (binding_status) return binding_status;
        const lpr_filed_backend_t *source = lpr_filed_backend(source_fd);
        if (source == 0) {
            return -LPR_LINUX_ENOENT;
        }
        return lpr_filed_handle_path(source->handle, target, capacity, 0);
    }
    uint64_t dir_handle;
    const int64_t status = lpr_dir_handle_for(dirfd, path, &dir_handle);
    if (status) return status;
    kb2_fs_request_t request = {.opcode = KB2_FILESYSTEM_OP_READLINKAT,
        .handle = dir_handle, .length = capacity};
    kb2_fs_response_t response;
    return lpr_fs_call(&request, path, 0, 0, 0, target, capacity, &response, -1, 0);
}

int lpr_resolve_final_symlink_path(const char *path, const char *target, char *out, uint64_t capacity)
{
    if (path == 0 || target == 0 || out == 0) {
        return 0;
    }
    if (capacity == 0) {
        return 0;
    }
    const uint64_t target_len = (uint64_t)lpr_strnlen(target, capacity);
    if (target_len == 0 || target_len >= capacity) {
        return 0;
    }
    lpr_memset(out, 0, capacity);
    if (target[0] == '/') {
        lpr_memcpy(out, target, target_len + 1u);
        return 1;
    }
    uint64_t prefix_len = 0;
    for (uint64_t i = 0; path[i] != 0 && i < capacity; i += 1) {
        if (path[i] == '/') {
            prefix_len = i + 1u;
        }
    }
    if (prefix_len + target_len >= capacity) {
        return 0;
    }
    if (prefix_len != 0) {
        lpr_memcpy(out, path, prefix_len);
    }
    lpr_memcpy(out + prefix_len, target, target_len + 1u);
    return 1;
}

int64_t lpr_linux_openat(uint64_t dirfd, uint64_t path_raw, uint64_t flags, uint64_t mode, unsigned *wait_not_ready __attribute__((unused)))
{
    /* Linux namei follows symlinks within its namespace. Personality path
     * concatenation cannot reproduce mount, dirfd or symlink escape rules. */
    return lpr_linux_openat_once(dirfd, path_raw, flags, mode, 0, wait_not_ready);
}


int64_t lpr_filed_handle_path(uint64_t handle, char *out, uint64_t capacity, int cwd)
{
    kb2_fs_request_t request = {.opcode = KB2_FILESYSTEM_OP_READLINK_HANDLE,
        .handle = handle, .length = capacity, .flags = cwd ? 1u : 0u};
    kb2_fs_response_t response;
    const int64_t result = lpr_fs_call(&request, 0, 0, 0, 0, out,
        capacity, &response, -1, 0);
    if (result < 0) return result;
    if ((uint64_t)result > capacity || response.data_length != (uint64_t)result ||
        (cwd && (uint64_t)result >= capacity))
        return -LPR_LINUX_EIO;
    if (cwd) out[result] = 0;
    return result;
}

int64_t lpr_linux_getcwd(uint64_t buf, uint64_t size)
{
    if (!buf) return -LPR_LINUX_EFAULT;
    if (!size) return -LPR_LINUX_ERANGE;
    lpr_cwd_init();
    if (!lpr_cwd_handle) {
        if (size < 2u) return -LPR_LINUX_ERANGE;
        lpr_memcpy((void *)(uintptr_t)buf, "/", 2);
        return 2;
    }
    const int64_t binding_status = lpr_filed_endpoint_ready();
    if (binding_status) return binding_status;
    const int64_t result = lpr_filed_handle_path(lpr_cwd_handle,
        (char *)(uintptr_t)buf, size, 1);
    return result < 0 ? result : result + 1;
}

int64_t lpr_supervisor_cwd_set(uint64_t handle, const char *path)
{
    if (!lpr_supervisor_enabled) {
        return 0;
    }
    void *page = 0;
    const pacha_fd_t page_fd = lpr_create_standalone_wire_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }
    lpr_memset(page, 0, PACHA_SERVICE_PAGE_BYTES);
    lprs_cwd_t *cwd = (lprs_cwd_t *)lpr_supervisor_payload(page);
    cwd->token = lpr_supervisor_token;
    cwd->cwd_handle = handle;
    if (path != 0) {
        const uint64_t len = (uint64_t)lpr_strnlen(path, LPRS_CWD_BYTES);
        if (len >= LPRS_CWD_BYTES) {
            lpr_destroy_standalone_wire_page(page_fd, page);
            return -LPR_LINUX_ENAMETOOLONG;
        }
        lpr_memcpy(cwd->cwd, path, (size_t)len + 1u);
    }
    const int64_t status = lpr_supervisor_call(
        LPRS_OP_CWD_SET,
        page_fd,
        page,
        sizeof(*cwd),
        -1,
        0);
    lpr_destroy_standalone_wire_page(page_fd, page);
    return status;
}

int64_t lpr_cwd_install(uint64_t handle)
{
    lpr_cwd_init();
    const int64_t binding_status = lpr_filed_endpoint_ready();
    if (binding_status) return binding_status;
    const uint64_t old_handle = lpr_cwd_handle;
    const uint64_t old_lease_handle = lpr_cwd_lease_handle;
    const pacha_fd_t old_lease_fd = lpr_cwd_lease_fd;
    const int root = handle == 0;
    uint64_t installed_handle = 0;
    uint64_t installed_escrow = 0;
    pacha_fd_t installed_lease_fd = -1;
    if (!root) {
        pacha_fd_t remote_lease_fd = -1;
        int status = lpr_filed_lease_pair(
            &installed_lease_fd,
            &remote_lease_fd);
        if (status == 0) {
            status = (int)lpr_filed_transfer_dup_handle(
                handle,
                0,
                remote_lease_fd,
                &installed_handle);
            installed_escrow = installed_handle;
            if (!status) status = lpr_filed_adopt(&installed_handle, installed_lease_fd);
        }
        if (status != 0 || installed_handle == 0) {
            (void)lpr_close_native_fd_if_open(
                (uint64_t)installed_lease_fd);
            (void)lpr_close_native_fd_if_open(
                (uint64_t)remote_lease_fd);
            (void)lpr_filed_close_handle(handle);
            return status != 0 ? status : -LPR_LINUX_EIO;
        }
        /* The process-wide cwd must outlive a reconnect of the filed RPC
         * session.  Keep the duplicate alive through its explicit lease and
         * retire the short-lived session-owned handle returned by open/dup. */
        (void)lpr_filed_close_handle(handle);
    }
    if (root) {
        lpr_cwd_handle = 0;
        lpr_cwd_lease_handle = 0;
        lpr_cwd_lease_fd = -1;
        if (handle != 0) {
            (void)lpr_filed_close_handle(handle);
        }
    } else {
        lpr_cwd_handle = installed_handle;
        lpr_cwd_lease_handle = installed_escrow;
        lpr_cwd_lease_fd = installed_lease_fd;
    }
    const int64_t supervisor_status = lpr_supervisor_cwd_set(lpr_cwd_handle, 0);
    if (supervisor_status != 0) {
        if (lpr_cwd_handle != 0 && lpr_cwd_handle != old_handle) {
            (void)lpr_filed_close_handle(lpr_cwd_handle);
            if (lpr_cwd_lease_fd >= 16)
                (void)lpr_close_native_fd_if_open(
                    (uint64_t)lpr_cwd_lease_fd);
        }
        lpr_cwd_handle = old_handle;
        lpr_cwd_lease_handle = old_lease_handle;
        lpr_cwd_lease_fd = old_lease_fd;
        return supervisor_status;
    }
    if (old_handle != 0) {
        (void)lpr_filed_close_handle(old_handle);
        if (old_lease_fd >= 16)
            (void)lpr_close_native_fd_if_open(
                (uint64_t)old_lease_fd);
    }
    return 0;
}

int64_t lpr_linux_chdir(uint64_t path_raw)
{
    const char *path = (const char *)(uintptr_t)path_raw;
    if (path == 0) {
        return -LPR_LINUX_EFAULT;
    }
    uint64_t handle = 0;
    int64_t status = lpr_filed_open_handle_at(
        (uint64_t)(int64_t)LPR_LINUX_AT_FDCWD,
        path,
        LPR_LINUX_O_RDONLY | LPR_LINUX_O_DIRECTORY,
        0,
        &handle,
        0);
    if (status != 0) {
        return status;
    }
    return lpr_cwd_install(handle);
}

int64_t lpr_linux_fchdir(uint64_t fd, unsigned *wait_not_ready __attribute__((unused)))
{
    if (!lpr_fd_is_filed(fd)) {
        return -LPR_LINUX_EBADF;
    }
    const int64_t binding_status = lpr_filed_endpoint_ready();
    if (binding_status) return binding_status;
    lpr_linux_stat_t st;
    const int64_t stat_status = lpr_linux_fstat(fd, (uint64_t)(uintptr_t)&st, wait_not_ready);
    if (stat_status != 0) {
        return stat_status;
    }
    if ((((uint64_t)st.st_mode) & LPR_LINUX_S_IFMT) != LPR_LINUX_S_IFDIR) {
        return -LPR_LINUX_ENOTDIR;
    }
    uint64_t dup_handle = 0;
    const int64_t dup_status = lpr_filed_dup_handle(lpr_filed_backend(fd)->handle, 0, &dup_handle);
    if (dup_status != 0) {
        return dup_status;
    }
    return lpr_cwd_install(dup_handle);
}

int64_t lpr_linux_validate_timespec(const struct pachaos_timespec *ts)
{
    if (ts == 0) {
        return -LPR_LINUX_EFAULT;
    }
    if ((int64_t)ts->tv_sec < 0 ||
        (int64_t)ts->tv_nsec < 0 ||
        ts->tv_nsec >= 1000000000ull)
    {
        return -LPR_LINUX_EINVAL;
    }
    return 0;
}

int lpr_timespec_less_equal(
    const struct pachaos_timespec *lhs,
    const struct pachaos_timespec *rhs)
{
    if (lhs->tv_sec != rhs->tv_sec) {
        return lhs->tv_sec < rhs->tv_sec;
    }
    return lhs->tv_nsec <= rhs->tv_nsec;
}

void lpr_timespec_subtract(
    const struct pachaos_timespec *end,
    const struct pachaos_timespec *start,
    struct pachaos_timespec *out)
{
    out->tv_sec = end->tv_sec - start->tv_sec;
    if (end->tv_nsec >= start->tv_nsec) {
        out->tv_nsec = end->tv_nsec - start->tv_nsec;
        return;
    }
    out->tv_sec -= 1u;
    out->tv_nsec = 1000000000ull + end->tv_nsec - start->tv_nsec;
}

int64_t lpr_pacha_clock_gettime(uint64_t clock_id, struct pachaos_timespec *out)
{
    lpr_memset(out, 0, sizeof(*out));
    int64_t status;
    do {
        status = lpr_pacha_syscall2(
            PACHAOS_SYSCALL_CLOCK_GETTIME,
            clock_id,
            (uint64_t)(uintptr_t)out);
        if (status == PACHA_SYSCALL_ERR_NOT_READY ||
            status == -PACHA_SYSCALL_ERR_NOT_READY)
        {
            __asm__ volatile("pause");
        }
    } while (status == PACHA_SYSCALL_ERR_NOT_READY ||
             status == -PACHA_SYSCALL_ERR_NOT_READY);
    return status == 0 ? 0 : lpr_pacha_status_to_errno(status);
}

int64_t lpr_pacha_nanosleep(const struct pachaos_timespec *req)
{
    const int64_t valid = lpr_linux_validate_timespec(req);
    if (valid != 0) {
        return valid;
    }
    const int64_t status = lpr_pacha_syscall1(
        PACHAOS_SYSCALL_NANOSLEEP,
        (uint64_t)(uintptr_t)req);
    return status == 0 ? 0 : lpr_pacha_status_to_errno(status);
}

static int64_t lpr_linux_relative_sleep(
    uint64_t clock_id,
    const struct pachaos_timespec *req,
    uint64_t rem_raw, unsigned *wait_not_ready __attribute__((unused)))
{
    struct pachaos_timespec remaining = *req;
    for (;;) {
        struct pachaos_timespec started;
        int64_t status = lpr_pacha_clock_gettime(clock_id, &started);
        if (status != 0) return status;

        status = lpr_pacha_nanosleep(&remaining);
        if (status != -LPR_LINUX_EAGAIN) return status;
        if (wait_not_ready) *wait_not_ready = 1;

        struct pachaos_timespec now;
        status = lpr_pacha_clock_gettime(clock_id, &now);
        if (status != 0) return status;

        struct pachaos_timespec interrupted_remaining = remaining;
        if (lpr_timespec_less_equal(&started, &now)) {
            struct pachaos_timespec elapsed;
            lpr_memset(&elapsed, 0, sizeof(elapsed));
            lpr_timespec_subtract(&now, &started, &elapsed);
            if (lpr_timespec_less_equal(&remaining, &elapsed)) {
                lpr_memset(&interrupted_remaining, 0, sizeof(interrupted_remaining));
            } else {
                lpr_timespec_subtract(&remaining, &elapsed, &interrupted_remaining);
            }
        }
        remaining = interrupted_remaining;
        if (rem_raw != 0) {
            *(struct pachaos_timespec *)(uintptr_t)rem_raw = remaining;
        }
        if (remaining.tv_sec == 0 && remaining.tv_nsec == 0) return 0;

        /* This mirrors the outer blocking-syscall contract without throwing
         * away the elapsed interval: a handled signal abandons this dispatch
         * with EINTR, while an ignored or spurious wake returns here and sleeps
         * only the remaining duration. */
        status = lpr_linux_dispatch_pending_signals_with_result(-LPR_LINUX_EINTR);
        if (status != 0) return status;
        lpr_linux_deliver_native_pending_frame_after_wait(-LPR_LINUX_EINTR, wait_not_ready);
    }
}

static int64_t lpr_linux_absolute_sleep(
    uint64_t clock_id,
    const struct pachaos_timespec *deadline, unsigned *wait_not_ready __attribute__((unused)))
{
    for (;;) {
        struct pachaos_timespec now;
        int64_t status = lpr_pacha_clock_gettime(clock_id, &now);
        if (status != 0) return status;
        if (lpr_timespec_less_equal(deadline, &now)) return 0;

        struct pachaos_timespec relative;
        lpr_memset(&relative, 0, sizeof(relative));
        lpr_timespec_subtract(deadline, &now, &relative);
        status = lpr_pacha_nanosleep(&relative);
        if (status != -LPR_LINUX_EAGAIN) return status;
        if (wait_not_ready) *wait_not_ready = 1;

        status = lpr_linux_dispatch_pending_signals_with_result(-LPR_LINUX_EINTR);
        if (status != 0) return status;
        lpr_linux_deliver_native_pending_frame_after_wait(-LPR_LINUX_EINTR, wait_not_ready);
    }
}

int64_t lpr_linux_nanosleep(uint64_t req_raw, uint64_t rem_raw, unsigned *wait_not_ready __attribute__((unused)))
{
    const struct pachaos_timespec *req = (const struct pachaos_timespec *)(uintptr_t)req_raw;
    const int64_t valid = lpr_linux_validate_timespec(req);
    if (valid != 0) {
        return valid;
    }
    return lpr_linux_relative_sleep(LPR_LINUX_CLOCK_MONOTONIC, req, rem_raw, wait_not_ready);
}

int64_t lpr_linux_clock_nanosleep(uint64_t clock_id, uint64_t flags, uint64_t req_raw, uint64_t rem_raw, unsigned *wait_not_ready __attribute__((unused)))
{
    if (clock_id != LPR_LINUX_CLOCK_REALTIME && clock_id != LPR_LINUX_CLOCK_MONOTONIC) {
        return -LPR_LINUX_EINVAL;
    }
    if ((flags & ~LPR_LINUX_TIMER_ABSTIME) != 0) {
        return -LPR_LINUX_EINVAL;
    }
    const struct pachaos_timespec *req = (const struct pachaos_timespec *)(uintptr_t)req_raw;
    const int64_t valid = lpr_linux_validate_timespec(req);
    if (valid != 0) {
        return valid;
    }
    if ((flags & LPR_LINUX_TIMER_ABSTIME) == 0) {
        return lpr_linux_relative_sleep(LPR_LINUX_CLOCK_MONOTONIC, req, rem_raw, wait_not_ready);
    }

    return lpr_linux_absolute_sleep(clock_id, req, wait_not_ready);
}
