/* SPDX-License-Identifier: MIT */
#ifndef PACHA_USBD_MEDIA_PROBE_H
#define PACHA_USBD_MEDIA_PROBE_H

#include "../kobox2_adapter/block_client.h"

/* Read-only. A failed or ambiguous GPT must never designate an install
 * target, and the caller must not infer blank media from this result. */
int usbd_media_probe(struct ph_block_client *block, struct ph_ipc *ipc,
    int process_fd, const kb2_block_disk_t *disk, int *is_install_media);
int usbd_media_verify_blank(struct ph_block_client *block, struct ph_ipc *ipc,
    int process_fd, const kb2_block_disk_t *disk);

#endif
