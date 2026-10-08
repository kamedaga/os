#include "filed_client/module_image.h"
#include "filed_client/client.h"
#include "filed/ipc_protocol.h"
#include "pacha/ipc.h"
#include <stdlib.h>
#include <string.h>

static uint64_t rd64(const unsigned char *p)
{
    uint64_t value=0;
    for(unsigned i=0;i<8;++i) value|=(uint64_t)p[i]<<(8*i);
    return value;
}
static int close_handle(int endpoint,uint64_t handle)
{
    kb2_fs_request_t r={.opcode=KB2_FILESYSTEM_OP_CLOSE,.handle=handle};
    kb2_fs_response_t response;
    int status=filedfs_call(endpoint,&r,NULL,NULL,NULL,0,NULL,0,&response,NULL,0,NULL,NULL);
    return status ? status : response.result<0 ? (int)response.result : 0;
}
int filed_client_load_module_image_limited(int endpoint,const char *path,
    const char *name,size_t max_bytes,struct filed_client_module_image *out)
{
    if(endpoint<16 || !path || !name || !out || !max_bytes) return -22;
    memset(out,0,sizeof(*out));
    kb2_fs_request_t r={.opcode=KB2_FILESYSTEM_OP_OPENAT2,.flags=02000000};
    kb2_fs_response_t response;
    int status=filedfs_call(endpoint,&r,path,NULL,NULL,0,NULL,0,&response,NULL,0,NULL,NULL);
    if(!status && response.result<0) status=(int)response.result;
    if(status) return status;
    uint64_t handle=response.handle;
    unsigned char stat[KB2_FILESYSTEM_STATX_SIZE];
    r=(kb2_fs_request_t){.opcode=KB2_FILESYSTEM_OP_STATX,.handle=handle,
        .flags=0x1000,.mask=0x7ff};
    status=filedfs_call(endpoint,&r,NULL,NULL,NULL,0,stat,sizeof(stat),&response,NULL,0,NULL,NULL);
    if(!status && response.result<0) status=(int)response.result;
    if(!status && response.data_length!=sizeof(stat)) status=-71;
    uint64_t size=!status ? rd64(stat+KB2_FILESYSTEM_STATX_SIZE_OFFSET) : 0;
    if(!status && (!size || size>max_bytes || size>SIZE_MAX-4095)) status=-7;
    int image=-1;size_t count=1;
    if(!status) {
        r=(kb2_fs_request_t){.opcode=KB2_FILESYSTEM_OP_FILE_VMO,.handle=handle,.length=size};
        status=filedfs_call(endpoint,&r,NULL,NULL,NULL,0,NULL,0,&response,NULL,0,&image,&count);
        if(!status && response.result<0) status=(int)response.result;
        if(!status && (count!=1 || (uint64_t)response.result!=size || response.handle)) status=-5;
    }
    int closed=close_handle(endpoint,handle);
    if(!status) status=closed;
    if(status) { if(image>=16) (void)pacha_fd_close(image); return status; }
    size_t mapped=((size_t)size+4095)&~(size_t)4095;
    const void *bytes=pacha_mmap(image,mapped,PACHA_PROT_READ,PACHA_MMAP_SHARED,0);
    if(!bytes) { (void)pacha_fd_close(image);return -12; }
    unsigned char *copy=malloc((size_t)size);
    if(copy) memcpy(copy,bytes,(size_t)size);
    (void)pacha_munmap((void *)bytes,mapped);
    (void)pacha_fd_close(image);
    if(!copy) return -12;
    *out=(struct filed_client_module_image){.data=copy,.size=(size_t)size,.name=name};
    return 0;
}
int filed_client_load_module_image(int endpoint,const char *path,const char *name,
    struct filed_client_module_image *out)
{
    return filed_client_load_module_image_limited(endpoint,path,name,
        FILED_CLIENT_MODULE_MAX_BYTES,out);
}
void filed_client_release_module_image(struct filed_client_module_image *image)
{
    if(image) { free(image->data);memset(image,0,sizeof(*image)); }
}
