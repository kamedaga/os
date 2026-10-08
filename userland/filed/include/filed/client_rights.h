/* SPDX-License-Identifier: MIT */
#pragma once
#include <pacha/abi.h>

/* The supervisor retains only duplication/transfer authority in addition to
 * the callable client endpoint. IPC transfer sets CLOEXEC; neither holder
 * needs mutable descriptor flags or receive authority over filed's server.
 * Native policy stays separate from the payload/wire headers consumed by
 * the isolated libc build, which does not depend on native syscall headers. */
#define FILED_CLIENT_CHANNEL_RIGHTS (PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE | \
    PACHA_FD_RIGHT_CALL | PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL)
#define FILED_CLIENT_REGISTER_RIGHTS (FILED_CLIENT_CHANNEL_RIGHTS | \
    PACHA_FD_RIGHT_DUP | PACHA_FD_RIGHT_TRANSFER)
