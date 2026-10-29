/* SPDX-License-Identifier: MIT */
#ifndef PACHA_KOBOX_PS2_NATIVE_H
#define PACHA_KOBOX_PS2_NATIVE_H
#include "platform_authority.h"
#include "bootstrap.h"
#include "package.h"

int ph_ps2_grant_take(struct ph_bootstrap_receiver *bundle,
    const struct ph_package *package, const struct ph_platform_authority *authority);
int ph_ps2_prepare(void *core);
#endif
