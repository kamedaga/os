/* SPDX-License-Identifier: MIT */
#ifndef PACHA_LOG_H
#define PACHA_LOG_H

#include <stddef.h>
#include <stdint.h>
#include "pacha/abi.h"

/* Private userland IPC wire. It deliberately does not extend the kernel ABI. */
#define PACHA_LOG_WIRE_MAGIC UINT64_C(0x31474f4c48434150)
#define PACHA_LOG_FRAGMENT_BYTES 16u
#define PACHA_LOG_MAX_FRAGMENTS 16u
#define PACHA_LOG_MAX_RECORD_BYTES (PACHA_LOG_FRAGMENT_BYTES * PACHA_LOG_MAX_FRAGMENTS)

enum pacha_log_source {
    PACHA_LOG_SOURCE_NETD = 1,
    PACHA_LOG_SOURCE_KOBOX2_NET = 2,
    PACHA_LOG_SOURCE_GPUD = 3,
    PACHA_LOG_SOURCE_KOBOX2_GPU = 4,
};

/* SEND-only endpoint granted by the service launcher. No filesystem or
 * kernel log dependency is introduced in the producer. */
int pacha_log_emit(pacha_fd_t endpoint_fd, unsigned source, const char *text, size_t length);

#endif
