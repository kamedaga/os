/* SPDX-License-Identifier: MIT */
#include "filed/storage.h"
#include "filed/runtime.h"
#include "filed/storage_owner.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef FILED_INTERNAL_MICROBENCH
#include "../tests/internal_microbench_clock.h"
#endif

static uint64_t rd(const uint8_t *p, unsigned width)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < width; ++i) value |= (uint64_t)p[i] << (8 * i);
    return value;
}

int filed_storage_exchange(filed_runtime_t *rt, const uint8_t *request,
                           size_t size, uint8_t *reply, size_t capacity,
                           size_t *used)
{
    if (!rt || !rt->storage || !request || !reply || !used) return -22;
#ifdef FILED_INTERNAL_MICROBENCH
    uint64_t start = filed_bench_collect ? filed_bench_tsc() : 0;
#endif
    filed_mutex_lock(&rt->storage_exchange_lock);
    int status = filed_storage_owner_call(rt->storage, request, size, reply,
                                         capacity, used);
    filed_mutex_unlock(&rt->storage_exchange_lock);
#ifdef FILED_INTERNAL_MICROBENCH
    if (filed_bench_collect) {
        filed_bench_exchange_cycles += filed_bench_tsc() - start;
        ++filed_bench_exchange_count;
    }
#endif
    return status;
}

static int storage_request(filed_runtime_t *rt, kb2_fs_request_t *r,
                          const void *tail, size_t tail_size,
                          kb2_fs_response_t *response,
                          void *data, size_t capacity,int owner,
                          uint64_t pager_client, uint64_t pager_credential)
{
    if (!rt || !r || !response || (tail_size && !tail) ||
        (capacity && !data) ||
        tail_size > SIZE_MAX - KB2_FILESYSTEM_REQUEST_SIZE ||
        capacity > SIZE_MAX - KB2_FILESYSTEM_RESPONSE_SIZE) return -22;
    size_t request_size = KB2_FILESYSTEM_REQUEST_SIZE + tail_size;
    size_t reply_size = KB2_FILESYSTEM_RESPONSE_SIZE + capacity;
#ifdef FILED_INTERNAL_MICROBENCH
    uint64_t bench_stage=filed_bench_collect ? filed_bench_tsc() : 0;
#endif
    uint8_t *request = malloc(request_size), *reply = malloc(reply_size);
    if (!request || !reply) { free(request); free(reply); return -12; }
    r->generation = rt->storage_generation;
    if (rt->request_sequence == UINT64_MAX) { free(request); free(reply); return -75; }
    r->sequence = __atomic_add_fetch(&rt->request_sequence,1,__ATOMIC_RELAXED);
    r->client = pager_client ? pager_client : (rt->actor ? rt->actor->id : 1);
    r->credential_generation = pager_client ? pager_credential :
        (rt->actor ? rt->actor->credential_generation : 1);
    int status = kb2_fs_request_encode(request, KB2_FILESYSTEM_REQUEST_SIZE,
                                      request_size, r);
    if (!status && tail_size) memcpy(request + KB2_FILESYSTEM_REQUEST_SIZE, tail, tail_size);
    size_t used = 0;
#ifdef FILED_INTERNAL_MICROBENCH
    FILED_BENCH_STAGE(filed_bench_storage_cycles,0,bench_stage);
    /* Diagnostic control: move first writes out of the response memcpy while
     * retaining their cost in the enclosing operation, rather than hiding it. */
    if(filed_bench_collect && filed_bench_prefault && reply_size>4096) {
        for(size_t i=0;i<reply_size;i+=4096) ((volatile uint8_t *)reply)[i]=0;
        ((volatile uint8_t *)reply)[reply_size-1]=0;
        FILED_BENCH_STAGE(filed_bench_storage_cycles,3,bench_stage);
    }
#endif
    if (!status) {
        filed_mutex_lock(&rt->storage_exchange_lock);
        status = owner ? filed_storage_owner_control_call(rt->storage,
            request,request_size,reply,reply_size,&used) :
            filed_storage_owner_call(rt->storage,request,request_size,
                reply,reply_size,&used);
        filed_mutex_unlock(&rt->storage_exchange_lock);
    }
#ifdef FILED_INTERNAL_MICROBENCH
    if(filed_bench_collect) bench_stage=filed_bench_tsc();
#endif
    if (!status) {
        const int decode=kb2_fs_response_decode(reply,used,response);
        if (decode || !kb2_fs_response_matches(response,r)) {
            fprintf(stderr,"[filed] storage response mismatch op=%u seq=%llu decode=%d used=%zu generation=%llu/%llu response_seq=%llu\n",
                r->opcode,(unsigned long long)r->sequence,decode,used,
                (unsigned long long)r->generation,
                (unsigned long long)(decode ? 0 : response->generation),
                (unsigned long long)(decode ? 0 : response->sequence));
            status=-71;
        }
    }
    if (!status && response->data_length > capacity) {
        fprintf(stderr,"[filed] storage response oversize op=%u length=%llu capacity=%zu\n",
            r->opcode,(unsigned long long)response->data_length,capacity);
        status=-71;
    }
    if (!status && response->result < 0) status = (int)response->result;
    if (!status && response->data_length)
        memcpy(data, reply + KB2_FILESYSTEM_RESPONSE_SIZE,
               (size_t)response->data_length);
#ifdef FILED_INTERNAL_MICROBENCH
    FILED_BENCH_STAGE(filed_bench_storage_cycles,1,bench_stage);
#endif
    free(request);
    free(reply);
#ifdef FILED_INTERNAL_MICROBENCH
    FILED_BENCH_STAGE(filed_bench_storage_cycles,2,bench_stage);
#endif
    return status;
}
int filed_storage_request(filed_runtime_t *rt,kb2_fs_request_t *r,
    const void *tail,size_t tail_size,kb2_fs_response_t *response,void *data,size_t capacity)
{
    return storage_request(rt,r,tail,tail_size,response,data,capacity,0,0,0);
}

int filed_storage_open(filed_runtime_t *rt, uint64_t dir, const char *path,
                       uint64_t flags, uint64_t mode, uint32_t umask,
                       uint64_t *handle)
{
    if (!path || !handle) return -22;
    size_t length = strlen(path) + 1;
    kb2_fs_request_t r = { .opcode = KB2_FILESYSTEM_OP_OPENAT2,
        .handle = dir, .flags = flags, .mode = mode, .umask = umask,
        .path = { KB2_FILESYSTEM_REQUEST_SIZE, length } };
    kb2_fs_response_t response;
    int status = filed_storage_request(rt, &r, path, length, &response, NULL, 0);
    if (!status) *handle = response.handle;
    return status;
}
int filed_storage_close(filed_runtime_t *rt, uint64_t handle)
{
    kb2_fs_request_t r = { .opcode = KB2_FILESYSTEM_OP_CLOSE, .handle = handle };
    kb2_fs_response_t response;
    return filed_storage_request(rt, &r, NULL, 0, &response, NULL, 0);
}
int filed_storage_executable_mapping(filed_runtime_t *rt,uint64_t handle)
{
    uint8_t bytes[KB2_FILESYSTEM_STATFS_SIZE];
    kb2_fs_request_t r={.opcode=KB2_FILESYSTEM_OP_STATFS,.handle=handle};
    kb2_fs_response_t response;
    int status=filed_storage_request(rt,&r,NULL,0,&response,bytes,sizeof(bytes));
    if(status) return status;
    if(response.data_length!=sizeof(bytes)) return -71;
    /* Linux mmap(PROT_EXEC) checks the mount's noexec flag, not executable
     * inode mode bits: ordinary 0644 shared libraries must remain mappable. */
    return rd(bytes+KB2_FILESYSTEM_STATFS_FLAGS_OFFSET,8)&8 ? -13 : 0;
}
static int storage_stat(filed_runtime_t *rt,uint64_t handle,
                         filed_storage_stat_t *out,int owner)
{
    if (!out) return -22;
    uint8_t bytes[KB2_FILESYSTEM_STATX_SIZE];
    kb2_fs_request_t r = { .opcode = KB2_FILESYSTEM_OP_STATX,
        .handle = handle, .flags = 0x1000, .mask = 0x7ff };
    kb2_fs_response_t response;
    int status = storage_request(rt,&r,NULL,0,&response,bytes,sizeof(bytes),owner,0,0);
    if (status) return status;
    if (response.data_length != sizeof(bytes)) return -71;
    *out = (filed_storage_stat_t){
        .inode = rd(bytes + KB2_FILESYSTEM_STATX_INO_OFFSET, 8),
        .filesystem = rd(bytes + KB2_FILESYSTEM_STATX_DEV_MAJOR_OFFSET, 4)<<32 |
            rd(bytes + KB2_FILESYSTEM_STATX_DEV_MINOR_OFFSET, 4),
        .rdev_major = (uint32_t)rd(bytes + KB2_FILESYSTEM_STATX_RDEV_MAJOR_OFFSET, 4),
        .rdev_minor = (uint32_t)rd(bytes + KB2_FILESYSTEM_STATX_RDEV_MINOR_OFFSET, 4),
        .size = rd(bytes + KB2_FILESYSTEM_STATX_SIZE_OFFSET, 8),
        .blocks = rd(bytes + KB2_FILESYSTEM_STATX_BLOCKS_OFFSET, 8),
        .mode = (uint32_t)rd(bytes + KB2_FILESYSTEM_STATX_MODE_OFFSET, 2),
        .uid = (uint32_t)rd(bytes + KB2_FILESYSTEM_STATX_UID_OFFSET, 4),
        .gid = (uint32_t)rd(bytes + KB2_FILESYSTEM_STATX_GID_OFFSET, 4),
        .nlink = (uint32_t)rd(bytes + KB2_FILESYSTEM_STATX_NLINK_OFFSET, 4) };
    return 0;
}
int filed_storage_stat(filed_runtime_t *rt,uint64_t handle,filed_storage_stat_t *out)
{
    return storage_stat(rt,handle,out,0);
}
int filed_storage_snapshot_stat(filed_runtime_t *rt,uint64_t handle,filed_storage_stat_t *out)
{
    uint8_t bytes[KB2_FILESYSTEM_SNAPSHOT_STAT_SIZE];
    kb2_fs_request_t request={.opcode=KB2_FILESYSTEM_OP_STATX,.handle=handle,
        .flags=0x1000,.mode=KB2_FILESYSTEM_SNAPSHOT_QUERY,.mask=0x7ff};
    kb2_fs_response_t response;
    int status=storage_request(rt,&request,NULL,0,&response,bytes,sizeof(bytes),0,0,0);
    if (status) {
        fprintf(stderr,"[filed] snapshot STATX exchange status=%d handle=%llu\n",
            status,(unsigned long long)handle);
        return status;
    }
    if (response.data_length!=sizeof(bytes)) {
        fprintf(stderr,"[filed] snapshot STATX length=%llu expected=%zu handle=%llu\n",
            (unsigned long long)response.data_length,sizeof(bytes),
            (unsigned long long)handle);
        return -71;
    }
    const uint8_t *s=bytes+KB2_FILESYSTEM_SNAPSHOT_STAT_STATX_OFFSET;
    *out=(filed_storage_stat_t){
        .inode=rd(s+KB2_FILESYSTEM_STATX_INO_OFFSET,8),
        .filesystem=rd(s+KB2_FILESYSTEM_STATX_DEV_MAJOR_OFFSET,4)<<32 |
            rd(s+KB2_FILESYSTEM_STATX_DEV_MINOR_OFFSET,4),
        .size=rd(s+KB2_FILESYSTEM_STATX_SIZE_OFFSET,8),
        .mode=(uint32_t)rd(s+KB2_FILESYSTEM_STATX_MODE_OFFSET,2),
        .nlink=(uint32_t)rd(s+KB2_FILESYSTEM_STATX_NLINK_OFFSET,4),
        .inode_generation=(uint32_t)rd(bytes,4),
        .snapshot_valid=(rd(bytes+4,4)==KB2_FILESYSTEM_SNAPSHOT_VALID),
        .change_cookie=rd(bytes+8,8)};
    return 0;
}

int filed_storage_device_route(filed_runtime_t *rt,kb2_fs_request_t *request,
    const void *tail,size_t tail_size,filed_storage_stat_t *stat,uint64_t *handle,
    uint8_t statx[KB2_FILESYSTEM_STATX_SIZE])
{
    uint8_t bytes[KB2_FILESYSTEM_STATX_SIZE];
    kb2_fs_response_t response;
    if (!stat || !handle || request->opcode!=KB2_FILESYSTEM_OP_DEVICE_ROUTE) return -22;
    int status=storage_request(rt,request,tail,tail_size,&response,bytes,sizeof(bytes),1,0,0);
    if (status) return status;
    if (response.data_length!=sizeof(bytes) || !response.handle) return -71;
    *stat=(filed_storage_stat_t){
        .mode=(uint32_t)rd(bytes+KB2_FILESYSTEM_STATX_MODE_OFFSET,2),
        .rdev_major=(uint32_t)rd(bytes+KB2_FILESYSTEM_STATX_RDEV_MAJOR_OFFSET,4),
        .rdev_minor=(uint32_t)rd(bytes+KB2_FILESYSTEM_STATX_RDEV_MINOR_OFFSET,4)};
    *handle=response.handle;
    if (statx) memcpy(statx,bytes,sizeof(bytes));
    return 0;
}
static int storage_pread(filed_runtime_t *rt,uint64_t handle,uint64_t offset,
    void *data,size_t length,size_t *got,int owner,
    const filed_storage_stat_t *expected,int *valid,
    uint64_t pager_client,uint64_t pager_credential)
{
    if (!rt || !rt->storage || !got || (length && !data) || offset > INT64_MAX) return -22;
    /* These readers assemble private exec/FILE_VMO snapshots, not ordinary
     * client I/O. Their existing loops accept short reads. Keeping each
     * reply inside the configured arena avoids extra full-image copies in
     * ephemeral channels while clang's parent and child images are live.
     * Relayed READ/PREAD still performs one Linux operation at full length. */
    uint64_t overhead=KB2_FILESYSTEM_REQUEST_SIZE+KB2_FILESYSTEM_RESPONSE_SIZE;
    if(rt->bootstrap.queue_arena_bytes<=overhead) return -22;
    uint64_t budget=rt->bootstrap.queue_arena_bytes-overhead;
    if(length>budget) length=(size_t)budget;
    if(length>UINT32_MAX-KB2_FILESYSTEM_RESPONSE_SIZE)
        length=UINT32_MAX-KB2_FILESYSTEM_RESPONSE_SIZE;
    kb2_fs_request_t r = { .opcode = KB2_FILESYSTEM_OP_PREAD,
        .handle = handle, .offset = (int64_t)offset, .length = length };
    if(expected) r.mode=KB2_FILESYSTEM_SNAPSHOT_QUERY;
    if(rt->request_sequence==UINT64_MAX) return -75;
    r.generation=rt->storage_generation;
    r.sequence=__atomic_add_fetch(&rt->request_sequence,1,__ATOMIC_RELAXED);
    r.client=pager_client ? pager_client : (rt->actor ? rt->actor->id : 1);
    r.credential_generation=pager_client ? pager_credential :
        (rt->actor ? rt->actor->credential_generation : 1);
    uint8_t request[KB2_FILESYSTEM_REQUEST_SIZE];
    int status=kb2_fs_request_encode(request,sizeof(request),sizeof(request),&r);
    kb2_fs_response_t response;
    size_t used=0;
#ifdef FILED_INTERNAL_MICROBENCH
    uint64_t start=filed_bench_collect ? filed_bench_tsc() : 0;
#endif
    /* Snapshot readers already own the destination VMO. Copy from the queue
     * directly into it; a fresh header+payload malloc would fault in another
     * full image only to copy and immediately discard it. */
    if(!status) {
        filed_mutex_lock(&rt->storage_exchange_lock);
        status=owner ? filed_storage_owner_control_call_split(rt->storage,
            request,sizeof(request),&response,data,length,&used) :
            filed_storage_owner_call_split(rt->storage,request,sizeof(request),
                &response,data,length,&used);
        filed_mutex_unlock(&rt->storage_exchange_lock);
    }
#ifdef FILED_INTERNAL_MICROBENCH
    if(filed_bench_collect) {
        filed_bench_exchange_cycles+=filed_bench_tsc()-start;
        ++filed_bench_exchange_count;
    }
#endif
    if(!status && (!kb2_fs_response_matches(&response,&r) ||
        response.data_length>length || used!=KB2_FILESYSTEM_RESPONSE_SIZE+response.data_length))
        status=-71;
    if(!status && response.result<0) status=(int)response.result;
    if (!status) {
        if ((uint64_t)response.result != response.data_length) return -71;
        *got = (size_t)response.data_length;
        if (expected && (response.service!=KB2_FILESYSTEM_SNAPSHOT_VALID ||
            response.handle!=expected->change_cookie)) *valid=0;
    }
    return status;
}
int filed_storage_pread(filed_runtime_t *rt,uint64_t handle,uint64_t offset,
    void *data,size_t length,size_t *got)
{
    return storage_pread(rt,handle,offset,data,length,got,0,NULL,NULL,0,0);
}
int filed_storage_exec_open(filed_runtime_t *rt,uint64_t directory,
    const char *path,uint64_t *handle)
{
    if(!path || !handle) return -22;
    size_t length=strlen(path)+1;
    kb2_fs_request_t r={.opcode=KB2_FILESYSTEM_OP_EXEC_OPEN,.handle=directory,
        .path={KB2_FILESYSTEM_REQUEST_SIZE,length}};
    kb2_fs_response_t response;
    int status=storage_request(rt,&r,path,length,&response,NULL,0,1,0,0);
    if(!status) *handle=response.handle;
    return status;
}
int filed_storage_exec_stat(filed_runtime_t *rt,uint64_t handle,filed_storage_stat_t *out)
{
    return storage_stat(rt,handle,out,1);
}
int filed_storage_exec_pread(filed_runtime_t *rt,uint64_t handle,uint64_t offset,
    void *data,size_t length,size_t *got)
{
    return storage_pread(rt,handle,offset,data,length,got,1,NULL,NULL,0,0);
}
int filed_storage_exec_close(filed_runtime_t *rt,uint64_t handle)
{
    kb2_fs_request_t r={.opcode=KB2_FILESYSTEM_OP_CLOSE,.handle=handle};
    kb2_fs_response_t response;
    return storage_request(rt,&r,NULL,0,&response,NULL,0,1,0,0);
}

int filed_storage_snapshot_pread(filed_runtime_t *rt,uint64_t handle,uint64_t offset,
    void *data,size_t length,size_t *got,const filed_storage_stat_t *expected,int *valid)
{
    if (!expected || !valid) return -22;
    return storage_pread(rt,handle,offset,data,length,got,0,
        expected->snapshot_valid ? expected : NULL,valid,0,0);
}

int filed_storage_pager_request(filed_runtime_t *rt,uint64_t client,
    uint64_t credential,kb2_fs_request_t *request,kb2_fs_response_t *response,
    void *data,size_t capacity)
{
    if (!client || !credential) return -22;
    return storage_request(rt,request,NULL,0,response,data,capacity,0,
        client,credential);
}
int filed_storage_pager_request_ex(filed_runtime_t *rt,uint64_t client,
    uint64_t credential,kb2_fs_request_t *request,const void *tail,
    size_t tail_size,kb2_fs_response_t *response,void *data,size_t capacity)
{
    if (!client || !credential) return -22;
    return storage_request(rt,request,tail,tail_size,response,data,capacity,
        0,client,credential);
}

int filed_storage_pager_pread(filed_runtime_t *rt,uint64_t client,
    uint64_t credential,uint64_t handle,uint64_t offset,void *data,
    size_t length,size_t *got)
{
    if (!client || !credential) return -22;
    return storage_pread(rt,handle,offset,data,length,got,0,NULL,NULL,
        client,credential);
}
