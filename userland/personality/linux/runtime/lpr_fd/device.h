#pragma once

#include "table.h"

/* Local character devices have no external service handle. A transferred
 * description still needs shared F_SETFL state and a capability lifetime;
 * an anonymous shared VMO supplies both without teaching unixd device IDs. */
enum { LPR_DEVICE_DESCRIPTION_BYTES = 4096u };

static inline int lpr_device_kind_supported(unsigned major, unsigned minor)
{
    return major == 1 && (minor == 3 || minor == 5 || minor == 8 || minor == 9);
}

int lpr_device_description_ensure(lpr_device_backend_t *device);
int lpr_device_description_ensure_locked(lpr_device_backend_t *device);
int lpr_device_description_map(lpr_device_backend_t *device);
int64_t lpr_device_description_unmap(lpr_device_backend_t *device);
int64_t lpr_device_description_close(lpr_device_backend_t *device);
uint32_t lpr_device_description_flags(const lpr_device_backend_t *device);
void lpr_device_description_set_flags(lpr_device_backend_t *device, uint32_t flags);
int lpr_device_description_capability_valid(pacha_fd_t fd);
