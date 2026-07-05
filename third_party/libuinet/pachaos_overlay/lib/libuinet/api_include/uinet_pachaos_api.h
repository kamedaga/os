#ifndef _UINET_PACHAOS_API_H_
#define _UINET_PACHAOS_API_H_

#include "uinet_api.h"

typedef int (*uinet_pachaos_tx_func_t)(void *arg, const void *frame, size_t frame_len);

int uinet_pachaos_if_register_tx(uinet_if_t uif, uinet_pachaos_tx_func_t tx, void *arg);
int uinet_pachaos_if_deliver(uinet_if_t uif, const void *frame, size_t frame_len);

#endif
