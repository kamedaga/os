/* SPDX-License-Identifier: MIT */
#ifndef PACHA_USBD_PACKAGE_H
#define PACHA_USBD_PACKAGE_H

#include "../kobox2_adapter/bootstrap.h"
#include "../kobox2_adapter/device_authority.h"

#include <kobox2/controller.h>
#include <kobox2/closure_manifest.h>

enum { USBD_PACKAGE_ARTIFACTS = 10, USBD_PACKAGE_ITEMS = 13 };

struct usbd_package {
    const unsigned char *sandbox;
    size_t sandbox_size;
    void *sandbox_storage;
    unsigned char manifest[8192], grant[4096];
    size_t manifest_size, grant_size;
    kb2_closure_manifest_artifact_t artifacts[USBD_PACKAGE_ARTIFACTS];
    struct ph_bootstrap_item items[USBD_PACKAGE_ITEMS];
    struct ph_package_identity identity;
    struct ph_device_authority device;
    int device_fd;
};

/* The fixed closure is checked against its immutable manifest before any
 * upstream module is initialized. The capsule remains owned by usbd. */
int usbd_package_open(struct usbd_package *package, int filed_fd, int device_fd);
int usbd_package_bind(struct usbd_package *package, uint64_t generation);
int usbd_package_unbind(struct usbd_package *package, uint64_t generation);
int usbd_package_configure_controller(
    const struct usbd_package *package, kb2_controller_t *controller);
int usbd_package_close(struct usbd_package *package);

#endif
