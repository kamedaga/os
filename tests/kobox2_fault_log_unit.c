/* SPDX-License-Identifier: MIT */
#include "../userland/kobox2_adapter/boot.h"
#include <pacha/log.h>
#include <assert.h>
#include <string.h>

static char last[257];
static unsigned records;
int pacha_log_emit(pacha_fd_t fd, unsigned source, const char *text, size_t length) {
    assert(fd == 23 && source == PACHA_LOG_SOURCE_KOBOX2_GPU);
    assert(length <= 256);
    memcpy(last, text, length);
    last[length] = 0;
    ++records;
    return 0;
}
int main(void) {
    ph_boot_log_fault(14, 5, 1, 0x1234, 0x38);
    assert(records == 0);
    ph_boot_set_log_fd(23);
    ph_boot_log_fault(14, 5, 1, 0x1234, 0x38);
    assert(records == 1);
    assert(strstr(last, "vector=0xe"));
    assert(strstr(last, "error=0x5"));
    assert(strstr(last, "core_relative=0x1"));
    assert(strstr(last, "ip=0x1234"));
    assert(strstr(last, "address=0x38"));
    ph_boot_log_fatal(0x123, 66, (unsigned long)-12);
    assert(records == 2);
    assert(strstr(last, "source=0x123"));
    assert(strstr(last, "line=0x42"));
    assert(strstr(last, "result=0xfffffffffffffff4"));
    ph_boot_log_gpu_cache_transition(1, 0x4000000, 0x200000, 1, 4);
    assert(records == 3);
    assert(strcmp(last, "kobox-drm: cache transition stage=0x1 offset=0x4000000 bytes=0x200000 mode=0x1 status=0x4") == 0);
    assert(strlen(last) <= PACHA_LOG_FRAGMENT_BYTES * 8);
    ph_boot_log_gpu_page_view_failure(1, 24, 0x402047, 3, 512,
        0x4400, 0x4500, 0x45ff);
    assert(records == 4);
    assert(strstr(last, "status=0x1 parent_fd=0x18"));
    assert(strstr(last, "pages=0x200 first=0x4400 middle=0x4500 last=0x45ff"));
    ph_boot_log_gpu_bar_export_failure(2, 1, 6, 3, 2, 512,
        0xd1000, 0xe0000);
    assert(records == 5);
    assert(strstr(last, "BAR export refused reason=0x2 status=0x1 bar=0x6"));
    assert(strstr(last, "pages=0x200 first=0xd1000 last=0xe0000"));
    ph_boot_set_log_fd(0);
    ph_boot_log_fatal(1, 2, 3);
    assert(records == 5);
    return 0;
}
