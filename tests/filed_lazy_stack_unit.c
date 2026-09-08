#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "exec/linux_lpr/internal.h"

static unsigned char *buffer;
static uint64_t buffer_size, mapped_base, reservation_base, child_sp;
static unsigned batch_calls, close_calls;
static int collisions, batch_error, random_error;

uint64_t lpr_exec_now_ns(void) { return 0; }
uint64_t lpr_exec_now_cycles(void) { return 0; }
void lpr_exec_metric(const char *s, uint64_t a, uint64_t b) { (void)s; (void)a; (void)b; }
void lpr_exec_metric_cycles(const char *s, uint64_t a, uint64_t b) { (void)s; (void)a; (void)b; }
int pacha_vmo_create(uint64_t size, uint64_t rights, uint32_t flags)
{
    (void)rights; (void)flags;
    assert(size >= 65536 && size < 8 * 1024 * 1024);
    buffer_size = size;
    buffer = calloc(1, size);
    assert(buffer);
    return 20;
}
void *pacha_mmap(int fd, uint64_t size, uint64_t prot, uint64_t flags, uint64_t offset)
{
    (void)prot; (void)flags;
    assert(fd == 20 && size == buffer_size && offset == 0);
    return buffer;
}
int pacha_munmap(void *addr, uint64_t size)
{
    assert(addr == buffer && size == buffer_size);
    return 0;
}
int pacha_fd_close(int fd) { assert(fd >= 16); ++close_calls; return 0; }
long pacha_getrandom(void *out, uint64_t size, uint64_t flags)
{
    (void)flags;
    if (random_error) return -5;
    memset(out, (int)(batch_calls + 1), size);
    return (long)size;
}
int pacha_process_map_batch(int fd, const struct pacha_process_map_batch_entry *maps, uint64_t count)
{
    assert(fd == 19 && count == 2);
    ++batch_calls;
    assert(maps[0].vmo_fd == 0 && maps[0].vmo_offset == 0);
    assert(maps[0].flags == (PACHA_PROCESS_MAP_PRIVATE | PACHA_PROCESS_MAP_ANONYMOUS));
    assert(maps[0].target_va % 4096 == 0 && maps[0].size % 4096 == 0);
    assert(maps[0].size + maps[1].size == 8 * 1024 * 1024);
    assert(maps[1].target_va == maps[0].target_va + maps[0].size);
    assert(maps[1].vmo_fd == 20 && maps[1].size == buffer_size);
    assert(maps[1].flags == PACHA_PROCESS_MAP_PRIVATE);
    if (collisions > 0) { --collisions; return -22; }
    if (batch_error) return batch_error;
    mapped_base = maps[1].target_va;
    reservation_base = maps[0].target_va;
    return 0;
}
int pacha_thread_create(int fd, uint64_t entry, uint64_t sp, uint64_t flags, uint64_t fs, uint64_t rights)
{
    (void)entry; (void)flags; (void)fs; (void)rights;
    assert(fd == 19 && mapped_base != 0);
    assert(sp >= mapped_base && sp < mapped_base + buffer_size && sp % 16 == 0);
    assert(mapped_base + buffer_size == reservation_base + 8 * 1024 * 1024);
    child_sp = sp;
    return 21;
}
int pacha_thread_start(int fd) { assert(fd == 21); return 0; }

static void reset(void)
{
    free(buffer); buffer = NULL;
    buffer_size = mapped_base = reservation_base = child_sp = 0;
    batch_calls = close_calls = 0;
    collisions = batch_error = random_error = 0;
}
static unsigned char *guest_pointer(uint64_t va)
{
    assert(va >= mapped_base && va < mapped_base + buffer_size);
    return buffer + (va - mapped_base);
}
int main(void)
{
    filed_exec_path_t request = {0};
    strcpy(request.path, "/bin/test");
    lpr_exec_plan_t plan = { .process_fd = 19, .runtime_entry = 0x1234000 };
    assert(lpr_exec_start_plan(&plan, &request, -1, 1) == 0);
    assert(buffer_size == 65536 && batch_calls == 1 && close_calls == 1);
    uint64_t *sp = (uint64_t *)guest_pointer(child_sp);
    assert(sp[0] == 1 && strcmp((char *)guest_pointer(sp[1]), "/bin/test") == 0);
    assert(sp[2] == 0 && sp[3] == 0);
    reset();

    /* Repeated references can copy more than the wire string buffer's size. */
    request.argc = FILED_EXEC_MAX_ARGS;
    request.envc = FILED_EXEC_MAX_ENVS;
    request.string_bytes = sizeof(request.strings);
    memset(request.strings, 'x', sizeof(request.strings) - 1);
    request.strings[sizeof(request.strings) - 1] = 0;
    for (size_t i = 0; i < request.argc; ++i)
        request.argv[i] = (filed_exec_string_ref_t){0, sizeof(request.strings) - 1};
    for (size_t i = 0; i < request.envc; ++i)
        request.envp[i] = request.argv[0];
    assert(lpr_exec_start_plan(&plan, &request, -1, 0) == 0);
    assert(buffer_size > 1024 * 1024);
    sp = (uint64_t *)guest_pointer(child_sp);
    assert(sp[0] == request.argc);
    for (size_t i = 0; i < request.argc; ++i)
        assert(strlen((char *)guest_pointer(sp[i + 1])) == sizeof(request.strings) - 1);
    reset();

    request.argc = request.envc = 0;
    collisions = 2;
    assert(lpr_exec_start_plan(&plan, &request, -1, 0) == 0);
    assert(batch_calls == 3);
    reset();
    batch_error = -12;
    assert(lpr_exec_start_plan(&plan, &request, -1, 0) == -12);
    assert(batch_calls == 1 && child_sp == 0 && close_calls == 1);
    reset();
    random_error = 1;
    assert(lpr_exec_start_plan(&plan, &request, -1, 0) < 0);
    assert(batch_calls == 0 && child_sp == 0 && close_calls == 1);
    reset();
    puts("filed lazy stack: PASS");
}
