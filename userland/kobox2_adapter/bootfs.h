/* SPDX-License-Identifier: MIT */
#ifndef PACHA_KOBOX_BOOTFS_H
#define PACHA_KOBOX_BOOTFS_H

#include <stddef.h>

/* The returned ELF borrows storage from the boot archive for process lifetime. */
const void *ph_bootfs_core(size_t *size_out);
const void *ph_bootfs_file(const char *path, size_t *size_out);

#endif
