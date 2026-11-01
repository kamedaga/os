#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "exec/linux_lpr/internal.h"

static unsigned char *buffer;
static uint64_t buffer_size, vmo_size, mapped_base, reservation_base, child_sp;
static unsigned batch_calls, close_calls;
static int collisions, batch_error, random_error;

uint64_t lpr_exec_now_ns(void) { return 0; }
uint64_t lpr_exec_now_cycles(void) { return 0; }
void lpr_exec_metric(const char *s, uint64_t a, uint64_t b) { (void)s; (void)a; (void)b; }
void lpr_exec_metric_cycles(const char *s, uint64_t a, uint64_t b) { (void)s; (void)a; (void)b; }
pacha_fd_t pacha_vmo_create(uint64_t size, uint64_t rights, uint32_t flags)
{
    (void)rights;
    assert(size == 8 * 1024 * 1024);
    assert(flags == PACHA_VMO_CREATE_ZERO_ON_DEMAND);
    vmo_size = size;
    return 20;
}
void *pacha_mmap(pacha_fd_t fd, uint64_t size, uint64_t prot, uint64_t flags, uint64_t offset)
{
    (void)prot; (void)flags;
    assert(fd == 20 && size >= 65536 && size < vmo_size);
    assert(offset == vmo_size - size);
    buffer_size = size;
    buffer = calloc(1, size);
    assert(buffer);
    return buffer;
}
int pacha_munmap(void *addr, uint64_t size)
{
    assert(addr == buffer && size == buffer_size);
    return 0;
}
int pacha_fd_close(pacha_fd_t fd) { assert(fd >= 16); ++close_calls; return 0; }
long pacha_getrandom(void *out, uint64_t size, uint64_t flags)
{
    (void)flags;
    if (random_error) return -5;
    memset(out, (int)(batch_calls + 1), size);
    return (long)size;
}
int pacha_process_map_batch(pacha_fd_t fd, const struct pacha_process_map_batch_entry *maps, uint64_t count)
{
    assert(fd == 19 && count == 1);
    ++batch_calls;
    assert(maps[0].vmo_fd == 20 && maps[0].vmo_offset == 0);
    assert(maps[0].flags == PACHA_PROCESS_MAP_PRIVATE);
    assert(maps[0].target_va % 4096 == 0);
    assert(maps[0].size == vmo_size);
    if (collisions > 0) { --collisions; return -22; }
    if (batch_error) return batch_error;
    mapped_base = maps[0].target_va + vmo_size - buffer_size;
    reservation_base = maps[0].target_va;
    return 0;
}
pacha_fd_t pacha_thread_create(pacha_fd_t fd, uint64_t entry, uint64_t sp, uint64_t flags, uint64_t fs, uint64_t rights)
{
    (void)entry; (void)flags; (void)fs; (void)rights;
    assert(fd == 19 && mapped_base != 0);
    assert(sp >= mapped_base && sp < mapped_base + buffer_size && sp % 16 == 0);
    assert(mapped_base + buffer_size == reservation_base + 8 * 1024 * 1024);
    child_sp = sp;
    return 21;
}
int pacha_thread_start(pacha_fd_t fd) { assert(fd == 21); return 0; }

static void reset(void)
{
    free(buffer); buffer = NULL;
    buffer_size = vmo_size = mapped_base = reservation_base = child_sp = 0;
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
    char path[] = "/bin/test";
    char strings[4096];
    filed_exec_string_ref_t argv[256], envp[256];
    filed_exec_path_t request = {.path=path,.strings=strings,.argv=argv,.envp=envp};
    lpr_exec_plan_t plan = { .process_fd = 19, .runtime_entry = 0x1234000 };
    assert(lpr_exec_start_plan(&plan, &request, -1, 1) == 0);
    assert(buffer_size == 65536 && batch_calls == 1 && close_calls == 1);
    uint64_t *sp = (uint64_t *)guest_pointer(child_sp);
    assert(sp[0] == 1 && strcmp((char *)guest_pointer(sp[1]), "/bin/test") == 0);
    assert(sp[2] == 0 && sp[3] == 0);
    reset();

    /* Repeated references can copy more than the wire string buffer's size. */
    request.argc = sizeof(argv)/sizeof(argv[0]);
    request.envc = sizeof(envp)/sizeof(envp[0]);
    request.string_bytes = sizeof(strings);
    memset(request.strings, 'x', sizeof(strings) - 1);
    request.strings[sizeof(strings) - 1] = 0;
    for (size_t i = 0; i < request.argc; ++i)
        request.argv[i] = (filed_exec_string_ref_t){0, sizeof(strings)};
    for (size_t i = 0; i < request.envc; ++i)
        request.envp[i] = request.argv[0];
    assert(lpr_exec_start_plan(&plan, &request, -1, 0) == 0);
    assert(buffer_size > 1024 * 1024);
    sp = (uint64_t *)guest_pointer(child_sp);
    assert(sp[0] == request.argc);
    for (size_t i = 0; i < request.argc; ++i)
        assert(strlen((char *)guest_pointer(sp[i + 1])) == sizeof(strings) - 1);
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
