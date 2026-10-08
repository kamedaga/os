/* Retain the actual public prebind entry, with terminal admission fixed true.
 * Whole-program elimination makes any supervisor/kernel dependency a link
 * error, proving that this path does not request another filesystem identity. */
#include "../userland/personality/linux/runtime/lpr_vfs/path.c"
#include <assert.h>
#include <stdio.h>

lpr_state_t lpr_state;
int lpr_fs_session_terminal(void) { return 1; }
int lpr_fd_is_filed(uint64_t fd) { return fd == 3; }

int main(void)
{
    lpr_filed_endpoint_checked = 1;
    assert(lpr_filed_endpoint_ready() == -LPR_LINUX_EIO);
    assert(lpr_filed_fd_binding(3) == -LPR_LINUX_EIO);
    assert(!lpr_filed_fd_binding(4)); /* Local backends need no FS identity. */
    puts("LPR_FILED_TERMINAL_BINDING_UNIT_PASS");
}
