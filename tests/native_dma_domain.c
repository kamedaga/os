/* SPDX-License-Identifier: MIT */
/* Real virtio-rng DMA through the PachaOS kobox2 port, not a GPU substitute. */
#define main native_device_contract_main
#include "native_device_contract.c"
#undef main
#include "../userland/seed0boot/src/bootstrap_abi.h"
#include "../userland/kobox2_adapter/device_dma.h"

void *memcpy(void *destination, const void *source, size_t length) {
    unsigned char *out = destination;
    const unsigned char *in = source;
    for (size_t i = 0; i < length; ++i) out[i] = in[i];
    return destination;
}

_Noreturn void ph_fail(const char *file, unsigned line, uint64_t result) {
    log_text(file);
    last_result = result;
    fail_at(line);
}

void ph_log(const char *text) {
    log_text(text);
}

static void acquire_device(void) {
    const struct seed0_init_descriptor_page *boot = seed0_bootstrap_descriptor();
    CHECK(boot && boot->device_count <= SEED0_INIT_MAX_DEVICE_DESCRIPTORS);
    uint64_t source = 0;
    for (uint64_t i = 0; i < boot->device_count; ++i) {
        if (boot->devices[i].vendor_id == 0x1af4 && boot->devices[i].device_id == 0x1044) {
            CHECK(!source);
            source = boot->devices[i].init_device_fd;
        }
    }
    CHECK(is_fd(source));
    CHECK(call(PACHA_FD_SYSCALL_DUP, source, DEVICE_FD, query(source).rights, 0, 0, 0) == DEVICE_FD);
}

static void check_restricted_aliases(void) {
    const uint64_t rights = query(DEVICE_FD).rights;
    const uint64_t forbidden[] = {PACHA_FD_RIGHT_DERIVE_DMA, PACHA_FD_RIGHT_BUS_MASTER};
    for (unsigned i = 0; i < 2; ++i) {
        const uint64_t alias = call(PACHA_FD_SYSCALL_DUP, DEVICE_FD, 16,
            rights & ~forbidden[i], 0, 0, 0);
        CHECK(is_fd(alias));
        for (unsigned enabled = 0; enabled < 2; ++enabled)
            CHECK(call(PACHA_CAPSULE_SYSCALL_DMA_SET_ENABLED, alias, enabled, 0, 0, 0, 0) ==
                PACHA_SYSCALL_ERR_INVALID);
        close_fd(alias);
    }
    CHECK(call(PACHA_CAPSULE_SYSCALL_DMA_SET_ENABLED, DEVICE_FD, 2, 0, 0, 0, 0) ==
        PACHA_SYSCALL_ERR_INVALID);
    CHECK(call(PACHA_CAPSULE_SYSCALL_DMA_SET_ENABLED, UINT64_MAX, 0, 0, 0, 0, 0) ==
        PACHA_SYSCALL_ERR_INVALID);
}

static void expect_blocked_request(void) {
    /* A second valid descriptor, using mappings exercised immediately before
     * the stop. This detects stale IOTLB access as well as a missing block. */
    memset(dma_bytes + 3 * PAGE, 0xa5, 64);
    *(uint16_t *)&dma_bytes[PAGE + 6] = 0;
    barrier();
    w16((uintptr_t)dma_bytes + PAGE + 2, 2);
    barrier();
    // Bus-master reconfiguration must not bypass the IOMMU-domain barrier.
    set_config(4, 2, config(4, 2) & ~4u);
    set_config(4, 2, config(4, 2) | 4u);
    CHECK(config(4, 2) & 4u);
    /* QEMU clears DRIVER_OK when BM is cleared. Restore it so this tests
     * IOMMU rejection, not an idle virtqueue that never attempts DMA. */
    w8(common + 20, 15);
    CHECK(r8(common + 20) & 4);
    w16(notify, 0);
    const uint64_t until = now() + UINT64_C(50000000);
    while (now() < until) {
        CHECK(r16((uintptr_t)dma_bytes + 2 * PAGE + 2) == 1);
        for (unsigned i = 0; i < 64; ++i) CHECK(r8((uintptr_t)dma_bytes + 3 * PAGE + i) == 0xa5);
    }
}

int main(void) {
    stage = "dma-domain-bootstrap";
    acquire_device();
    discover();
    reset_device();
    struct ph_dma port = {0};
    struct ph_dma_mapping mappings[4];
    const struct ph_dma_config settings = {
        .device_fd = DEVICE_FD, .native_device = query(DEVICE_FD).device, .generation = 1,
        .ram = dma_bytes, .ram_length = sizeof(dma_bytes),
        .aperture_start = TEST_IOVA, .aperture_end = TEST_IOVA + DMA_SIZE - 1,
        .mappings = mappings, .mapping_capacity = 4,
    };
    CHECK(ph_dma_init(&port, &settings) == 0); /* Empty-domain stop before first map. */
    const struct kobox_linux_dma_host *host = &port.host;
    check_restricted_aliases();
    for (unsigned round = 0; round < 4; ++round) {
        stage = "dma-domain-blocked-map";
        memset(dma_bytes, 0, sizeof(dma_bytes));
        for (unsigned page = 0; page < 4; ++page)
            CHECK(host->map(&port, TEST_IOVA + page * PAGE, page * PAGE, PAGE,
                page < 2 ? KOBOX_DMA_DEVICE_READ : KOBOX_DMA_DEVICE_WRITE) == 0);
        const uint64_t identity = query(mappings[0].fd).object_id;
        CHECK(host->map(&port, TEST_IOVA, 0, PAGE, KOBOX_DMA_DEVICE_READ) < 0);
        CHECK(host->enable(&port, 1) == 0);
        w32(table + 12, 1);
        set_config(msix_cap + 2, 2, config(msix_cap + 2, 2) | 0xc000);
        setup_queue();
        stage = "dma-domain-enabled-request";
        request_rng(1);
        CHECK(host->enable(&port, 0) == 0);
        CHECK(query(mappings[0].fd).object_id == identity);
        stage = "dma-domain-blocked-request";
        expect_blocked_request();
        /* A deliberately faulted virtqueue may require device reset. Mapping
         * FDs/IOVAs stay unchanged across that reset and domain restart. */
        reset_device();
        memset(dma_bytes, 0, sizeof(dma_bytes));
        CHECK(host->enable(&port, 1) == 0);
        CHECK(query(mappings[0].fd).object_id == identity);
        set_config(msix_cap + 2, 2, config(msix_cap + 2, 2) | 0xc000);
        setup_queue();
        stage = "dma-domain-resumed-request";
        request_rng(1);
        reset_device();
        CHECK(host->enable(&port, 0) == 0);
        stage = "dma-domain-drained-unmap";
        for (unsigned page = 0; page < 4; ++page)
            CHECK(host->unmap(&port, TEST_IOVA + page * PAGE, PAGE) == 0);
        CHECK(!(query(DEVICE_FD).flags & PACHA_CAPSULE_DMA_QUARANTINED));
    }
    CHECK(ph_dma_revoke(&port, 1) == 0);
    CHECK(host->enable(&port, 1) < 0);
    CHECK(ph_dma_destroy(&port) == 0);
    for (unsigned i = 0; i < map_count; ++i) close_fd(maps[i].fd);
    close_fd(DEVICE_FD);
    log_text("NATIVE_DMA_DOMAIN=PASS\n");
    return 0;
}
