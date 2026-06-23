#include "pacha/capsule.h"
#include "pacha/syscall.h"

static int pacha_capsule_valid_width(unsigned width) {
    return width == 1 || width == 2 || width == 4;
}

int pacha_capsule_is_fd(int fd) {
    return fd >= 16 && fd < 256;
}

int pacha_capsule_has_rights(const struct pacha_capsule_info *info, uint64_t rights) {
    if (!info) return 0;
    return (info->rights & rights) == rights;
}

int pacha_capsule_query(int fd, struct pacha_capsule_info *out) {
    if (!out) return -1;
    uint64_t words[11] = {0};
    const long ret = pacha_syscall3(PACHA_CAPSULE_SYSCALL_QUERY, (uint64_t)(uint32_t)fd, (uint64_t)(uintptr_t)words, 11);
    if (ret < 0 || ret < 11) return pacha_status_to_int(ret);
    *out = (struct pacha_capsule_info){
        .fd = words[0],
        .kind = words[1],
        .rights = words[2],
        .owner = words[3],
        .device = words[4],
        .object_id = words[5],
        .user_va = words[6],
        .iova = words[7],
        .size = words[8],
        .index = words[9],
        .flags = words[10],
    };
    return 0;
}

int pacha_capsule_expect_kind(int fd, uint64_t kind, struct pacha_capsule_info *out) {
    struct pacha_capsule_info local;
    struct pacha_capsule_info *info = out ? out : &local;
    const int status = pacha_capsule_query(fd, info);
    if (status != 0) return status;
    return info->kind == kind ? 0 : -1;
}

int pacha_capsule_close(int fd) {
    return pacha_status_to_int(pacha_syscall1(PACHA_FD_SYSCALL_CLOSE, (uint64_t)(uint32_t)fd));
}

int pacha_capsule_pci_config_read(int device_fd, uint16_t offset, unsigned width, uint32_t *out) {
    if (!out || !pacha_capsule_valid_width(width)) return -1;
    uint8_t bytes[4] = {0, 0, 0, 0};
    const long ret = pacha_syscall4(PACHA_CAPSULE_SYSCALL_PCI_CONFIG_READ, (uint64_t)(uint32_t)device_fd, offset, (uint64_t)(uintptr_t)bytes, width);
    if (ret != 0) return pacha_status_to_int(ret);
    uint32_t value = 0;
    for (unsigned i = 0; i < width; i++) value |= ((uint32_t)bytes[i]) << (i * 8u);
    *out = value;
    return 0;
}

int pacha_capsule_pci_config_write(int device_fd, uint16_t offset, unsigned width, uint32_t value) {
    if (!pacha_capsule_valid_width(width)) return -1;
    uint8_t bytes[4] = {
        (uint8_t)(value & 0xffu),
        (uint8_t)((value >> 8u) & 0xffu),
        (uint8_t)((value >> 16u) & 0xffu),
        (uint8_t)((value >> 24u) & 0xffu),
    };
    return pacha_status_to_int(pacha_syscall4(PACHA_CAPSULE_SYSCALL_PCI_CONFIG_WRITE, (uint64_t)(uint32_t)device_fd, offset, (uint64_t)(uintptr_t)bytes, width));
}

int pacha_capsule_pci_bar_info(int device_fd, unsigned bar, struct pacha_capsule_bar_info *out) {
    if (!out) return -1;
    uint64_t words[4] = {0, 0, 0, 0};
    const long ret = pacha_syscall4(PACHA_CAPSULE_SYSCALL_PCI_BAR_INFO, (uint64_t)(uint32_t)device_fd, bar, (uint64_t)(uintptr_t)words, 4);
    if (ret < 0 || ret < 4) return pacha_status_to_int(ret);
    *out = (struct pacha_capsule_bar_info){
        .start = words[0],
        .end = words[1],
        .size = words[2],
        .flags = words[3],
    };
    return 0;
}

int pacha_capsule_derive_mmio(int device_fd, unsigned bar, void *addr, size_t len, uint64_t flags) {
    return pacha_fd_result_to_int(pacha_syscall5(PACHA_CAPSULE_SYSCALL_DERIVE_MMIO, (uint64_t)(uint32_t)device_fd, bar, (uint64_t)(uintptr_t)addr, len, flags));
}

int pacha_capsule_derive_dma_buffer(int device_fd, void *addr, uint64_t iova, size_t len, uint64_t flags) {
    return pacha_fd_result_to_int(pacha_syscall5(PACHA_CAPSULE_SYSCALL_DERIVE_DMA_BUFFER, (uint64_t)(uint32_t)device_fd, (uint64_t)(uintptr_t)addr, iova, len, flags));
}

int pacha_capsule_derive_dma_mapping(int device_fd, void *addr, uint64_t iova, size_t len, unsigned direction, uint64_t flags) {
    return pacha_fd_result_to_int(pacha_syscall6(PACHA_CAPSULE_SYSCALL_DERIVE_DMA_MAPPING, (uint64_t)(uint32_t)device_fd, (uint64_t)(uintptr_t)addr, iova, len, direction, flags));
}

int pacha_capsule_derive_dma_mapping_from_buffer(int dma_buffer_fd, uint64_t iova, size_t len, unsigned direction, uint64_t flags) {
    return pacha_fd_result_to_int(pacha_syscall5(PACHA_CAPSULE_SYSCALL_DERIVE_DMA_MAPPING_FROM_BUFFER, (uint64_t)(uint32_t)dma_buffer_fd, iova, len, direction, flags));
}

int pacha_capsule_derive_irq(int device_fd, unsigned kind, unsigned vector, uint64_t flags) {
    return pacha_fd_result_to_int(pacha_syscall4(PACHA_CAPSULE_SYSCALL_DERIVE_IRQ, (uint64_t)(uint32_t)device_fd, kind, vector, flags));
}

int pacha_capsule_mmio_from_fd(int mmio_fd, struct pacha_capsule_mmio *out) {
    if (!out) return -1;
    struct pacha_capsule_info info;
    const int status = pacha_capsule_expect_kind(mmio_fd, PACHA_CAPSULE_KIND_MMIO, &info);
    if (status != 0) return status;
    *out = (struct pacha_capsule_mmio){
        .fd = mmio_fd,
        .addr = (void *)(uintptr_t)info.user_va,
        .len = (size_t)info.size,
    };
    return 0;
}

int pacha_capsule_mmio_mapping(int mmio_fd, void **addr, size_t *len) {
    struct pacha_capsule_mmio mmio;
    const int status = pacha_capsule_mmio_from_fd(mmio_fd, &mmio);
    if (status != 0) return status;
    if (addr) *addr = mmio.addr;
    if (len) *len = mmio.len;
    return 0;
}

int pacha_capsule_dma_from_fd(int dma_fd, struct pacha_capsule_dma *out) {
    if (!out) return -1;
    struct pacha_capsule_info info;
    const int status = pacha_capsule_query(dma_fd, &info);
    if (status != 0) return status;
    if (info.kind != PACHA_CAPSULE_KIND_DMA_BUFFER && info.kind != PACHA_CAPSULE_KIND_DMA_MAPPING) return -1;
    *out = (struct pacha_capsule_dma){
        .fd = dma_fd,
        .addr = (void *)(uintptr_t)info.user_va,
        .len = (size_t)info.size,
        .iova = info.iova,
    };
    return 0;
}

int pacha_capsule_dma_mapping(int dma_fd, void **addr, size_t *len, uint64_t *iova) {
    struct pacha_capsule_dma dma;
    const int status = pacha_capsule_dma_from_fd(dma_fd, &dma);
    if (status != 0) return status;
    if (addr) *addr = dma.addr;
    if (len) *len = dma.len;
    if (iova) *iova = dma.iova;
    return 0;
}

int pacha_capsule_irq_poll(int irq_fd, uint64_t last_count, uint64_t *out_count) {
    if (!out_count) return -1;
    const long ret = pacha_syscall5(PACHA_CAPSULE_SYSCALL_IRQ_POLL, (uint64_t)(uint32_t)irq_fd, last_count, (uint64_t)(uintptr_t)out_count, 1, 0);
    return ret == 1 ? 0 : pacha_status_to_int(ret);
}

int pacha_capsule_irq_wait(int irq_fd, uint64_t last_count, uint64_t *out_count) {
    if (!out_count) return -1;
    for (;;) {
        const int status = pacha_capsule_irq_poll(irq_fd, last_count, out_count);
        if (status != PACHA_ERR_NOT_READY) return status;
    }
}

int pacha_capsule_irq_from_fd(int irq_fd, struct pacha_capsule_irq *out) {
    if (!out) return -1;
    struct pacha_capsule_info info;
    const int status = pacha_capsule_expect_kind(irq_fd, PACHA_CAPSULE_KIND_IRQ, &info);
    if (status != 0) return status;
    uint64_t count = 0;
    const int poll_status = pacha_capsule_irq_poll(irq_fd, PACHA_CAPSULE_IRQ_CURRENT_COUNT, &count);
    if (poll_status != 0) return poll_status;
    *out = (struct pacha_capsule_irq){
        .fd = irq_fd,
        .count = count,
    };
    return 0;
}

int pacha_capsule_device_derive_mmio(int device_fd, unsigned bar, void *addr, size_t len, uint64_t flags, struct pacha_capsule_mmio *out) {
    if (!out) return -1;
    const int fd = pacha_capsule_derive_mmio(device_fd, bar, addr, len, flags);
    if (!pacha_capsule_is_fd(fd)) return fd;
    const int status = pacha_capsule_mmio_from_fd(fd, out);
    if (status != 0) {
        (void)pacha_capsule_close(fd);
        return status;
    }
    return 0;
}

int pacha_capsule_device_derive_dma_buffer(int device_fd, void *addr, uint64_t iova, size_t len, uint64_t flags, struct pacha_capsule_dma *out) {
    if (!out) return -1;
    const int fd = pacha_capsule_derive_dma_buffer(device_fd, addr, iova, len, flags);
    if (!pacha_capsule_is_fd(fd)) return fd;
    const int status = pacha_capsule_dma_from_fd(fd, out);
    if (status != 0) {
        (void)pacha_capsule_close(fd);
        return status;
    }
    return 0;
}

int pacha_capsule_dma_derive_mapping(const struct pacha_capsule_dma *buffer, uint64_t iova, size_t len, unsigned direction, uint64_t flags, struct pacha_capsule_dma *out) {
    if (!buffer || !out) return -1;
    const int fd = pacha_capsule_derive_dma_mapping_from_buffer(buffer->fd, iova, len, direction, flags);
    if (!pacha_capsule_is_fd(fd)) return fd;
    const int status = pacha_capsule_dma_from_fd(fd, out);
    if (status != 0) {
        (void)pacha_capsule_close(fd);
        return status;
    }
    return 0;
}

int pacha_capsule_device_derive_irq(int device_fd, unsigned kind, unsigned vector, uint64_t flags, struct pacha_capsule_irq *out) {
    if (!out) return -1;
    const int fd = pacha_capsule_derive_irq(device_fd, kind, vector, flags);
    if (!pacha_capsule_is_fd(fd)) return fd;
    const int status = pacha_capsule_irq_from_fd(fd, out);
    if (status != 0) {
        (void)pacha_capsule_close(fd);
        return status;
    }
    return 0;
}

int pacha_capsule_irq_next(struct pacha_capsule_irq *irq) {
    if (!irq) return -1;
    uint64_t next = 0;
    const int status = pacha_capsule_irq_wait(irq->fd, irq->count, &next);
    if (status != 0) return status;
    irq->count = next;
    return 0;
}
