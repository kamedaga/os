/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#define PH_ARENA_LAZY 1
#define PH_SANDBOX_FS 1
#include "../userland/kobox2_adapter/image.c"
#include "../kobox2/linux-sandbox/kobox/memory/reporting_ranges.h"

/* Actual adapter -> libipc wrapper -> native syscall boundary. This fixture
 * models view-retained pages and holes, not a native-kernel execution test.
 */
static unsigned char resident[10000];
static uint64_t view_page = UINT64_MAX;
static unsigned calls, fail_call;
static int failure;
static uint64_t submitted;
long pacha_syscall4(uint64_t nr, uint64_t fd, uint64_t ptr, uint64_t count, uint64_t flags)
{
    assert(nr == PACHA_FD_SYSCALL_VMO_DISCARD && fd == 42 && flags == 0);
    assert(count && count <= 32);
    const struct pacha_vmo_discard_range *ranges = (const void *)(uintptr_t)ptr;
    uint64_t total = 0, end = 0;
    for (size_t i = 0; i < count; i++) {
        assert(ranges[i].page_count && ranges[i].first_page >= end);
        end = ranges[i].first_page + ranges[i].page_count;
        assert(end <= sizeof(resident));
        total += ranges[i].page_count;
    }
    assert(total <= 1024);
    calls++;
    if (calls == fail_call) return failure;
    submitted += total;
    int status = 0;
    for (size_t i = 0; i < count; i++) {
        uint64_t first = ranges[i].first_page, end_page = first + ranges[i].page_count;
        if (view_page >= first && view_page < end_page) {
            status = PACHA_ERR_NOT_READY;
            continue;
        }
        for (uint64_t j = first; j < end_page; j++) resident[j] = 0;
    }
    return status;
}

static struct ph_image image = {.ram_fd = 42, .ram_size = sizeof(resident) * PH_PAGE_SIZE};
static void reset(void) {
    memset(resident, 1, sizeof(resident));
    calls = fail_call = 0;
    submitted = 0;
    view_page = UINT64_MAX;
}
int main(void) {
    struct kobox_linux_memory_range out;
    assert(!kobox_reporting_convert((struct kobox_reporting_segment){0,0,4096},100,&out));
    assert(out.first_page == 0 && out.page_count == 1);
    assert(!kobox_reporting_convert((struct kobox_reporting_segment){98,4096,4096},100,&out));
    assert(out.first_page == 99 && out.page_count == 1);
    assert(kobox_reporting_convert((struct kobox_reporting_segment){100,0,4096},100,&out));
    assert(kobox_reporting_convert((struct kobox_reporting_segment){99,0,8192},100,&out));
    assert(kobox_reporting_convert((struct kobox_reporting_segment){99,4096,4096},100,&out));
    assert(kobox_reporting_convert((struct kobox_reporting_segment){UINT64_MAX,4096,4096},100,&out));
    assert(kobox_reporting_convert((struct kobox_reporting_segment){0,1,4096},100,&out));
    assert(kobox_reporting_convert((struct kobox_reporting_segment){0,0,1},100,&out));
    assert(kobox_reporting_convert((struct kobox_reporting_segment){0,0,0},100,&out));
    struct kobox_linux_memory_range ranges[33];
    reset();
    ranges[0] = (struct kobox_linux_memory_range){4000,2050};
    assert(!kobox_reporting_submit(ranges,1,memory_discard,&image));
    assert(calls == 3 && submitted == 2050 && resident[3999] && !resident[6049] && resident[6050]);
    reset();
    for (size_t i = 0; i < 32; i++) ranges[i] = (struct kobox_linux_memory_range){(31-i)*2,1};
    assert(!kobox_reporting_submit(ranges,32,memory_discard,&image));
    assert(calls == 1 && submitted == 32);
    for (size_t i = 0; i < 64; i++) assert(resident[i] == (i % 2));
    assert(!kobox_reporting_submit(ranges,32,memory_discard,&image)); /* existing holes */
    assert(calls == 2);
    assert(kobox_reporting_submit(ranges,33,memory_discard,&image));
    assert(calls == 2);
    reset();
    ranges[0] = (struct kobox_linux_memory_range){100,1024};
    ranges[1] = (struct kobox_linux_memory_range){2000,1};
    assert(!kobox_reporting_submit(ranges,2,memory_discard,&image));
    assert(calls == 2 && submitted == 1025);
    reset();
    ranges[0] = (struct kobox_linux_memory_range){20,1};
    ranges[1] = (struct kobox_linux_memory_range){10,1};
    view_page = 20;
    assert(kobox_reporting_submit(ranges,2,memory_discard,&image));
    assert(calls == 1 && !resident[10] && resident[20]);
    view_page = UINT64_MAX;
    assert(!kobox_reporting_submit(ranges,2,memory_discard,&image));
    assert(!resident[20]);
    for (int status = 1; status <= 6; status++) {
        reset();
        ranges[0] = (struct kobox_linux_memory_range){100,2050};
        failure = status; fail_call = 2;
        assert(kobox_reporting_submit(ranges,1,memory_discard,&image));
        assert(calls == 2 && !resident[100] && resident[1124]);
        fail_call = 0;
        assert(!kobox_reporting_submit(ranges,1,memory_discard,&image));
        assert(!resident[2149]);
    }
    reset();
    ranges[0] = (struct kobox_linux_memory_range){100,2050};
    failure = PACHA_ERR_NOT_READY; fail_call = 3; /* final short batch */
    assert(kobox_reporting_submit(ranges,1,memory_discard,&image));
    assert(calls == 3 && !resident[2147] && resident[2148]);
    reset();
    ranges[0] = (struct kobox_linux_memory_range){100,2};
    ranges[1] = (struct kobox_linux_memory_range){101,1};
    assert(kobox_reporting_submit(ranges,2,memory_discard,&image));
    assert(calls == 0);
    ranges[0] = (struct kobox_linux_memory_range){UINT64_MAX,2};
    assert(kobox_reporting_submit(ranges,1,memory_discard,&image));
    assert(calls == 0);
    ranges[0] = (struct kobox_linux_memory_range){9999,2};
    assert(memory_discard(&image,ranges,1));
    ranges[0] = (struct kobox_linux_memory_range){0,1025};
    assert(memory_discard(&image,ranges,1));
    assert(calls == 0);
    puts("page reporting: conversion, gaps/holes, batching, view NOT_READY and failure retry OK");
}
