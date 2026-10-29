/* SPDX-License-Identifier: MIT */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "filed/runtime.h"
#include "filed/storage.h"
#include "filed/storage_owner.h"
#include "../../kobox2_owner/bootfs.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int path_call(filed_runtime_t *rt, uint32_t opcode, const char *path,
    uint64_t flags, uint32_t mode, kb2_fs_response_t *response)
{
    const size_t length = strlen(path) + 1;
    kb2_fs_request_t request = {.opcode=opcode, .flags=flags, .mode=mode,
        .path={KB2_FILESYSTEM_REQUEST_SIZE,length}};
    return filed_storage_pager_request_ex(rt,1,1,&request,path,length,response,NULL,0);
}
static int make_directory(filed_runtime_t *rt, const char *path, uint32_t mode)
{
    kb2_fs_response_t response;
    int status = path_call(rt,KB2_FILESYSTEM_OP_MKDIRAT,path,0,mode,&response);
    return status == -EEXIST ? 0 : status;
}
static int parents(filed_runtime_t *rt, char *path)
{
    for (char *p=strchr(path+1,'/'); p; p=strchr(p+1,'/')) {
        *p=0;
        int status=make_directory(rt,path,0755);
        *p='/';
        if (status) return status;
    }
    return 0;
}
int filed_live_root_import(filed_runtime_t *rt)
{
    if (!rt || !rt->live_root) return -EINVAL;
    const struct ph_bootfs *archive=filed_storage_owner_archive(rt->storage);
    if (!archive) return -EINVAL;
    /* Source entries borrow the validated immutable boot VMO. Only the
     * newly mounted tmpfs is writable; this path owns no block capability.
     * Publish LIVE_READY only after every entry and its permissions exist. */
    for (uint32_t i=0;i<archive->entry_count;++i) {
        const unsigned char *name,*bytes;
        size_t name_size,length;
        uint32_t mode;
        int status=ph_bootfs_entry(archive,i,&name,&name_size,&bytes,&length,&mode);
        if (status) return status;
        char *path=malloc(name_size+1);
        if (!path) return -ENOMEM;
        memcpy(path,name,name_size);path[name_size]=0;
        status=parents(rt,path);
        kb2_fs_response_t response;
        if (!status) status=path_call(rt,KB2_FILESYSTEM_OP_OPENAT2,path,
            O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,mode&07777,&response);
        uint64_t handle=status ? 0 : response.handle;
        for (size_t done=0;!status && done<length;) {
            size_t chunk=length-done;
            if (chunk>64*1024) chunk=64*1024;
            kb2_fs_request_t write={.opcode=KB2_FILESYSTEM_OP_PWRITE,
                .handle=handle,.offset=(int64_t)done,.length=chunk,
                .data={KB2_FILESYSTEM_REQUEST_SIZE,chunk}};
            status=filed_storage_pager_request_ex(rt,1,1,&write,bytes+done,chunk,
                &response,NULL,0);
            if (!status && response.result!=(int64_t)chunk) status=-EIO;
            done+=chunk;
        }
        if (handle) {
            kb2_fs_request_t close={.opcode=KB2_FILESYSTEM_OP_CLOSE,.handle=handle};
            int closed=filed_storage_pager_request(rt,1,1,&close,&response,NULL,0);
            if (!status) status=closed;
        }
        if (status) fprintf(stderr,"[filed] live archive import failed path=%s status=%d\n",path,status);
        free(path);
        if (status) return status;
    }
    static const struct { const char *path; uint32_t mode; } directories[]={
        {"/tmp",01777},{"/run",0755},{"/proc",0555},{"/sys",0755},
        {"/dev/shm",01777},{"/dev/pts",0755},{"/etc/dropbear",0700},
        {"/root/.ssh",0700},{"/var/log",0755},{"/root/Desktop",0755}};
    for (size_t i=0;i<sizeof(directories)/sizeof(directories[0]);++i) {
        char *path=strdup(directories[i].path);
        if (!path) return -ENOMEM;
        int status=parents(rt,path);
        if (!status) status=make_directory(rt,path,directories[i].mode);
        /* Existing parents and the owner's umask must not leave /tmp or
         * shared memory unwritable to ordinary users. */
        kb2_fs_response_t response;
        if (!status) status=path_call(rt,KB2_FILESYSTEM_OP_FCHMODAT,path,0,
            directories[i].mode,&response);
        free(path);
        if (status) return status;
    }
    printf("[filed] live tmpfs ready entries=%u; no block authority\n",archive->entry_count);
    return 0;
}
