/* SPDX-License-Identifier: MIT */
#include "../userland/kobox2_adapter/boot.h"
#include <pacha/log.h>
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static unsigned calls;
static int send_error;
static char records[2][PACHA_LOG_MAX_RECORD_BYTES + 1];

int pacha_log_emit(pacha_fd_t fd, unsigned source, const char *text, size_t size)
{
    assert(fd == 17 && source == PACHA_LOG_SOURCE_KOBOX2_NET);
    assert(calls < 2 && size > 0 && size <= PACHA_LOG_MAX_RECORD_BYTES);
    memcpy(records[calls],text,size); records[calls][size] = 0;
    ++calls;
    return send_error;
}

int main(void)
{
    ph_boot_set_log_fd(0);
    ph_boot_log_dma_failure(0,3,0x1000000,4096,2,8192,0,4097,4000);
    assert(calls == 0);
    ph_boot_set_log_fd(17);
    ph_boot_log_dma_failure(0,3,0x1000000,4096,2,8192,0,4097,4000);
    assert(calls == 2);
    assert(strstr(records[0],"stage=0x0 native=0x3"));
    assert(strstr(records[0],"bytes=0x1000 mappings=0x2"));
    assert(strstr(records[1],"fd_capacity=0x1001 fd_free=0xfa0"));
    calls = 0;
    ph_boot_log_dma_failure(ULONG_MAX,-1,ULONG_MAX,ULONG_MAX,ULONG_MAX,
        ULONG_MAX,-1,ULONG_MAX,ULONG_MAX);
    assert(calls == 2); /* Maximum values must not silently drop a record. */
    calls = 0; send_error = -EAGAIN;
    ph_boot_log_dma_failure(0,3,0x1000000,4096,2,8192,0,4097,4000);
    assert(calls == 2); /* Diagnostic backpressure must not retry/spin. */
    puts("DMA RAM diagnostics: fields, bounded records, disabled sink and backpressure PASS");
    return 0;
}
