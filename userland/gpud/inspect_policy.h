/* SPDX-License-Identifier: MIT */
#ifndef PACHA_GPUD_INSPECT_POLICY_H
#define PACHA_GPUD_INSPECT_POLICY_H

#include <gpud/boot_config.h>
#include <stddef.h>

/* Parse a root-owned RAM policy without changing the boot-granted PCI owner
 * or package location. Digests are committed only after the entire file has
 * been validated. */
int gpud_inspect_policy_parse(const void *bytes, size_t size,
    struct gpud_boot_config *config);

#endif
