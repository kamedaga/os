/* SPDX-License-Identifier: MIT */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* Sorted, disjoint [start,end) file-offset ranges of executable PT_LOAD bytes
 * covered by .eh_frame FDEs, i.e. compiled or CFI-annotated functions. */
typedef struct filed_rx_code_ranges {
    uint64_t *starts,*ends;
    size_t count;
} filed_rx_code_ranges_t;
typedef int (*filed_rx_read_fn)(void *context,uint64_t file_offset,
    uint8_t *destination,size_t length);

void filed_rx_code_ranges_free(filed_rx_code_ranges_t *);
/* Locate .eh_frame through PT_GNU_EH_FRAME and collect FDE ranges. Fails with
 * -ENOENT when the image has none, -ENOEXEC for any record it cannot parse. */
int filed_rx_code_ranges_load(filed_rx_read_fn read,void *context,
    uint64_t file_size,filed_rx_code_ranges_t *out);
/* Nonzero when [file_offset,file_offset+2) intersects no range. */
int filed_rx_code_ranges_outside(const filed_rx_code_ranges_t *,uint64_t file_offset);
