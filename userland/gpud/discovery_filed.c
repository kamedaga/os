/* SPDX-License-Identifier: MIT */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "discovery.h"
#include <filed_client/client.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>

static int call(int endpoint, kb2_fs_request_t *request, const char *path,
    const char *second_path, const void *data, size_t bytes,
    kb2_fs_response_t *response)
{
    int status = filedfs_call(endpoint, request, path, second_path, data, bytes,
        NULL, 0, response, NULL, 0, NULL, NULL);
    return status ? status : response->result < 0 ? (int)response->result : 0;
}

static int create(int endpoint, const struct gpud_discovery_entry *e, int *owned)
{
    kb2_fs_request_t request = {0};
    kb2_fs_response_t response;
    if (e->kind == GPUD_DISC_DIR) {
        request.opcode = KB2_FILESYSTEM_OP_MKDIRAT;
        request.mode = 0755;
    } else if (e->kind == GPUD_DISC_LINK) {
        request.opcode = KB2_FILESYSTEM_OP_SYMLINKAT;
    } else if (e->kind == GPUD_DISC_NODE) {
        request.opcode = KB2_FILESYSTEM_OP_MKNODAT;
        request.mode = 0020660;
        request.length = e->dev;
    } else {
        request.opcode = KB2_FILESYSTEM_OP_OPENAT2;
        request.flags = O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC;
        request.mode = 0644;
    }
    int status = call(endpoint, &request,
        e->kind == GPUD_DISC_LINK ? e->value : e->path,
        e->kind == GPUD_DISC_LINK ? e->path : NULL, NULL, 0, &response);
    *owned = status == 0;
    if (status || e->kind != GPUD_DISC_TEXT) return status;
    const uint64_t handle = response.handle;
    const size_t length = strlen(e->value);
    for (size_t offset = 0; !status && offset < length;) {
        request = (kb2_fs_request_t){.opcode = KB2_FILESYSTEM_OP_PWRITE,
            .handle = handle, .offset = (int64_t)offset, .length = length - offset};
        status = call(endpoint, &request, NULL, NULL, e->value + offset,
            length - offset, &response);
        if (!status && (response.result <= 0 ||
            (uint64_t)response.result > length - offset)) status = -EIO;
        if (!status) offset += (size_t)response.result;
    }
    request = (kb2_fs_request_t){.opcode = KB2_FILESYSTEM_OP_CLOSE, .handle = handle};
    const int closed = call(endpoint, &request, NULL, NULL, NULL, 0, &response);
    return status ? status : closed;
}

int gpud_discovery_remove(struct gpud_discovery *d, int filed_fd)
{
    if (!d || filed_fd < 16) return -EINVAL;
    int first = 0;
    for (size_t n = d->published; n; --n) {
        size_t i = n - 1;
        if (!(d->owned & (UINT32_C(1) << i))) continue;
        kb2_fs_request_t request = {.opcode = KB2_FILESYSTEM_OP_UNLINKAT,
            .flags = d->entries[i].kind == GPUD_DISC_DIR ? AT_REMOVEDIR : 0};
        kb2_fs_response_t response;
        int status = call(filed_fd, &request, d->entries[i].path, NULL,
            NULL, 0, &response);
        if (!status || status == -ENOENT) d->owned &= ~(UINT32_C(1) << i);
        else if (!first) first = status;
    }
    if (!d->owned) d->published = 0;
    return first;
}

int gpud_discovery_publish(struct gpud_discovery *d, int filed_fd)
{
    if (!d || !d->count || d->count > 32 || d->published || filed_fd < 16)
        return -EINVAL;
    int status = 0;
    for (size_t i = 0; i < d->count; ++i) {
        int owned = 0;
        status = create(filed_fd, &d->entries[i], &owned);
        d->published = i + 1;
        if (owned) d->owned |= UINT32_C(1) << i;
        /* Shared ancestors may preexist, but another publisher's metadata
         * must never be replaced or removed during rollback. */
        if (status == -EEXIST && i < d->ancestor_count) status = 0;
        if (status) break;
    }
    if (status) (void)gpud_discovery_remove(d, filed_fd);
    return status;
}
