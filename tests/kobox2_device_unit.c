/* SPDX-License-Identifier: MIT */
/* Composition/ownership oracle; each native port has its own syscall tests. */
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include "../userland/kobox2_adapter/device.c"

const struct kobox_linux_memory_host_operations ph_memory_ops = {0};
static int pci_error, dma_error, irq_error, drain_error, expect_fatal;
static unsigned int calls, destroys, irq_revokes;
static jmp_buf fatal_jump;

_Noreturn void ph_fail(const char *file, unsigned int line, uint64_t result) {
    (void)file; (void)line; (void)result;
    assert(expect_fatal);
    longjmp(fatal_jump, 1);
}

int ph_pci_init(struct ph_pci *pci, const struct ph_pci_config *config) {
    ++calls;
    if (pci_error) return pci_error;
    pci->config = *config;
    pci->native_device = 42;
    pci->admitted = 1;
    return 0;
}

int ph_pci_revoke(struct ph_pci *pci, uint64_t generation) {
    ++calls;
    assert(pci->config.generation == generation);
    pci->admitted = 0;
    return 0;
}

int ph_pci_destroy(struct ph_pci *pci) {
    ++calls; ++destroys;
    assert(pci->config.generation && !pci->admitted);
    *pci = (struct ph_pci){0};
    return 0;
}

int ph_dma_init(struct ph_dma *dma, const struct ph_dma_config *config) {
    ++calls;
    assert(config->native_device == 42 && config->mapping_capacity == PH_DEVICE_DMA_MAPPINGS);
    assert(config->mapping_capacity >= PACHA_FD_TABLE_LIMIT);
    if (dma_error) return dma_error;
    dma->config = *config;
    dma->admitted = dma->drained = 1;
    return 0;
}

int ph_dma_revoke(struct ph_dma *dma, uint64_t generation) {
    ++calls;
    assert(dma->config.generation == generation);
    dma->admitted = 0;
    dma->drained = !drain_error;
    return drain_error;
}

int ph_dma_destroy(struct ph_dma *dma) {
    ++calls; ++destroys;
    assert(!dma->admitted && dma->drained && dma->config.generation);
    *dma = (struct ph_dma){0};
    return 0;
}

int ph_irq_init(struct ph_irq *irq, const struct ph_irq_config *config) {
    ++calls;
    assert(config->native_device == 42 && config->cpu_count == PH_CPU_COUNT);
    if (irq_error) return irq_error;
    irq->config = *config;
    irq->admitted = 1;
    return 0;
}

int ph_irq_revoke(struct ph_irq *irq, uint64_t generation) {
    ++calls; ++irq_revokes;
    assert(irq->config.generation == generation);
    irq->admitted = 0;
    return 0;
}

int ph_irq_destroy(struct ph_irq *irq) {
    ++calls; ++destroys;
    assert(irq->config.generation && !irq->admitted);
    *irq = (struct ph_irq){0};
    return 0;
}

int main(void) {
    struct ph_image image = {.ram_fd = 100, .direct = (void *)UINT64_C(0x200000000)};
    static struct ph_device device;
    uint64_t sequence = 17;
    struct ph_device_config config = {
        .device_fd = 224, .generation = 7, .image = &image, .irq_cookie_sequence = &sequence,
        .memory = {.size = sizeof(config.memory), .identity = KOBOX_LINUX_MEMORY_HOST_IDENTITY,
            .operations = &ph_memory_ops, .ram_backing = &image,
            .direct_map = image.direct, .ram_size = PH_RAM_SIZE,
            .vmalloc_base = (void *)UINT64_C(0x300000000), .vmalloc_size = 256ul << 20},
    };
    struct ph_device_config invalid = config;
    invalid.memory.direct_map = (void *)UINT64_C(0x400000000);
    assert(ph_device_init(&device, &invalid) == -EINVAL && !calls);
    invalid = config; invalid.memory.ram_backing = &invalid;
    assert(ph_device_init(&device, &invalid) == -EINVAL && !calls);
    invalid = config; invalid.memory.operations = NULL;
    assert(ph_device_init(&device, &invalid) == -EINVAL && !calls);
    pci_error = -EACCES;
    assert(ph_device_init(&device, &config) == -EACCES && !device.generation && !destroys);
    pci_error = 0; dma_error = -EIO;
    assert(ph_device_init(&device, &config) == -EIO && !device.pci.config.generation && destroys == 1);
    dma_error = 0; irq_error = -ENOMEM;
    assert(ph_device_init(&device, &config) == -ENOMEM && !device.dma.config.generation && destroys == 3);
    irq_error = 0;
    assert(!ph_device_init(&device, &config) && device.generation == 7);
    assert(device.pci.config.reservation == config.memory.vmalloc_base);
    assert(device.dma.config.ram == image.direct && device.irq.config.cookie_sequence == &sequence);
    unsigned int before = calls;
    assert(ph_device_init(&device, &config) == -EINVAL && calls == before);
    assert(ph_device_finish(&device, 6) == -ESTALE && calls == before);
    device.mmio_mappings[2].length = 4096;
    assert(ph_device_finish(&device, 7) == -EBUSY && calls == before);
    device.mmio_mappings[2].length = 0;
    device.dma_mappings[PH_DEVICE_DMA_MAPPINGS - 1].length = 4096;
    assert(ph_device_finish(&device, 7) == -EBUSY && calls == before);
    device.dma_mappings[PH_DEVICE_DMA_MAPPINGS - 1].length = 0;
    device.irq.slots[2].route.cookie = 1;
    assert(ph_device_finish(&device, 7) == -EBUSY && calls == before);
    device.irq.slots[2].route.cookie = 0;
    drain_error = -EIO;
    assert(ph_device_finish(&device, 7) == -EIO && device.generation == 7 && destroys == 3);
    assert(!device.irq.admitted && !device.dma.admitted && device.pci.admitted && irq_revokes == 1);
    drain_error = 0;
    assert(!ph_device_finish(&device, 7) && !device.generation && destroys == 6);
    assert(sequence == 17); /* Composition never rewinds a route counter. */
    config.generation = 8;
    assert(!ph_device_init(&device, &config));
    assert(ph_device_finish(&device, 7) == -ESTALE);
    assert(!ph_device_finish(&device, 8));
    irq_error = -ENOMEM; drain_error = -EIO; expect_fatal = 1;
    if (!setjmp(fatal_jump)) {
        ph_device_init(&device, &config);
        abort();
    }
    /* A failed rollback stop cannot return ordinary failure/backing ownership. */
    assert(device.dma.config.generation == 8 && device.pci.config.generation == 8);
    puts("kobox2 device bootstrap/lifetime unit: PASS");
    return 0;
}
