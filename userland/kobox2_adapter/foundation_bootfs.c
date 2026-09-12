/* SPDX-License-Identifier: MIT */
#include "bootfs.h"
#include "foundation.h"

/* Initial-process fixture packaging. A gpud-launched sandbox instead obtains
 * its independently verified core from the native bootstrap channel. */
void ph_main(void) {
    size_t size;
    const void *core = ph_bootfs_core(&size);
    ph_foundation_run(core, size, NULL, NULL);
}
