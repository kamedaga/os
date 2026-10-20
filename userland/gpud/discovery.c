/* SPDX-License-Identifier: MIT */
#include "discovery.h"
#include <pacha/capsule.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static void add(struct gpud_discovery *d, enum gpud_discovery_kind kind,
    const char *path, const char *value, uint64_t dev)
{
    struct gpud_discovery_entry *e = &d->entries[d->count++];
    e->kind = kind;
    snprintf(e->path, sizeof(e->path), "%s", path);
    snprintf(e->value, sizeof(e->value), "%s", value ? value : "");
    e->dev = dev;
}

int gpud_discovery_prepare(struct gpud_discovery *d, pacha_fd_t device_fd, int primary)
{
    struct pacha_capsule_info cap = {0};
    uint32_t id, sub, revision, header;
    if (!d) return -EINVAL;
    memset(d, 0, sizeof(*d));
    int err = pacha_capsule_query(device_fd, &cap);
    if (err) return err;
    /* The native PCI resource ID encodes bus/device/function. Reject other
     * resource kinds rather than manufacturing a PCI address for libdrm. */
    if (cap.kind != PACHA_CAPSULE_KIND_DEVICE ||
        (cap.device & UINT64_C(0xffffffff00000000)) != UINT64_C(0x5043490000000000) ||
        (cap.device & UINT64_C(0xff00e0f8))) return -ENODEV;
    if ((err = pacha_capsule_pci_config_read(device_fd, 0, 4, &id)) ||
        (err = pacha_capsule_pci_config_read(device_fd, 8, 1, &revision)) ||
        (err = pacha_capsule_pci_config_read(device_fd, 14, 1, &header)) ||
        (err = pacha_capsule_pci_config_read(device_fd, 44, 4, &sub))) return err;
    if ((id & 0xffffu) == 0xffffu || (header & 0x7f)) return -ENODEV;
    /* Both standard indexes exist even when this publisher provides only
     * character devices; an absent block index aborts libudev enumeration. */
    const char *parents[] = {"/sys", "/sys/dev", "/sys/dev/char", "/sys/dev/block",
        "/sys/bus", "/sys/bus/pci", "/sys/class", "/sys/class/drm", "/dev/dri"};
    d->ancestor_count = sizeof(parents)/sizeof(*parents);
    for (size_t i = 0; i < d->ancestor_count; ++i)
        add(d, GPUD_DISC_DIR, parents[i], NULL, 0);
    const char *base = "/sys/dev/char/226:128";
    char path[160], value[160];
    add(d, GPUD_DISC_DIR, base, NULL, 0);
    snprintf(path, sizeof(path), "%s/device", base);
    add(d, GPUD_DISC_DIR, path, NULL, 0);
    snprintf(path, sizeof(path), "%s/device/drm", base);
    add(d, GPUD_DISC_DIR, path, NULL, 0);
    snprintf(path, sizeof(path), "%s/device/drm/renderD128", base);
    add(d, GPUD_DISC_LINK, path, base, 0);
    snprintf(path, sizeof(path), "%s/device/subsystem", base);
    add(d, GPUD_DISC_LINK, path, "/sys/bus/pci", 0);
    snprintf(path, sizeof(path), "%s/device/uevent", base);
    snprintf(value, sizeof(value), "PCI_SLOT_NAME=0000:%02x:%02x.%u\nPCI_ID=%04X:%04X\n",
        (unsigned)(cap.device >> 16) & 255, (unsigned)(cap.device >> 8) & 31,
        (unsigned)cap.device & 7, id & 65535, id >> 16);
    add(d, GPUD_DISC_TEXT, path, value, 0);
    const char *names[] = {"vendor", "device", "subsystem_vendor", "subsystem_device", "revision"};
    const uint32_t values[] = {id & 65535, id >> 16, sub & 65535, sub >> 16, revision};
    for (size_t i = 0; i < sizeof(names)/sizeof(*names); ++i) {
        snprintf(path, sizeof(path), "%s/device/%s", base, names[i]);
        snprintf(value, sizeof(value), "0x%04x\n", values[i]);
        add(d, GPUD_DISC_TEXT, path, value, 0);
    }
    snprintf(path, sizeof(path), "%s/dev", base);
    add(d, GPUD_DISC_TEXT, path, "226:128\n", 0);
    /* libdrm resolves an already-open DRM fd through the node's DEVNAME,
     * not the PCI device's uevent. Xorg caches this path for DRI3 client
     * opens, so enumeration alone is insufficient even when glamor works. */
    snprintf(path, sizeof(path), "%s/uevent", base);
    add(d, GPUD_DISC_TEXT, path, "MAJOR=226\nMINOR=128\nDEVNAME=dri/renderD128\n", 0);
    add(d, GPUD_DISC_LINK, "/sys/class/drm/renderD128", base, 0);
    /* Publish the directory entry last. LPR already dispatches this node's
     * operations to gpud; filed supplies enumeration for unmodified libdrm. */
    add(d, GPUD_DISC_NODE, "/dev/dri/renderD128", NULL, (226u << 8) | 128u);
    if (primary) {
        add(d, GPUD_DISC_DIR, "/sys/dev/char/226:0", NULL, 0);
        add(d, GPUD_DISC_LINK, "/sys/dev/char/226:0/device", "/sys/dev/char/226:128/device", 0);
        add(d, GPUD_DISC_TEXT, "/sys/dev/char/226:0/dev", "226:0\n", 0);
        add(d, GPUD_DISC_TEXT, "/sys/dev/char/226:0/uevent",
            "MAJOR=226\nMINOR=0\nDEVNAME=dri/card0\n", 0);
        add(d, GPUD_DISC_LINK, "/sys/class/drm/card0", "/sys/dev/char/226:0", 0);
        add(d, GPUD_DISC_LINK, "/sys/dev/char/226:128/device/drm/card0", "/sys/dev/char/226:0", 0);
        add(d, GPUD_DISC_NODE, "/dev/dri/card0", NULL, 226u << 8);
    }
    return 0;
}
