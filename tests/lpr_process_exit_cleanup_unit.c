#include "../userland/personality/linux/runtime/lpr_process/exec.c"
#include <assert.h>
#include <stdio.h>

lpr_state_t lpr_state;
static unsigned initialized, closed, sessions;
void lpr_trace_process_event(const char *stage, uint64_t a, uint64_t b, int64_t c)
{ (void)stage; (void)a; (void)b; (void)c; }
int lpr_runtime_reserved_fd(uint64_t fd) { (void)fd; return 0; }
void lpr_fd_arrays_init(void) { ++initialized; }
int lpr_control_fd_active(uint64_t fd) { return fd == 3; }
void lpr_control_close_fd(uint64_t fd) { assert(fd == 3); ++closed; }
int64_t lpr_close_native_fd_if_open(uint64_t fd) { (void)fd; assert(0); return -1; }
int64_t lpr_filed_close_handle(uint64_t handle) { (void)handle; assert(0); return -1; }
void lpr_filed_session_drop(void) { ++sessions; }

int main(void)
{
    lpr_fd_table_capacity = 8;
    lpr_state.thread_count = 2;
    lpr_linux_prepare_process_exit(37);
    assert(!initialized && !closed && !sessions);
    lpr_state.thread_count = 1;
    lpr_linux_prepare_process_exit(37);
    assert(initialized == 1 && closed == 1 && sessions == 1);
    puts("Process exit: leave concurrent FDs intact until native retirement; single-thread cleanup PASS");
}
