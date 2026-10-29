/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "filed/runtime.h"
#include "filed/storage_owner.h"
#include "../../kobox2_owner/bootfs.h"
static struct ph_bootfs archive;
static unsigned opened, closed, writes, short_write, mkdir_fail;
static unsigned char expected[130123], copied[130123];
static size_t copied_size;
const struct ph_bootfs *filed_storage_owner_archive(const struct filed_storage_owner *owner)
{ return owner ? &archive : NULL; }
int filed_storage_pager_request_ex(struct filed_runtime *rt,uint64_t client,uint64_t cred,
    kb2_fs_request_t *request,const void *tail,size_t length,kb2_fs_response_t *response,void *data,size_t capacity)
{
    (void)rt; assert(client==1 && cred==1 && !data && !capacity);
    *response=(kb2_fs_response_t){0};
    if (request->opcode==KB2_FILESYSTEM_OP_MKDIRAT) {
        assert(((const char *)tail)[length-1]==0);
        return mkdir_fail ? -EACCES : -EEXIST;
    }
    if (request->opcode==KB2_FILESYSTEM_OP_FCHMODAT) {
        if (!strcmp(tail,"/tmp") || !strcmp(tail,"/dev/shm")) assert(request->mode==01777);
        return 0;
    }
    if (request->opcode==KB2_FILESYSTEM_OP_OPENAT2) {
        assert(!strcmp(tail,"/usr/bin/demo") && request->mode==0755);
        assert((request->flags&(O_CREAT|O_EXCL))==(O_CREAT|O_EXCL));
        response->handle=++opened;return 0;
    }
    assert(request->opcode==KB2_FILESYSTEM_OP_PWRITE && request->handle==1);
    assert(length<=64*1024 && request->offset==(int64_t)copied_size);
    assert(request->length==length && !memcmp(tail,expected+copied_size,length));
    memcpy(copied+copied_size,tail,length);copied_size+=length;writes++;
    response->result=(int64_t)length-(short_write ? 1 : 0);return 0;
}
int filed_storage_pager_request(struct filed_runtime *rt,uint64_t client,uint64_t cred,
    kb2_fs_request_t *request,kb2_fs_response_t *response,void *data,size_t capacity)
{
    (void)rt;(void)response;assert(client==1 && cred==1 && !data && !capacity);
    assert(request->opcode==KB2_FILESYSTEM_OP_CLOSE && request->handle==1);
    closed++;return 0;
}
static void put(unsigned char *p,uint64_t value,unsigned n)
{ for(unsigned i=0;i<n;i++)p[i]=(unsigned char)(value>>(8*i)); }
int main(void)
{
    static const char name[]="/usr/bin/demo";
    const size_t data_offset=176,total=data_offset+sizeof(expected);
    unsigned char *image=calloc(1,total);assert(image);
    put(image,0x53465442,4);put(image+4,104,4);put(image+8,total,8);
    put(image+16,1,4);put(image+20,48,4);put(image+24,104,8);
    put(image+32,152,8);put(image+40,sizeof(name),8);
    put(image+48,data_offset,8);put(image+56,sizeof(expected),8);
    put(image+104+4,sizeof(name)-1,2);image[104+6]=1;
    put(image+104+8,data_offset,8);put(image+104+16,sizeof(expected),8);
    put(image+104+24,0755,4);memcpy(image+152,name,sizeof(name));
    for(size_t i=0;i<sizeof(expected);i++)expected[i]=(unsigned char)(i*17);
    memcpy(image+data_offset,expected,sizeof(expected));
    assert(!ph_bootfs_view(&archive,image,total));
    filed_runtime_t rt={.live_root=1,.storage=(void *)1};
    assert(!filed_live_root_import(&rt));
    assert(opened==1 && closed==1 && writes==2 && copied_size==sizeof(expected));
    assert(!memcmp(expected,copied,sizeof(expected)));
    opened=closed=writes=0;copied_size=0;short_write=1;
    assert(filed_live_root_import(&rt)==-EIO && opened==1 && closed==1 && writes==1);
    short_write=0;mkdir_fail=1;opened=closed=writes=0;copied_size=0;
    assert(filed_live_root_import(&rt)==-EACCES && !opened && !closed && !writes);
    rt.live_root=0;assert(filed_live_root_import(&rt)==-EINVAL);
    free(image);puts("RAM-root archive import: exact bytes/mode, bounded writes, close on failure and no partial readiness PASS");
}
