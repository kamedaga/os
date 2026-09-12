/* SPDX-License-Identifier: MIT */
#ifndef PACHA_KOBOX_PACKAGE_H
#define PACHA_KOBOX_PACKAGE_H

#include "bootstrap.h"
#include "boot/package.h"

/* Sandbox-side only. This header uses the GPL package API; gpud's Apache
 * controller must not include/link this backend. Native bootstrap stays MIT
 * and independent of both runtimes. */
struct ph_package_snapshot {
    void *address;
    size_t mapping_size;
};

typedef void (*ph_package_fatal_fn)(void *context, long native_status);

struct ph_package {
    struct kobox_boot_package common;
    struct kobox_package_operations operations;
    struct ph_package_snapshot snapshots[2 + PH_BOOTSTRAP_MAX_ARTIFACTS];
    ph_package_fatal_fn fatal_cleanup;
    void *fatal_context;
    long native_error;
};

/* Zero-initialize package. Borrow a COMPLETE bootstrap bundle and independent
 * launch-owner identity for the duration of this call. All blobs are copied
 * into new private anonymous mappings, then made read-only before the common
 * package verifier sees them. No FD or writable alias to a snapshot is exported.
 * A concurrent publisher write either leaves the expected copied bytes intact
 * or causes digest rejection; it cannot change a successfully verified view.
 *
 * Success owns all snapshots independently of the bundle's FD lifetime.
 * Failure releases snapshots and leaves package empty, but never closes input
 * FDs. Resource handles remain caller-owned and still need native role/kind/
 * rights import checks. A package is NOT an already loaded Linux image.
 *
 * elf_matches is the launcher's selected CPU architecture profile; the native
 * memory backend does not choose an ELF architecture or invoke an OS loader.
 * Single owner; no concurrent package mutation/close. fatal_cleanup MUST NOT
 * return: a failed native unmap cannot satisfy the common close/rollback
 * contract. It must terminate this sandbox, not release backing speculatively.
 */
int ph_package_open(struct ph_package *package, const struct ph_bootstrap_receiver *bundle,
    const struct ph_package_identity *identity,
    bool (*elf_matches)(const void *data, size_t size, bool core),
    ph_package_fatal_fn fatal_cleanup, void *fatal_context);
/* Only after Linux module/image users have stopped borrowing package bytes. */
void ph_package_close(struct ph_package *package);

#endif
