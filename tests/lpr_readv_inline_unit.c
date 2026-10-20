#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../userland/personality/linux/runtime/lpr_filed_internal.h"
#include "../userland/personality/linux/runtime/lpr_vfs/session.h"

static unsigned char data[65536];
static uint64_t cursor, file_size, calls;
static int injected_error, create_error;

void *lpr_memcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
uint64_t lpr_page_align_up(uint64_t n) { return (n + 4095) & ~4095ull; }
int64_t lpr_pacha_status_to_errno(int64_t status) { return status; }
int64_t lpr_pacha_syscall6(uint64_t nr, uint64_t fd, uint64_t at,
    uint64_t bytes, uint64_t prot, uint64_t flags, uint64_t offset)
{
    (void)nr;(void)fd;(void)at;(void)bytes;(void)prot;(void)flags;(void)offset;
    assert(0); return -1; /* No pre-admission full-vector scratch mapping. */
}
int64_t lpr_pacha_syscall2(uint64_t nr, uint64_t at, uint64_t bytes)
{
    (void)nr;(void)at;(void)bytes;assert(0);return -1;
}
int64_t lpr_fs_call_buffers(kb2_fs_request_t *request,
    const struct lpr_fs_buffer *input, unsigned input_count, kb2_fs_response_t *reply,
    const struct lpr_fs_buffer *output, unsigned output_count)
{
    assert(request->handle == 42 && request->offset == 0);
    if (create_error) return create_error; /* Admission failure, no Linux operation. */
    ++calls;
    if (injected_error) return injected_error;
    memset(reply, 0, sizeof(*reply));
    if (request->opcode == KB2_FILESYSTEM_OP_WRITE) {
        assert(!output_count && cursor + request->length <= sizeof(data));
        uint64_t copied = 0;
        for (unsigned i = 0; i < input_count; ++i) {
            if (input[i].length) memcpy(data + cursor + copied, input[i].bytes, input[i].length);
            copied += input[i].length;
        }
        assert(copied == request->length);
        cursor += copied; return copied;
    }
    assert(request->opcode == KB2_FILESYSTEM_OP_READ && !input_count);
    uint64_t n = cursor < file_size ? file_size - cursor : 0;
    if (n > request->length) n = request->length;
    uint64_t copied = 0;
    for (unsigned i = 0; i < output_count && copied < n; ++i) {
        uint64_t part = output[i].length;
        if (part > n - copied) part = n - copied;
        if (part) memcpy((void *)output[i].bytes, data + cursor + copied, part);
        copied += part;
    }
    assert(copied == n); cursor += n; reply->data_length = n; return n;
}
int64_t lpr_fs_call(kb2_fs_request_t *request, const char *path,
    const char *second, const void *input, uint64_t input_bytes,
    void *output, uint64_t output_bytes, kb2_fs_response_t *reply,
    pacha_fd_t transfer, pacha_fd_t *cap)
{
    assert(!path && !second && transfer == -1 && !cap);
    const struct lpr_fs_buffer in = {input, input_bytes}, out = {output, output_bytes};
    return lpr_fs_call_buffers(request, input_bytes ? &in : 0, input_bytes ? 1 : 0,
        reply, output_bytes ? &out : 0, output_bytes ? 1 : 0);
}
uint64_t lpr_scatter_iov(const lpr_linux_iovec_t *iov, uint64_t count,
    const unsigned char *src, uint64_t length)
{
    uint64_t copied = 0;
    for (uint64_t i = 0; i < count && copied < length; ++i) {
        uint64_t n = iov[i].len < length - copied ? iov[i].len : length - copied;
        if (n) memcpy((void *)(uintptr_t)iov[i].base, src + copied, n);
        copied += n;
    }
    return copied;
}

int main(void)
{
    for (unsigned i = 0; i < sizeof(data); ++i) data[i] = (unsigned char)(i * 37u);
    unsigned char a[1024], b[20000];
    lpr_linux_iovec_t iov[] = {{(uintptr_t)a, sizeof(a)}, {0, 0}, {(uintptr_t)b, sizeof(b)}};
    file_size = sizeof(data);
    assert(lpr_fs_iov(42, (uintptr_t)iov, 3, 0) == sizeof(a) + sizeof(b));
    assert(calls == 1 && cursor == sizeof(a) + sizeof(b));
    assert(!memcmp(a, data, sizeof(a)) && !memcmp(b, data + sizeof(a), sizeof(b)));
    assert(lpr_fs_iov(42, (uintptr_t)iov, 3, 0) == sizeof(a) + sizeof(b));
    assert(calls == 2 && cursor == 2 * (sizeof(a) + sizeof(b)));
    file_size = cursor + 1029;
    memset(b, 0xa5, sizeof(b));
    assert(lpr_fs_iov(42, (uintptr_t)iov, 3, 0) == 1029);
    assert(calls == 3 && cursor == file_size && b[5] == 0xa5);
    assert(lpr_fs_iov(42, (uintptr_t)iov, 3, 0) == 0 && calls == 4);
    injected_error = -5;
    memset(a, 0x55, sizeof(a));
    assert(lpr_fs_iov(42, (uintptr_t)iov, 3, 0) == -5 && a[0] == 0x55);
    injected_error = 0; create_error = -12;
    assert(lpr_fs_iov(42, (uintptr_t)iov, 3, 0) == -12 && calls == 5);
    create_error = 0; iov[0].base = 0;
    assert(lpr_fs_iov(42, (uintptr_t)iov, 3, 0) == -14 && calls == 5);
    iov[0].base = UINT64_MAX - 2;
    assert(lpr_fs_iov(42, (uintptr_t)iov, 3, 0) == -14 && calls == 5);
    iov[0].base = (uintptr_t)a;
    assert(lpr_fs_iov(42, (uintptr_t)iov, 1025, 0) == -22);
    cursor = 0; memset(data, 0, sizeof(data));
    assert(lpr_fs_iov(42, (uintptr_t)iov, 3, 1) == sizeof(a) + sizeof(b));
    assert(calls == 6 && cursor == sizeof(a) + sizeof(b));
    assert(!memcmp(data, a, sizeof(a)) && !memcmp(data + sizeof(a), b, sizeof(b)));
    puts("LPR_FS_VECTOR_UNIT_PASS");
    return 0;
}

#include "lpr_ipc_mock.h"
int64_t lpr_ipc_message(uint64_t nr, uint64_t fd, uint64_t send, uint64_t receive,
    uint64_t timeout, uint64_t flags, int *committed) {
    return test_lpr_ipc_noncall(nr, fd, send, receive, timeout, flags, committed);
}

int64_t lpr_pacha_syscall4(uint64_t nr,uint64_t a,uint64_t b,uint64_t c,uint64_t d) {
    (void)nr;(void)a;(void)b;(void)c;(void)d;abort();
}
