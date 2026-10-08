/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>
struct lpr_linux_user_frame { uint64_t marker; };
#include "../userland/personality/linux/runtime/lpr_frame_anchor.h"

int main(void)
{
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    unsigned char *memory = mmap(NULL, page * 2, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(memory != MAP_FAILED);
    assert(mprotect(memory + page, page, PROT_NONE) == 0);
    uintptr_t *head = (uintptr_t *)memory;
    /* Reproduce QEMU's candidate five bytes before an unmapped guard page.
     * Reading its first uint64_t without the alignment check faults. */
    *head = (uintptr_t)(memory + page - 5);
    assert(lpr_frame_anchor_find((uintptr_t)head) == NULL);
    assert(lpr_frame_anchor_find((uintptr_t)head + 1) == NULL);
    assert(lpr_frame_anchor_find(0) == NULL);
    *head = (uintptr_t)head;
    assert(lpr_frame_anchor_find((uintptr_t)head) == NULL);
    *head = (uintptr_t)head + 1024u * 1024u + 8u;
    assert(lpr_frame_anchor_find((uintptr_t)head) == NULL);
    struct lpr_linux_user_frame frame = {.marker = 42};
    uint64_t *anchor = (uint64_t *)(memory + 64);
    *head = (uintptr_t)anchor;
    anchor[0] = UINT64_C(0x4c50524652414d45);
    anchor[1] = (uintptr_t)&frame;
    anchor[2] = UINT64_C(0x454d41524652504c);
    assert(lpr_frame_anchor_find((uintptr_t)head) == &frame);
    anchor[2] = 0;
    assert(lpr_frame_anchor_find((uintptr_t)head) == NULL);
    assert(munmap(memory, page * 2) == 0);
    puts("lpr frame anchor: PASS");
    return 0;
}
