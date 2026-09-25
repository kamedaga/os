#ifndef SEED0BOOT_BOOTSTRAP_ABI_H
#define SEED0BOOT_BOOTSTRAP_ABI_H

#include <stdint.h>

enum {
    SEED0_PROCESS_STANDARD_CONFIG_TARGET_VA = 0x0000700000002000ull,
    SEED0_INIT_BOOTSTRAP_MAGIC = 0x49425453ull,
    SEED0_INIT_BOOTSTRAP_VERSION = 20,
    SEED0_INIT_CONFIG_MAGIC = 0x49425443ull,
    SEED0_INIT_CONFIG_VERSION = 1,
    SEED0_INIT_BOOT_ARCHIVE_FLAG_PRESENT = 1ull,
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
    uint64_t boot_image_count;
    uint64_t reserved_counts[3];
    struct seed0_boot_archive_descriptor bootfs_archive;
    struct seed0_display_descriptor primary_display;
    struct seed0_spawn_page_descriptor spawn_pages[8];
};

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
