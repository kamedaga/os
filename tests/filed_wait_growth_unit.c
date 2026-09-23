#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../userland/filed/src/runtime.c"

static unsigned fail_malloc;
void *__real_malloc(size_t);
void *__wrap_malloc(size_t size)
{
    if (fail_malloc && --fail_malloc == 0) return NULL;
    return __real_malloc(size);
}

int main(void)
{
    struct filed_wait_storage wait = {0};
    assert(filed_wait_reserve(&wait, 1) == 0);
    wait.fds[0].fd = 42;
    wait.session_indices[0] = 43;
    wait.lease_handles[0] = 44;
    struct filed_wait_storage saved = wait;
    for (unsigned allocation = 1; allocation <= 3; ++allocation) {
        fail_malloc = allocation;
        assert(filed_wait_reserve(&wait, 2049) == -12);
        assert(!fail_malloc);
        assert(wait.capacity == saved.capacity && wait.fds == saved.fds);
        assert(wait.session_indices == saved.session_indices && wait.lease_handles == saved.lease_handles);
        assert(wait.fds[0].fd == 42 && wait.session_indices[0] == 43 && wait.lease_handles[0] == 44);
    }
    assert(filed_wait_reserve(&wait, 2049) == 0);
    assert(wait.capacity >= 2049);
    wait.fds[2048].fd = 100;
    wait.session_indices[2048] = 101;
    wait.lease_handles[2048] = 102;
    saved = wait;
    assert(filed_wait_reserve(&wait, 2049) == 0 && wait.fds == saved.fds);
    assert(filed_wait_reserve(&wait, SIZE_MAX) == -12 && wait.fds == saved.fds);
    filed_wait_destroy(&wait);
    assert(!wait.fds && !wait.session_indices && !wait.lease_handles && !wait.capacity);
    puts("FileD wait storage: grows beyond fixed wait limit, all allocation failures atomic, overflow checked PASS");
}
