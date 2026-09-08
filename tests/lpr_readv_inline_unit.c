#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../userland/personality/linux/runtime/lpr_filed_internal.h"

static unsigned char wire[FILED_PAGE_BYTES], data[16000];
static uint64_t cursor, file_size, calls;
static int injected_error, create_error;

void *lpr_memset(void *p, int c, size_t n) { return memset(p,c,n); }
void *lpr_memcpy(void *d, const void *s, size_t n) { return memcpy(d,s,n); }
int lpr_create_wire_page(void **page)
{ if (create_error) return create_error; *page = wire; return 35; }
void lpr_destroy_wire_page(int fd, void *page)
{ assert(fd == 35 && page == wire); }
int64_t lpr_filed_call(uint32_t op, int fd, uint64_t word, uint64_t *result)
{
    assert(op == FILED_OP_VFS_READ && fd == 35 && word == 0);
    filed_io_t *io = (void *)wire;
    assert(io->handle == 42 && io->offset == 0 && io->length <= FILED_IO_BYTES);
    ++calls;
    if (injected_error) return injected_error;
    uint64_t n = cursor < file_size ? file_size - cursor : 0;
    if (n > io->length) n = io->length;
    memcpy(io->data, data+cursor, n);
    cursor += n;
    *result = n;
    return 0;
}
uint64_t lpr_scatter_iov(const lpr_linux_iovec_t *iov, uint64_t count,
    const unsigned char *src, uint64_t length)
{
    uint64_t copied = 0;
    for (uint64_t i = 0; i < count && copied < length; ++i) {
        uint64_t n = iov[i].len < length-copied ? iov[i].len : length-copied;
        if (n) memcpy((void *)(uintptr_t)iov[i].base, src+copied, n);
        copied += n;
    }
    return copied;
}
#include "../userland/personality/linux/runtime/lpr_vfs/io.c"

int main(void)
{
    for (unsigned i = 0; i < sizeof(data); ++i) data[i] = (unsigned char)(i*37u);
    unsigned char a[1024], b[FILED_IO_BYTES];
    lpr_linux_iovec_t iov[] = {{(uintptr_t)a, sizeof(a)}, {0,0}, {(uintptr_t)b,19}};
    file_size = sizeof(data);
    assert(lpr_filed_readv_inline(42,iov,3,1043) == 1043);
    assert(calls == 1 && cursor == 1043);
    assert(!memcmp(a,data,1024) && !memcmp(b,data+1024,19));
    assert(lpr_filed_readv_inline(42,iov,3,1043) == 1043);
    assert(calls == 2 && cursor == 2086 && !memcmp(a,data+1043,1024));
    file_size = cursor+1029;
    memset(b,0xa5,sizeof(b));
    assert(lpr_filed_readv_inline(42,iov,3,1043) == 1029);
    assert(calls == 3 && cursor == file_size && b[5] == 0xa5);
    assert(lpr_filed_readv_inline(42,iov,3,1043) == 0 && calls == 4);
    injected_error = -5;
    memset(a,0x55,sizeof(a));
    assert(lpr_filed_readv_inline(42,iov,3,1043) == -5 && a[0] == 0x55);
    assert(calls == 5);
    injected_error = 0;
    create_error = -12;
    assert(lpr_filed_readv_inline(42,iov,3,1043) == -12 && calls == 5);
    create_error = 0;
    iov[0].base = 0;
    assert(lpr_filed_readv_inline(42,iov,3,1043) == -14 && calls == 5);
    iov[0].base = UINT64_MAX-2;
    assert(lpr_filed_readv_inline(42,iov,3,1043) == -14 && calls == 5);
    iov[0].base = (uintptr_t)a;
    assert(lpr_filed_readv_inline(42,iov,3,FILED_IO_BYTES+1) == -22);
    cursor = 0; file_size = sizeof(data); iov[2].len = FILED_IO_BYTES-sizeof(a);
    assert(lpr_filed_readv_inline(42,iov,3,FILED_IO_BYTES) == FILED_IO_BYTES);
    assert(calls == 6 && cursor == FILED_IO_BYTES);
    assert(!memcmp(b,data+1024,FILED_IO_BYTES-1024));
    puts("READV_INLINE_UNIT_PASS");
    return 0;
}
