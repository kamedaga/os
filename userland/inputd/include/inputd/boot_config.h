#ifndef INPUTD_BOOT_CONFIG_H
#define INPUTD_BOOT_CONFIG_H

#include <stdint.h>

#define INPUTD_BOOT_CONFIG_MAGIC UINT64_C(0x494e5054424f4f55)
#define INPUTD_BOOT_READY_MAGIC UINT64_C(0x3159445254504e49)

enum {
    INPUTD_BOOT_CONFIG_VERSION = 2,
    INPUTD_BOOT_CONFIG_MAX_BYTES = 4096,
};

/* A boot record identifies a capsule fd without assigning it an input role. */
struct inputd_device_config {
    uint64_t device_fd;
    uint64_t resource_id;
    uint32_t pci_segment;
    uint32_t pci_bus;
    uint32_t pci_device;
    uint32_t pci_function;
    uint32_t vendor_id;
    uint32_t device_id;
    uint32_t subsystem_id;
    uint32_t reserved0;
};

/*
 * devices are a dense variable-length table at devices_offset.
 * Record sizes are explicit so malformed or mismatched producers fail closed.
 */
struct inputd_boot_config {
    uint64_t magic;
    uint32_t version;
    uint32_t header_size;
    uint64_t total_size;
    uint64_t input_endpoint_fd;
    uint64_t ready_channel_fd;
    uint64_t netd_endpoint_fd;
    uint32_t device_count;
    uint32_t device_record_size;
    uint64_t devices_offset;
    uint64_t reserved[6];
};

_Static_assert(sizeof(struct inputd_device_config) == 48,
    "inputd boot device record ABI");
_Static_assert(sizeof(struct inputd_boot_config) == 112,
    "inputd boot header ABI");

static inline const struct inputd_device_config *inputd_boot_devices(
    const struct inputd_boot_config *config)
{
    return (const struct inputd_device_config *)((const uint8_t *)config +
        config->devices_offset);
}

#endif
