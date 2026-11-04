#include "../lpr_filed_internal.h"
#include "../lpr_wait.h"

int64_t lpr_linux_flock(uint64_t fd, uint64_t operation, unsigned *wait_not_ready)
{
    if (fd > LPR_LINUX_FD_MAX) return -LPR_LINUX_EBADF;
    lpr_fd_arrays_init();
    lpr_fd_pin_t pin;
    if (lpr_fd_table_pin(&lpr_control_fd_table, (uint32_t)fd, &pin))
        return -LPR_LINUX_EBADF;
    int64_t result = -LPR_LINUX_EOPNOTSUPP;
    if (pin.ops_id == LPR_FD_OPS_FILED) {
        result = lpr_filed_endpoint_ready();
        while (!result) {
            const lpr_filed_backend_t *file = pin.state;
            kb2_fs_request_t request = {.opcode=KB2_FILESYSTEM_OP_FLOCK,
                .handle=file->handle, .flags=(uint32_t)operation|LPR_LINUX_LOCK_NB};
            kb2_fs_response_t response;
            result = lpr_fs_call(&request,0,0,0,0,0,0,&response,-1,0);
            if (result != -LPR_LINUX_EAGAIN || (operation & LPR_LINUX_LOCK_NB)) break;
            /* As for F_SETLKW, a blocked client retains its OFD but leaves
             * no sleeping worker/control request or orphan queued acquisition.
             * Each EAGAIN is a completed non-acquisition; never replay an
             * uncertain/error reply. Native sleep defers delivery until this pin is released; its
             * wait marker reaches the syscall dispatcher for signal delivery.
             * The pin spans retries: close/reuse cannot switch the target. */
            const struct pachaos_timespec delay={.tv_sec=0,.tv_nsec=1000000};
            result = lpr_pacha_nanosleep(&delay);
            if (result == -LPR_LINUX_EAGAIN) {
                if (wait_not_ready) *wait_not_ready=1;
                result=LPR_WAIT_RESTART_SYSCALL;
            }
        }
    }
    /* Local native eventfd/tty/socket/pipe capabilities have no Linux struct
     * file at this service. They cannot honestly claim advisory OFD locking;
     * exporting backing OFDs would require an explicit additional contract. */
    lpr_fd_unpin(&pin);
    return result;
}
