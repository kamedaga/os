/* SPDX-License-Identifier: MIT */
#include "../src/relay.c"
#include <assert.h>
#include <stdio.h>

static uint8_t *mutable_request;
static kb2_fs_request_t observed;
static unsigned exchanges,transfers,closed_count;
static int close_status;
static uint64_t lease_target,closed_handle;
static struct pacha_ipc_msg lease_reply;
static uint8_t *vmo_memory;
static size_t vmo_size;
static int executable_status;
int filed_storage_executable_mapping(filed_runtime_t *rt,uint64_t handle)
{
    assert(rt && handle==90);return executable_status;
}

int filed_storage_exchange(filed_runtime_t *rt,const uint8_t *bytes,size_t size,
    uint8_t *reply,size_t capacity,size_t *used)
{
    (void)rt;++exchanges;
    assert(!kb2_fs_request_decode(bytes,size,size,&observed));
    assert(observed.client==1 && observed.credential_generation==1);
    assert(!observed.capabilities && !observed.groups.length);
    if(mutable_request) memset(mutable_request+KB2_FILESYSTEM_REQUEST_SIZE,'Z',3);
    assert(!memcmp(bytes+KB2_FILESYSTEM_REQUEST_SIZE,"ok\0",3));
    kb2_fs_response_t response={.opcode=observed.opcode,.generation=observed.generation,
        .sequence=observed.sequence,.result=0,.handle=123};
    assert(!kb2_fs_response_encode(reply,capacity,&response));
    *used=KB2_FILESYSTEM_RESPONSE_SIZE;
    return 0;
}
int filed_owner_control(filed_runtime_t *rt,kb2_fs_request_t *r,
    const uint8_t *bytes,size_t size,kb2_fs_response_t *response)
{
    (void)rt;(void)bytes;
    assert(size==KB2_FILESYSTEM_REQUEST_SIZE);
    assert(r->opcode==KB2_FILESYSTEM_OP_TRANSFER_DUP);
    assert(r->client==1 && r->second_handle==9 && r->handle==44);
    ++transfers;*response=(kb2_fs_response_t){.handle=88};return 0;
}
int filed_storage_close(filed_runtime_t *rt,uint64_t handle)
{
    assert(!rt->actor);closed_handle=handle;return close_status;
}
int filed_storage_stat(filed_runtime_t *rt,uint64_t handle,filed_storage_stat_t *stat)
{
    assert(rt && handle==90);*stat=(filed_storage_stat_t){.mode=0100644,.size=10000};return 0;
}
int filed_storage_pread(filed_runtime_t *rt,uint64_t handle,uint64_t offset,
    void *data,size_t length,size_t *got)
{
    assert(rt && handle==90 && offset<=10000);
    if(length>10000-offset) length=(size_t)(10000-offset);
    if(length>1024) length=1024;
    for(size_t i=0;i<length;++i) ((uint8_t *)data)[i]=(uint8_t)(offset+i);
    *got=length;return 0;
}
pacha_fd_t pacha_vmo_create(uint64_t size,uint64_t rights,uint32_t flags)
{
    assert((rights&PACHA_FD_RIGHT_MAP_READ) && !flags);
    assert(!vmo_memory);vmo_size=(size_t)size;vmo_memory=malloc(vmo_size);assert(vmo_memory);
    /* Match the native eager VMO_CREATE zero-fill contract, including EOF. */
    memset(vmo_memory,0,vmo_size);return 80;
}
void *pacha_mmap(pacha_fd_t fd,uint64_t size,uint64_t protection,uint64_t flags,uint64_t offset)
{
    assert(fd==80 && size==vmo_size && !offset);
    assert(protection==(PACHA_PROT_READ|PACHA_PROT_WRITE) && flags==PACHA_MMAP_SHARED);
    return vmo_memory;
}
int pacha_munmap(void *memory,uint64_t size)
{
    assert(memory==vmo_memory && size==vmo_size);return 0;
}
int pacha_ipc_recv(pacha_fd_t fd,struct pacha_ipc_msg *message)
{
    assert(fd==60);message->word0=FILED_LEASE_MAGIC;message->word1=44;
    message->word2=lease_target;message->word3=99;message->fd_count=0;
    return 0;
}
int pacha_fd_get_info(pacha_fd_t fd,struct pacha_fd_info *info)
{
    assert(fd==70);*info=(struct pacha_fd_info){.kind=PACHA_FD_KIND_REPLY};return 0;
}
int pacha_ipc_reply(pacha_fd_t fd,const struct pacha_ipc_msg *message)
{
    assert(fd==PACHA_IPC_REPLY_IMPLICIT);lease_reply=*message;return 0;
}
int pacha_fd_close(pacha_fd_t fd)
{
    assert(fd==70);++closed_count;return 0;
}
int main(void)
{
    struct filed_client client={.id=9,.credential_generation=7,.rights=FILED_RIGHT_WRITE};
    filed_runtime_t runtime={.actor=&client,.clients=&client,.storage_generation=31};
    kb2_fs_request_t request={.opcode=KB2_FILESYSTEM_OP_OPENAT2,.flags=2};
    assert(authorize(&runtime,&request)==-EACCES);
    client.rights=FILED_RIGHT_LOOKUP|FILED_RIGHT_WRITE;
    assert(authorize(&runtime,&request)==-EACCES);
    client.rights|=FILED_RIGHT_READ;
    assert(!authorize(&runtime,&request));
    request.flags=1;client.rights=FILED_RIGHT_LOOKUP|FILED_RIGHT_WRITE;
    assert(!authorize(&runtime,&request));
    request.flags=01001;assert(!authorize(&runtime,&request));
    client.rights=FILED_RIGHT_LOOKUP;assert(authorize(&runtime,&request)==-EACCES);
    request.flags=010000000;assert(!authorize(&runtime,&request));
    request=(kb2_fs_request_t){.opcode=KB2_FILESYSTEM_OP_READLINK_HANDLE};
    assert(authorize(&runtime,&request)==-EACCES);
    client.rights=FILED_RIGHT_STAT;assert(!authorize(&runtime,&request));
    client.rights=0;
    request=(kb2_fs_request_t){.opcode=KB2_FILESYSTEM_OP_SYNC};
    assert(!authorize(&runtime,&request));
    request.opcode=KB2_FILESYSTEM_OP_SYNCFS;request.handle=90;
    assert(!authorize(&runtime,&request));
    client.rights=1023;
    const uint32_t controls[]={KB2_FILESYSTEM_OP_CLIENT_REGISTER,
        KB2_FILESYSTEM_OP_CLIENT_CREDENTIALS,KB2_FILESYSTEM_OP_CLIENT_UNREGISTER,
        KB2_FILESYSTEM_OP_SERVICE_BIND,KB2_FILESYSTEM_OP_EXEC_OPEN};
    for(size_t i=0;i<sizeof(controls)/sizeof(controls[0]);++i) {
        request.opcode=controls[i];assert(authorize(&runtime,&request)==-EPERM);
    }
    uint8_t bytes[KB2_FILESYSTEM_REQUEST_SIZE+3]={0},output[KB2_FILESYSTEM_RESPONSE_SIZE];
    request=(kb2_fs_request_t){.opcode=KB2_FILESYSTEM_OP_OPENAT2,.generation=1,
        .sequence=87,.client=1,.credential_generation=99,.capabilities=UINT64_MAX,
        .path={KB2_FILESYSTEM_REQUEST_SIZE,3}};
    assert(!kb2_fs_request_encode(bytes,KB2_FILESYSTEM_REQUEST_SIZE,sizeof(bytes),&request));
    memcpy(bytes+KB2_FILESYSTEM_REQUEST_SIZE,"ok\0",3);mutable_request=bytes;
    size_t used=0;
    assert(relay(&runtime,&request,bytes,sizeof(bytes),output,sizeof(output),&used)==-EPERM);
    assert(!exchanges);
    runtime.actor=NULL;
    assert(!relay(&runtime,&request,bytes,sizeof(bytes),output,sizeof(output),&used));
    assert(exchanges==1 && observed.generation==31 && observed.sequence==1);
    kb2_fs_response_t response;
    assert(!kb2_fs_response_decode(output,used,&response));
    assert(response.generation==1 && response.sequence==87 && response.handle==123);
    runtime.request_sequence=UINT64_MAX;mutable_request=NULL;
    assert(relay(&runtime,&request,bytes,sizeof(bytes),output,sizeof(output),&used)==-EOVERFLOW);
    assert(exchanges==1);
    runtime.actor=&client;
    struct filed_lease lease={.id=44,.handle=44,.client=1,.fd=60};
    lease_target=9;assert(!lease_receive(&runtime,&lease));
    assert(transfers==1 && closed_handle==44 && closed_count==0);
    assert(lease.client==9 && lease.handle==88 && !lease_reply.word1 && lease_reply.word2==88);
    assert(!lease_receive(&runtime,&lease));
    assert(transfers==1 && closed_count==0 && lease_reply.word2==88);
    struct filed_client other={.id=10};client.next=&other;lease_target=10;
    assert(!lease_receive(&runtime,&lease));
    assert((int64_t)lease_reply.word1==-EPERM && transfers==1 && closed_count==0);
    lease_target=9;lease=(struct filed_lease){.id=44,.handle=44,.client=1,.fd=60};
    close_status=-EIO;runtime.storage_generation=31;
    assert(!lease_receive(&runtime,&lease));
    assert(!runtime.storage_generation && transfers==2 && closed_count==0);
    assert((int64_t)lease_reply.word1==-EIO && !lease_reply.word2);
    request=(kb2_fs_request_t){.handle=90,.offset=9000,.length=5000};
    response=(kb2_fs_response_t){0};pacha_fd_t vmo=-1;
    assert(!make_file_vmo(&runtime,&request,&response,&vmo));
    assert(vmo==80 && vmo_size==8192 && response.result==1000 && !response.handle);
    for(size_t i=0;i<1000;++i) assert(vmo_memory[i]==(uint8_t)(9000+i));
    for(size_t i=1000;i<vmo_size;++i) assert(!vmo_memory[i]);
    free(vmo_memory);vmo_memory=NULL;
    request.flags=FILED_FILE_VMO_EXEC;executable_status=-EACCES;
    assert(make_file_vmo(&runtime,&request,&response,&vmo)==-EACCES && !vmo_memory);
    request.flags=0;
    request.length=UINT64_MAX;assert(make_file_vmo(&runtime,&request,&response,&vmo)==-EINVAL);
    request.length=1;request.offset=-1;assert(make_file_vmo(&runtime,&request,&response,&vmo)==-EINVAL);
    puts("filed relay authentication / immutable request / lease adoption: PASS");
}
