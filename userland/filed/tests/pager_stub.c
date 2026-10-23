/* SPDX-License-Identifier: MIT */
#include "filed/pager.h"
#include <stdlib.h>

/* Unit fixtures which compile relay/cache without the worker have only
 * eager FILE_VMO requests. An accidental pager request is a test failure. */
int filed_pager_create(struct filed_runtime *rt,uint64_t handle,
    uint64_t offset,uint64_t length,uint64_t rx_length,int rx,
    const filed_storage_stat_t *stat,
    uint64_t rights,pacha_fd_t *fd,struct filed_pager_record **record)
{
    (void)rt;(void)handle;(void)offset;(void)length;(void)rx_length;(void)rx;(void)stat;
    (void)rights;(void)fd;(void)record;
    abort();
}
uint64_t filed_pager_resident_bytes(const struct filed_pager_record *record)
{ (void)record;abort(); }
