/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "wifi_files.h"
#include <filed_client/client.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/* File permissions carry the administration authority. Keep all traversal
 * symlink-free; never let a user-writable substitute become netd's policy. */
#define NO_SYMLINKS 4u
static uint64_t publication;

static uint64_t read_le(const unsigned char *bytes, unsigned size)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < size; ++i) value |= (uint64_t)bytes[i] << (i * 8);
    return value;
}

static int call(pacha_fd_t endpoint, kb2_fs_request_t *request,
    const char *path, const char *second, const void *input, size_t size,
    void *output, size_t capacity, kb2_fs_response_t *response)
{
    int result = filedfs_call(endpoint, request, path, second, input, size,
        output, capacity, response, NULL, 0, NULL, NULL);
    return result ? result : response->result < 0 ? (int)response->result : 0;
}

static int close_file(pacha_fd_t endpoint, uint64_t handle)
{
    kb2_fs_request_t request = {.opcode = KB2_FILESYSTEM_OP_CLOSE, .handle = handle};
    kb2_fs_response_t response;
    return call(endpoint, &request, NULL, NULL, NULL, 0, NULL, 0, &response);
}

static int metadata(pacha_fd_t endpoint, uint64_t handle, int directory,
    int private_directory, int sticky_runtime_parent, uint64_t *size)
{
    unsigned char stat[KB2_FILESYSTEM_STATX_SIZE];
    kb2_fs_request_t request = {.opcode = KB2_FILESYSTEM_OP_STATX,
        .handle = handle, .flags = 0x1000, .mask = 0x7ff};
    kb2_fs_response_t response;
    int result = call(endpoint, &request, NULL, NULL, NULL, 0, stat, sizeof(stat), &response);
    if (result) return result;
    if (response.data_length != sizeof(stat)) return -EPROTO;
    uint32_t mode = (uint32_t)read_le(stat + KB2_FILESYSTEM_STATX_MODE_OFFSET, 2);
    uint32_t uid = (uint32_t)read_le(stat + KB2_FILESYSTEM_STATX_UID_OFFSET, 4);
    if (uid) return -EPERM;
    if (directory) {
        if (!S_ISDIR(mode) || ((mode & 0022) && !(sticky_runtime_parent && (mode & S_ISVTX))) ||
            (private_directory && (mode & 07777) != 0700)) return -EPERM;
    } else if (!S_ISREG(mode) || (mode & 07777) != 0600 ||
        read_le(stat + KB2_FILESYSTEM_STATX_NLINK_OFFSET, 4) != 1) return -EPERM;
    if (size) *size = read_le(stat + KB2_FILESYSTEM_STATX_SIZE_OFFSET, 8);
    return 0;
}

static int directory(pacha_fd_t endpoint, const char *path, unsigned mode)
{
    kb2_fs_request_t request = {.opcode = KB2_FILESYSTEM_OP_MKDIRAT, .mode = mode};
    kb2_fs_response_t response;
    int result = call(endpoint, &request, path, NULL, NULL, 0, NULL, 0, &response);
    if (result && result != -EEXIST) return result;
    request = (kb2_fs_request_t){.opcode = KB2_FILESYSTEM_OP_OPENAT2,
        .flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC, .resolve = NO_SYMLINKS};
    result = call(endpoint, &request, path, NULL, NULL, 0, NULL, 0, &response);
    if (result) return result;
    uint64_t handle = response.handle;
    result = metadata(endpoint, handle, 1, mode == 0700, !strcmp(path, "/run"), NULL);
    int closed = close_file(endpoint, handle);
    return result ? result : closed;
}

int netd_wifi_files_prepare(pacha_fd_t endpoint)
{
    int result = directory(endpoint, "/etc", 0755);
    if (!result) result = directory(endpoint, "/etc/pachaos", 0755);
    if (!result) result = directory(endpoint, "/run", 0755);
    if (!result) result = directory(endpoint, "/run/pachaos", 0755);
    if (!result) result = directory(endpoint, NETD_WIFI_DIRECTORY, 0700);
    return result;
}

int netd_wifi_file_read(pacha_fd_t endpoint, const char *path,
    void *bytes, size_t capacity, size_t *length)
{
    if (!path || !bytes || capacity < 2 || !length) return -EINVAL;
    *length = 0;
    kb2_fs_request_t request = {.opcode = KB2_FILESYSTEM_OP_OPENAT2,
        .flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW, .resolve = NO_SYMLINKS};
    kb2_fs_response_t response;
    int result = call(endpoint, &request, path, NULL, NULL, 0, NULL, 0, &response);
    if (result) return result;
    uint64_t handle = response.handle, size = 0;
    result = metadata(endpoint, handle, 0, 0, 0, &size);
    if (!result && (!size || size >= capacity)) result = -E2BIG;
    size_t offset = 0;
    while (!result && offset < size) {
        request = (kb2_fs_request_t){.opcode = KB2_FILESYSTEM_OP_PREAD,
            .handle = handle, .offset = (int64_t)offset, .length = size - offset};
        result = call(endpoint, &request, NULL, NULL, NULL, 0,
            (char *)bytes + offset, (size_t)size - offset, &response);
        if (!result && (response.result <= 0 || (uint64_t)response.result > size - offset ||
            response.data_length != (uint64_t)response.result)) result = -EIO;
        if (!result) offset += (size_t)response.result;
    }
    int closed = close_file(endpoint, handle);
    if (!result) result = closed;
    if (!result && memchr(bytes, 0, offset)) result = -EINVAL;
    if (!result) { ((char *)bytes)[offset] = 0; *length = offset; }
    return result;
}

int netd_wifi_file_write(pacha_fd_t endpoint, const char *name,
    const void *bytes, size_t length)
{
    if (!name || !*name || strchr(name, '/') || !strcmp(name, ".") || !strcmp(name, "..") ||
        (!bytes && length)) return -EINVAL;
    char path[160], temporary[160];
    int count = snprintf(path, sizeof(path), NETD_WIFI_DIRECTORY "/%s", name);
    if (count < 0 || (size_t)count >= sizeof(path)) return -ENAMETOOLONG;
    count = snprintf(temporary, sizeof(temporary), NETD_WIFI_DIRECTORY "/.netd-%llu",
        (unsigned long long)++publication);
    if (count < 0 || (size_t)count >= sizeof(temporary)) return -EOVERFLOW;
    kb2_fs_request_t request = {.opcode = KB2_FILESYSTEM_OP_OPENAT2,
        .flags = O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW,
        .mode = 0600, .resolve = NO_SYMLINKS};
    kb2_fs_response_t response;
    int result = call(endpoint, &request, temporary, NULL, NULL, 0, NULL, 0, &response);
    if (result) return result;
    const uint64_t handle = response.handle;
    for (size_t offset = 0; !result && offset < length;) {
        request = (kb2_fs_request_t){.opcode = KB2_FILESYSTEM_OP_PWRITE,
            .handle = handle, .offset = (int64_t)offset, .length = length - offset};
        result = call(endpoint, &request, NULL, NULL, (const char *)bytes + offset,
            length - offset, NULL, 0, &response);
        if (!result && (response.result <= 0 || (uint64_t)response.result > length - offset))
            result = -EIO;
        if (!result) offset += (size_t)response.result;
    }
    int closed = close_file(endpoint, handle);
    if (!result) result = closed;
    if (!result) {
        request = (kb2_fs_request_t){.opcode = KB2_FILESYSTEM_OP_RENAMEAT2};
        result = call(endpoint, &request, temporary, path, NULL, 0, NULL, 0, &response);
    }
    if (result) {
        request = (kb2_fs_request_t){.opcode = KB2_FILESYSTEM_OP_UNLINKAT};
        (void)call(endpoint, &request, temporary, NULL, NULL, 0, NULL, 0, &response);
    }
    return result;
}
