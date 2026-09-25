/* SPDX-License-Identifier: MIT */
#ifndef PACHA_KOBOX_DEVICE_IRQ_H
#define PACHA_KOBOX_DEVICE_IRQ_H

#include "device_irq_queue.h"
#include <stdatomic.h>

/* Current native per-device lease limit; hardware may support fewer entries. */
#define PH_IRQ_ROUTES 16u

struct ph_task;

struct ph_irq_config {
    int device_fd;
    uint64_t native_device;
    uint64_t generation;
    uint32_t cpu_count;
    uint64_t *cookie_sequence;
};

struct ph_irq_native_route {
    int fd;
    unsigned int retired;
    uint64_t count;
};

struct ph_irq {
    struct kobox_linux_irq_host host;
    struct ph_irq_config config;
    struct ph_irq_queue queue;
    struct ph_irq_slot slots[PH_IRQ_ROUTES];
    struct ph_irq_native_route native[PH_IRQ_ROUTES];
    atomic_uint lock;
    struct ph_task *collector;
    int control_fd;
    unsigned int admitted;
    unsigned int stopping;
    uint64_t wait_epoch;
};

/* Initialize a zeroed port on a registered ph_task after core image/TLS setup,
 * before Linux can allocate routes. Init creates the collector and its stacks;
 * no leaf callback allocates memory or calls Linux. Device FD is borrowed and
 * exclusively owned by this sandbox's device lifecycle. Route/control FDs are
 * private: never duplicate, transfer or close them outside this port.
 *
 * The cookie counter outlives replacement ports. All callbacks serialize queue
 * and native FD ownership with notifications masked. Collector doorbells enter
 * only through ph_task_ops.cpu_notify, never through a Linux handler directly.
 * Stop new callers before revoke/destroy. Revoke closes admission and masks
 * delivery, but is NOT hardware drain; stop the source and release all routes
 * before destroy. Failed release retains ownership and is fatal to Linux's
 * caller. Destroy joins the real collector before reclaiming its storage. */
int ph_irq_init(struct ph_irq *irq, const struct ph_irq_config *config);
int ph_irq_revoke(struct ph_irq *irq, uint64_t generation);
int ph_irq_destroy(struct ph_irq *irq);

#endif
