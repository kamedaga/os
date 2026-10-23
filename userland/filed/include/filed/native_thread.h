/* SPDX-License-Identifier: MIT */
#pragma once
#include <pacha/abi.h>

int filed_native_thread_start(void (*entry)(void *),void *context,
    pacha_fd_t *thread_fd);
