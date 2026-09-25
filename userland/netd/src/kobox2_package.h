/* SPDX-License-Identifier: MIT */
#ifndef PACHA_NETD_KOBOX2_PACKAGE_H
#define PACHA_NETD_KOBOX2_PACKAGE_H

#include "../../kobox2_adapter/bootstrap.h"
#include "../../kobox2_adapter/device_authority.h"

#include <kobox2/controller.h>
#include <kobox2/closure_manifest.h>

enum { NETD_PACKAGE_MAX_ARTIFACTS = 64, NETD_PACKAGE_ITEMS = 67 };

struct netd_kobox2_package {
    const unsigned char *sandbox;
    size_t sandbox_size;
    void *sandbox_storage;
    unsigned char manifest[8192], grant[4096];
    size_t manifest_size, grant_size;
    kb2_closure_manifest_t decoded;
    size_t artifact_count;
    struct ph_bootstrap_item items[NETD_PACKAGE_ITEMS];
    struct ph_package_identity identity;
    struct ph_device_authority device;
    int device_fd;
};

int netd_kobox2_package_open(struct netd_kobox2_package *package,
    int filed_fd, int device_fd);
int netd_kobox2_package_bind(struct netd_kobox2_package *package,
    uint64_t generation);
int netd_kobox2_package_unbind(struct netd_kobox2_package *package,
    uint64_t generation);
int netd_kobox2_package_configure_controller(
    const struct netd_kobox2_package *package, kb2_controller_t *controller);
int netd_kobox2_package_close(struct netd_kobox2_package *package);

#endif
