/* SPDX-License-Identifier: MIT */
#include "filed/runtime.h"
#include "filed/storage_owner.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int linux_result;
static uint64_t mount_flags;
static int bad_size;
static unsigned ordinary_calls,owner_calls;
static size_t read_budget=20000;
static int read_error;
static unsigned read_calls;
static int exchange(struct filed_storage_owner *owner,const uint8_t *input,
    size_t size,uint8_t *output,size_t capacity,size_t *used,int control)
{
    assert(owner);
    kb2_fs_request_t request;
    assert(!kb2_fs_request_decode(input,size,size,&request));
    assert(request.client==9 && request.credential_generation==7 && request.handle==42);
    kb2_fs_response_t response={.opcode=request.opcode,.generation=request.generation,
        .sequence=request.sequence,.result=linux_result};
    if(request.opcode==KB2_FILESYSTEM_OP_EXEC_OPEN) {
        assert(control);
        assert(!request.mode && !request.flags);
        assert(!strcmp((const char *)input+request.path.offset,"/exec-only"));
        if(!linux_result) response.handle=100;
    } else if(request.opcode==KB2_FILESYSTEM_OP_CLOSE) {
        assert(control);
    } else if(request.opcode==KB2_FILESYSTEM_OP_PREAD) {
        assert(request.length<=read_budget && capacity==KB2_FILESYSTEM_RESPONSE_SIZE+request.length);
        ++read_calls;
        response.result=read_error ? read_error : (int64_t)request.length;
        response.data_length=read_error ? 0 : request.length;
        for(size_t i=0;i<response.data_length;++i)
            output[KB2_FILESYSTEM_RESPONSE_SIZE+i]=(uint8_t)(request.offset+i);
    } else if(request.opcode==KB2_FILESYSTEM_OP_STATX) {
        assert(control);
        response.data_length=KB2_FILESYSTEM_STATX_SIZE;
        memset(output+KB2_FILESYSTEM_RESPONSE_SIZE,0,KB2_FILESYSTEM_STATX_SIZE);
        output[KB2_FILESYSTEM_RESPONSE_SIZE+KB2_FILESYSTEM_STATX_SIZE_OFFSET]=99;
    } else {
        assert(!control);
        assert(request.opcode==KB2_FILESYSTEM_OP_STATFS);
        if(!linux_result) {
            assert(capacity>=KB2_FILESYSTEM_RESPONSE_SIZE+KB2_FILESYSTEM_STATFS_SIZE);
            response.data_length=bad_size ? 0 : KB2_FILESYSTEM_STATFS_SIZE;
            memset(output+KB2_FILESYSTEM_RESPONSE_SIZE,0,KB2_FILESYSTEM_STATFS_SIZE);
            for(unsigned i=0;i<8;++i)
                output[KB2_FILESYSTEM_RESPONSE_SIZE+KB2_FILESYSTEM_STATFS_FLAGS_OFFSET+i]=
                    (uint8_t)(mount_flags>>(8*i));
        }
    }
    assert(!kb2_fs_response_encode(output,capacity,&response));
    *used=KB2_FILESYSTEM_RESPONSE_SIZE+(size_t)response.data_length;return 0;
}
int filed_storage_owner_call(struct filed_storage_owner *owner,const uint8_t *input,
    size_t size,uint8_t *output,size_t capacity,size_t *used)
{
    ++ordinary_calls;
    return exchange(owner,input,size,output,capacity,used,0);
}
int filed_storage_owner_control_call(struct filed_storage_owner *owner,
    const uint8_t *input,size_t size,uint8_t *output,size_t capacity,size_t *used)
{
    ++owner_calls;
    return exchange(owner,input,size,output,capacity,used,1);
}
static int exchange_split(struct filed_storage_owner *owner,const uint8_t *input,
    size_t size,kb2_fs_response_t *response,void *data,size_t capacity,
    size_t *used,int control)
{
    uint8_t *wire=malloc(KB2_FILESYSTEM_RESPONSE_SIZE+capacity);assert(wire);
    int status=exchange(owner,input,size,wire,KB2_FILESYSTEM_RESPONSE_SIZE+capacity,
        used,control);
    if(!status) status=kb2_fs_response_decode(wire,*used,response);
    if(!status && response->data_length) memcpy(data,wire+KB2_FILESYSTEM_RESPONSE_SIZE,
        (size_t)response->data_length);
    free(wire);return status;
}
int filed_storage_owner_call_split(struct filed_storage_owner *owner,
    const uint8_t *input,size_t size,kb2_fs_response_t *response,
    void *data,size_t capacity,size_t *used)
{
    ++ordinary_calls;return exchange_split(owner,input,size,response,data,capacity,used,0);
}
int filed_storage_owner_control_call_split(struct filed_storage_owner *owner,
    const uint8_t *input,size_t size,kb2_fs_response_t *response,
    void *data,size_t capacity,size_t *used)
{
    ++owner_calls;return exchange_split(owner,input,size,response,data,capacity,used,1);
}
int main(void)
{
    struct filed_client client={.id=9,.credential_generation=7};
    filed_runtime_t runtime={.actor=&client,.storage_generation=1,
        .storage=(struct filed_storage_owner *)&client};
    uint64_t handle=0;
    assert(!filed_storage_exec_open(&runtime,42,"/exec-only",&handle) && handle==100);
    linux_result=-EACCES;assert(filed_storage_exec_open(&runtime,42,"/exec-only",&handle)==-EACCES);
    linux_result=-EBADF;assert(filed_storage_exec_open(&runtime,42,"/exec-only",&handle)==-EBADF);
    linux_result=0;assert(!filed_storage_executable_mapping(&runtime,42));
    mount_flags=8;assert(filed_storage_executable_mapping(&runtime,42)==-EACCES);
    mount_flags=1|2|4;assert(!filed_storage_executable_mapping(&runtime,42));
    bad_size=1;assert(filed_storage_executable_mapping(&runtime,42)==-EPROTO);
    bad_size=0;linux_result=-EBADF;
    assert(filed_storage_executable_mapping(&runtime,42)==-EBADF);
    assert(owner_calls==3 && ordinary_calls==5);
    linux_result=0;
    uint8_t data[20000];size_t got=0;
    runtime.bootstrap.queue_arena_bytes=4096;
    read_budget=4096-KB2_FILESYSTEM_REQUEST_SIZE-KB2_FILESYSTEM_RESPONSE_SIZE;
    for(int owner=0;owner<2;++owner) {
        memset(data,0,sizeof(data));
        size_t done=0;
        while(done<sizeof(data)) {
            int status=owner ? filed_storage_exec_pread(&runtime,42,done,
                data+done,sizeof(data)-done,&got) : filed_storage_pread(&runtime,42,
                done,data+done,sizeof(data)-done,&got);
            assert(!status && got && got<=read_budget);
            done+=got;
        }
        for(size_t i=0;i<sizeof(data);++i) assert(data[i]==(uint8_t)i);
    }
    assert(read_calls==12 && owner_calls==9 && ordinary_calls==11);
    read_error=-EIO;got=123;
    assert(filed_storage_exec_pread(&runtime,42,0,data,sizeof(data),&got)==-EIO && got==123);
    read_error=0;
    runtime.bootstrap.queue_arena_bytes=KB2_FILESYSTEM_REQUEST_SIZE+KB2_FILESYSTEM_RESPONSE_SIZE;
    assert(filed_storage_pread(&runtime,42,0,data,sizeof(data),&got)==-EINVAL);
    runtime.bootstrap.queue_arena_bytes=4096;
    filed_storage_stat_t stat;
    assert(!filed_storage_exec_stat(&runtime,42,&stat) && stat.size==99);
    assert(!filed_storage_exec_close(&runtime,42));
    assert(owner_calls==12 && ordinary_calls==11);
    puts("filed private EXEC_OPEN owner RPC / executable mmap noexec policy: PASS");
}
