/* SPDX-License-Identifier: MIT */
/* Link guards for boot-only symbols referenced by the native trap stubs.
 * The waiter unit must never enter these hardware/exception paths. */
#include <stdint.h>

__attribute__((ms_abi)) void saveCurrentThreadXState(void) {
    __builtin_trap();
}

void restoreCurrentThreadXState(void) {
    __builtin_trap();
}

__attribute__((ms_abi)) void resumeAfterFatalUserException(
    uint32_t principal, uint8_t vector, void *frame) {
    (void)principal;
    (void)vector;
    (void)frame;
    __builtin_trap();
}
