/* SPDX-License-Identifier: MIT */
#pragma once
#include <pacha/abi.h>
pacha_fd_t lpr_input_wire_acquire(void **page);
void lpr_input_wire_release(void);
void lpr_input_wire_fork_lock(void);
void lpr_input_wire_fork_unlock(void);
void lpr_input_wire_after_fork_child(void);
