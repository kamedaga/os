/* SPDX-License-Identifier: MIT */
#ifndef PACHA_KOBOX_FOUNDATION_H
#define PACHA_KOBOX_FOUNDATION_H

#include <stddef.h>

struct kobox_linux_boot_layout;

/* Boot-only Gate harness, not a DRM service entry. Borrow verified core bytes
 * for this process's lifetime. The optional callback runs after all selected
 * Gates pass, before native process exit; it must not release the running
 * core/package or report DRM service readiness. */
_Noreturn void ph_foundation_run(const void *core, size_t core_size,
    void (*completed)(void *), void *context);

/* prepare runs once with the actual reserved layout, after machine/TLS setup
 * and before Linux entry. It may initialize already-authorized native ports.
 * It must not recreate the layout or retain a pointer to this stack object.
 * Both callbacks borrow context until process exit. */
_Noreturn void ph_foundation_run_prepared(const void *core, size_t core_size,
    void (*prepare)(const struct kobox_linux_boot_layout *, void *),
    void (*completed)(void *), void *context);

#endif
