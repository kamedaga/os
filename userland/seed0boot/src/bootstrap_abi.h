#ifndef SEED0BOOT_BOOTSTRAP_ABI_H
#define SEED0BOOT_BOOTSTRAP_ABI_H

#include <stdint.h>

enum {
    SEED0_PROCESS_STANDARD_CONFIG_TARGET_VA = 0x3C002000ull,
    SEED0_INIT_BOOTSTRAP_MAGIC = 0x49425453ull,
    SEED0_INIT_BOOTSTRAP_VERSION = 19,
    SEED0_INIT_CONFIG_MAGIC = 0x49425443ull,
    SEED0_INIT_CONFIG_VERSION = 1,
    SEED0_INIT_DEVICE_FLAG_PRESENT = 1ull,
    SEED0_INIT_MAX_DEVICE_DESCRIPTORS = 8,
    SEED0_INIT_MAX_DEVICE_QUEUE_GRANTS = 4,
};

struct seed0_device_queue_grant {
    uint64_t queue_index;
    uint64_t submit_token;
    uint64_t notify_token;
};

struct seed0_device_descriptor {
    uint64_t transport;
    uint64_t flags;
    uint64_t bootstrap_source_va;
    uint64_t vendor_id;
    uint64_t device_id;
    uint64_t subsystem_id;
    uint64_t pci_bus;
    uint64_t pci_device;
    uint64_t pci_function;
    uint64_t resource_id;
    uint64_t queue_count;
    uint64_t common_page_paddr;
    uint64_t notify_page_paddr;
    uint64_t isr_page_paddr;
    uint64_t device_page_paddr;
    uint64_t common_page_offset;
    uint64_t notify_page_offset;
    uint64_t isr_page_offset;
    uint64_t device_page_offset;
    uint64_t notify_off_multiplier;
    uint64_t init_iommu_token;
    uint64_t init_queue_grant_count;
    struct seed0_device_queue_grant init_queue_grants[SEED0_INIT_MAX_DEVICE_QUEUE_GRANTS];
    uint64_t init_command_token;
    uint64_t init_device_fd;
};

struct seed0_boot_archive_descriptor {
    uint64_t flags;
    uint64_t image_va;
    uint64_t size_bytes;
    uint64_t page_count;
};

struct seed0_display_descriptor {
    uint64_t flags;
    uint64_t width;
    uint64_t height;
    uint64_t pitch;
    uint64_t framebuffer_paddr;
    uint64_t framebuffer_size_bytes;
};

struct seed0_spawn_page_descriptor {
    uint64_t kind;
    uint64_t subject;
    uint64_t flags;
    uint64_t source_va;
    uint64_t target_va;
    uint64_t spawn_flags;
};

struct seed0_init_config_page {
    uint64_t magic;
    uint64_t version;
    uint64_t descriptor_page_va;
    uint64_t reserved0;
};

struct seed0_init_descriptor_page {
    uint64_t magic;
    uint64_t version;
    uint64_t spawn_page_count;
    uint64_t device_count;
    uint64_t boot_image_count;
    uint64_t reserved_counts[3];
    struct seed0_boot_archive_descriptor bootfs_archive;
    struct seed0_display_descriptor primary_display;
    struct seed0_spawn_page_descriptor spawn_pages[8];
    struct seed0_device_descriptor devices[SEED0_INIT_MAX_DEVICE_DESCRIPTORS];
};

static inline int seed0_fd_is_dynamic(uint64_t value)
{
    return value >= 16 && value < 256;
}

static inline const struct seed0_init_descriptor_page *seed0_bootstrap_descriptor(void)
{
    const struct seed0_init_config_page *cfg =
        (const struct seed0_init_config_page *)SEED0_PROCESS_STANDARD_CONFIG_TARGET_VA;
    if (cfg->magic != SEED0_INIT_CONFIG_MAGIC ||
        cfg->version != SEED0_INIT_CONFIG_VERSION ||
        cfg->descriptor_page_va == 0) {
        return 0;
    }

    const struct seed0_init_descriptor_page *desc =
        (const struct seed0_init_descriptor_page *)(uintptr_t)cfg->descriptor_page_va;
    if (desc->magic != SEED0_INIT_BOOTSTRAP_MAGIC ||
        desc->version != SEED0_INIT_BOOTSTRAP_VERSION) {
        return 0;
    }
    return desc;
}

#endif
