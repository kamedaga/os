/* SPDX-License-Identifier: MIT */
/* Call/ownership oracle, not evidence of native MMIO or hardware execution. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../userland/kobox2_adapter/device_pci.c"

#define BASE UINT64_C(0x200000000)
#define BAR_BASE UINT64_C(0x120000000)
#define DEVICE_FD 224

static struct pacha_capsule_bar_info native_bars[6];
static uint32_t configuration[1024];
static struct pacha_capsule_info device;
static unsigned int calls, derives, closes;
static long derive_result = 100, close_result, query_result = 11;
static uint64_t last_bar, last_offset, last_flags;
static uintptr_t last_target;
static size_t last_length;

long pacha_syscall3(uint64_t number, uint64_t fd, uint64_t output, uint64_t words) {
    ++calls;
    assert(number == PACHA_CAPSULE_SYSCALL_QUERY && fd == DEVICE_FD && words == 11);
    *(struct pacha_capsule_info *)(uintptr_t)output = device;
    return query_result;
}

long pacha_syscall4(uint64_t number, uint64_t fd, uint64_t offset,
    uint64_t output, uint64_t width) {
    ++calls;
    assert(fd == DEVICE_FD);
    if (number == PACHA_CAPSULE_SYSCALL_PCI_BAR_INFO) {
        assert(offset < 6 && width == 4);
        if (!native_bars[offset].size) return PACHA_SYSCALL_ERR_INVALID;
        *(struct pacha_capsule_bar_info *)(uintptr_t)output = native_bars[offset];
        return 4;
    }
    assert(config_range(offset, width));
    unsigned char *bytes = (unsigned char *)configuration;
    if (number == PACHA_CAPSULE_SYSCALL_PCI_CONFIG_READ)
        memcpy((void *)(uintptr_t)output, bytes + offset, width);
    else {
        assert(number == PACHA_CAPSULE_SYSCALL_PCI_CONFIG_WRITE);
        memcpy(bytes + offset, (void *)(uintptr_t)output, width);
    }
    return 0;
}

long pacha_syscall6(uint64_t number, uint64_t fd, uint64_t bar, uint64_t target,
    uint64_t length, uint64_t flags, uint64_t offset) {
    ++calls;
    ++derives;
    assert(number == PACHA_CAPSULE_SYSCALL_DERIVE_MMIO && fd == DEVICE_FD);
    last_bar = bar;
    last_offset = offset;
    last_flags = flags;
    last_target = target;
    last_length = length;
    return derive_result;
}

long pacha_syscall1(uint64_t number, uint64_t fd) {
    ++calls;
    ++closes;
    assert(number == PACHA_FD_SYSCALL_CLOSE && fd == 100);
    return close_result;
}

static void defaults(void) {
    memset(configuration, 0, sizeof(configuration));
    memset(native_bars, 0, sizeof(native_bars));
    configuration[0] = 0x10501af4;
    configuration[4] = (uint32_t)BAR_BASE | 4;
    configuration[5] = BAR_BASE >> 32;
    configuration[6] = 0x30000100;
    native_bars[0] = (struct pacha_capsule_bar_info){
        .start = BAR_BASE, .end = BAR_BASE + 0x3fff, .size = 0x4000,
        .flags = PACHA_CAPSULE_BAR_MEM | PACHA_CAPSULE_BAR_64BIT,
    };
    native_bars[2] = (struct pacha_capsule_bar_info){
        .start = 0x30000100, .end = 0x300001ff, .size = 0x100,
        .flags = PACHA_CAPSULE_BAR_MEM,
    };
    device = (struct pacha_capsule_info){
        .fd = DEVICE_FD, .kind = PACHA_CAPSULE_KIND_DEVICE, .device = 42,
        .rights = PACHA_FD_RIGHT_QUERY | PACHA_FD_RIGHT_CONFIG_READ |
            PACHA_FD_RIGHT_CONFIG_WRITE | PACHA_FD_RIGHT_DERIVE_MMIO |
            PACHA_FD_RIGHT_MMIO_MAP_READ | PACHA_FD_RIGHT_MMIO_MAP_WRITE,
    };
}

int main(void) {
    struct ph_pci pci = {0};
    struct ph_pci_mapping mappings[2];
    struct ph_pci_config config = {
        .device_fd = DEVICE_FD, .generation = 7, .bus = 2, .devfn = 24,
        .reservation = (void *)BASE, .reservation_size = 0x10000,
        .mappings = mappings, .mapping_capacity = 2,
    };
    defaults();
    assert(!ph_pci_init(&pci, &config));
    const struct kobox_linux_pci_host *host = &pci.host;
    assert(host->context == &pci && host->bus == 2 && host->devfn == 24);
    assert(host->window_count == 2 && host->windows[0].start == BAR_BASE);
    assert(host->windows[1].length == 0x100 && pci.native_device == 42);
    assert(!host->memory_read && !host->memory_write);
    uint32_t value;
    assert(!host->config_read(&pci, 0, 4, &value) && value == 0x10501af4);
    assert(!host->config_read(&pci, 1, 1, &value) && value == 0x1a);
    assert(!host->config_write(&pci, 4, 2, 0x1234));
    assert(!host->config_read(&pci, 4, 2, &value) && value == 0x1234);

    unsigned int before = calls;
    assert(host->config_read(&pci, 4096, 1, &value) == -EINVAL);
    assert(host->config_read(&pci, UINT32_MAX, 4, &value) == -EINVAL);
    assert(host->config_read(&pci, 1, 2, &value) == -EINVAL);
    assert(host->config_read(&pci, 0, 3, &value) == -EINVAL);
    assert(host->config_read(&pci, 0, 4, NULL) == -EINVAL);
    assert(host->config_write(&pci, 4095, 4, 0) == -EINVAL);
    assert(calls == before);

    for (unsigned int cache = KOBOX_MMIO_UC; cache <= KOBOX_MMIO_WP; ++cache) {
        assert(!host->memory_map(&pci, (void *)BASE, BAR_BASE + 4096, 4096,
            KOBOX_LINUX_MEMORY_READ, cache));
        assert(last_flags == (PACHA_CAPSULE_MMIO_REPLACE_EXISTING |
            PACHA_CAPSULE_MMIO_READ_ONLY | ((uint64_t)cache << 2)));
        assert(last_bar == 0 && last_offset == 4096);
        assert(ph_pci_destroy(&pci) == -EBUSY);
        assert(!host->memory_unmap(&pci, (void *)BASE, 4096));
    }
    assert(derives == 6 && closes == 6);

    before = calls;
    assert(host->memory_map(&pci, (void *)BASE, BAR_BASE, 4096, 2, KOBOX_MMIO_UC) == -EINVAL);
    assert(host->memory_map(&pci, (void *)BASE, BAR_BASE, 4096, 5, KOBOX_MMIO_UC) == -EINVAL);
    assert(host->memory_map(&pci, (void *)BASE, BAR_BASE, 4096, 1, -1) == -EINVAL);
    assert(host->memory_map(&pci, (void *)BASE, BAR_BASE, 4096, 1, 6) == -EINVAL);
    assert(host->memory_map(&pci, (void *)BASE, BAR_BASE + 1, 4096, 1, 0) == -EINVAL);
    assert(host->memory_map(&pci, (void *)(BASE + 1), BAR_BASE, 4096, 1, 0) == -EINVAL);
    assert(host->memory_map(&pci, (void *)BASE, BAR_BASE, 0, 1, 0) == -EINVAL);
    assert(host->memory_map(&pci, (void *)BASE, BAR_BASE + 0x3000, 8192, 1, 0) == -ERANGE);
    assert(host->memory_map(&pci, (void *)BASE, UINT64_MAX - 4095, 4096, 1, 0) == -EINVAL);
    assert(host->memory_map(&pci, (void *)(UINTPTR_MAX - 4095), BAR_BASE, 4096, 1, 0) == -EINVAL);
    assert(host->memory_map(&pci, (void *)(BASE - 4096), BAR_BASE, 4096, 1, 0) == -ERANGE);
    assert(host->memory_map(&pci, (void *)(BASE + 0xf000), BAR_BASE, 8192, 1, 0) == -ERANGE);
    assert(calls == before);

    assert(!host->memory_map(&pci, (void *)BASE, 0x30000000, 4096, 3, 0));
    assert(last_bar == 2 && last_offset == 0);
    assert(last_flags == PACHA_CAPSULE_MMIO_REPLACE_EXISTING);
    before = calls;
    assert(host->memory_map(&pci, (void *)BASE, BAR_BASE, 4096, 1, 0) == -EBUSY);
    assert(host->memory_unmap(&pci, (void *)BASE, 8192) == -ENOENT);
    assert(calls == before);
    close_result = PACHA_SYSCALL_ERR_NOT_READY;
    assert(host->memory_unmap(&pci, (void *)BASE, 4096) == -EBUSY);
    assert(mappings[0].length == 4096);
    close_result = 0;
    assert(!host->memory_unmap(&pci, (void *)BASE, 4096));

    const long failures[] = {0, 1, 2, 3, 4, 5, 6, PACHA_FD_TABLE_LIMIT};
    for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); ++i) {
        derive_result = failures[i];
        assert(host->memory_map(&pci, (void *)BASE, BAR_BASE, 4096, 1, 0) < 0);
        assert(!mappings[0].length);
    }
    derive_result = 100;
    assert(!host->memory_map(&pci, (void *)BASE, BAR_BASE, 4096, 1, 0));
    assert(ph_pci_revoke(&pci, 6) == -ESTALE && pci.admitted);
    assert(!ph_pci_revoke(&pci, 7));
    assert(ph_pci_destroy(&pci) == -EBUSY);
    before = calls;
    assert(host->config_read(&pci, 0, 4, &value) == -ENODEV);
    assert(host->config_write(&pci, 4, 2, 0) == -ENODEV);
    assert(host->memory_map(&pci, (void *)(BASE + 4096), BAR_BASE, 4096, 1, 0) == -ENODEV);
    assert(calls == before);
    assert(!host->memory_unmap(&pci, (void *)BASE, 4096));
    assert(!ph_pci_destroy(&pci) && !pci.config.generation);

    defaults();
    device.rights &= ~PACHA_FD_RIGHT_QUERY;
    assert(ph_pci_init(&pci, &config) == -EACCES);
    defaults();
    device.flags = PACHA_CAPSULE_DMA_QUARANTINED;
    assert(ph_pci_init(&pci, &config) == -ENODEV);
    defaults();
    native_bars[2].end++;
    assert(ph_pci_init(&pci, &config) == -EPROTO);
    defaults();
    configuration[4] |= 1;
    assert(ph_pci_init(&pci, &config) == -EOPNOTSUPP);
    defaults();
    query_result = PACHA_SYSCALL_ERR_CLOSED;
    assert(ph_pci_init(&pci, &config) == -ENODEV);
    assert(!pci.config.generation);
    puts("KOBOX2_PCI_UNIT=PASS (native-call oracle only)");
    return 0;
}
