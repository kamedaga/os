/* Whole-program focused build retains the actual public READ/pin/dispatch
 * path while discarding unrelated backend operations and their services. */
#include "../userland/personality/linux/runtime/lpr_fd/ops.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>

lpr_state_t lpr_state;
#if defined(LPR_BOOT_DIAGNOSTICS) && LPR_BOOT_DIAGNOSTICS
lpr_boot_diagnostic_state_t lpr_boot_diagnostic_state;
#endif
static uint8_t kind;
static unsigned pins, unpins, dispatched, notes, logs;
static int64_t log_result, read_result;
static char last_log[256];

/* READ is the only exercised operation. Retained dispatch alternatives must
 * trap if accidentally selected, rather than mask a changed dispatch. */
#define UNUSED3(name) int64_t name(uint64_t a, uint64_t b, uint64_t c) \
    { (void)a; (void)b; (void)c; assert(0); return -1; }
#define UNUSED2(name) int64_t name(uint64_t a, uint64_t b) \
    { (void)a; (void)b; assert(0); return -1; }
UNUSED3(lpr_linux_socket_read)
UNUSED3(lpr_linux_socket_write)
UNUSED3(lpr_linux_socket_readv)
UNUSED3(lpr_linux_socket_writev)
UNUSED3(lpr_linux_socket_ioctl)
UNUSED2(lpr_linux_socket_fstat)
UNUSED3(lpr_backend_write)
UNUSED3(lpr_backend_writev)
UNUSED3(lpr_backend_ioctl)
UNUSED2(lpr_backend_fstat)
int64_t lpr_unix_socket_io(const lpr_fd_pin_t *p, uint64_t a, uint64_t b, int w, uint64_t f)
{ (void)p; (void)a; (void)b; (void)w; (void)f; assert(0); return -1; }
int64_t lpr_unix_socket_iov(const lpr_fd_pin_t *p, uint64_t a, uint64_t b, int w, uint64_t f, uint32_t *m)
{ (void)p; (void)a; (void)b; (void)w; (void)f; (void)m; assert(0); return -1; }
int64_t lpr_unix_socket_ioctl(const lpr_fd_pin_t *p, uint64_t a, uint64_t b)
{ (void)p; (void)a; (void)b; assert(0); return -1; }
int64_t lpr_unix_socket_stat(const lpr_fd_pin_t *p, uint64_t a)
{ (void)p; (void)a; assert(0); return -1; }
int64_t lpr_backend_readv(const lpr_fd_pin_t *p, uint64_t a, uint64_t b)
{ (void)p; (void)a; (void)b; assert(0); return -1; }
int64_t lpr_filed_fstat_pinned(const lpr_fd_pin_t *p, uint64_t a)
{ (void)p; (void)a; assert(0); return -1; }
int64_t lpr_filed_endpoint_ready(void) { assert(0); return -1; }
int64_t lpr_filed_io_handle(uint32_t op, uint64_t h, uint64_t b, uint64_t n, uint64_t off)
{ (void)op; (void)h; (void)b; (void)n; (void)off; assert(0); return -1; }
int64_t lpr_fs_iov(uint64_t h, uint64_t b, uint64_t n, int w)
{ (void)h; (void)b; (void)n; (void)w; assert(0); return -1; }

int lpr_fd_table_pin(lpr_fd_table_t *table, lpr_linux_fd_t fd, lpr_fd_pin_t *pin)
{
    assert(table == &lpr_control_fd_table && fd == 3);
    *pin = (lpr_fd_pin_t){.fd = fd, .ops_id = kind, .effective_rights = LPR_FD_RIGHT_READ};
    ++pins; return 0;
}
int lpr_fd_table_unpin(lpr_fd_table_t *table, const lpr_fd_pin_t *pin, lpr_fd_drop_t *drop)
{ assert(table == &lpr_control_fd_table && pin->fd == 3); *drop = (lpr_fd_drop_t){0}; ++unpins; return 0; }
int64_t lpr_backend_read(const lpr_fd_pin_t *pin, uint64_t buf, uint64_t count)
{
    assert(pin->fd == 3 && pin->ops_id == kind && buf == 0x10000 && count == UINT64_MAX);
    ++dispatched;
    return read_result;
}
void lpr_epoll_note_fd_state(uint64_t fd, uint32_t events, int64_t result)
{ assert(fd == 3 && events == LPR_EPOLL_IO_READ && result == read_result); ++notes; }
int64_t lpr_pacha_syscall2(uint64_t nr, uint64_t data, uint64_t bytes)
{
    assert(nr == PACHAOS_SYSCALL_LOG && bytes > 0 && bytes < sizeof(last_log));
    memcpy(last_log, (const void *)(uintptr_t)data, bytes); last_log[bytes] = 0;
    assert(strstr(last_log, "fd3.read.enter") && strstr(last_log, " a=0x0000000000000003"));
    assert(strstr(last_log, " c=0xffffffffffffffff") && last_log[bytes - 1] == '\n');
    ++logs; return log_result;
}

int main(void)
{
#if defined(LPR_BOOT_DIAGNOSTICS) && LPR_BOOT_DIAGNOSTICS
    lpr_process_manifest.linux_pid = 0;
    lpr_boot_diagnostic_read_enter(0, 3, LPR_FD_OPS_PIPE, 1);
    lpr_process_manifest.linux_pid = 17;
    lpr_boot_diagnostic_read_enter(0, 3, LPR_FD_OPS_PIPE, 1);
    lpr_process_manifest.linux_pid = 16;
    lpr_boot_diagnostic_read_enter(1, 3, LPR_FD_OPS_PIPE, 1);
    lpr_boot_diagnostic_read_enter(0, 4, LPR_FD_OPS_PIPE, 1);
    assert(!logs && !lpr_boot_diagnostic_state.fd3_reads);
#endif
    const uint8_t kinds[] = {LPR_FD_OPS_FILED, LPR_FD_OPS_TTY, LPR_FD_OPS_PIPE};
    const int64_t log_results[] = {0, 19, -19};
    for (unsigned k = 0; k < sizeof(kinds); ++k) {
        for (unsigned s = 0; s < sizeof(log_results) / sizeof(log_results[0]); ++s) {
            kind = kinds[k]; log_result = log_results[s];
            pins = unpins = dispatched = notes = logs = 0;
            lpr_linux_current_pid = 0; lpr_process_manifest.linux_pid = 16;
#if defined(LPR_BOOT_DIAGNOSTICS) && LPR_BOOT_DIAGNOSTICS
            memset(&lpr_boot_diagnostic_state, 0, sizeof(lpr_boot_diagnostic_state));
#endif
            for (unsigned i = 0; i < 10; ++i) {
                read_result = i & 1 ? -LPR_LINUX_EAGAIN : 37;
                assert(lpr_linux_read(3, 0x10000, UINT64_MAX) == read_result);
            }
            assert(pins == 10 && unpins == 10 && dispatched == 10 && notes == 10);
#if defined(LPR_BOOT_DIAGNOSTICS) && LPR_BOOT_DIAGNOSTICS
            assert(logs == (log_result ? 1 : 3));
            assert(lpr_boot_diagnostic_state.fd3_reads == 3);
            assert(strstr(last_log, "pid=16"));
            char expected[32];
            snprintf(expected, sizeof(expected), " b=0x%016llx", (unsigned long long)kind);
            assert(strstr(last_log, expected));
#else
            assert(!logs);
            unsigned evaluated = 0;
            lpr_boot_diagnostic_read_enter(++evaluated, ++evaluated, ++evaluated, ++evaluated);
            assert(!evaluated);
#endif
        }
    }
    puts("LPR_FD3_READ_DIAGNOSTIC_UNIT_PASS");
}
