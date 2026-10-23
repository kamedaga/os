/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../userland/personality/linux/runtime/lpr_fd/metadata.c"
static uint32_t expected;
int64_t lpr_fs_call(kb2_fs_request_t *r,const char *path,const char *second,
    const void *data,uint64_t n,void *out,uint64_t capacity,kb2_fs_response_t *reply,
    pacha_fd_t transfer,pacha_fd_t *returned)
{
    assert(r->opcode==KB2_FILESYSTEM_OP_STATX && r->mask==expected && !r->mode);
    assert(path && !second && !data && !n && out && capacity==KB2_FILESYSTEM_STATX_SIZE);
    assert(transfer==-1 && !returned);
    memset(out,0,capacity);*reply=(kb2_fs_response_t){.data_length=capacity};
    return 0;
}
int main(void)
{
    const uint32_t masks[]={0x7fffffff,0x40000000,0x400007ff,0x200007ff};
    uint8_t wire[KB2_FILESYSTEM_STATX_SIZE];
    for (unsigned i=0;i<sizeof(masks)/sizeof(masks[0]);++i) {
        expected=masks[i]&~UINT32_C(0x40000000);
        assert(!fs_stat(0,"/file",0,masks[i],wire));
        assert(!fs_stat(7,"",LPR_LINUX_AT_EMPTY_PATH,masks[i],wire));
    }
    puts("LPR common statx wire boundary: kernel-only mask cleared, unknown bits preserved, snapshot selector absent: PASS");
}
