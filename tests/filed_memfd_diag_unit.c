#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static unsigned lines;
static char last_line[512];
static int capture_printf(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    int length = vsnprintf(last_line, sizeof(last_line), format, arguments);
    va_end(arguments);
    ++lines;
    return length;
}

#define printf capture_printf
#include "../userland/filed/src/dispatch/ops_file.c"
#undef printf

static unsigned stats_calls;
static int stats_error;
int filed_tmpfs_backend_statfs(filed_tmpfs_backend_t *backend, storage_statfs_reply_t *stats)
{
    assert(backend);
    ++stats_calls;
    if (stats_error) return stats_error;
    stats->files = 256;
    stats->files_free = 0;
    stats->blocks = 16384;
    stats->blocks_free = 16376;
    return 0;
}

int main(void)
{
    static filed_runtime_t runtime;
    for (unsigned i = 1; i <= 65; ++i) {
        unsigned before = lines;
        filed_memfd_failure(&runtime, "tmpfs_create", -28);
        bool expected = i <= 4 || (i & (i - 1)) == 0;
        assert(lines == before + expected);
        assert(stats_calls == lines);
    }
    assert(lines == 8);
    assert(strstr(last_line, "stage=tmpfs_create status=-28 count=64 "));
    assert(strstr(last_line, "stats_status=0 inodes_free=0 inodes=256 pages_free=16376 pages=16384"));
    stats_error = -5;
    for (unsigned i = 66; i <= 128; ++i)
        filed_memfd_failure(&runtime, "vfs_open", -28);
    assert(lines == 9 && stats_calls == 9);
    assert(strstr(last_line, "stage=vfs_open status=-28 count=128 "));
    assert(strstr(last_line, "stats_status=-5 inodes_free=0 inodes=0 pages_free=0 pages=0"));
    puts("FileD memfd failure diagnostics: bounded output, failure-only stats, initialized error snapshot PASS");
}
