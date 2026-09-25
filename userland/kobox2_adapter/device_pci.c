/* SPDX-License-Identifier: MIT */
#include "device_pci.h"
#include <pacha/syscall.h>
#include <errno.h>
#include <string.h>

#define PH_PCI_PAGE_SIZE 4096ul

static int native_status(long result) {
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

static int config_range(uint32_t offset, uint32_t width) {
    return (width == 1 || width == 2 || width == 4) &&
        offset < 4096 && width <= 4096 - offset && offset % width == 0;
}

static int config_read_raw(struct ph_pci *pci, uint32_t offset, uint32_t width,
    uint32_t *out) {
    uint32_t value = 0;

    if (!pci->admitted) return -ENODEV;
    if (!out || !config_range(offset, width)) return -EINVAL;
    int result = native_status(pacha_syscall4(PACHA_CAPSULE_SYSCALL_PCI_CONFIG_READ,
        pci->config.device_fd, offset, (uintptr_t)&value, width));
    if (!result) *out = value;
    return result;
}

static int hidden_io_bar(const struct ph_pci *pci, uint32_t offset) {
    return offset >= 0x10 && offset < 0x28 &&
        (pci->hidden_io_bars & (1u << ((offset - 0x10) / 4)));
}

static int config_read(void *context, uint32_t offset, uint32_t width, uint32_t *out) {
    struct ph_pci *pci = context;
    int result = config_read_raw(pci, offset, width, out);
    if (result) return result;
    /* The host exposes MMIO, not port I/O. Hide an optional I/O BAR from
     * Linux's resource scan, including its all-ones sizing probe; otherwise
     * pci_enable_device() rejects the unclaimable I/O resource. */
    if (hidden_io_bar(pci, offset)) *out = 0;
    if (offset == 0x04 && pci->hidden_io_bars) *out &= ~1u;
    return 0;
}

static int config_write(void *context, uint32_t offset, uint32_t width, uint32_t value) {
    struct ph_pci *pci = context;

    if (!pci->admitted) return -ENODEV;
    if (!config_range(offset, width)) return -EINVAL;
    /* Never permit the guest to re-enable decoding for a BAR it cannot use.
     * Ignore writes to that BAR without changing its physical assignment. */
    if (hidden_io_bar(pci, offset)) return 0;
    if (offset == 0x04 && pci->hidden_io_bars) value &= ~1u;
    return native_status(pacha_syscall4(PACHA_CAPSULE_SYSCALL_PCI_CONFIG_WRITE,
        pci->config.device_fd, offset, (uintptr_t)&value, width));
}

static int memory_map(void *context, void *address, uint64_t physical,
    size_t length, unsigned int protection, enum kobox_mmio_cache cache) {
    static const uint64_t cache_flags[] = {
        [KOBOX_MMIO_UC] = PACHA_CAPSULE_MMIO_CACHE_UC,
        [KOBOX_MMIO_UC_MINUS] = PACHA_CAPSULE_MMIO_CACHE_UC_MINUS,
        [KOBOX_MMIO_WC] = PACHA_CAPSULE_MMIO_CACHE_WC,
        [KOBOX_MMIO_WB] = PACHA_CAPSULE_MMIO_CACHE_WB,
        [KOBOX_MMIO_WT] = PACHA_CAPSULE_MMIO_CACHE_WT,
        [KOBOX_MMIO_WP] = PACHA_CAPSULE_MMIO_CACHE_WP,
    };
    struct ph_pci *pci = context;
    const struct ph_pci_config *config = &pci->config;
    const uintptr_t target = (uintptr_t)address;
    const uintptr_t base = (uintptr_t)config->reservation;
    struct ph_pci_mapping *slot = NULL;

    if (!pci->admitted) return -ENODEV;
    /* x86 MMIO cannot implement write-only access. Do not silently add READ. */
    if (!target || !length || (target | physical | length) % PH_PCI_PAGE_SIZE ||
        target > UINTPTR_MAX - length || physical > UINT64_MAX - length ||
        !(protection & KOBOX_LINUX_MEMORY_READ) ||
        protection & ~(KOBOX_LINUX_MEMORY_READ | KOBOX_LINUX_MEMORY_WRITE) ||
        (unsigned int)cache >= sizeof(cache_flags) / sizeof(cache_flags[0])) return -EINVAL;
    if (target < base || target - base >= config->reservation_size ||
        length > config->reservation_size - (target - base)) return -ERANGE;

    for (size_t i = 0; i < config->mapping_capacity; ++i) {
        struct ph_pci_mapping *mapping = &config->mappings[i];
        if (!mapping->length) {
            if (!slot) slot = mapping;
        } else if (target < mapping->address + mapping->length &&
            mapping->address < target + length) {
            return -EBUSY;
        }
    }
    if (!slot) return -ENOSPC;

    for (unsigned int bar = 0; bar < KOBOX_PCI_MEMORY_WINDOWS; ++bar) {
        const struct pacha_capsule_bar_info *info = &pci->bars[bar];
        if (!info->size) continue;
        const uint64_t first_page = info->start & ~(uint64_t)(PH_PCI_PAGE_SIZE - 1);
        const uint64_t end_page = (info->end + PH_PCI_PAGE_SIZE) &
            ~(uint64_t)(PH_PCI_PAGE_SIZE - 1);
        if (physical < first_page || physical >= end_page ||
            length > end_page - physical) continue;

        uint64_t flags = PACHA_CAPSULE_MMIO_REPLACE_EXISTING | cache_flags[cache];
        if (!(protection & KOBOX_LINUX_MEMORY_WRITE)) flags |= PACHA_CAPSULE_MMIO_READ_ONLY;
        long fd = pacha_syscall6(PACHA_CAPSULE_SYSCALL_DERIVE_MMIO,
            config->device_fd, bar, target, length, flags, physical - first_page);
        if (fd < 16 || fd >= PACHA_FD_TABLE_LIMIT) {
            return fd == 0 ? -EPROTO : native_status(fd);
        }
        *slot = (struct ph_pci_mapping){.address = target, .length = length, .fd = (int)fd};
        return 0;
    }
    return -ERANGE;
}

static int memory_unmap(void *context, void *address, size_t length) {
    struct ph_pci *pci = context;

    for (size_t i = 0; i < pci->config.mapping_capacity; ++i) {
        struct ph_pci_mapping *mapping = &pci->config.mappings[i];
        if (!length || mapping->address != (uintptr_t)address || mapping->length != length)
            continue;
        int result = native_status(pacha_syscall1(PACHA_FD_SYSCALL_CLOSE, mapping->fd));
        if (result) return result; /* Keep ownership; Linux must not reuse it. */
        *mapping = (struct ph_pci_mapping){0};
        return 0;
    }
    return -ENOENT;
}

static int discover_bars(struct ph_pci *pci) {
    uint32_t header;
    int result = config_read_raw(pci, 0x0e, 1, &header);
    if (result) return result;
    if (header & 0x7f) return -EOPNOTSUPP; /* Endpoint, not a PCI bridge. */

    for (unsigned int bar = 0; bar < KOBOX_PCI_MEMORY_WINDOWS; ++bar) {
        struct pacha_capsule_bar_info *info = &pci->bars[bar];
        uint32_t raw;
        result = config_read_raw(pci, 0x10 + bar * 4, 4, &raw);
        if (result) return result;
        if (!raw) continue;
        if (raw & 1) {
            pci->hidden_io_bars |= 1u << bar;
            continue;
        }
        if (((raw >> 1) & 3) == 1 || ((raw >> 1) & 3) == 3)
            return -EOPNOTSUPP; /* No obsolete MMIO BAR emulation. */
        long words = pacha_syscall4(PACHA_CAPSULE_SYSCALL_PCI_BAR_INFO,
            pci->config.device_fd, bar, (uintptr_t)info, 4);
        if (words != 4) return words ? native_status(words) : -EPROTO;
        if (!(info->flags & PACHA_CAPSULE_BAR_MEM) || !info->size || !info->start ||
            info->start > UINT64_MAX - info->size ||
            info->end != info->start + info->size - 1 ||
            info->end > UINT64_MAX - PH_PCI_PAGE_SIZE) return -EPROTO;
        for (unsigned int prior = 0; prior < bar; ++prior) {
            const struct pacha_capsule_bar_info *other = &pci->bars[prior];
            if (other->size && other->start <= info->end && info->start <= other->end)
                return -EPROTO;
        }
        pci->host.windows[pci->host.window_count++] =
            (struct kobox_linux_pci_window){.start = info->start, .length = info->size};
        if (raw & 4) {
            if (++bar == KOBOX_PCI_MEMORY_WINDOWS) return -EPROTO;
        }
    }
    if (!pci->host.window_count)
        return pci->hidden_io_bars ? -EOPNOTSUPP : -ENODEV;
    if (pci->hidden_io_bars) {
        uint32_t command;
        result = config_read_raw(pci, 0x04, 2, &command);
        if (result) return result;
        if (command & 1u) {
            command &= ~1u;
            /* Port-I/O decoding may have been left on by firmware. Disable
             * it before publishing the MMIO-only PCI view to Linux. */
            result = native_status(pacha_syscall4(PACHA_CAPSULE_SYSCALL_PCI_CONFIG_WRITE,
                pci->config.device_fd, 0x04, (uintptr_t)&command, 2));
            if (result) return result;
        }
    }
    return 0;
}

int ph_pci_init(struct ph_pci *pci, const struct ph_pci_config *config) {
    struct pacha_capsule_info device = {0};
    const uint64_t rights = PACHA_FD_RIGHT_QUERY | PACHA_FD_RIGHT_CONFIG_READ |
        PACHA_FD_RIGHT_CONFIG_WRITE | PACHA_FD_RIGHT_DERIVE_MMIO |
        PACHA_FD_RIGHT_MMIO_MAP_READ;

    if (!pci || !config || pci->config.generation || !config->generation ||
        config->device_fd < 16 || config->device_fd >= PACHA_FD_TABLE_LIMIT ||
        config->segment > UINT16_MAX || config->bus > UINT8_MAX || config->devfn > UINT8_MAX ||
        !config->reservation || !config->reservation_size ||
        ((uintptr_t)config->reservation | config->reservation_size) % PH_PCI_PAGE_SIZE ||
        (uintptr_t)config->reservation > UINTPTR_MAX - config->reservation_size ||
        !config->mappings || !config->mapping_capacity ||
        config->mapping_capacity > SIZE_MAX / sizeof(*config->mappings))
        return -EINVAL;
    long words = pacha_syscall3(PACHA_CAPSULE_SYSCALL_QUERY,
        config->device_fd, (uintptr_t)&device, 11);
    if (words != 11) return words ? native_status(words) : -EPROTO;
    if (device.kind != PACHA_CAPSULE_KIND_DEVICE || !device.device ||
        (device.rights & rights) != rights) return -EACCES;
    if (device.flags & PACHA_CAPSULE_DMA_QUARANTINED) return -ENODEV;

    struct ph_pci candidate = {
        .config = *config, .native_device = device.device, .admitted = 1,
        .host = {.size = sizeof(candidate.host), .segment = config->segment,
            .bus = config->bus, .devfn = config->devfn,
            .config_read = config_read, .config_write = config_write,
            .memory_map = memory_map, .memory_unmap = memory_unmap},
    };
    int result = discover_bars(&candidate);
    if (result) return result;
    memset(config->mappings, 0, config->mapping_capacity * sizeof(*config->mappings));
    *pci = candidate;
    pci->host.context = pci;
    return 0;
}

int ph_pci_revoke(struct ph_pci *pci, uint64_t generation) {
    if (!pci || !generation || pci->config.generation != generation) return -ESTALE;
    pci->admitted = 0;
    return 0;
}

int ph_pci_destroy(struct ph_pci *pci) {
    if (!pci || !pci->config.generation) return -EINVAL;
    if (pci->admitted) return -EBUSY;
    for (size_t i = 0; i < pci->config.mapping_capacity; ++i)
        if (pci->config.mappings[i].length) return -EBUSY;
    *pci = (struct ph_pci){0};
    return 0;
}
