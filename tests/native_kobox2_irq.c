/* SPDX-License-Identifier: MIT */
/* Real native adapter collector/route lifecycle with dedicated virtio-rng.
 * The doorbell oracle is NOT a Linux hardirq handler or a logical CPU owner. */
#include "../userland/kobox2_adapter/host.h"
#include "../userland/kobox2_adapter/device_irq.h"
#include "../userland/seed0boot/src/bootstrap_abi.h"
#include <errno.h>
#define NATIVE_DEVICE_EXTERNAL_RUNTIME
#define main native_device_contract_main
#include "native_device_contract.c"
#undef main

/* Native thread machinery is real, but no Linux image is loaded here. */
struct ph_image ph_core;
static struct ph_task boot_task;
static struct ph_irq irq_port;
static atomic_uint delivered_cpus;
static uint64_t cookie_sequence;

void ph_dispatch_notifications(void) {
    ph_fail(__FILE__, __LINE__, 0); /* The oracle never sends native signals. */
}

void ph_exception(struct pacha_native_fault_frame *frame) {
    ph_fail(__FILE__, __LINE__, frame->address);
}

static int record_doorbell(uint32_t cpu, enum kobox_linux_task_notification kind) {
    PH_CHECK(cpu < 2 && kind == KOBOX_LINUX_TASK_DEVICE_IRQ);
    atomic_fetch_or_explicit(&delivered_cpus, 1u << cpu, memory_order_release);
    return 0;
}

const struct kobox_linux_task_host_operations ph_task_ops = {.cpu_notify = record_doorbell};

static void acquire_device(void) {
    CHECK(seed0_bootstrap_descriptor() != NULL);
    const uint64_t source = claim_pci_device(0x1af4, 0x1044);
    CHECK(call(PACHA_FD_SYSCALL_DUP, source, DEVICE_FD,
        query(source).rights, 0, 0, 0) == DEVICE_FD);
    close_fd(source);
}

static void program_route(struct kobox_linux_irq_route route) {
    w32(table + 12, 1);
    w64(table, route.address);
    w32(table + 8, route.data);
    set_config(msix_cap + 2, 2, (config(msix_cap + 2, 2) | 0x8000) & ~0x4000u);
    memset(dma_bytes, 0, sizeof(dma_bytes));
    setup_queue();
    w32(table + 12, 0);
}

static void await_event(struct kobox_linux_irq_route route, unsigned int cpu) {
    const uint64_t until = now() + UINT64_C(2000000000);
    struct kobox_linux_irq_event event;
    for (;;) {
        unsigned int delivered = atomic_exchange_explicit(&delivered_cpus, 0, memory_order_acq_rel);
        if (delivered & (1u << cpu)) {
            CHECK(irq_port.host.next(&irq_port, cpu ^ 1u, &event) == -EAGAIN);
            int result = irq_port.host.next(&irq_port, cpu, &event);
            if (!result) break;
            CHECK(result == -EAGAIN); /* A previously computed doorbell may arrive late. */
        }
        CHECK(now() < until);
    }
    CHECK(event.hwirq == route.hwirq && event.cookie == route.cookie);
    CHECK(!irq_port.host.ack(&irq_port, event.hwirq, event.cookie));
}

static void wait_until_collected(struct kobox_linux_irq_route route) {
    const uint64_t until = now() + UINT64_C(2000000000);
    for (;;) {
        /* Inspect only under the same lock. Do not call active here: it can
         * sample a native count itself and would hide a broken collector. */
        ph_lock(&irq_port.lock);
        struct ph_irq_slot *slot = ph_irq_queue_lookup(&irq_port.queue, route.hwirq, route.cookie);
        CHECK(slot);
        unsigned int pending = slot->pending;
        ph_unlock(&irq_port.lock);
        if (pending) return;
        CHECK(now() < until);
    }
}

void ph_main(void) {
    boot_task.self = &boot_task;
    boot_task.logical_cpu = -1;
    boot_task.bound = true;
    PH_OK(pacha_syscall1(PACHA_THREAD_SYSCALL_SET_FS_BASE, (uintptr_t)&boot_task));
    boot_task.fault_stack = ph_alloc(PH_THREAD_STACK_SIZE);
    ph_thread_setup(&boot_task);
    stage = "irq-adapter-bootstrap";
    acquire_device();
    discover();
    reset_device();
    const uint64_t dma = call(PACHA_CAPSULE_SYSCALL_DERIVE_DMA_MAPPING, DEVICE_FD,
        (uintptr_t)dma_bytes, TEST_IOVA, DMA_SIZE, PACHA_CAPSULE_DMA_BIDIRECTIONAL, 0);
    CHECK(is_fd(dma));
    struct kobox_linux_irq_route old = {0};
    for (unsigned int round = 0; round < 8; ++round) {
        atomic_store_explicit(&delivered_cpus, 0, memory_order_release);
        struct ph_irq_config irq_config = {
            .device_fd = DEVICE_FD, .native_device = query(DEVICE_FD).device,
            .generation = round + 1, .cpu_count = 2, .cookie_sequence = &cookie_sequence,
        };
        CHECK(!ph_irq_init(&irq_port, &irq_config));
        const struct kobox_linux_irq_host *host = &irq_port.host;
        struct kobox_linux_irq_route route;
        stage = "irq-adapter-allocate";
        CHECK(!host->allocate(&irq_port, KOBOX_IRQ_MSIX, 0, 1, 0, &route));
        CHECK(route.cookie > old.cookie);
        CHECK(host->mask(&irq_port, old.hwirq, old.cookie, 0) == -ESTALE);
        program_route(route);
        request_rng(1);
        stage = "irq-adapter-collector-masked";
        wait_until_collected(route);
        CHECK(!atomic_load_explicit(&delivered_cpus, memory_order_acquire));
        CHECK(!host->affinity(&irq_port, route.hwirq, route.cookie, 1));
        CHECK(!host->mask(&irq_port, route.hwirq, route.cookie, 0));
        await_event(route, 1);
        stage = "irq-adapter-live-edge";
        CHECK(!host->affinity(&irq_port, route.hwirq, route.cookie, 0));
        request_rng(2);
        await_event(route, 0); /* This doorbell must originate from the collector. */
        stage = "irq-adapter-pending-retire";
        w32(table + 12, 1);
        request_rng(3);
        CHECK(r32(pba) & 1);
        CHECK(host->release(&irq_port, route.hwirq, route.cookie) == -EBUSY);
        reset_device();
        CHECK(!host->quiesce(&irq_port, route.hwirq, route.cookie));
        CHECK(!host->release(&irq_port, route.hwirq, route.cookie));
        old = route;
        stage = "irq-adapter-fd-reuse";
        for (unsigned int reuse = 0; reuse < 64; ++reuse) {
            CHECK(!host->allocate(&irq_port, KOBOX_IRQ_MSIX, 0, 1, reuse % 2, &route));
            CHECK(route.cookie > old.cookie);
            CHECK(host->retrigger(&irq_port, old.hwirq, old.cookie) == -ESTALE);
            CHECK(!host->release(&irq_port, route.hwirq, route.cookie));
            old = route;
        }
        CHECK(!host->allocate(&irq_port, KOBOX_IRQ_MSIX, 0, 1, 1, &route));
        program_route(route);
        CHECK(!host->mask(&irq_port, route.hwirq, route.cookie, 0));
        request_rng(1);
        await_event(route, 1);
        reset_device();
        CHECK(!ph_irq_revoke(&irq_port, irq_config.generation));
        CHECK(host->mask(&irq_port, route.hwirq, route.cookie, 0) == -ENODEV);
        CHECK(!host->release(&irq_port, route.hwirq, route.cookie));
        old = route;
        CHECK(!ph_irq_destroy(&irq_port));
        ph_number("IRQ adapter round", round + 1);
    }
    close_fd(dma);
    for (unsigned int i = 0; i < map_count; ++i) close_fd(maps[i].fd);
    close_fd(DEVICE_FD);
    ph_log("NATIVE_KOBOX2_IRQ=PASS\n");
    PH_OK(pacha_syscall1(PACHA_PROCESS_SYSCALL_EXIT, 0));
    ph_fail(__FILE__, __LINE__, 0);
}
