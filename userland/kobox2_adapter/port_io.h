/* SPDX-License-Identifier: MIT */
#ifndef PACHA_KOBOX_PORT_IO_H
#define PACHA_KOBOX_PORT_IO_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef long (*ph_port_io_fn)(void *, uint16_t, bool, uint8_t);

/* Only unprefixed scalar byte IN/OUT via DX. The host capability remains
 * the authority for the actual port/direction. Neither a rejected operation
 * nor an unsupported opcode may manufacture data or advance execution. */
static inline bool ph_scalar_port_io(uint8_t opcode, uint64_t vector,
    uint64_t error, uint64_t *ax, uint64_t dx, uint64_t *ip,
    ph_port_io_fn operation, void *context) {
    if (!operation || vector != 13 || error || !ax || !ip || *ip == UINT64_MAX ||
        (opcode != 0xec && opcode != 0xee)) return false;
    const bool writing = opcode == 0xee;
    const long result = operation(context, (uint16_t)dx, writing, (uint8_t)*ax);
    if (result < 0 || (writing ? result != 0 : result > UINT8_MAX)) return false;
    if (!writing) *ax = (*ax & ~UINT64_C(255)) | (uint8_t)result;
    ++*ip;
    return true;
}

#endif
