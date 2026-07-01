#include "lpr_vfs_local.h"
#include "lpr_linux_syscall.h"
#include "support/string.h"

int64_t lpr_linux_getcwd(uint64_t buf, uint64_t size)
{
    static const char cwd[] = "/";
    if (buf == 0) {
        return -LPR_LINUX_EFAULT;
    }
    if (size < sizeof(cwd)) {
        return -LPR_LINUX_ERANGE;
    }
    lpr_memcpy((void *)(uintptr_t)buf, cwd, sizeof(cwd));
    return sizeof(cwd);
}
