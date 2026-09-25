/* SPDX-License-Identifier: MIT */
#ifndef PACHA_GPUD_GPU_LIMITS_H
#define PACHA_GPUD_GPU_LIMITS_H

/* Trusted launch policy shared by the frontend and sandbox. Xorg and a
 * multi-process browser together exceed sixteen simultaneously open files. */
enum { GPUD_GPU_NATIVE_SESSION_LIMIT = 32 };

#endif
