/* Real loader with modeled filed/VM boundaries; no filesystem or device I/O. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <filed_client/module_image.h>
#include <filed_client/client.h>
#include <pacha/ipc.h>

static int logs, allocations, frees, fail_allocation;
static char diagnostic[1024];
static int log_printf(const char *format, ...)
{
    va_list ap; va_start(ap, format);
    int n = vsnprintf(diagnostic, sizeof(diagnostic), format, ap);
    va_end(ap); ++logs; return n;
}
static void *image_malloc(size_t size)
{ if (fail_allocation) return NULL; ++allocations; return malloc(size); }
static void image_free(void *p) { if (p) ++frees; free(p); }
#define printf log_printf
#define malloc image_malloc
#define free image_free
#include "../userland/libfiled_client/src/module_image.c"
#undef printf
#undef malloc
#undef free

static int open_transport, open_error, stat_error, vmo_error, close_error, fail_mapping;
static unsigned opens, stats, vmos, closes, fd_closes, maps, unmaps;
static uint64_t file_size, stat_length, vmo_length, vmo_offset;
static size_t vmo_count;
static unsigned char payload[4096] = "probe\n";
static const char path[] = "/run/pacha/gpu-control";

int filedfs_call(pacha_fd_t endpoint, kb2_fs_request_t *request,
    const char *first, const char *second, const void *data, size_t size,
    void *output, size_t capacity, kb2_fs_response_t *response,
    const pacha_fd_t *source, size_t source_count,
    pacha_fd_t *fds, size_t *count)
{
    assert(endpoint == 32 && !second && !data && !size && !source && !source_count);
    memset(response, 0, sizeof(*response));
    switch (request->opcode) {
    case KB2_FILESYSTEM_OP_OPENAT2:
        ++opens; assert(first && !strcmp(first, path));
        response->handle = 123; response->result = open_error; return open_transport;
    case KB2_FILESYSTEM_OP_STATX:
        ++stats; assert(!first && request->handle == 123);
        assert(capacity == KB2_FILESYSTEM_STATX_SIZE);
        memset(output, 0, capacity);
        for (unsigned i = 0; i < 8; ++i)
            ((unsigned char *)output)[KB2_FILESYSTEM_STATX_SIZE_OFFSET + i] = file_size >> (i * 8);
        response->data_length = stat_length; response->result = stat_error; return 0;
    case KB2_FILESYSTEM_OP_FILE_VMO:
        ++vmos; assert(request->handle == 123 && request->length == file_size);
        assert(fds && count && *count == 1); *fds = 77; *count = vmo_count;
        response->result = vmo_error ? vmo_error : (int64_t)vmo_length;
        response->handle = vmo_offset; return 0;
    case KB2_FILESYSTEM_OP_CLOSE:
        ++closes; assert(request->handle == 123);
        response->result = close_error; return 0;
    default: assert(!"unexpected filed operation"); return -ENOSYS;
    }
}
int pacha_fd_close(pacha_fd_t fd) { assert(fd == 77); ++fd_closes; return 0; }
void *pacha_mmap(pacha_fd_t fd, uint64_t size, uint64_t protection,
    uint64_t flags, uint64_t offset)
{
    assert(fd == 77 && size == 4096 && protection == PACHA_PROT_READ);
    assert(flags == PACHA_MMAP_PRIVATE && offset == 4096);
    ++maps; return fail_mapping ? NULL : payload;
}
int pacha_munmap(void *address, uint64_t size)
{ assert(address == payload && size == 4096); ++unmaps; return 0; }

static void reset(void)
{
    open_transport = open_error = stat_error = vmo_error = close_error = fail_mapping = 0;
    logs = allocations = frees = fail_allocation = 0; diagnostic[0] = 0;
    opens = stats = vmos = closes = fd_closes = maps = unmaps = 0;
    file_size = vmo_length = 6; stat_length = KB2_FILESYSTEM_STATX_SIZE;
    vmo_offset = 4096; vmo_count = 1;
}
static int load(int optional, size_t limit, struct filed_client_module_image *out)
{
    return optional ? filed_client_load_optional_module_image_limited(32, path,
        "gpu-control", limit, out) : filed_client_load_module_image_limited(32, path,
        "gpu-control", limit, out);
}
static void empty(const struct filed_client_module_image *out)
{ assert(!out->data && !out->size && !out->name); }

int main(void)
{
    struct filed_client_module_image out;
    for (int transport = 0; transport <= 1; ++transport) {
        for (int optional = 0; optional <= 1; ++optional) {
            reset(); if (transport) open_transport = -ENOENT; else open_error = -ENOENT;
            memset(&out, 0xa5, sizeof(out));
            assert(load(optional, 16, &out) == -ENOENT); empty(&out);
            assert(logs == !optional && opens == 1 && !stats && !closes && !fd_closes);
            if (!optional) assert(strstr(diagnostic, "OPEN /run/pacha/gpu-control status=-2"));
        }
    }
    puts("PASS optional OPEN ENOENT quiet / required logged (transport and server errors)");
    for (int optional = 0; optional <= 1; ++optional) {
        reset(); open_error = -EACCES;
        assert(load(optional, 16, &out) == -EACCES && logs == 1);
        assert(strstr(diagnostic, "OPEN") && strstr(diagnostic, "status=-13")); empty(&out);
        reset(); stat_error = -ENOENT;
        assert(load(optional, 16, &out) == -ENOENT && logs == 1);
        assert(strstr(diagnostic, "STATX") && closes == 1 && !vmos); empty(&out);
        reset(); stat_length = 1;
        assert(load(optional, 16, &out) == -EPROTO && logs > 0 && closes == 1); empty(&out);
        reset(); file_size = 17;
        assert(load(optional, 16, &out) == -E2BIG && closes == 1 && !vmos); empty(&out);
        reset(); file_size = 0;
        assert(load(optional, 16, &out) == -E2BIG && closes == 1 && !vmos); empty(&out);
        reset(); vmo_offset = 1;
        assert(load(optional, 16, &out) == -EIO && logs == 1);
        assert(closes == 1 && fd_closes == 1 && !maps); empty(&out);
        reset(); vmo_count = 0;
        assert(load(optional, 16, &out) == -EIO && closes == 1 && fd_closes == 1); empty(&out);
        reset(); vmo_error = -EACCES;
        assert(load(optional, 16, &out) == -EACCES && logs == 1);
        assert(closes == 1 && fd_closes == 1 && !maps); empty(&out);
        reset(); close_error = -EIO;
        assert(load(optional, 16, &out) == -EIO && closes == 1 && fd_closes == 1 && !maps); empty(&out);
        reset(); fail_mapping = 1;
        assert(load(optional, 16, &out) == -ENOMEM);
        assert(closes == 1 && fd_closes == 1 && maps == 1 && !unmaps); empty(&out);
        reset(); fail_allocation = 1;
        assert(load(optional, 16, &out) == -ENOMEM);
        assert(closes == 1 && fd_closes == 1 && maps == 1 && unmaps == 1); empty(&out);
        reset(); assert(load(optional, 16, &out) == 0);
        assert(out.data != payload && out.size == 6 && !strcmp(out.name, "gpu-control"));
        assert(!memcmp(out.data, payload, 6) && allocations == 1 && !frees && !logs);
        assert(closes == 1 && fd_closes == 1 && maps == 1 && unmaps == 1);
        filed_client_release_module_image(&out); empty(&out); assert(frees == 1);
        filed_client_release_module_image(&out); assert(frees == 1);
    }
    puts("PASS optional/required identical validation, success ownership and failure cleanup");
    puts("FILED_OPTIONAL_MODULE_IMAGE_UNIT=PASS");
    return 0;
}
