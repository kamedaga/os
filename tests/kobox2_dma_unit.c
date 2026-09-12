/* SPDX-License-Identifier: MIT */
/* Native-call and ownership oracle; this does not execute an IOMMU. */
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include "../userland/kobox2_adapter/device_dma.c"

#define DEVICE_FD 224
#define RAM UINT64_C(0x200000000)
#define IOVA UINT64_C(0x83000000)

static struct pacha_capsule_info device;
static long query_result = 11, derive_result = 100, close_result, enable_result;
static unsigned int calls, enables, last_enabled;
static uint64_t last_address, last_iova, last_length, last_direction;
static jmp_buf fatal_jump;
static int expect_fatal;

_Noreturn void ph_fail(const char *file, unsigned line, uint64_t result) {
    (void)file; (void)line; (void)result;
    assert(expect_fatal);
    longjmp(fatal_jump, 1);
}

long pacha_syscall3(uint64_t nr, uint64_t fd, uint64_t out, uint64_t words) {
    ++calls;
    assert(nr == PACHA_CAPSULE_SYSCALL_QUERY && fd == DEVICE_FD && words == 11);
    *(struct pacha_capsule_info *)(uintptr_t)out = device;
    return query_result;
}

long pacha_syscall2(uint64_t nr, uint64_t fd, uint64_t enabled) {
    ++calls;
    ++enables;
    assert(nr == PACHA_CAPSULE_SYSCALL_DMA_SET_ENABLED && fd == DEVICE_FD && enabled <= 1);
    last_enabled = enabled;
    return enable_result;
}

long pacha_syscall6(uint64_t nr, uint64_t fd, uint64_t address, uint64_t iova,
    uint64_t length, uint64_t direction, uint64_t flags) {
    ++calls;
    assert(nr == PACHA_CAPSULE_SYSCALL_DERIVE_DMA_MAPPING && fd == DEVICE_FD && flags == 0);
    last_address = address; last_iova = iova;
    last_length = length; last_direction = direction;
    return derive_result;
}

long pacha_syscall1(uint64_t nr, uint64_t fd) {
    ++calls;
    assert(nr == PACHA_FD_SYSCALL_CLOSE && fd == 100);
    return close_result;
}

int main(void) {
    struct ph_dma dma = {0};
    struct ph_dma_mapping mappings[2];
    struct ph_dma_config config = {
        .device_fd = DEVICE_FD, .native_device = 42, .generation = 7,
        .ram = (void *)RAM, .ram_length = 0x10000,
        .aperture_start = IOVA, .aperture_end = IOVA + 0xffff,
        .mappings = mappings, .mapping_capacity = 2,
    };
    device = (struct pacha_capsule_info){
        .kind = PACHA_CAPSULE_KIND_DEVICE, .device = 42,
        .flags = PACHA_CAPSULE_DMA_TRANSLATED,
        .rights = PACHA_FD_RIGHT_QUERY | PACHA_FD_RIGHT_DERIVE_DMA |
            PACHA_FD_RIGHT_DMA_READ | PACHA_FD_RIGHT_DMA_WRITE | PACHA_FD_RIGHT_BUS_MASTER,
    };
    assert(!ph_dma_init(&dma, &config));
    assert(enables == 1 && !last_enabled && dma.drained && !dma.enabled);
    assert(dma.host.aperture_start == IOVA && dma.host.aperture_end == IOVA + 0xffff);
    assert(dma.host.coherent == 1 && dma.host.context == &dma);
    const struct kobox_linux_dma_host *host = &dma.host;
    for (unsigned int protection = 1; protection <= 3; ++protection) {
        assert(!host->map(&dma, IOVA, 4096, 8192, protection));
        assert(last_address == RAM + 4096 && last_iova == IOVA && last_length == 8192);
        assert(last_direction == protection);
        assert(!host->enable(&dma, 1) && dma.enabled && !dma.drained);
        assert(!host->enable(&dma, 0) && !dma.enabled && dma.drained);
        assert(mappings[0].length == 8192); /* Disable retains mapping ownership. */
        assert(!host->unmap(&dma, IOVA, 8192));
    }
    unsigned int before = calls;
    assert(host->enable(&dma, 2) == -EINVAL);
    assert(host->map(&dma, IOVA + 1, 0, 4096, 3) == -EINVAL);
    assert(host->map(&dma, IOVA, 1, 4096, 3) == -EINVAL);
    assert(host->map(&dma, IOVA, 0, 0, 3) == -EINVAL);
    assert(host->map(&dma, IOVA, 0, 4096, 0) == -EINVAL);
    assert(host->map(&dma, IOVA, 0, 4096, 4) == -EINVAL);
    assert(host->map(&dma, IOVA - 4096, 0, 4096, 3) == -ERANGE);
    assert(host->map(&dma, IOVA + 0xf000, 0, 8192, 3) == -ERANGE);
    assert(host->map(&dma, IOVA, 0xf000, 8192, 3) == -ERANGE);
    assert(host->map(&dma, IOVA, UINT64_MAX - 4095, 4096, 3) == -ERANGE);
    assert(calls == before);
    assert(!host->map(&dma, IOVA, 0, 4096, 3));
    before = calls;
    assert(host->map(&dma, IOVA, 4096, 4096, 1) == -EBUSY);
    assert(host->unmap(&dma, IOVA, 8192) == -ENOENT);
    assert(calls == before);
    assert(!host->map(&dma, IOVA + 4096, 0, 4096, 1)); /* RAM alias, different IOVA. */
    assert(host->map(&dma, IOVA + 8192, 0, 4096, 1) == -ENOSPC);
    close_result = PACHA_SYSCALL_ERR_MAP;
    assert(host->unmap(&dma, IOVA, 4096) == -EIO && mappings[0].fd == 100);
    close_result = 0;
    assert(!host->unmap(&dma, IOVA, 4096));
    assert(!host->unmap(&dma, IOVA + 4096, 4096));

    const long errors[] = {0, 1, 2, 3, 4, 5, 6, PACHA_FD_TABLE_LIMIT};
    for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
        derive_result = errors[i];
        assert(host->map(&dma, IOVA, 0, 4096, 3) < 0);
        assert(!mappings[0].length);
    }
    expect_fatal = 1;
    device.flags = PACHA_CAPSULE_DMA_QUARANTINED;
    if (!setjmp(fatal_jump)) {
        host->map(&dma, IOVA, 0, 4096, 3);
        abort();
    }
    device.flags = PACHA_CAPSULE_DMA_TRANSLATED;
    query_result = PACHA_SYSCALL_ERR_INVALID;
    if (!setjmp(fatal_jump)) {
        host->map(&dma, IOVA, 0, 4096, 3);
        abort();
    }
    query_result = 11;
    expect_fatal = 0;
    assert(ph_dma_revoke(&dma, 6) == -ESTALE && dma.admitted);
    enable_result = PACHA_SYSCALL_ERR_MAP;
    assert(ph_dma_revoke(&dma, 7) == -EIO && !dma.admitted && !dma.drained);
    assert(ph_dma_destroy(&dma) == -EBUSY);
    assert(host->enable(&dma, 1) == -ENODEV);
    assert(host->map(&dma, IOVA, 0, 4096, 3) == -ENODEV);
    enable_result = 0;
    assert(!ph_dma_revoke(&dma, 7));
    assert(!ph_dma_destroy(&dma));

    device.rights &= ~PACHA_FD_RIGHT_BUS_MASTER;
    before = enables;
    assert(ph_dma_init(&dma, &config) == -EACCES && enables == before);
    device.rights |= PACHA_FD_RIGHT_BUS_MASTER;
    device.flags = 0;
    assert(ph_dma_init(&dma, &config) == -EIO);
    device.flags = PACHA_CAPSULE_DMA_TRANSLATED;
    device.device = 43;
    assert(ph_dma_init(&dma, &config) == -ENODEV);
    device.device = 42;
    config.ram = (void *)(UINTPTR_MAX - 4095);
    assert(ph_dma_init(&dma, &config) == -EINVAL);
    puts("kobox2 DMA native-call/ownership unit: PASS");
    return 0;
}
