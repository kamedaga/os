/* SPDX-License-Identifier: MIT */
#ifndef PACHA_GPUD_PACKAGE_H
#define PACHA_GPUD_PACKAGE_H

#include "../kobox2_adapter/bootstrap.h"
#include "../kobox2_adapter/device_authority.h"

#include <kobox2/controller.h>
#include <kobox2/closure_manifest.h>

enum { GPUD_PACKAGE_ARTIFACTS = 13, GPUD_PACKAGE_ITEMS = 16 };

struct gpud_package {
    const unsigned char *sandbox;
    size_t sandbox_size;
    void *sandbox_storage;
    unsigned char manifest[8192], grant[4096];
    size_t manifest_size, grant_size;
    kb2_closure_manifest_artifact_t artifacts[GPUD_PACKAGE_ARTIFACTS];
    struct ph_bootstrap_item items[GPUD_PACKAGE_ITEMS];
    struct ph_package_identity identity;
    struct ph_device_authority device;
    int device_fd;
};

/* Loads the fixed verified VirGL closure through filed and converts immutable
 * bytes to read-only transferable VMOs. device_fd remains caller-owned. */
int gpud_package_open(struct gpud_package *package, int filed_fd, int device_fd);
/* Creates the per-generation resource grant after controller generation is
 * known. The logical IDs are private owner policy, never native FD numbers. */
int gpud_package_bind(struct gpud_package *package, uint64_t generation);
/* Retire only the generation-specific grant VMO after sandbox/resource
 * cleanup. Immutable module and sandbox inputs remain cached for restart. */
int gpud_package_unbind(struct gpud_package *package, uint64_t generation);
int gpud_package_configure_controller(
    const struct gpud_package *package, kb2_controller_t *controller);
int gpud_package_close(struct gpud_package *package);

#endif
