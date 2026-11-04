#pragma once
#include <pacha/abi.h>
/* The trusted root handoff channel is retained after device handoff solely
 * for registration. A finite power wait drives this independent event loop
 * in both live and installed boots, even after seed0root exits headless. */
unsigned seed0_pressure_poll(pacha_fd_t registration);

void seed0_pressure_shutdown(void);
