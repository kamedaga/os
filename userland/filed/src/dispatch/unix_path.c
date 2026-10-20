#include "filed/unix_path.h"
#include "filed/runtime.h"
#include "filed/flags.h"
#include "pacha/ipc.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct filed_unix_hold {
    struct filed_unix_hold *next;
    uint64_t id,handle,client;
};
static int release(filed_runtime_t *rt,uint64_t id)
{
    struct filed_unix_hold **cursor=&rt->unix_holds;
    while(*cursor && (*cursor)->id!=id) cursor=&(*cursor)->next;
    if(!*cursor) return -EBADF;
    struct filed_unix_hold *hold=*cursor;
    struct filed_client *client=rt->clients;
    while(client && client->id!=hold->client) client=client->next;
    struct filed_client *saved=rt->actor;
    rt->actor=client;
    int status=client ? filed_storage_close(rt,hold->handle) : 0;
    rt->actor=saved;
    *cursor=hold->next;
    free(hold);
    return status;
}
void filed_unix_path_disconnect(filed_runtime_t *rt)
{
    while(rt->unix_holds) (void)release(rt,rt->unix_holds->id);
    if(rt->unix_path_fd>=16) (void)pacha_fd_close(rt->unix_path_fd);
    rt->unix_path_fd=-1;
}
static int open_path(filed_runtime_t *rt,struct filed_unix_path *r)
{
    if(r->operation==FILED_UNIX_PATH_RELEASE) return release(rt,r->hold);
    struct filed_client *client=rt->clients;
    while(client && (client->pid!=r->pid ||
        client->subject_generation!=r->generation)) client=client->next;
    if(!client) return -EACCES;
    if(r->reserved || r->operation>FILED_UNIX_PATH_CREATE ||
        !r->path[0] || !memchr(r->path,0,sizeof(r->path))) return -EINVAL;
    uint32_t rights=FILED_RIGHT_STAT|FILED_RIGHT_LOOKUP;
    if(r->operation==FILED_UNIX_PATH_CREATE) rights|=FILED_RIGHT_CREATE;
    if(rights & ~client->rights) return -EACCES;
    if(rt->unix_hold_sequence==UINT64_MAX) return -EOVERFLOW;
    struct filed_unix_hold *hold=calloc(1,sizeof(*hold));
    if(!hold) return -ENOMEM;
    rt->actor=client;
    int status=0,created=0;
    size_t length=strlen(r->path)+1;
    if(r->operation==FILED_UNIX_PATH_CREATE) {
        kb2_fs_request_t create={.opcode=KB2_FILESYSTEM_OP_MKNODAT,
            .handle=r->directory,.mode=0140000|(r->mode&0777),
            .path={KB2_FILESYSTEM_REQUEST_SIZE,length}};
        kb2_fs_response_t response;
        status=filed_storage_request(rt,&create,r->path,length,&response,NULL,0);
        if(status==-EEXIST) status=-EADDRINUSE;
        created=!status;
    }
    uint64_t handle=0;
    if(!status) status=filed_storage_open(rt,r->directory,r->path,010000000,0,0,&handle);
    filed_storage_stat_t stat;
    if(!status) status=filed_storage_stat(rt,handle,&stat);
    if(!status && (stat.mode&0170000)!=0140000) status=-ECONNREFUSED;
    if(!status) {
        *hold=(struct filed_unix_hold){.next=rt->unix_holds,
            .id=++rt->unix_hold_sequence,.handle=handle,.client=client->id};
        rt->unix_holds=hold;r->hold=hold->id;
        r->filesystem=stat.filesystem;r->inode=stat.inode;
    } else {
        if(handle) (void)filed_storage_close(rt,handle);
        if(created) {
            kb2_fs_request_t unlink={.opcode=KB2_FILESYSTEM_OP_UNLINKAT,
                .handle=r->directory,.path={KB2_FILESYSTEM_REQUEST_SIZE,length}};
            kb2_fs_response_t response;
            (void)filed_storage_request(rt,&unlink,r->path,length,&response,NULL,0);
        }
        free(hold);
    }
    rt->actor=NULL;
    return status;
}
int filed_unix_path_receive(filed_runtime_t *rt)
{
    struct pacha_ipc_fd fds[PACHA_IPC_MAX_TRANSFER_FDS]={{0}};
    struct pacha_ipc_msg message={.fds=fds,.fd_capacity=PACHA_IPC_MAX_TRANSFER_FDS};
    int status=pacha_ipc_recv(rt->unix_path_fd,&message);
    if(status) return status;
    pacha_fd_t reply=PACHA_IPC_REPLY_IMPLICIT;
    struct pacha_fd_info info;
    struct filed_unix_path *page=NULL;
    status=-EINVAL;
    if(message.fd_count==1 && message.word0==FILED_UNIX_PATH_MAGIC &&
        !pacha_fd_get_info((pacha_fd_t)fds[0].fd,&info) && info.kind==PACHA_FD_KIND_VMO &&
        info.size>=4096)
        page=pacha_mmap((pacha_fd_t)fds[0].fd,4096,PACHA_PROT_READ|PACHA_PROT_WRITE,
                         PACHA_MMAP_SHARED,0);
    if(page) {
        struct filed_unix_path request=*page;
        status=request.magic==FILED_UNIX_PATH_MAGIC ? open_path(rt,&request) : -EINVAL;
        request.status=status;*page=request;
    }
    {
        struct pacha_ipc_msg response={.word0=FILED_UNIX_PATH_MAGIC,
            .word1=(uint64_t)(int64_t)status};
        (void)pacha_ipc_reply(reply,&response);
    }
    if(page) (void)pacha_munmap(page,4096);
    for(size_t i=0;i<message.fd_count;++i) (void)pacha_fd_close((pacha_fd_t)fds[i].fd);
    return 0;
}
