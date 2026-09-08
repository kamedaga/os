#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include "../userland/filed/src/dispatch/ops_statat.c"

static filed_runtime_t runtime;
static unsigned opens, stats, closes, live;
static int64_t open_error, stat_error, close_error;
static uint64_t expected_flags;
filed_page_dispatch_result_t filed_page_result(int64_t status, uint64_t result)
{ return (filed_page_dispatch_result_t){.status=status, .result=result}; }
int filed_name_is_terminated(const char *s, size_t n)
{ return memchr(s, 0, n) != NULL; }
int64_t filed_openat_path(filed_runtime_t *rt, const filed_openat_t *op, filed_vfs_open_result_t *out)
{
    assert(rt == &runtime && op->rights == FILED_RIGHT_STAT);
    assert(op->open_flags == expected_flags);
    ++opens;
    if (open_error) return open_error;
    ++live; out->handle_id = 72; return 0;
}
filed_page_dispatch_result_t filed_dispatch_stat_page(filed_runtime_t *rt, void *page)
{
    filed_statx_t *st = page;
    assert(rt == &runtime && st->handle == 72 && live == 1);
    ++stats; st->size = 1234; st->inode_number = 5678;
    return filed_page_result(stat_error, stat_error ? 0 : sizeof(*st));
}
int64_t filed_close_handle_runtime(filed_runtime_t *rt, filed_handle_id_t handle)
{
    assert(rt == &runtime && handle == 72 && live == 1);
    ++closes; --live; return close_error;
}
int main(void)
{
    filed_statat_t op = {.dir_handle=9, .name="relative"};
    for (unsigned i = 0; i < 10000; ++i) {
        assert(filed_dispatch_statat_page(&runtime, &op).status == 0);
        assert(op.stat.handle == 0 && op.stat.size == 1234 && op.stat.inode_number == 5678);
        assert(!live && opens == stats && stats == closes);
    }
    expected_flags = FILED_OPEN_NOFOLLOW; op.flags = FILED_STATAT_NOFOLLOW;
    assert(filed_dispatch_statat_page(&runtime, &op).status == 0);
    expected_flags = op.flags = 0;
    open_error = -2;
    unsigned before = closes;
    assert(filed_dispatch_statat_page(&runtime, &op).status == -2 && closes == before);
    open_error = 0; stat_error = -5;
    assert(filed_dispatch_statat_page(&runtime, &op).status == -5 && closes == before + 1);
    assert(!live && !op.stat.handle && !op.stat.size);
    close_error = -12;
    assert(filed_dispatch_statat_page(&runtime, &op).status == -5);
    stat_error = 0;
    assert(filed_dispatch_statat_page(&runtime, &op).status == -12 && !op.stat.size);
    close_error = 0;
    before = opens;
    op.flags = 2;
    assert(filed_dispatch_statat_page(&runtime, &op).status == -22 && opens == before);
    op.flags = 0; op.name[0] = 0;
    assert(filed_dispatch_statat_page(&runtime, &op).status == -2 && opens == before);
    memset(op.name, 'a', sizeof(op.name));
    assert(filed_dispatch_statat_page(&runtime, &op).status == -22 && opens == before);
    strcpy(op.name, "relative"); op.dir_handle = UINT64_MAX;
    assert(filed_dispatch_statat_page(&runtime, &op).status == -9 && opens == before);
    strcpy(op.name, "/absolute");
    assert(filed_dispatch_statat_page(&runtime, &op).status == 0 && !live);
    assert(filed_dispatch_statat_page(NULL, &op).status == -22);
    assert(filed_dispatch_statat_page(&runtime, NULL).status == -22);
    puts("filed statat unit: PASS");
}
