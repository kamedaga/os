/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "filed/manifest.h"
#include "filed/runtime.h"
#include "filed/storage.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char manifest_directory[]="/var/cache/filed/zpoline";
static uint64_t temporary_sequence;
static uint64_t le64(const uint8_t *p)
{ uint64_t v=0;for (unsigned i=0;i<8;++i) v|=(uint64_t)p[i]<<(8*i);return v; }

static int filename(const uint8_t key[32],char *out,size_t capacity)
{
    static const char digits[]="0123456789abcdef";
    const size_t prefix=sizeof(manifest_directory)-1;
    if (capacity<prefix+2+64) return -ENAMETOOLONG;
    memcpy(out,manifest_directory,prefix);
    out[prefix]='/';
    for (unsigned i=0;i<32;++i) {
        out[prefix+1+i*2]=digits[key[i]>>4];
        out[prefix+2+i*2]=digits[key[i]&15];
    }
    out[prefix+65]=0;
    return 0;
}

static int path_call(filed_runtime_t *rt,uint32_t opcode,const char *path,
    uint64_t flags,uint64_t mode,kb2_fs_response_t *response)
{
    const size_t length=strlen(path)+1;
    kb2_fs_request_t request={.opcode=opcode,.handle=0,.flags=flags,
        .mode=mode,.umask=0077,
        .path={KB2_FILESYSTEM_REQUEST_SIZE,length}};
    return filed_storage_pager_request_ex(rt,1,1,&request,path,length,
        response,NULL,0);
}
static int handle_call(filed_runtime_t *rt,uint32_t opcode,uint64_t handle,
    kb2_fs_response_t *response)
{
    kb2_fs_request_t request={.opcode=opcode,.handle=handle};
    return filed_storage_pager_request(rt,1,1,&request,response,NULL,0);
}
static void close_handle(filed_runtime_t *rt,uint64_t handle)
{
    kb2_fs_response_t response;
    (void)handle_call(rt,KB2_FILESYSTEM_OP_CLOSE,handle,&response);
}
static int ensure_directory(filed_runtime_t *rt)
{
    kb2_fs_response_t response;
    int status=path_call(rt,KB2_FILESYSTEM_OP_MKDIRAT,
        "/var/cache/filed",0,0700,&response);
    if (status && status!=-EEXIST) return status;
    status=path_call(rt,KB2_FILESYSTEM_OP_MKDIRAT,
        manifest_directory,0,0700,&response);
    return status==-EEXIST ? 0 : status;
}

int filed_manifest_storage_load(filed_runtime_t *rt,const uint8_t key[32],
    uint8_t **out,size_t *out_length)
{
    if (!rt || !key || !out || !out_length) return -EINVAL;
    *out=NULL;*out_length=0;
    char path[128];
    int status=filename(key,path,sizeof(path));
    if (status) return status;
    kb2_fs_response_t response;
    status=path_call(rt,KB2_FILESYSTEM_OP_OPENAT2,path,
        O_RDONLY|O_CLOEXEC,0,&response);
    if (status) return status;
    const uint64_t handle=response.handle;
    uint8_t stat[KB2_FILESYSTEM_STATX_SIZE];
    kb2_fs_request_t query={.opcode=KB2_FILESYSTEM_OP_STATX,
        .handle=handle,.flags=0x1000,.mask=0x7ff};
    status=filed_storage_pager_request(rt,1,1,&query,&response,
        stat,sizeof(stat));
    uint64_t length=!status && response.data_length==sizeof(stat) ?
        le64(stat+KB2_FILESYSTEM_STATX_SIZE_OFFSET) : 0;
    /* A cache file is untrusted input. A larger manifest is regenerated
     * from the source file instead of reserving unbounded pager memory. */
    if (!status && (!length || length>256u*1024u*1024u ||
            length>SIZE_MAX)) status=-EINVAL;
    uint8_t *bytes=!status ? malloc((size_t)length) : NULL;
    if (!status && !bytes) status=-ENOMEM;
    for (size_t done=0;!status && done<length;) {
        size_t got=0;
        status=filed_storage_pager_pread(rt,1,1,handle,done,
            bytes+done,(size_t)length-done,&got);
        if (!status && !got) status=-EIO;
        done+=got;
    }
    close_handle(rt,handle);
    if (status) { free(bytes);return status; }
    *out=bytes;*out_length=(size_t)length;
    return 0;
}

void filed_manifest_storage_remove(filed_runtime_t *rt,const uint8_t key[32])
{
    char path[128];kb2_fs_response_t response;
    if (rt && key && !filename(key,path,sizeof(path)))
        (void)path_call(rt,KB2_FILESYSTEM_OP_UNLINKAT,path,0,0,&response);
}

int filed_manifest_storage_save(filed_runtime_t *rt,const uint8_t key[32],
    const uint8_t *bytes,size_t length)
{
    if (!rt || !key || !bytes || !length) return -EINVAL;
    int status=ensure_directory(rt);
    if (status) return status;
    char path[128],temporary[160];
    status=filename(key,path,sizeof(path));
    if (status) return status;
    kb2_fs_response_t response;
    /* A crash can leave a temporary from an earlier boot. Never reuse it;
     * advance the name until O_EXCL claims a fresh inode. */
    for (unsigned attempt=0;attempt<1024;++attempt) {
        const uint64_t sequence=__atomic_add_fetch(&temporary_sequence,1,
            __ATOMIC_RELAXED);
        if (snprintf(temporary,sizeof(temporary),"%s/.tmp-%llu",
                manifest_directory,(unsigned long long)sequence)>=
                (int)sizeof(temporary)) return -ENAMETOOLONG;
        status=path_call(rt,KB2_FILESYSTEM_OP_OPENAT2,temporary,
            O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600,&response);
        if (status!=-EEXIST) break;
    }
    if (status) return status;
    const uint64_t handle=response.handle;
    for (size_t done=0;!status && done<length;) {
        size_t chunk=length-done;
        if (chunk>16*1024) chunk=16*1024;
        kb2_fs_request_t write={.opcode=KB2_FILESYSTEM_OP_PWRITE,
            .handle=handle,.offset=(int64_t)done,.length=chunk,
            .data={KB2_FILESYSTEM_REQUEST_SIZE,chunk}};
        status=filed_storage_pager_request_ex(rt,1,1,&write,
            bytes+done,chunk,&response,NULL,0);
        if (!status && response.result!=(int64_t)chunk) status=-EIO;
        done+=chunk;
    }
    if (!status) status=handle_call(rt,KB2_FILESYSTEM_OP_FSYNC,
        handle,&response);
    close_handle(rt,handle);
    if (!status) {
        const size_t old_len=strlen(temporary)+1,new_len=strlen(path)+1;
        char paths[288];
        memcpy(paths,temporary,old_len);memcpy(paths+old_len,path,new_len);
        kb2_fs_request_t rename={.opcode=KB2_FILESYSTEM_OP_RENAMEAT2,
            .handle=0,.second_handle=0,
            .path={KB2_FILESYSTEM_REQUEST_SIZE,old_len},
            .second_path={KB2_FILESYSTEM_REQUEST_SIZE+old_len,new_len}};
        status=filed_storage_pager_request_ex(rt,1,1,&rename,paths,
            old_len+new_len,&response,NULL,0);
    }
    if (!status) {
        status=path_call(rt,KB2_FILESYSTEM_OP_OPENAT2,manifest_directory,
            O_RDONLY|O_DIRECTORY|O_CLOEXEC,0,&response);
        if (!status) {
            const uint64_t directory=response.handle;
            status=handle_call(rt,KB2_FILESYSTEM_OP_FSYNC,directory,&response);
            close_handle(rt,directory);
        }
    }
    if (status) (void)path_call(rt,KB2_FILESYSTEM_OP_UNLINKAT,
        temporary,0,0,&response);
    return status;
}
