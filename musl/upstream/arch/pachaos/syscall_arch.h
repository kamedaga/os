/* SPDX-License-Identifier: MIT */
/* The PachaOS syscall ABI has one definition, kept in sync with the kernel by
 * tests/check_native_syscall_numbers.py. This arch slot only forwards to it:
 * a second copy here drifted (wrong numbers, an obsolete filed client) while
 * every build already resolved musl/pachaos/syscall first. */
#include "../../../pachaos/syscall/syscall_arch.h"
