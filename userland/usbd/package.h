#include <pacha/abi.h>
/* SPDX-License-Identifier: MIT */
#ifndef PACHA_USBD_PACKAGE_H
#define PACHA_USBD_PACKAGE_H

#include "../kobox2_adapter/bootstrap.h"
#include "../kobox2_adapter/device_authority.h"
#include "../kobox2_adapter/platform_authority.h"

#include <kobox2/controller.h>
#include <kobox2/closure_manifest.h>
#include <stdint.h>

enum { USBD_PACKAGE_MAX_ARTIFACTS = 64, USBD_PACKAGE_ITEMS = 67 };

struct usbd_package {
    const unsigned char *sandbox;
    size_t sandbox_size;
    void *sandbox_storage;
    unsigned char manifest[8192], grant[4096];
    size_t manifest_size, grant_size;
    kb2_closure_manifest_t decoded;
    size_t artifact_count;
    struct ph_bootstrap_item items[USBD_PACKAGE_ITEMS];
    struct ph_package_identity identity;
    struct ph_device_authority device;
    struct ph_platform_authority platform;
    uint32_t kind;
    pacha_fd_t device_fd;
};

/* The fixed closure is checked against its immutable manifest before any
 * upstream module is initialized. The capsule remains owned by usbd. */
int usbd_package_open(struct usbd_package *package, pacha_fd_t filed_fd, pacha_fd_t device_fd,
    uint32_t kind);
/* Trusted owners may reuse the verified PCI package loader for another role. */
int usbd_package_open_root(struct usbd_package *package, pacha_fd_t filed_fd,
    pacha_fd_t device_fd, const char *root);
int usbd_package_bind(struct usbd_package *package, uint64_t generation);
int usbd_package_unbind(struct usbd_package *package, uint64_t generation);
int usbd_package_configure_controller(
    const struct usbd_package *package, kb2_controller_t *controller);
int usbd_package_close(struct usbd_package *package);

#endif
