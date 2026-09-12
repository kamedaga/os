/* SPDX-License-Identifier: MIT */
#ifndef PACHA_KOBOX_GPU_QUERY_MESSAGE_H
#define PACHA_KOBOX_GPU_QUERY_MESSAGE_H

/* Native capability transport for a pre-bound read-only render query session.
 * The VMO carries standard GPU protocol envelopes/commands/completions, not
 * Linux ioctl structures. It is staging, not a hardware DMA mapping.
 * Request and response share correlation/generation; value is envelope size.
 * One borrowed VMO per request, no ancillary GPU object exchanges yet.
 */
#define PH_GPU_QUERY_OPERATION 0x200u
#define PH_GPU_QUERY_PAGE 4096u
#define PH_GPU_QUERY_VMO_SIZE (3u * PH_GPU_QUERY_PAGE)
#define PH_GPU_QUERY_OUTPUT_OFFSET PH_GPU_QUERY_PAGE
#define PH_GPU_QUERY_REPLY_OFFSET (2u * PH_GPU_QUERY_PAGE)
#define PH_GPU_QUERY_OUTPUT_REGION 1u
#define PH_GPU_QUERY_RIGHTS (PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE | \
    PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE)

#endif
