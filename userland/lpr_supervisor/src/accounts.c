#include "accounts.h"
#include "filed/ipc_protocol.h"
#include "filed/payload.h"
#include "filed_client/client.h"
#include "pacha/ipc.h"
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static int read_file(pacha_fd_t endpoint, const char *path, char **text, size_t *length)
{
    kb2_fs_request_t request = {.opcode = KB2_FILESYSTEM_OP_OPENAT2};
    kb2_fs_response_t response = {0};
    int status = filedfs_call(endpoint, &request, path, NULL, NULL, 0,
        NULL, 0, &response, NULL, 0, NULL, NULL);
    if (!status && response.result < 0) status = (int)response.result;
    if (status) return status;
    uint64_t handle = response.handle;
    char chunk[16384];
    size_t capacity = 0;
    for (;;) {
        request = (kb2_fs_request_t){.opcode = KB2_FILESYSTEM_OP_PREAD,
            .handle = handle, .offset = *length, .length = sizeof(chunk)};
        status = filedfs_call(endpoint, &request, NULL, NULL, NULL, 0,
            chunk, sizeof(chunk), &response, NULL, 0, NULL, NULL);
        if (!status && response.result < 0) status = (int)response.result;
        if (status || !response.result) break;
        size_t bytes = (size_t)response.result;
        /* Bound boot-time memory use; reject large input, never truncate. */
        if (bytes > sizeof(chunk) || bytes != response.data_length ||
            bytes > 1024 * 1024 - *length) { status = -E2BIG; break; }
        if (*length + bytes > capacity) {
            capacity = *length + (size_t)bytes;
            void *next = realloc(*text, capacity);
            if (!next) { status = ENOMEM; break; }
            *text = next;
        }
        memcpy(*text + *length, chunk, bytes);
        *length += (size_t)bytes;
    }
    request = (kb2_fs_request_t){.opcode = KB2_FILESYSTEM_OP_CLOSE, .handle = handle};
    int closed = filedfs_call(endpoint, &request, NULL, NULL, NULL, 0,
        NULL, 0, &response, NULL, 0, NULL, NULL);
    if (!closed && response.result < 0) closed = (int)response.result;
    return status ? status : closed;
}

int lprs_accounts_load(pacha_fd_t endpoint, struct pacha_accounts *db)
{
    char *passwd = NULL, *group = NULL;
    size_t plen = 0, glen = 0;
    int status = read_file(endpoint, "/etc/passwd", &passwd, &plen);
    if (status) fprintf(stderr, "[lprs-accounts] read /etc/passwd status=%d\n", status);
    if (!status) status = read_file(endpoint, "/etc/group", &group, &glen);
    if (status) fprintf(stderr, "[lprs-accounts] read definitions status=%d passwd=%zu group=%zu\n", status, plen, glen);
    if (!status) {
        status = pacha_accounts_parse(db, passwd ? passwd : "", plen, group ? group : "", glen);
        if (status) fprintf(stderr, "[lprs-accounts] parse status=%d passwd=%zu group=%zu\n", status, plen, glen);
    }
    free(passwd); free(group);
    return status;
}
