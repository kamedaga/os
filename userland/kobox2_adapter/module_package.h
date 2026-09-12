/* SPDX-License-Identifier: MIT */
#ifndef PACHA_KOBOX_MODULE_PACKAGE_H
#define PACHA_KOBOX_MODULE_PACKAGE_H

#include "package.h"
#include "boot/module_launch.h"

#define PH_MODULE_NAME_CAPACITY 128u
struct ph_module_package {
    size_t count;
    struct kobox_linux_native_module modules[PH_BOOTSTRAP_MAX_ARTIFACTS - 1];
    char names[PH_BOOTSTRAP_MAX_ARTIFACTS - 1][PH_MODULE_NAME_CAPACITY];
};

/* Successfully opened, immutable package only. Borrow its module bytes until
 * Linux has unloaded every module. Names are copied into caller-owned storage;
 * the output must not move after success. No ELF/module layout is interpreted:
 * upstream module loading checks actual names before constructors/init.
 * Caller serializes access. Failure leaves output unchanged. */
int ph_module_package_open(struct ph_module_package *modules, const struct ph_package *package);

#endif
