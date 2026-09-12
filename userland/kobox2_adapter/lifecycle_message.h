/* SPDX-License-Identifier: MIT */
#ifndef PACHA_KOBOX_LIFECYCLE_MESSAGE_H
#define PACHA_KOBOX_LIFECYCLE_MESSAGE_H

/* Native management messages after bootstrap FINISH, not GPU queue opcodes.
 * All carry the launch generation, value=0, and no ancillary capabilities.
 * READY has correlation=0; QUIESCE carries the opaque controller action token.
 * READY means the selected module closure initialized, not DRM publication.
 * There is no "stopped" acknowledgement: the owner must observe PROCESS_WAIT
 * before completing QUIESCE, then separately revoke/reset device resources. */
enum ph_lifecycle_operation {
    PH_LIFECYCLE_READY = 0x100,
    PH_LIFECYCLE_QUIESCE = 0x101,
};

#endif
