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
    /* Request endpoints stay in the first batch even when the following
     * lifetime-only lease set grows past the native wait bound. */
    static filed_runtime_t runtime;
    struct filed_client second = { .id = 202, .fd = 52 };
    struct filed_client first = { .id = 201, .fd = 51, .next = &second };
    runtime.clients = &first;
    uint64_t count = 35;
    const uint64_t clients_begin = count;
    assert(filed_wait_add_clients(&runtime, &wait, wait.capacity, &count) == 0);
    const uint64_t clients_end = count;
    assert(clients_begin == 35 && clients_end == 37);
    assert(wait.fds[35].fd == 51 && wait.session_indices[35] == 201);
    assert(wait.fds[36].fd == 52 && wait.session_indices[36] == 202);
    assert(wait.fds[35].events == (PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP));
    assert(clients_end < 256);
    for (; count < 2049; count++) {
        wait.lease_handles[count] = count;
        assert(!(count >= clients_begin && count < clients_end));
    }
    count = wait.capacity;
    assert(filed_wait_add_clients(&runtime, &wait, wait.capacity, &count) == -22);
    assert(count == wait.capacity);
    filed_wait_destroy(&wait);
    assert(!wait.fds && !wait.session_indices && !wait.lease_handles && !wait.capacity);
    puts("FileD wait storage: grows beyond fixed wait limit, all allocation failures atomic, overflow checked PASS");
}
