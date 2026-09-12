/* SPDX-License-Identifier: MIT */
#ifndef PACHA_KOBOX_EXEC_IMAGE_H
#define PACHA_KOBOX_EXEC_IMAGE_H

#include <stddef.h>
#include <stdint.h>

/* Immutable decoder provenance, borrowed by VMs only while the image runs. */
struct ph_exec_image {
    void *bytes;
    size_t length;
    uint64_t *syscall_sites;
    size_t site_count;
    unsigned elf_type;
};

struct ph_exec_image ph_exec_prepare_image(const void *file, size_t length);
void ph_exec_release_image(struct ph_exec_image *image);

#endif
