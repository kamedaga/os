#pragma once
#ifndef __ASSEMBLER__
#include <stdint.h>
struct unix_tx;
struct unix_rx;
void lpr_flow_record(unsigned op, uint64_t sender, const struct unix_tx *tx,
                     const struct unix_rx *rx, uint64_t detail);
#define LPR_FLOW(...) lpr_flow_record(__VA_ARGS__)
uint64_t lpr_flow_clock(void);
void lpr_drm_flow_record(uint64_t command, uint64_t file, uint64_t data,
                         uint64_t start, int64_t result);
#endif
