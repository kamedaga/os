#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../userland/personality/linux/runtime/lpr_filed_internal.h"

static lpr_filed_backend_t file = {.active = 1, .handle = 123};
static unsigned closes, calls, next_cap = 40;
static int64_t close_result;

lpr_filed_backend_t *lpr_filed_backend(uint64_t fd)
{ return fd == 3 ? &file : NULL; }
int64_t lpr_filed_fd_binding(uint64_t fd) { (void)fd; return 0; }
int64_t lpr_pacha_syscall1(uint64_t nr, uint64_t fd)
{
    assert(nr == PACHAOS_SYSCALL_FD_CLOSE && (fd == 40 || fd == 41));
    ++closes; return 0;
}
int64_t lpr_fs_call(kb2_fs_request_t *request, const char *path,
    const char *second, const void *data, uint64_t data_bytes,
    void *output, uint64_t output_bytes, kb2_fs_response_t *response,
    int transfer, int *cap)
{
    assert(!path && !second && !data && !data_bytes && !output &&
        !output_bytes && transfer == -1 && request->handle == 123);
    ++calls;
    *response = (kb2_fs_response_t){0};
    if (request->opcode == KB2_FILESYSTEM_OP_CLOSE) {
        assert(!cap); return close_result;
    }
    if (request->opcode == KB2_FILESYSTEM_OP_SHARED_FILE_VMO)
        return -LPR_LINUX_EOPNOTSUPP;
    assert(request->opcode == KB2_FILESYSTEM_OP_FILE_VMO && cap);
    assert(request->offset == 4096 && request->length == 88301568);
    response->result = (int64_t)request->length;
    *cap = (int)next_cap++;
    return response->result;
}

int main(void)
{
    uint64_t loaded = 0;
    const int64_t first = lpr_linux_file_vmo(3, 4096, 88301568, 0, &loaded);
    assert(first == 40 && loaded == 88301568 && calls == 1 && !closes);
    const int64_t second = lpr_linux_file_vmo(3, 4096, 88301568, 0, &loaded);
    /* No optional image cache retains a hidden native descriptor. Every
     * snapshot has explicit ownership handed directly to the mmap caller. */
    assert(second == 41 && calls == 2 && !closes);
    assert(!lpr_filed_close_handle(123));
    assert(calls == 3 && !closes);
    close_result = -5;
    assert(lpr_filed_close_handle(123) == -5 && calls == 4 && !closes);
    assert(lpr_linux_shared_file_vmo(3, 0, 4096, 1, 0, &loaded) == -LPR_LINUX_EOPNOTSUPP);
    assert(!loaded && calls == 5 && !closes);
    assert(lpr_linux_file_vmo(4, 4096, 88301568, 0, &loaded) == -LPR_LINUX_EBADF);
    assert(calls == 5 && !loaded);
    assert(!lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, first));
    assert(!lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, second));
    assert(closes == 2);
    puts("LPR_FILE_IMAGE_LIFETIME_UNIT_PASS");
}
