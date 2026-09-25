#ifndef PACHA_CAPSULE_H
#define PACHA_CAPSULE_H

#include "pacha/abi.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
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
    PACHA_CAPSULE_MMIO_READ_ONLY = 1ull << 1,
    PACHA_CAPSULE_MMIO_CACHE_UC = 0ull << 2,
    PACHA_CAPSULE_MMIO_CACHE_UC_MINUS = 1ull << 2,
    PACHA_CAPSULE_MMIO_CACHE_WC = 2ull << 2,
    PACHA_CAPSULE_MMIO_CACHE_WB = 3ull << 2,
    PACHA_CAPSULE_MMIO_CACHE_WT = 4ull << 2,
    PACHA_CAPSULE_MMIO_CACHE_WP = 5ull << 2,

    PACHA_CAPSULE_DMA_IOVA_KERNEL_CHOOSE = UINT64_MAX,

    /* Terminal domain failure: stop all DMA users before releasing or
     * repurposing their RAM. Query the device also after a failed derive. */
    PACHA_CAPSULE_DMA_QUARANTINED = UINT64_C(1) << 32,
    /* Required for an isolated DMA host contract. Equal CPU/device addresses
     * alone are not evidence of isolation. Absent on a quarantined device. */
    PACHA_CAPSULE_DMA_TRANSLATED = UINT64_C(1) << 33,
    PACHA_CAPSULE_IRQ_RETIRED = UINT64_C(1) << 34,

    PACHA_CAPSULE_IRQ_CURRENT_COUNT = UINT64_MAX,
};

struct pacha_capsule_info {
    uint64_t fd;
    uint64_t kind;
    uint64_t rights;
    uint64_t owner;
    uint64_t device;
    uint64_t object_id;
    uint64_t user_va;
    /* DEVICE: one continuous translated DMA aperture; size=0 if unavailable.
     * This is not an endpoint DMA mask. DMA objects: allocation address/size. */
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

struct pacha_capsule_pci_function {
    uint64_t resource_id;
    uint64_t vendor_id;
    uint64_t device_id;
    uint64_t subsystem_id;
    uint64_t class_code;
    uint64_t bus;
    uint64_t device;
    uint64_t function;
};

struct pacha_capsule_mmio {
    int fd;
    void *addr;
    size_t len;
};

struct pacha_capsule_dma {
    int fd;
    void *addr;
    size_t len;
    uint64_t iova;
};

struct pacha_capsule_irq {
    int fd;
    uint64_t count;
};

struct pacha_capsule_irq_route {
    uint64_t message_address;
    uint64_t message_data;
    uint64_t hwirq;
};

int pacha_capsule_is_fd(int fd);
int pacha_capsule_has_rights(const struct pacha_capsule_info *info, uint64_t rights);

int pacha_capsule_query(int fd, struct pacha_capsule_info *out);
int pacha_capsule_expect_kind(int fd, uint64_t kind, struct pacha_capsule_info *out);
int pacha_capsule_close(int fd);

/* Enumeration and claiming are restricted to the bootstrap owner. The
 * catalog includes all non-bridge functions captured before user drivers.
 * Each entry may be claimed once and transferred to its driver. */
long pacha_capsule_pci_function_count(void);
int pacha_capsule_pci_function_at(uint64_t index, struct pacha_capsule_pci_function *out);
int pacha_capsule_pci_claim(uint64_t index);

int pacha_capsule_pci_config_read(int device_fd, uint16_t offset, unsigned width, uint32_t *out);
int pacha_capsule_pci_config_write(int device_fd, uint16_t offset, unsigned width, uint32_t value);
int pacha_capsule_pci_bar_info(int device_fd, unsigned bar, struct pacha_capsule_bar_info *out);

int pacha_capsule_derive_mmio(int device_fd, unsigned bar, void *addr, size_t len, uint64_t flags);
/* page_offset is relative to the page containing BAR start, not a physical
 * address. Unsupported cache types fail; they are never silently substituted. */
int pacha_capsule_derive_mmio_range(int device_fd, unsigned bar, void *addr, size_t len, uint64_t flags, uint64_t page_offset);
int pacha_capsule_derive_dma_buffer(int device_fd, void *addr, uint64_t iova, size_t len, uint64_t flags);
int pacha_capsule_derive_dma_mapping(int device_fd, void *addr, uint64_t iova, size_t len, unsigned direction, uint64_t flags);
/* Returns one DMA-mapping fd. out_page_dma[0] addresses user_va exactly;
 * later entries address the base of each following page. Keep the userspace
 * range mapped and unchanged until the returned fd is closed. */
int pacha_capsule_derive_dma_mapping_pages(int device_fd, void *user_va, size_t size, unsigned direction, uint64_t *out_page_dma, size_t out_capacity_entries);
int pacha_capsule_derive_dma_mapping_from_buffer(int dma_buffer_fd, uint64_t iova, size_t len, unsigned direction, uint64_t flags);
/* Requires DERIVE_DMA and BUS_MASTER. Controls the shared device domain,
 * including mappings created through other aliases. Disable retains mapping
 * FDs/IOVAs and synchronously drains DMA; failure is not a completed stop.
 * Only isolated legacy VT-d domains with read/write drain are supported. */
int pacha_capsule_dma_set_enabled(int device_fd, unsigned enabled);
int pacha_capsule_derive_irq(int device_fd, unsigned kind, unsigned vector, uint64_t flags);

int pacha_capsule_mmio_from_fd(int mmio_fd, struct pacha_capsule_mmio *out);
int pacha_capsule_mmio_mapping(int mmio_fd, void **addr, size_t *len);
int pacha_capsule_dma_from_fd(int dma_fd, struct pacha_capsule_dma *out);
int pacha_capsule_dma_mapping(int dma_fd, void **addr, size_t *len, uint64_t *iova);
int pacha_capsule_irq_from_fd(int irq_fd, struct pacha_capsule_irq *out);
int pacha_capsule_irq_route(int irq_fd, struct pacha_capsule_irq_route *out);
/* Explicit MSI/MSI-X routes only. Both operations require IRQ_ACK and keep
 * the source physically masked. Failure retains the lease/FD; it may still
 * have masked the source. Quiesce drains without dropping the route.
 * Retire ends the shared object, including dup/transfer aliases: WAIT_MANY
 * reports HANGUP and route/poll/control operations report CLOSED. Closing an
 * old alias never touches a replacement route. Caller must stop the device's
 * interrupt source and synchronize its userspace collector/handlers first. */
int pacha_capsule_irq_quiesce(int irq_fd);
int pacha_capsule_irq_retire(int irq_fd);
int pacha_capsule_irq_poll(int irq_fd, uint64_t last_count, uint64_t *out_count);
int pacha_capsule_irq_wait(int irq_fd, uint64_t last_count, uint64_t *out_count);

int pacha_capsule_device_derive_mmio(int device_fd, unsigned bar, void *addr, size_t len, uint64_t flags, struct pacha_capsule_mmio *out);
int pacha_capsule_device_derive_dma_buffer(int device_fd, void *addr, uint64_t iova, size_t len, uint64_t flags, struct pacha_capsule_dma *out);
int pacha_capsule_dma_derive_mapping(const struct pacha_capsule_dma *buffer, uint64_t iova, size_t len, unsigned direction, uint64_t flags, struct pacha_capsule_dma *out);
int pacha_capsule_device_derive_irq(int device_fd, unsigned kind, unsigned vector, uint64_t flags, struct pacha_capsule_irq *out);
int pacha_capsule_irq_next(struct pacha_capsule_irq *irq);

#ifdef __cplusplus
}
#endif

#endif
