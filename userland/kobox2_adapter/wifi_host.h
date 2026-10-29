/* SPDX-License-Identifier: MIT */
#ifndef PH_WIFI_HOST_H
#define PH_WIFI_HOST_H
#include "boot/user_process.h"
/* Native memory/VM setup happens before entering Linux. Linux receives no
 * device authority in the child: only the isolated owning sandbox has it. */
void ph_wifi_host_prepare(void);
const struct kobox_linux_user_process_config *ph_wifi_host_config(void);
/* Only after Linux stopped/destroyed the process and retired its binding. */
void ph_wifi_host_finish(void);
#endif
