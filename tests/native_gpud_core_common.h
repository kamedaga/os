/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_GPUD_CORE_COMMON_H
#define NATIVE_GPUD_CORE_COMMON_H

#include "../userland/kobox2_adapter/bootstrap.h"
#include "../userland/kobox2_adapter/device_authority.h"
#include "../userland/kobox2_adapter/sandbox_config.h"

#ifndef GPUD_CORE_TEST_DEVICE
#define GPUD_CORE_TEST_DEVICE 0
#endif
#define GPUD_CORE_TEST_ARTIFACTS (GPUD_CORE_TEST_DEVICE ? 12u : 2u)
#define GPUD_CORE_TEST_RESOURCES (GPUD_CORE_TEST_DEVICE ? 1u : 0u)
#define GPUD_CORE_TEST_BLOBS (2u + GPUD_CORE_TEST_ARTIFACTS)

/* Native Gate executable's fixed startup profile, not the DRM wire protocol.
 * The launcher maps this as a private read-only copy, before THREAD_START.
 * Neither expected identity nor counts are learned from channel messages. */
#define GPUD_CORE_TEST_CONFIG_ADDRESS PH_SANDBOX_CONFIG_ADDRESS
#define GPUD_CORE_TEST_CHANNEL_FD PH_SANDBOX_CHANNEL_FD

#endif
