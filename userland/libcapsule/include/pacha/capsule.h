#ifndef PACHA_CAPSULE_H
#define PACHA_CAPSULE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    PACHA_FD_SYSCALL_CLOSE = 14,
    PACHA_CAPSULE_SYSCALL_QUERY = 27,
    PACHA_CAPSULE_SYSCALL_DERIVE_MMIO = 28,
    PACHA_CAPSULE_SYSCALL_DERIVE_DMA_BUFFER = 29,
    PACHA_CAPSULE_SYSCALL_DERIVE_DMA_MAPPING = 30,
    PACHA_CAPSULE_SYSCALL_DERIVE_DMA_MAPPING_FROM_BUFFER = 31,
    PACHA_CAPSULE_SYSCALL_DERIVE_IRQ = 32,
    PACHA_CAPSULE_SYSCALL_PCI_CONFIG_READ = 33,
    PACHA_CAPSULE_SYSCALL_PCI_CONFIG_WRITE = 34,
    PACHA_CAPSULE_SYSCALL_PCI_BAR_INFO = 35,
    PACHA_CAPSULE_SYSCALL_IRQ_POLL = 36,

    PACHA_CAPSULE_KIND_DEVICE = 2,
    PACHA_CAPSULE_KIND_MMIO = 3,
    PACHA_CAPSULE_KIND_DMA_BUFFER = 4,
    PACHA_CAPSULE_KIND_DMA_MAPPING = 5,
    PACHA_CAPSULE_KIND_IRQ = 6,

    PACHA_CAPSULE_DMA_TO_DEVICE = 1,
    PACHA_CAPSULE_DMA_FROM_DEVICE = 2,
    PACHA_CAPSULE_DMA_BIDIRECTIONAL = 3,

    PACHA_CAPSULE_IRQ_AUTO = 0,
    PACHA_CAPSULE_IRQ_INTX = 1,
    PACHA_CAPSULE_IRQ_MSI = 2,
    PACHA_CAPSULE_IRQ_MSIX = 3,

    PACHA_CAPSULE_BAR_IO = 1ull << 0,
    PACHA_CAPSULE_BAR_MEM = 1ull << 1,
    PACHA_CAPSULE_BAR_PREFETCHABLE = 1ull << 2,
    PACHA_CAPSULE_BAR_64BIT = 1ull << 3,

    PACHA_CAPSULE_MMIO_REPLACE_EXISTING = 1ull << 0,

    PACHA_FD_RIGHT_INSPECT = 1ull << 0,
    PACHA_FD_RIGHT_DUP = 1ull << 1,
    PACHA_FD_RIGHT_TRANSFER = 1ull << 2,
    PACHA_FD_RIGHT_CLOSE = 1ull << 6,
    PACHA_FD_RIGHT_QUERY = 1ull << 27,
    PACHA_FD_RIGHT_CONFIG_READ = 1ull << 28,
    PACHA_FD_RIGHT_CONFIG_WRITE = 1ull << 29,
    PACHA_FD_RIGHT_DERIVE_MMIO = 1ull << 30,
    PACHA_FD_RIGHT_DERIVE_DMA = 1ull << 31,
    PACHA_FD_RIGHT_DERIVE_IRQ = 1ull << 32,
    PACHA_FD_RIGHT_MMIO_MAP_READ = 1ull << 33,
    PACHA_FD_RIGHT_MMIO_MAP_WRITE = 1ull << 34,
    PACHA_FD_RIGHT_CPU_READ = 1ull << 35,
    PACHA_FD_RIGHT_CPU_WRITE = 1ull << 36,
    PACHA_FD_RIGHT_DMA_READ = 1ull << 37,
    PACHA_FD_RIGHT_DMA_WRITE = 1ull << 38,
    PACHA_FD_RIGHT_IRQ_WAIT = 1ull << 39,
    PACHA_FD_RIGHT_IRQ_ACK = 1ull << 40,
    PACHA_FD_RIGHT_BUS_MASTER = 1ull << 41,
};

struct pacha_capsule_info {
    uint64_t fd;
    uint64_t parent_fd;
    uint64_t kind;
    uint64_t state;
    uint64_t rights;
    uint64_t owner;
    uint64_t generation;
    uint64_t revoke_generation;
    uint64_t device;
    uint64_t object_id;
    uint64_t user_va;
    uint64_t iova;
    uint64_t size;
    uint64_t index;
    uint64_t flags;
};

struct pacha_capsule_bar_info {
    uint64_t start;
    uint64_t end;
    uint64_t size;
    uint64_t flags;
};

int pacha_capsule_query(int fd, struct pacha_capsule_info *out);
int pacha_capsule_expect_kind(int fd, uint64_t kind, struct pacha_capsule_info *out);
int pacha_capsule_close(int fd);

int pacha_capsule_pci_config_read(int device_fd, uint16_t offset, unsigned width, uint32_t *out);
int pacha_capsule_pci_config_write(int device_fd, uint16_t offset, unsigned width, uint32_t value);
int pacha_capsule_pci_bar_info(int device_fd, unsigned bar, struct pacha_capsule_bar_info *out);

int pacha_capsule_derive_mmio(int device_fd, unsigned bar, void *addr, size_t len, uint64_t flags);
int pacha_capsule_derive_dma_buffer(int device_fd, void *addr, uint64_t iova, size_t len, uint64_t flags);
int pacha_capsule_derive_dma_mapping(int device_fd, void *addr, uint64_t iova, size_t len, unsigned direction, uint64_t flags);
int pacha_capsule_derive_dma_mapping_from_buffer(int dma_buffer_fd, uint64_t iova, size_t len, unsigned direction, uint64_t flags);
int pacha_capsule_derive_irq(int device_fd, unsigned kind, unsigned vector, uint64_t flags);

int pacha_capsule_mmio_mapping(int mmio_fd, void **addr, size_t *len);
int pacha_capsule_dma_mapping(int dma_fd, void **addr, size_t *len, uint64_t *iova);
int pacha_capsule_irq_wait(int irq_fd, uint64_t last_count, uint64_t *out_count);

#ifdef __cplusplus
}
#endif

#endif
