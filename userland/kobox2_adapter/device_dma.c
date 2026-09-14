/* SPDX-License-Identifier: MIT */
#include "device_dma.h"
#include "host.h"
#include <pacha/capsule.h>
#include <errno.h>

static int dma_native_status(long result) {
    switch (result) {
    case 0: return 0;
    case PACHA_SYSCALL_ERR_INVALID: return -EINVAL;
    case PACHA_SYSCALL_ERR_NOT_READY: return -EBUSY;
    case PACHA_SYSCALL_ERR_ALLOC: return -ENOMEM;
    case PACHA_SYSCALL_ERR_MAP: return -EIO;
    case PACHA_SYSCALL_ERR_CLOSED: return -ENODEV;
    default: return -EPROTO;
    }
}

static int dma_query(const struct ph_dma_config *config, struct pacha_capsule_info *info) {
    long words = pacha_syscall3(PACHA_CAPSULE_SYSCALL_QUERY, config->device_fd,
        (uintptr_t)info, 11);
    if (words != 11) return words ? dma_native_status(words) : -EPROTO;
    if (info->kind != PACHA_CAPSULE_KIND_DEVICE || info->device != config->native_device)
        return -ENODEV;
    if ((info->flags & PACHA_CAPSULE_DMA_QUARANTINED) ||
        !(info->flags & PACHA_CAPSULE_DMA_TRANSLATED)) return -EIO;
    return 0;
}

static int dma_enable(void *context, unsigned int enabled) {
    struct ph_dma *dma = context;

    if (enabled > 1) return -EINVAL;
    if (enabled && !dma->admitted) return -ENODEV;
    /* Even an apparently redundant disable reaches the kernel: this is a
     * device-domain barrier, not just a local admission flag. */
    dma->drained = 0;
    int result = dma_native_status(pacha_syscall2(PACHA_CAPSULE_SYSCALL_DMA_SET_ENABLED,
        dma->config.device_fd, enabled));
    if (!result) {
        dma->enabled = enabled;
        dma->drained = !enabled;
    }
    return result;
}

static int dma_map(void *context, uint64_t iova, uint64_t ram_offset,
    size_t length, unsigned int protection) {
    struct ph_dma *dma = context;
    const struct ph_dma_config *config = &dma->config;
    struct ph_dma_mapping *slot = NULL;
    unsigned int direction;

    if (!dma->admitted) return -ENODEV;
    if (!length || (iova | ram_offset | length) % PH_PAGE_SIZE ||
        !protection || protection & ~(KOBOX_DMA_DEVICE_READ | KOBOX_DMA_DEVICE_WRITE))
        return -EINVAL;
    if (iova < config->aperture_start || iova > config->aperture_end ||
        length - 1 > config->aperture_end - iova ||
        ram_offset >= config->ram_length || length > config->ram_length - ram_offset)
        return -ERANGE;
    for (size_t i = 0; i < config->mapping_capacity; ++i) {
        struct ph_dma_mapping *mapping = &config->mappings[i];
        if (!mapping->length) {
            if (!slot) slot = mapping;
        } else if (iova <= mapping->iova + mapping->length - 1 &&
            mapping->iova <= iova + length - 1) {
            return -EBUSY;
        }
    }
    if (!slot)
        return -ENOSPC;
    switch (protection) {
    case KOBOX_DMA_DEVICE_READ: direction = PACHA_CAPSULE_DMA_TO_DEVICE; break;
    case KOBOX_DMA_DEVICE_WRITE: direction = PACHA_CAPSULE_DMA_FROM_DEVICE; break;
    default: direction = PACHA_CAPSULE_DMA_BIDIRECTIONAL; break;
    }
    long fd = pacha_syscall6(PACHA_CAPSULE_SYSCALL_DERIVE_DMA_MAPPING, config->device_fd,
        (uintptr_t)config->ram + ram_offset, iova, length, direction, 0);
    if (fd < 16 || fd >= PACHA_FD_TABLE_LIMIT) {
        struct pacha_capsule_info info = {0};
        /* Native rollback can quarantine on failed IOTLB drain. Returning a
         * recoverable map error then would let Linux reuse still-exposed RAM. */
        if (dma_query(config, &info)) ph_fail(__FILE__, __LINE__, (uint64_t)fd);
        return fd ? dma_native_status(fd) : -EPROTO;
    }
    *slot = (struct ph_dma_mapping){.iova = iova, .length = length, .fd = (int)fd};
    return 0;
}

static int dma_unmap(void *context, uint64_t iova, size_t length) {
    struct ph_dma *dma = context;

    for (size_t i = 0; i < dma->config.mapping_capacity; ++i) {
        struct ph_dma_mapping *mapping = &dma->config.mappings[i];
        if (!length || mapping->iova != iova || mapping->length != length) continue;
        int result = dma_native_status(pacha_syscall1(PACHA_FD_SYSCALL_CLOSE, mapping->fd));
        if (result) return result; /* Retain FD, IOVA and RAM ownership on failure. */
        *mapping = (struct ph_dma_mapping){0};
        return 0;
    }
    return -ENOENT;
}

int ph_dma_init(struct ph_dma *dma, const struct ph_dma_config *config) {
    const uint64_t required = PACHA_FD_RIGHT_QUERY | PACHA_FD_RIGHT_DERIVE_DMA |
        PACHA_FD_RIGHT_DMA_READ | PACHA_FD_RIGHT_DMA_WRITE | PACHA_FD_RIGHT_BUS_MASTER;
    struct pacha_capsule_info info = {0};
    struct pacha_fd_table_info fd_table = {0};

    if (!dma || !config || dma->config.generation || config->device_fd < 0 ||
        config->device_fd >= PACHA_FD_TABLE_LIMIT || !config->native_device ||
        !config->generation || !config->ram || !config->ram_length ||
        ((uintptr_t)config->ram | config->ram_length | config->aperture_start) % PH_PAGE_SIZE ||
        (uintptr_t)config->ram > UINTPTR_MAX - config->ram_length ||
        config->aperture_start > config->aperture_end ||
        config->aperture_end % PH_PAGE_SIZE != PH_PAGE_SIZE - 1 ||
        !config->mappings || !config->mapping_capacity ||
        config->mapping_capacity > SIZE_MAX / sizeof(*config->mappings)) return -EINVAL;
    int result = dma_query(config, &info);
    if (result) return result;
    if ((info.rights & required) != required) return -EACCES;
    result = dma_native_status(pacha_syscall2(PACHA_FD_SYSCALL_TABLE,
        PACHA_FD_TABLE_LIMIT, (uintptr_t)&fd_table));
    if (result) return result;
    if (fd_table.capacity != PACHA_FD_TABLE_LIMIT ||
        fd_table.free_slots < config->mapping_capacity) return -ENOSPC;
    result = dma_native_status(pacha_syscall2(PACHA_CAPSULE_SYSCALL_DMA_SET_ENABLED,
        config->device_fd, 0));
    if (result) return result;
    memset(config->mappings, 0, config->mapping_capacity * sizeof(*config->mappings));
    *dma = (struct ph_dma){
        .config = *config, .admitted = 1, .drained = 1,
        .host = {
            .size = sizeof(dma->host), .context = dma,
            .aperture_start = config->aperture_start, .aperture_end = config->aperture_end,
            .coherent = 1, /* This native machine port is x86-64 coherent DMA. */
            .enable = dma_enable, .map = dma_map, .unmap = dma_unmap,
        },
    };
    return 0;
}

int ph_dma_revoke(struct ph_dma *dma, uint64_t generation) {
    if (!dma || !dma->config.generation) return -EINVAL;
    if (generation != dma->config.generation) return -ESTALE;
    dma->admitted = 0;
    return dma_enable(dma, 0);
}

int ph_dma_destroy(struct ph_dma *dma) {
    if (!dma || !dma->config.generation) return -EINVAL;
    if (dma->admitted || dma->enabled || !dma->drained) return -EBUSY;
    for (size_t i = 0; i < dma->config.mapping_capacity; ++i)
        if (dma->config.mappings[i].length) return -EBUSY;
    *dma = (struct ph_dma){0};
    return 0;
}
