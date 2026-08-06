#pragma once

#include <stdint.h>

enum {
    PACHA_ROOT_READY_MAGIC = 0x3159445252545330ull,
    PACHA_ROOT_HANDOFF_MAGIC = 0x32464f444e414852ull,
    PACHA_ROOT_HANDOFF_VERSION = 1,
    PACHA_ROOT_HANDOFF_MAX_DEVICES = 8,
};

struct pacha_root_device_record {
    uint64_t transfer_index;
    uint64_t resource_id;
    uint64_t vendor_id;
    uint64_t device_id;
    uint64_t subsystem_id;
    uint32_t pci_segment;
    uint32_t pci_bus;
    uint32_t pci_device;
    uint32_t pci_function;
};

struct pacha_root_handoff {
    uint64_t magic;
    uint64_t version;
    uint64_t device_count;
    uint64_t reserved0;
    struct pacha_root_device_record devices[PACHA_ROOT_HANDOFF_MAX_DEVICES];
};

_Static_assert(sizeof(struct pacha_root_device_record) == 56,
    "root handoff device record ABI");
_Static_assert(sizeof(struct pacha_root_handoff) == 480,
    "root handoff metadata ABI");
