#include "pacha/status.h"

int64_t pacha_kernel_status_to_errno(int64_t status)
{
    if (status == 0) {
        return 0;
    }
    int negative = 0;
    if (status < 0) {
        negative = 1;
        status = -status;
    }
    if (status > 6) {
        return negative ? -status : status;
    }
    switch (status) {
    case 1:
        return PACHA_STATUS_EINVAL;
    case 2:
    case 5:
        return PACHA_STATUS_EAGAIN;
    case 3:
    case 4:
        return PACHA_STATUS_ENOMEM;
    case 6:
        return -PACHA_LINUX_EPIPE;
    default:
        return PACHA_STATUS_EINVAL;
    }
}
