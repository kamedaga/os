#pragma once

#include <stdint.h>

enum {
    PACHA_ROOT_READY_MAGIC = 0x3159445252545330ull,
    PACHA_ROOT_HANDOFF_MAGIC = 0x32464f444e414852ull,
    PACHA_ROOT_HANDOFF_VERSION = 2,
    /* One VMO plus these Device FDs fits the IPC transfer limit. This is a
     * batch size, not a total device count. */
    PACHA_ROOT_HANDOFF_BATCH_DEVICES = 18,
    PACHA_ROOT_HANDOFF_FLAG_LAST = 1,
};

struct pacha_root_device_record {
    uint64_t transfer_index;
    uint64_t resource_id;
    uint64_t vendor_id;
    uint64_t device_id;
    uint64_t subsystem_id;
    uint64_t class_code;
    uint32_t pci_segment;
    uint32_t pci_bus;
    uint32_t pci_device;
    uint32_t pci_function;
};

struct pacha_root_handoff {
    uint64_t magic;
    uint64_t version;
    uint64_t device_count;
    uint64_t flags;
    struct pacha_root_device_record devices[PACHA_ROOT_HANDOFF_BATCH_DEVICES];
};

_Static_assert(sizeof(struct pacha_root_device_record) == 64,
    "root handoff device record ABI");
_Static_assert(sizeof(struct pacha_root_handoff) == 1184,
    "root handoff metadata ABI");
