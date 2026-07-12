#include "../lpr_filed_internal.h"

uint64_t lpr_open_rights(uint64_t flags)
{
    uint64_t rights = FILED_RIGHT_STAT;
    const uint64_t accmode = flags & LPR_LINUX_O_ACCMODE;
    if (accmode != LPR_LINUX_O_WRONLY) {
        rights |= FILED_RIGHT_GETDENTS;
        if ((flags & LPR_LINUX_O_DIRECTORY) == 0) {
            rights |= FILED_RIGHT_READ;
        }
    }
    if (accmode == LPR_LINUX_O_WRONLY || accmode == LPR_LINUX_O_RDWR) {
        rights |= FILED_RIGHT_WRITE;
    }
    if ((flags & LPR_LINUX_O_DIRECTORY) != 0) {
        rights |= FILED_RIGHT_LOOKUP | FILED_RIGHT_GETDENTS;
        if (accmode == LPR_LINUX_O_WRONLY || accmode == LPR_LINUX_O_RDWR) {
            rights |= FILED_RIGHT_CREATE | FILED_RIGHT_REMOVE | FILED_RIGHT_RENAME;
        }
    }
    if ((flags & LPR_LINUX_O_CREAT) != 0) {
        rights |= FILED_RIGHT_CREATE | FILED_RIGHT_WRITE;
    }
    return rights;
}

uint64_t lpr_open_flags(uint64_t flags)
{
    uint64_t out = 0;
    if ((flags & LPR_LINUX_O_CREAT) != 0) {
        out |= FILED_OPEN_CREATE;
    }
    if ((flags & LPR_LINUX_O_EXCL) != 0) {
        out |= FILED_OPEN_EXCLUSIVE;
    }
    if ((flags & LPR_LINUX_O_TRUNC) != 0) {
        out |= FILED_OPEN_TRUNCATE;
    }
    if ((flags & LPR_LINUX_O_DIRECTORY) != 0) {
        out |= FILED_OPEN_DIRECTORY;
    }
    if ((flags & LPR_LINUX_O_NOFOLLOW) != 0) {
        out |= FILED_OPEN_NOFOLLOW;
    }
    if ((flags & LPR_LINUX_O_CLOEXEC) != 0) {
        out |= FILED_OPEN_CLOEXEC;
    }
    if ((flags & LPR_LINUX_O_APPEND) != 0) {
        out |= FILED_OPEN_APPEND;
    }
    if ((flags & LPR_LINUX_O_NONBLOCK) != 0) {
        out |= FILED_OPEN_NONBLOCK;
    }
    return out;
}

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
    if ((int64_t)dirfd == LPR_LINUX_AT_FDCWD) {
        lpr_cwd_init();
        *out = lpr_cwd_handle;
        return 0;
    }
    if (!lpr_fd_is_filed(dirfd)) {
        return -LPR_LINUX_EBADF;
    }
    *out = lpr_fd_filed_payload(dirfd)->handle;
    return 0;
}

int64_t lpr_filed_close_handle(uint64_t handle)
{
    uint64_t ignored = 0;
    return lpr_filed_call(FILED_OP_VFS_CLOSE, -1, handle, &ignored);
}

int64_t lpr_filed_dup_handle(uint64_t handle, uint64_t fd_flags, uint64_t *out_handle)
{
    if (out_handle == 0) {
        return -LPR_LINUX_EFAULT;
    }
    *out_handle = 0;
    if (handle == 0) {
        return 0;
    }
    void *page = 0;
    const int page_fd = lpr_create_wire_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }
    filed_handle_flags_t *flags = (filed_handle_flags_t *)page;
    lpr_memset(flags, 0, sizeof(*flags));
    flags->handle = handle;
    flags->fd_flags = fd_flags;
    uint64_t dup_handle = 0;
    const int64_t status = lpr_filed_call(FILED_OP_VFS_DUP, page_fd, 0, &dup_handle);
    lpr_destroy_wire_page(page_fd, page);
    if (status != 0) {
        return status;
    }
    *out_handle = dup_handle;
    return 0;
}

int64_t lpr_linux_fsync(uint64_t fd)
{
    if (!lpr_fd_is_filed(fd)) {
        return -LPR_LINUX_EINVAL;
    }
    uint64_t ignored = 0;
    return lpr_filed_call(FILED_OP_VFS_FSYNC, -1, lpr_fd_filed_payload(fd)->handle, &ignored);
}

int64_t lpr_linux_ftruncate(uint64_t fd, uint64_t length)
{
    if (!lpr_fd_is_filed(fd)) {
        return -LPR_LINUX_EINVAL;
    }
    if ((int64_t)length < 0) {
        return -LPR_LINUX_EINVAL;
    }
    const uint8_t memfd_state = lpr_fd_filed_payload(fd)->reserved1;
    if ((memfd_state & (LPR_FILED_FD_MEMFD | LPR_LINUX_F_SEAL_SHRINK)) ==
        (LPR_FILED_FD_MEMFD | LPR_LINUX_F_SEAL_SHRINK)) {
        lpr_linux_stat_t st;
        lpr_memset(&st, 0, sizeof(st));
        const int64_t stat_status = lpr_linux_fstat(fd, (uint64_t)(uintptr_t)&st);
        if (stat_status != 0) return stat_status;
        if (st.st_size >= 0 && length < (uint64_t)st.st_size) return -LPR_LINUX_EPERM;
    }
    void *page = 0;
    const int page_fd = lpr_create_wire_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }
    filed_truncate_t *truncate = (filed_truncate_t *)page;
    lpr_memset(truncate, 0, sizeof(*truncate));
    truncate->handle = lpr_fd_filed_payload(fd)->handle;
    truncate->size = length;
    uint64_t ignored = 0;
    const int64_t status = lpr_filed_call(FILED_OP_VFS_TRUNCATE, page_fd, 0, &ignored);
    lpr_destroy_wire_page(page_fd, page);
    if (status == 0) {
        lpr_page_cache_clear();
    }
    return status;
}

int64_t lpr_linux_memfd_create(uint64_t name_raw, uint64_t flags)
{
    const uint64_t known_flags =
        LPR_LINUX_MFD_CLOEXEC |
        LPR_LINUX_MFD_ALLOW_SEALING;
    if (name_raw == 0) {
        return -LPR_LINUX_EFAULT;
    }
    if ((flags & ~known_flags) != 0) {
        return -LPR_LINUX_EINVAL;
    }
    const char *name = (const char *)(uintptr_t)name_raw;
    const size_t name_length = lpr_strnlen(name, FILED_MEMFD_NAME_BYTES);
    if (name_length >= FILED_MEMFD_NAME_BYTES) {
        return -LPR_LINUX_EINVAL;
    }

    void *page = 0;
    const int page_fd = lpr_create_wire_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }
    filed_memfd_create_t *memfd = (filed_memfd_create_t *)page;
    lpr_memset(memfd, 0, sizeof(*memfd));
    memfd->flags =
        ((flags & LPR_LINUX_MFD_CLOEXEC) != 0 ? FILED_MEMFD_CLOEXEC : 0) |
        ((flags & LPR_LINUX_MFD_ALLOW_SEALING) != 0 ? FILED_MEMFD_ALLOW_SEALING : 0);
    lpr_memcpy(memfd->name, name, name_length);
    memfd->name[name_length] = '\0';

    uint64_t handle = 0;
    const int64_t status = lpr_filed_call(
        FILED_OP_VFS_MEMFD_CREATE,
        page_fd,
        0,
        &handle);
    lpr_destroy_wire_page(page_fd, page);
    if (status != 0) {
        return status;
    }
    uint64_t open_flags = LPR_LINUX_O_RDWR;
    if ((flags & LPR_LINUX_MFD_CLOEXEC) != 0) {
        open_flags |= LPR_LINUX_O_CLOEXEC;
    }
    const int fd = lpr_fd_alloc(handle, open_flags);
    if (fd < 0) {
        (void)lpr_filed_close_handle(handle);
        return fd;
    }
    lpr_filed_fd_t *file = lpr_fd_filed_payload((uint64_t)(uint32_t)fd);
    if (file != 0) {
        file->reserved1 = LPR_FILED_FD_MEMFD |
            ((flags & LPR_LINUX_MFD_ALLOW_SEALING) != 0 ?
                LPR_FILED_FD_ALLOW_SEALING : LPR_LINUX_F_SEAL_SEAL);
    }
    return fd;
}

int64_t lpr_linux_sync(void)
{
    uint64_t ignored = 0;
    return lpr_filed_call(FILED_OP_VFS_SYNC_ALL, -1, 0, &ignored);
}

int64_t lpr_linux_mkdirat(uint64_t dirfd, uint64_t path_raw, uint64_t mode)
{
    lpr_readlink_cache_clear();
    lpr_page_cache_clear();
    void *page = 0;
    const char *path = (const char *)(uintptr_t)path_raw;
    uint64_t dir_handle = 0;
    int64_t status = lpr_dir_handle_for(dirfd, path, &dir_handle);
    if (status != 0) {
        return status;
    }
    const int page_fd = lpr_create_wire_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }
    filed_mkdir_t *mkdir_req = (filed_mkdir_t *)page;
    lpr_memset(mkdir_req, 0, sizeof(*mkdir_req));
    mkdir_req->dir_handle = dir_handle;
    mkdir_req->mode = mode;
    status = lpr_copy_path(mkdir_req->name, sizeof(mkdir_req->name), path);
    uint64_t ignored = 0;
    if (status == 0) {
        status = lpr_filed_call(FILED_OP_VFS_MKDIR, page_fd, 0, &ignored);
    }
    lpr_destroy_wire_page(page_fd, page);
    return status;
}

int64_t lpr_linux_unlinkat(uint64_t dirfd, uint64_t path_raw, uint64_t flags)
{
    lpr_readlink_cache_clear();
    lpr_page_cache_clear();
    const uint64_t known_flags = LPR_LINUX_AT_REMOVEDIR;
    if ((flags & ~known_flags) != 0) {
        return -LPR_LINUX_EINVAL;
    }
    void *page = 0;
    const char *path = (const char *)(uintptr_t)path_raw;
    uint64_t dir_handle = 0;
    int64_t status = lpr_dir_handle_for(dirfd, path, &dir_handle);
    if (status != 0) {
        return status;
    }
    const int page_fd = lpr_create_wire_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }
    uint32_t op = FILED_OP_VFS_UNLINK;
    if ((flags & LPR_LINUX_AT_REMOVEDIR) != 0) {
        filed_rmdir_t *rmdir_req = (filed_rmdir_t *)page;
        lpr_memset(rmdir_req, 0, sizeof(*rmdir_req));
        rmdir_req->dir_handle = dir_handle;
        status = lpr_copy_path(rmdir_req->name, sizeof(rmdir_req->name), path);
        op = FILED_OP_VFS_RMDIR;
    } else {
        filed_unlink_t *unlink_req = (filed_unlink_t *)page;
        lpr_memset(unlink_req, 0, sizeof(*unlink_req));
        unlink_req->dir_handle = dir_handle;
        status = lpr_copy_path(unlink_req->name, sizeof(unlink_req->name), path);
    }
    uint64_t ignored = 0;
    if (status == 0) {
        status = lpr_filed_call(op, page_fd, 0, &ignored);
    }
    lpr_destroy_wire_page(page_fd, page);
    return status;
}

int64_t lpr_linux_renameat(uint64_t old_dirfd, uint64_t old_path_raw, uint64_t new_dirfd, uint64_t new_path_raw)
{
    lpr_readlink_cache_clear();
    lpr_page_cache_clear();
    void *page = 0;
    const char *old_path = (const char *)(uintptr_t)old_path_raw;
    const char *new_path = (const char *)(uintptr_t)new_path_raw;
    uint64_t old_dir_handle = 0;
    uint64_t new_dir_handle = 0;
    int64_t status = lpr_dir_handle_for(old_dirfd, old_path, &old_dir_handle);
    if (status != 0) {
        return status;
    }
    status = lpr_dir_handle_for(new_dirfd, new_path, &new_dir_handle);
    if (status != 0) {
        return status;
    }
    const int page_fd = lpr_create_wire_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }
    filed_rename_t *rename_req = (filed_rename_t *)page;
    lpr_memset(rename_req, 0, sizeof(*rename_req));
    rename_req->old_dir_handle = old_dir_handle;
    rename_req->new_dir_handle = new_dir_handle;
    status = lpr_copy_path(rename_req->old_name, sizeof(rename_req->old_name), old_path);
    if (status == 0) {
        status = lpr_copy_path(rename_req->new_name, sizeof(rename_req->new_name), new_path);
    }
    uint64_t ignored = 0;
    if (status == 0) {
        status = lpr_filed_call(FILED_OP_VFS_RENAME, page_fd, 0, &ignored);
    }
    lpr_destroy_wire_page(page_fd, page);
    return status;
}

int64_t lpr_linux_fchownat(uint64_t dirfd, uint64_t path, uint64_t owner, uint64_t group, uint64_t flags)
{
    (void)owner;
    (void)group;
    const uint64_t known_flags = LPR_LINUX_AT_SYMLINK_NOFOLLOW | LPR_LINUX_AT_EMPTY_PATH;
    if ((flags & ~known_flags) != 0) {
        return -LPR_LINUX_EINVAL;
    }
    if ((flags & LPR_LINUX_AT_EMPTY_PATH) != 0 && path != 0 && ((const char *)(uintptr_t)path)[0] == 0) {
        return lpr_fd_is_filed(dirfd) || lpr_pipe_fd_is_active(dirfd) ? 0 : -LPR_LINUX_EBADF;
    }
    if ((flags & LPR_LINUX_AT_SYMLINK_NOFOLLOW) != 0) {
        return path != 0 ? 0 : -LPR_LINUX_EFAULT;
    }
    return lpr_linux_faccessat(dirfd, path, 0, flags & LPR_LINUX_AT_SYMLINK_NOFOLLOW);
}

int64_t lpr_linux_mknodat(uint64_t dirfd, uint64_t path, uint64_t mode, uint64_t dev)
{
    const uint64_t type = mode & LPR_LINUX_S_IFMT;
    if (path == 0) return -LPR_LINUX_EFAULT;
    if (type != LPR_LINUX_S_IFIFO && type != LPR_LINUX_S_IFCHR &&
        type != LPR_LINUX_S_IFBLK && type != LPR_LINUX_S_IFSOCK)
    {
        return -LPR_LINUX_EINVAL;
    }
    const char *path_string = (const char *)(uintptr_t)path;
    uint64_t dir_handle = 0;
    int64_t status = lpr_dir_handle_for(dirfd, path_string, &dir_handle);
    if (status != 0) return status;
    void *page = 0;
    const int page_fd = lpr_create_wire_page(&page);
    if (page_fd < 0) return page_fd;
    filed_mknod_t *request = (filed_mknod_t *)page;
    lpr_memset(request, 0, sizeof(*request));
    request->dir_handle = dir_handle;
    request->mode = type | ((mode & 07777ull) & ~lpr_linux_umask_value);
    request->dev = dev;
    status = lpr_copy_path(request->name, sizeof(request->name), path_string);
    uint64_t ignored = 0;
    if (status == 0) {
        status = lpr_filed_call(FILED_OP_VFS_MKNOD, page_fd, 0, &ignored);
    }
    lpr_destroy_wire_page(page_fd, page);
    if (status == 0) {
        lpr_readlink_cache_clear();
        lpr_page_cache_clear();
    }
    return status;
}

int64_t lpr_linux_readlinkat_to_buffer(uint64_t dirfd, uint64_t path_raw, char *target, uint64_t capacity);
int lpr_resolve_final_symlink_path(const char *path, const char *target, char *out, uint64_t capacity);

int64_t lpr_linux_symlinkat(uint64_t target_raw, uint64_t new_dirfd, uint64_t linkpath_raw)
{
    const char *target = (const char *)(uintptr_t)target_raw;
    const char *linkpath = (const char *)(uintptr_t)linkpath_raw;
    if (target == 0 || linkpath == 0) {
        return -LPR_LINUX_EFAULT;
    }
    lpr_readlink_cache_clear();
    lpr_page_cache_clear();
    uint64_t dir_handle = 0;
    int64_t status = lpr_dir_handle_for(new_dirfd, linkpath, &dir_handle);
    if (status != 0) {
        return status;
    }
    void *page = 0;
    const int page_fd = lpr_create_wire_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }
    filed_symlink_t *symlink_req = (filed_symlink_t *)page;
    lpr_memset(symlink_req, 0, sizeof(*symlink_req));
    symlink_req->dir_handle = dir_handle;
    status = lpr_copy_path(symlink_req->name, sizeof(symlink_req->name), linkpath);
    const uint64_t target_len = (uint64_t)lpr_strnlen(target, FILED_SYMLINK_TARGET_BYTES);
    if (status == 0 && (target_len == 0 || target_len >= FILED_SYMLINK_TARGET_BYTES)) {
        status = target_len == 0 ? -LPR_LINUX_EINVAL : -LPR_LINUX_ENAMETOOLONG;
    }
    if (status == 0) {
        symlink_req->target_length = target_len;
        lpr_memcpy(symlink_req->target, target, target_len + 1u);
        uint64_t ignored = 0;
        status = lpr_filed_call(FILED_OP_VFS_SYMLINK, page_fd, 0, &ignored);
    }
    lpr_destroy_wire_page(page_fd, page);
    return status;
}

int64_t lpr_linux_linkat(uint64_t old_dirfd, uint64_t old_path_raw, uint64_t new_dirfd, uint64_t new_path_raw, uint64_t flags)
{
    const uint64_t known_flags = LPR_LINUX_AT_SYMLINK_FOLLOW;
    const char *old_path = (const char *)(uintptr_t)old_path_raw;
    const char *new_path = (const char *)(uintptr_t)new_path_raw;
    char followed_old_path[FILED_PATH_BYTES];
    if (old_path == 0 || new_path == 0) {
        return -LPR_LINUX_EFAULT;
    }
    if ((flags & ~known_flags) != 0) {
        return -LPR_LINUX_EINVAL;
    }
    if ((flags & LPR_LINUX_AT_SYMLINK_FOLLOW) != 0) {
        char target[FILED_SYMLINK_TARGET_BYTES];
        lpr_memset(target, 0, sizeof(target));
        const int64_t len = lpr_linux_readlinkat_to_buffer(old_dirfd, old_path_raw, target, sizeof(target) - 1u);
        if (len > 0) {
            target[(uint64_t)len < sizeof(target) ? (uint64_t)len : sizeof(target) - 1u] = 0;
            if (!lpr_resolve_final_symlink_path(old_path, target, followed_old_path, sizeof(followed_old_path))) {
                return -LPR_LINUX_ENAMETOOLONG;
            }
            old_dirfd = LPR_LINUX_AT_FDCWD;
            old_path_raw = (uint64_t)(uintptr_t)followed_old_path;
            old_path = followed_old_path;
        } else if (len != -LPR_LINUX_EINVAL) {
            return len == 0 ? -LPR_LINUX_EINVAL : len;
        }
    }
    lpr_readlink_cache_clear();
    lpr_page_cache_clear();

    uint64_t old_dir_handle = 0;
    int64_t status = lpr_dir_handle_for(old_dirfd, old_path, &old_dir_handle);
    if (status != 0) {
        return status;
    }
    uint64_t new_dir_handle = 0;
    status = lpr_dir_handle_for(new_dirfd, new_path, &new_dir_handle);
    if (status != 0) {
        return status;
    }
    void *page = 0;
    const int page_fd = lpr_create_wire_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }
    filed_link_t *link_req = (filed_link_t *)page;
    lpr_memset(link_req, 0, sizeof(*link_req));
    link_req->old_dir_handle = old_dir_handle;
    link_req->new_dir_handle = new_dir_handle;
    link_req->flags = flags;
    status = lpr_copy_path(link_req->old_name, sizeof(link_req->old_name), old_path);
    if (status == 0) {
        status = lpr_copy_path(link_req->new_name, sizeof(link_req->new_name), new_path);
    }
    if (status == 0) {
        uint64_t ignored = 0;
        status = lpr_filed_call(FILED_OP_VFS_LINK, page_fd, 0, &ignored);
    }
    lpr_destroy_wire_page(page_fd, page);
    return status;
}

int64_t lpr_filed_open_handle_at(
    uint64_t dirfd,
    const char *path,
    uint64_t flags,
    uint64_t mode,
    uint64_t *out_handle)
{
    (void)mode;
    if (out_handle == 0) {
        return -LPR_LINUX_EFAULT;
    }
    *out_handle = 0;
    if ((flags & (LPR_LINUX_O_CREAT | LPR_LINUX_O_TRUNC)) != 0) {
        lpr_readlink_cache_clear();
        lpr_page_cache_clear();
    }
    void *page = 0;
    uint64_t dir_handle = 0;
    int64_t status = lpr_dir_handle_for(dirfd, path, &dir_handle);
    if (status != 0) {
        return status;
    }
    status = lpr_filed_endpoint_ready();
    if (status != 0) {
        return status;
    }
    const int page_fd = lpr_create_wire_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }

    filed_openat_t *open_req = (filed_openat_t *)page;
    lpr_memset(open_req, 0, sizeof(*open_req));
    open_req->dir_handle = dir_handle;
    open_req->rights = lpr_open_rights(flags);
    open_req->open_flags = lpr_open_flags(flags);
    status = lpr_copy_path(open_req->name, sizeof(open_req->name), path);
    uint64_t handle = 0;
    if (status == 0) {
        status = lpr_filed_call(FILED_OP_VFS_OPENAT, page_fd, 0, &handle);
    }
    lpr_destroy_wire_page(page_fd, page);
    if (status != 0) {
        return status;
    }
    *out_handle = handle;
    return 0;
}

int64_t lpr_linux_openat_once(uint64_t dirfd, uint64_t path_raw, uint64_t flags, uint64_t mode)
{
    const char *path = (const char *)(uintptr_t)path_raw;
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
    uint64_t handle = 0;
    const int64_t status = lpr_filed_open_handle_at(dirfd, path, flags, mode, &handle);
    if (status != 0) {
        return status;
    }
    const int fd = lpr_fd_alloc(handle, flags);
    if (fd < 0) {
        (void)lpr_filed_close_handle(handle);
        return fd;
    }
    lpr_linux_stat_t st;
    lpr_memset(&st, 0, sizeof(st));
    if (lpr_linux_fstat((uint64_t)(uint32_t)fd, (uint64_t)(uintptr_t)&st) == 0 &&
        (st.st_mode & LPR_LINUX_S_IFMT) == LPR_LINUX_S_IFCHR &&
        st.st_rdev == ((1ull << 8u) | 3ull))
    {
        const int64_t close_status = lpr_linux_close((uint64_t)(uint32_t)fd);
        if (close_status != 0) return close_status;
        const uint64_t device_id = (1ull << 32u) | 3ull;
        const int install = lpr_control_install_fd(
            (uint64_t)(uint32_t)fd, LPR_FD_TABLE_KIND_DEVICE, flags, device_id, 0);
        if (install != 0) return install;
        lpr_device_fd_t *device = lpr_fd_device_payload((uint64_t)(uint32_t)fd);
        if (device == 0) {
            lpr_control_close_fd((uint64_t)(uint32_t)fd);
            return -LPR_LINUX_EIO;
        }
        device->active = 1;
        device->major = 1;
        device->minor = 3;
        device->flags = (uint32_t)flags;
    }
    return fd;
}

int64_t lpr_linux_readlinkat_to_buffer(uint64_t dirfd, uint64_t path_raw, char *target, uint64_t capacity)
{
    if (target == 0 || capacity == 0) {
        return -LPR_LINUX_EFAULT;
    }
    const char *path = (const char *)(uintptr_t)path_raw;
    uint64_t dir_handle = 0;
    int64_t status = lpr_dir_handle_for(dirfd, path, &dir_handle);
    if (status != 0) {
        return status;
    }
    void *page = 0;
    const int page_fd = lpr_create_wire_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }
    filed_readlink_t *readlink_req = (filed_readlink_t *)page;
    lpr_memset(readlink_req, 0, sizeof(*readlink_req));
    readlink_req->dir_handle = dir_handle;
    status = lpr_copy_path(readlink_req->name, sizeof(readlink_req->name), path);
    uint64_t length = 0;
    if (status == 0) {
        status = lpr_filed_call(FILED_OP_VFS_READLINK, page_fd, 0, &length);
    }
    if (status == 0) {
        if (length > capacity) {
            length = capacity;
        }
        lpr_memcpy(target, readlink_req->target, length);
    }
    lpr_destroy_wire_page(page_fd, page);
    return status == 0 ? (int64_t)length : status;
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

int64_t lpr_linux_openat(uint64_t dirfd, uint64_t path_raw, uint64_t flags, uint64_t mode)
{
    int64_t fd = lpr_linux_openat_once(dirfd, path_raw, flags, mode);
    if (fd < 0 ||
        (flags & (LPR_LINUX_O_NOFOLLOW | LPR_LINUX_O_CREAT | LPR_LINUX_O_TRUNC)) != 0)
    {
        return fd;
    }
    lpr_linux_stat_t st;
    const int64_t stat_status = lpr_linux_fstat((uint64_t)fd, (uint64_t)(uintptr_t)&st);
    if (stat_status != 0 ||
        (((uint64_t)st.st_mode & LPR_LINUX_S_IFMT) != LPR_LINUX_S_IFLNK))
    {
        return fd;
    }
    char target[FILED_SYMLINK_TARGET_BYTES];
    lpr_memset(target, 0, sizeof(target));
    const int64_t len = lpr_linux_readlinkat_to_buffer(dirfd, path_raw, target, sizeof(target) - 1u);
    (void)lpr_linux_close((uint64_t)fd);
    if (len <= 0) {
        return len == 0 ? -LPR_LINUX_EINVAL : len;
    }
    target[len < (int64_t)(sizeof(target) - 1u) ? (uint64_t)len : sizeof(target) - 1u] = 0;
    const char *path = (const char *)(uintptr_t)path_raw;
    char resolved[FILED_PATH_BYTES];
    if (!lpr_resolve_final_symlink_path(path, target, resolved, sizeof(resolved))) {
        return -LPR_LINUX_ENAMETOOLONG;
    }
    return lpr_linux_openat_once(dirfd, (uint64_t)(uintptr_t)resolved, flags, mode);
}

void lpr_cwd_pop_component(char *path, uint64_t *len)
{
    if (path == 0 || len == 0 || *len <= 1u) {
        if (path != 0 && len != 0) {
            path[0] = '/';
            path[1] = 0;
            *len = 1;
        }
        return;
    }
    uint64_t i = *len;
    while (i > 1u && path[i - 1u] == '/') {
        i -= 1u;
    }
    while (i > 1u && path[i - 1u] != '/') {
        i -= 1u;
    }
    if (i <= 1u) {
        path[0] = '/';
        path[1] = 0;
        *len = 1;
        return;
    }
    path[i - 1u] = 0;
    *len = i - 1u;
}

int lpr_cwd_append_component(char *out, uint64_t capacity, uint64_t *len, const char *component, uint64_t component_len)
{
    if (out == 0 || len == 0 || component == 0 || component_len == 0) {
        return 0;
    }
    uint64_t need = *len;
    if (need == 0) {
        if (capacity < 2u) {
            return 0;
        }
        out[0] = '/';
        out[1] = 0;
        need = 1;
    }
    if (need > 1u && out[need - 1u] != '/') {
        need += 1u;
    }
    if (need + component_len >= capacity) {
        return 0;
    }
    if (*len > 1u && out[*len - 1u] != '/') {
        out[*len] = '/';
        *len += 1u;
    }
    lpr_memcpy(out + *len, component, (size_t)component_len);
    *len += component_len;
    out[*len] = 0;
    return 1;
}

int64_t lpr_cwd_normalize(const char *path, char *out, uint64_t capacity)
{
    if (path == 0 || out == 0 || capacity < 2u) {
        return -LPR_LINUX_EFAULT;
    }
    if (!lpr_path_is_terminated(path, FILED_PATH_BYTES)) {
        return -LPR_LINUX_ENAMETOOLONG;
    }
    lpr_cwd_init();
    lpr_memset(out, 0, capacity);
    uint64_t len = 1;
    out[0] = '/';
    if (path[0] != '/') {
        const uint64_t cwd_len = (uint64_t)lpr_strnlen(lpr_cwd_path, sizeof(lpr_cwd_path));
        if (cwd_len == 0 || cwd_len >= capacity) {
            return -LPR_LINUX_ENAMETOOLONG;
        }
        lpr_memcpy(out, lpr_cwd_path, (size_t)cwd_len + 1u);
        len = cwd_len;
    }

    const char *cursor = path;
    while (*cursor != 0) {
        while (*cursor == '/') {
            cursor += 1;
        }
        const char *component = cursor;
        while (*cursor != 0 && *cursor != '/') {
            cursor += 1;
        }
        const uint64_t component_len = (uint64_t)(cursor - component);
        if (component_len == 0 ||
            (component_len == 1u && component[0] == '.'))
        {
            continue;
        }
        if (component_len == 2u && component[0] == '.' && component[1] == '.') {
            lpr_cwd_pop_component(out, &len);
            continue;
        }
        if (!lpr_cwd_append_component(out, capacity, &len, component, component_len)) {
            return -LPR_LINUX_ENAMETOOLONG;
        }
    }
    if (len == 0) {
        out[0] = '/';
        out[1] = 0;
    }
    return 0;
}

int64_t lpr_linux_getcwd(uint64_t buf, uint64_t size)
{
    if (buf == 0) {
        return -LPR_LINUX_EFAULT;
    }
    lpr_cwd_init();
    const uint64_t len = (uint64_t)lpr_strnlen(lpr_cwd_path, sizeof(lpr_cwd_path));
    if (len + 1u > size) {
        return -LPR_LINUX_ERANGE;
    }
    lpr_memcpy((void *)(uintptr_t)buf, lpr_cwd_path, (size_t)len + 1u);
    return (int64_t)(len + 1u);
}

int64_t lpr_supervisor_cwd_set(uint64_t handle, const char *path)
{
    if (!lpr_supervisor_enabled) {
        return 0;
    }
    void *page = 0;
    const int page_fd = lpr_create_standalone_wire_page(&page);
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

int64_t lpr_cwd_install(uint64_t handle, const char *path)
{
    if (path == 0 || !lpr_path_is_terminated(path, sizeof(lpr_cwd_path))) {
        if (handle != 0) {
            (void)lpr_filed_close_handle(handle);
        }
        return -LPR_LINUX_ENAMETOOLONG;
    }
    lpr_cwd_init();
    const uint64_t old_handle = lpr_cwd_handle;
    char old_path[FILED_PATH_BYTES];
    lpr_memcpy(old_path, lpr_cwd_path, sizeof(old_path));
    lpr_memset(lpr_cwd_path, 0, sizeof(lpr_cwd_path));
    const uint64_t len = (uint64_t)lpr_strnlen(path, sizeof(lpr_cwd_path));
    lpr_memcpy(lpr_cwd_path, path, (size_t)len + 1u);
    if (len == 1u && path[0] == '/') {
        lpr_cwd_handle = 0;
        if (handle != 0) {
            (void)lpr_filed_close_handle(handle);
        }
    } else {
        lpr_cwd_handle = handle;
    }
    const int64_t supervisor_status = lpr_supervisor_cwd_set(lpr_cwd_handle, lpr_cwd_path);
    if (supervisor_status != 0) {
        if (lpr_cwd_handle != 0 && lpr_cwd_handle != old_handle) {
            (void)lpr_filed_close_handle(lpr_cwd_handle);
        }
        lpr_cwd_handle = old_handle;
        lpr_memcpy(lpr_cwd_path, old_path, sizeof(lpr_cwd_path));
        return supervisor_status;
    }
    if (old_handle != 0) {
        (void)lpr_filed_close_handle(old_handle);
    }
    return 0;
}

int64_t lpr_linux_chdir(uint64_t path_raw)
{
    const char *path = (const char *)(uintptr_t)path_raw;
    if (path == 0) {
        return -LPR_LINUX_EFAULT;
    }
    char normalized[FILED_PATH_BYTES];
    int64_t status = lpr_cwd_normalize(path, normalized, sizeof(normalized));
    if (status != 0) {
        return status;
    }
    uint64_t handle = 0;
    status = lpr_filed_open_handle_at(
        (uint64_t)(int64_t)LPR_LINUX_AT_FDCWD,
        path,
        LPR_LINUX_O_RDONLY | LPR_LINUX_O_DIRECTORY,
        0,
        &handle);
    if (status != 0) {
        return status;
    }
    lpr_readlink_cache_clear();
    lpr_page_cache_clear();
    return lpr_cwd_install(handle, normalized);
}

int64_t lpr_linux_fchdir(uint64_t fd)
{
    if (!lpr_fd_is_filed(fd)) {
        return -LPR_LINUX_EBADF;
    }
    lpr_linux_stat_t st;
    const int64_t stat_status = lpr_linux_fstat(fd, (uint64_t)(uintptr_t)&st);
    if (stat_status != 0) {
        return stat_status;
    }
    if ((((uint64_t)st.st_mode) & LPR_LINUX_S_IFMT) != LPR_LINUX_S_IFDIR) {
        return -LPR_LINUX_ENOTDIR;
    }
    uint64_t dup_handle = 0;
    const int64_t dup_status = lpr_filed_dup_handle(lpr_fd_filed_payload(fd)->handle, 0, &dup_handle);
    if (dup_status != 0) {
        return dup_status;
    }
    char cwd_copy[FILED_PATH_BYTES];
    lpr_memset(cwd_copy, 0, sizeof(cwd_copy));
    lpr_cwd_init();
    lpr_memcpy(cwd_copy, lpr_cwd_path, sizeof(cwd_copy));
    return lpr_cwd_install(dup_handle, cwd_copy);
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
    const int64_t status = lpr_pacha_syscall2(
        PACHAOS_SYSCALL_CLOCK_GETTIME,
        clock_id,
        (uint64_t)(uintptr_t)out);
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

int64_t lpr_linux_sleep_result(int64_t status)
{
    if (status != -LPR_LINUX_EAGAIN) {
        return status;
    }
    const int64_t signal_status = lpr_linux_dispatch_pending_signals();
    return signal_status != 0 ? signal_status : -LPR_LINUX_EINTR;
}

int64_t lpr_linux_nanosleep(uint64_t req_raw, uint64_t rem_raw)
{
    (void)rem_raw;
    const struct pachaos_timespec *req = (const struct pachaos_timespec *)(uintptr_t)req_raw;
    const int64_t valid = lpr_linux_validate_timespec(req);
    if (valid != 0) {
        return valid;
    }
    return lpr_linux_sleep_result(lpr_pacha_nanosleep(req));
}

int64_t lpr_linux_clock_nanosleep(uint64_t clock_id, uint64_t flags, uint64_t req_raw, uint64_t rem_raw)
{
    (void)rem_raw;
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
        return lpr_linux_sleep_result(lpr_pacha_nanosleep(req));
    }

    struct pachaos_timespec now;
    int64_t status = lpr_pacha_clock_gettime(clock_id, &now);
    if (status != 0) {
        return status;
    }
    if (lpr_timespec_less_equal(req, &now)) {
        return 0;
    }
    struct pachaos_timespec relative;
    lpr_memset(&relative, 0, sizeof(relative));
    lpr_timespec_subtract(req, &now, &relative);
    return lpr_linux_sleep_result(lpr_pacha_nanosleep(&relative));
}
