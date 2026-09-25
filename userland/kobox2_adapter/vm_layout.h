/* SPDX-License-Identifier: MIT */
#ifndef PACHA_KOBOX_VM_LAYOUT_H
#define PACHA_KOBOX_VM_LAYOUT_H

#include "arch/x86_64/user_layout.h"

/* Keep the complete native bootstrap outside Linux's ELF/mmap address space.
 * This header is also preprocessed into the native ELF linker script. */
#define PH_VM_CLIENT_IMAGE_BASE KOBOX_X86_MACHINE_CONTEXT
#define PH_VM_CLIENT_IMAGE_LIMIT (PH_VM_CLIENT_IMAGE_BASE + 0x01000000)
#define PH_VM_CLIENT_CONTROL PH_VM_CLIENT_IMAGE_LIMIT
#define PH_VM_CLIENT_STACK (PH_VM_CLIENT_IMAGE_BASE + 0x02000000)
#define PH_VM_CLIENT_STACK_SIZE (64 * 1024)

#endif
