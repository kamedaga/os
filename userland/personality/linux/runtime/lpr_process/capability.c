#include "capability.h"

#include <pacha/status.h>

static uint32_t lpr_linux_capability_words(uint32_t version)
{
    switch (version) {
    case LPR_LINUX_CAPABILITY_VERSION_1:
        return 1;
    case LPR_LINUX_CAPABILITY_VERSION_2:
    case LPR_LINUX_CAPABILITY_VERSION_3:
        return 2;
    default:
        return 0;
    }
}

int64_t lpr_linux_capget(uint64_t header_raw,
                         uint64_t data_raw,
                         int64_t current_pid)
{
    if (header_raw == 0) {
        return -LPR_LINUX_EFAULT;
    }
    lpr_linux_capability_header_t *header =
        (lpr_linux_capability_header_t *)(uintptr_t)header_raw;
    const uint32_t words = lpr_linux_capability_words(header->version);
    if (words == 0) {
        header->version = LPR_LINUX_CAPABILITY_VERSION_3;
        return -LPR_LINUX_EINVAL;
    }
    if (header->pid < 0 ||
        (header->pid != 0 && header->pid != current_pid)) {
        return -LPR_LINUX_ESRCH;
    }
    if (data_raw == 0) {
        return 0;
    }
    lpr_linux_capability_data_t *data =
        (lpr_linux_capability_data_t *)(uintptr_t)data_raw;
    for (uint32_t i = 0; i < words; ++i) {
        data[i].effective = 0;
        data[i].permitted = 0;
        data[i].inheritable = 0;
    }
    return 0;
}

int64_t lpr_linux_capset(uint64_t header_raw,
                         uint64_t data_raw,
                         int64_t current_pid)
{
    if (header_raw == 0 || data_raw == 0) {
        return -LPR_LINUX_EFAULT;
    }
    lpr_linux_capability_header_t *header =
        (lpr_linux_capability_header_t *)(uintptr_t)header_raw;
    const uint32_t words = lpr_linux_capability_words(header->version);
    if (words == 0) {
        header->version = LPR_LINUX_CAPABILITY_VERSION_3;
        return -LPR_LINUX_EINVAL;
    }
    if (header->pid < 0 ||
        (header->pid != 0 && header->pid != current_pid)) {
        return -LPR_LINUX_EPERM;
    }
    const lpr_linux_capability_data_t *data =
        (const lpr_linux_capability_data_t *)(uintptr_t)data_raw;
    for (uint32_t i = 0; i < words; ++i) {
        if (data[i].effective != 0 || data[i].permitted != 0 ||
            data[i].inheritable != 0) {
            return -LPR_LINUX_EPERM;
        }
    }
    return 0;
}
