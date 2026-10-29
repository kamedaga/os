/* SPDX-License-Identifier: MIT */
#ifndef USBD_DIAGNOSTIC_H
#define USBD_DIAGNOSTIC_H
#include <pacha/ipc.h>
#include <usbd/boot_config.h>
#include <pacha/log.h>

struct usbd_log_pending {
    uint32_t record;
    uint16_t received;
    unsigned count, last_bytes;
    char text[PACHA_LOG_MAX_RECORD_BYTES];
};
struct usbd_diagnostic {
    pacha_fd_t endpoint;
    struct usbd_log_pending pending[16];
    unsigned next_slot;
    char text[32768];
    size_t used;
    int dirty;
};
int usbd_diagnostic_open(struct usbd_diagnostic *log, uint64_t generation);
void usbd_diagnostic_drain(struct usbd_diagnostic *log);
int usbd_diagnostic_publish(struct usbd_diagnostic *log,
    const struct usbd_boot_config *config);
int usbd_diagnostic_close(struct usbd_diagnostic *log);
/* Generation-scoped, explicit reload request. Never automatically loops a
 * failing driver or reuses DMA resources before controller cleanup. */
int usbd_reload_requested(const struct usbd_boot_config *config, uint64_t generation);
#endif
