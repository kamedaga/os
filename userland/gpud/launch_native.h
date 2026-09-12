/* SPDX-License-Identifier: MIT */
#ifndef PACHA_GPUD_LAUNCH_NATIVE_H
#define PACHA_GPUD_LAUNCH_NATIVE_H

#include "launch_image.h"
#include "process_native.h"
#include <pacha/ipc.h>

#define GPUD_LAUNCH_MAX_BLOBS 8u

/* Independent native startup configuration, copied into a child-only VMO.
 * Bootstrap resources themselves travel through the later IPC transaction. */
struct gpud_launch_blob {
    const void *bytes;
    size_t size;
    uint64_t address;
};

struct gpud_launch_request {
    uint64_t generation, load_bias, stack_address, stack_size;
    const void *image;
    size_t image_size;
    const struct gpud_launch_blob *blobs;
    size_t blob_count;
    const struct pacha_process_fd_grant *grants;
    size_t grant_count;
};

struct gpud_native_launch {
    struct gpud_native_process process;
    int thread_fd, scratch_fd;
    void *scratch_view;
    size_t scratch_size;
    unsigned int prepared, started, failed;
    int error;
};

/* Single-owner, initially zero. Validate ELF/configuration before creating any
 * native object. Input bytes/grants remain immutable and borrowed for this
 * call only; there is no reference to them after return. Segments are copied,
 * BSS is zeroed, and local writable views/FDs are closed BEFORE THREAD_START.
 * Every acquired handle/view is recorded, including on failure. A failure
 * after PROCESS_CREATE requires abort; do not zero/reuse an owned object.
 * All initial grants are explicit; this backend adds no ambient capabilities.
 * The caller supplies launch policy and trusted image identity separately. */
int gpud_native_launch_prepare(struct gpud_native_launch *launch,
    const struct gpud_launch_request *request);
/* Success means native thread START accepted, not package READY or core boot.
 * The custom freestanding entry receives an empty stack (no libc auxv/TLS). */
int gpud_native_launch_start(struct gpud_native_launch *launch, uint64_t generation);
/* Nonblocking failure cleanup before publishing LAUNCH/TRANSFERRING resources.
 * KILL/WAIT must prove process termination before handles are released.
 * After device resources have been delegated, the resource owner must prove
 * revoke/reset first and use discard at REAP instead of this abort shortcut. */
int gpud_native_launch_abort(struct gpud_native_launch *launch, uint64_t generation);
/* Only after terminal WAIT, at REAP after resource revoke/reset. Failed native
 * closes/unmaps retain inventory for retry. The generation watermark survives. */
int gpud_native_launch_discard(struct gpud_native_launch *launch, uint64_t generation);

#endif
