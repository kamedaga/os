#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../userland/personality/linux/runtime/lpr_filed_internal.h"

lpr_state_t lpr_state;
static unsigned closes, calls;
static int64_t close_result;
static uint64_t expected_handle;

void lpr_state_lock(volatile uint32_t *word) { assert(!*word); *word = 1; }
void lpr_state_unlock(volatile uint32_t *word) { assert(*word); *word = 0; }
void *lpr_memset(void *dst, int value, size_t bytes) { return memset(dst, value, bytes); }

int64_t lpr_pacha_syscall1(uint64_t nr, uint64_t fd)
{
    assert(nr == PACHAOS_SYSCALL_FD_CLOSE);
    assert(fd == 40 || fd == 41);
    ++closes;
    return 0;
}

int64_t lpr_filed_call(uint32_t op, int page_fd, uint64_t handle, uint64_t *result)
{
    assert(op == FILED_OP_VFS_CLOSE && page_fd == -1);
    assert(handle == expected_handle);
    (void)result;
    ++calls;
    return close_result;
}

static void seed(void)
{
    memset(&lpr_state, 0, sizeof(lpr_state));
    lpr_file_image_cache[0] = (lpr_file_image_cache_entry_t) {
        .active = 1, .handle = 123, .vmo_fd = 40, .length = 88301568,
        .object_generation = 1,
    };
    lpr_file_image_cache[1] = (lpr_file_image_cache_entry_t) {
        .active = 1, .handle = 456, .vmo_fd = 42, .length = 4096,
        .object_generation = 1,
    };
    /* A stale generation must not keep another snapshot pinned either. */
    lpr_file_image_cache[2] = (lpr_file_image_cache_entry_t) {
        .active = 1, .handle = 123, .vmo_fd = 41, .length = 88301568,
        .object_generation = 2,
    };
    closes = calls = 0;
    expected_handle = 123;
}

int main(void)
{
    seed();
    assert(lpr_filed_close_handle(123) == 0);
    assert(calls == 1 && closes == 2);
    assert(!lpr_file_image_cache[0].active && !lpr_file_image_cache[2].active);
    assert(lpr_file_image_cache[1].active && lpr_file_image_cache[1].vmo_fd == 42);
    assert(lpr_filed_close_handle(123) == 0 && closes == 2);
    expected_handle = 0;
    assert(lpr_filed_close_handle(0) == 0 && closes == 2);
    seed();
    close_result = -5;
    assert(lpr_filed_close_handle(123) == -5);
    /* Dropping an optional cache is safe even if the close RPC fails. */
    assert(closes == 2 && lpr_file_image_cache[1].active);
    puts("lpr file image lifetime unit: PASS");
    return 0;
}
