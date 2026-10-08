/* SPDX-License-Identifier: MIT */
#include "../userland/personality/linux/runtime/lpr_filed_internal.h"
#include "../userland/personality/linux/runtime/lpr_common/boot_diagnostic.h"

void lpr_boot_diagnostic_other_tu(void)
{
    lpr_boot_diagnostic_emit("other-tu", UINT64_MAX, 0, 0, -22, 19);
}
