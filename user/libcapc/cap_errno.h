#ifndef CAP_ABILITY_OS_CAP_ERRNO_H
#define CAP_ABILITY_OS_CAP_ERRNO_H

#include <stdint.h>

#include "cap_syscall.h"

/*
 * Minimal errno values for CapabilityOS libc bootstrap.
 * Use this set first; extend only when needed by real call sites.
 */
#define CAP_EPERM 1
#define CAP_ENOENT 2
#define CAP_EIO 5
#define CAP_ENOMEM 12
#define CAP_EACCES 13
#define CAP_EFAULT 14
#define CAP_EBUSY 16
#define CAP_EEXIST 17
#define CAP_ENODEV 19
#define CAP_EINVAL 22
#define CAP_ENOSYS 38
#define CAP_EOVERFLOW 75

extern int cap_errno;

static inline int *__cap_errno_location(void) {
    return &cap_errno;
}

static inline int cap_sys_status_to_errno(uint64_t status) {
    switch (status) {
    case CAP_SYSCALL_OK:
        return 0;
    case CAP_SYSCALL_ERR_INVALID:
        return CAP_EINVAL;
    case CAP_SYSCALL_ERR_NOT_READY:
        return CAP_EBUSY;
    case CAP_SYSCALL_ERR_ALLOC:
        return CAP_ENOMEM;
    case CAP_SYSCALL_ERR_MAP:
        return CAP_EFAULT;
    case CAP_SYSCALL_ERR_MOVE:
        return CAP_EACCES;
    case CAP_SYSCALL_ERR_DROP_PRESENT:
        return CAP_EFAULT;
    case CAP_SYSCALL_ERR_SEND:
        return CAP_EIO;
    case CAP_SYSCALL_ERR_ENDPOINT:
        return CAP_ENODEV;
    case CAP_SYSCALL_ERR_REVOKE:
        return CAP_EACCES;
    case CAP_SYSCALL_ERR_GRANT:
        return CAP_EACCES;
    case CAP_SYSCALL_ERR_LOG:
        return CAP_EINVAL;
    case CAP_SYSCALL_ERR_EMPTY:
        return CAP_ENOENT;
    default:
        return CAP_EIO;
    }
}

/*
 * Convert kernel status (0 / syscall_err_*) to libc-style return value.
 * success_value is returned on CAP_SYSCALL_OK.
 */
static inline int cap_status_to_ret(uint64_t status, int success_value) {
    if (status == CAP_SYSCALL_OK) {
        return success_value;
    }
    * __cap_errno_location() = cap_sys_status_to_errno(status);
    return -1;
}

#endif /* CAP_ABILITY_OS_CAP_ERRNO_H */
