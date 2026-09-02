#ifndef LPR_PROCESS_COMPAT_H
#define LPR_PROCESS_COMPAT_H

#include <stdint.h>

/* Process-control operations that are deliberately implemented without
 * native-kernel state.  Keeping their policy here makes it possible to test
 * unsupported namespace behavior on the host. */
int64_t lpr_linux_prctl_compat(uint64_t option,
                               uint64_t arg2,
                               uint64_t arg3,
                               uint64_t arg4,
                               uint64_t arg5);
int64_t lpr_linux_unshare(uint64_t flags);
int64_t lpr_linux_clone_namespace_guard(uint64_t flags);

#endif
