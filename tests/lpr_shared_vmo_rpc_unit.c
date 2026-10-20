/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include "../userland/personality/linux/runtime/lpr_filed_internal.h"

static _Thread_local unsigned slot;
static lpr_filed_backend_t files[2];
static unsigned calls[2];
static int overlap, failure;
static pthread_barrier_t barrier;

lpr_filed_backend_t *lpr_filed_backend(uint64_t fd)
{
    return fd == 10 + slot ? &files[slot] : NULL;
}
int64_t lpr_filed_fd_binding(uint64_t fd)
{
    /* Connection setup is outside this concurrency fixture; the operation
     * still resolves its handle only after the binding entry returns. */
    (void)fd;
    return 0;
}

int64_t lpr_fs_call(kb2_fs_request_t *request, const char *path,
    const char *second, const void *data, uint64_t data_bytes,
    void *output, uint64_t output_bytes, kb2_fs_response_t *response,
    pacha_fd_t transfer, pacha_fd_t *returned)
{
    assert(!path && !second && !data && !data_bytes && !output &&
        !output_bytes && transfer == -1 && returned);
    assert(request->handle == files[slot].handle);
    assert(request->offset == slot * 4096 && request->length == 8192);
    if (request->opcode == KB2_FILESYSTEM_OP_SHARED_FILE_VMO)
        assert(request->flags == (FILED_FILE_VMO_EXEC |
            (slot ? FILED_FILE_VMO_WRITE : 0)));
    else assert(request->opcode == KB2_FILESYSTEM_OP_FILE_VMO && !request->flags);
    calls[slot]++;
    if (overlap) {
        const int status = pthread_barrier_wait(&barrier);
        assert(!status || status == PTHREAD_BARRIER_SERIAL_THREAD);
    }
    memset(response, 0, sizeof(*response));
    *returned = -1;
    if (failure) {
        const int64_t errors[] = {0, -38, -12, -5, -5, -5, -13};
        if (failure < 7) return errors[failure];
        return 0; /* A valid success without its promised capability. */
    }
    response->result = 4096 + slot;
    *returned = 200 + slot;
    return response->result;
}

static void *run(void *index)
{
    slot = (unsigned)(uintptr_t)index;
    uint64_t loaded = 0;
    assert(lpr_linux_shared_file_vmo(10 + slot, slot * 4096, 8192,
        slot != 0, 1, &loaded) == (int64_t)(200 + slot));
    assert(loaded == 4096 + slot);
    return NULL;
}

int main(void)
{
    files[0].handle = 100; files[1].handle = 101;
    assert(!pthread_barrier_init(&barrier, NULL, 2));
    overlap = 1;
    pthread_t a, b;
    assert(!pthread_create(&a, NULL, run, (void *)0));
    assert(!pthread_create(&b, NULL, run, (void *)1));
    assert(!pthread_join(a, NULL) && !pthread_join(b, NULL));
    overlap = 0;
    assert(calls[0] == 1 && calls[1] == 1);
    const int64_t expected[] = {0, -38, -12, -5, -5, -5, -13, -5};
    for (failure = 1; failure <= 7; ++failure) {
        uint64_t loaded = 99;
        assert(lpr_linux_shared_file_vmo(10, 0, 8192, 0, 1, &loaded) == expected[failure]);
        assert(!loaded);
    }
    failure = 0;
    uint64_t loaded = 0;
    assert(lpr_linux_file_vmo(10, 0, 8192, 0, &loaded) == 200 && loaded == 4096);
    assert(lpr_linux_shared_file_vmo(99, 0, 8192, 0, 1, &loaded) == -LPR_LINUX_EBADF);
    assert(!loaded);
    assert(!pthread_barrier_destroy(&barrier));
    puts("shared VMO cacheless RPC, independent requests and failure propagation: PASS");
}
