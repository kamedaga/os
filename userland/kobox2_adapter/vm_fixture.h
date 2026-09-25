/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef PACHA_KOBOX_VM_FIXTURE_H
#define PACHA_KOBOX_VM_FIXTURE_H

#include <stdint.h>

/* Trusted conformance programs only; not part of the VM host contract. */
uint64_t ph_vm_fixture_access(unsigned int operation, uint64_t base, uint64_t value);

#endif
