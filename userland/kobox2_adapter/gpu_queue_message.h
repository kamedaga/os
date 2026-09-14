/* SPDX-License-Identifier: MIT */
#ifndef PACHA_KOBOX_GPU_QUEUE_MESSAGE_H
#define PACHA_KOBOX_GPU_QUEUE_MESSAGE_H

/* Native doorbells only. Work identity and sizes come from the shared ring
 * and envelope, not the packet. BIND carries one VMO; NOTIFY carries no FDs.
 * value is channel_id for BIND, notification_id for NOTIFY; correlation is 0.
 */
enum {
    PH_GPU_QUEUE_BIND = 0x201,
    PH_GPU_QUEUE_NOTIFY = 0x202,
    PH_GPU_MAPPING_ATTACHMENT = 0x203,
    PH_GPU_MAPPING_RELEASE = 0x204,
    PH_GPU_MAPPING_RELEASED = 0x205,
    PH_GPU_DMA_BUF_ATTACHMENT = 0x206,
};

#endif
