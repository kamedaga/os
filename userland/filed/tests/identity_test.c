/* SPDX-License-Identifier: MIT */
#include "filed/runtime.h"
#include "filed/flags.h"
#include "pacha/ipc.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned calls,channels,closes,released;
static int service_error,transport_error;
static kb2_fs_request_t observed;
int filed_storage_owner_control_call(struct filed_storage_owner *owner,
    const uint8_t *bytes,size_t size,
    uint8_t *output,size_t capacity,size_t *used)
{
    (void)owner;++calls;
    assert(!kb2_fs_request_decode(bytes,size,size,&observed));
    if(observed.opcode!=KB2_FILESYSTEM_OP_CLIENT_UNREGISTER) {
        kb2_fs_credentials_t credentials;
        assert(!kb2_fs_credentials_decode(bytes+observed.data.offset,
            observed.data.length,&credentials));
        assert(credentials.fsuid==1001 && credentials.fsgid==1002);
        assert(observed.groups.length==8 && bytes[observed.groups.offset]==29);
    }
    kb2_fs_response_t response={.opcode=observed.opcode,.generation=observed.generation,
        .sequence=observed.sequence,.result=service_error};
    assert(!kb2_fs_response_encode(output,capacity,&response));
    *used=KB2_FILESYSTEM_RESPONSE_SIZE;return transport_error;
}
int pacha_ipc_channel_create(struct pacha_ipc_channel_pair *pair,uint64_t rights,uint32_t flags)
{
    assert(rights&PACHA_FD_RIGHT_RECV);assert(!flags);
    pair->a=100+(int)channels*2;pair->b=pair->a+1;++channels;return 0;
}
int pacha_fd_close(pacha_fd_t fd)
{
    assert(fd>=100);++closes;return 0;
}
int filed_sessions_release_client(filed_runtime_t *rt,struct filed_client *client)
{
    assert(rt && client && client->retiring && client->revoked);
    assert(observed.opcode==KB2_FILESYSTEM_OP_CLIENT_UNREGISTER && observed.client==client->id);
    ++released;return 0;
}
int main(void)
{
    uint8_t bytes[KB2_FILESYSTEM_REQUEST_SIZE+KB2_FILESYSTEM_CREDENTIALS_SIZE+8]={0};
    kb2_fs_credentials_t credentials={.uid=1001,.gid=1002,.euid=1001,.egid=1002,
        .suid=1001,.sgid=1002,.fsuid=1001,.fsgid=1002,.umask=0027};
    assert(!kb2_fs_credentials_encode(bytes+KB2_FILESYSTEM_REQUEST_SIZE,
        KB2_FILESYSTEM_CREDENTIALS_SIZE,&credentials));
    bytes[KB2_FILESYSTEM_REQUEST_SIZE+KB2_FILESYSTEM_CREDENTIALS_SIZE]=29;
    bytes[KB2_FILESYSTEM_REQUEST_SIZE+KB2_FILESYSTEM_CREDENTIALS_SIZE+4]=31;
    kb2_fs_request_t request={.opcode=KB2_FILESYSTEM_OP_CLIENT_REGISTER,.generation=1,
        .credential_generation=1,.second_handle=555,.offset=1234,
        .capabilities=FILED_RIGHT_LOOKUP|FILED_RIGHT_READ,
        .data={KB2_FILESYSTEM_REQUEST_SIZE,KB2_FILESYSTEM_CREDENTIALS_SIZE},
        .groups={KB2_FILESYSTEM_REQUEST_SIZE+KB2_FILESYSTEM_CREDENTIALS_SIZE,8}};
    filed_runtime_t runtime={.storage_generation=21,.client_sequence=1};
    pacha_fd_t fd=-1;uint64_t id=0;
    request.capabilities|=UINT64_C(1)<<40;
    assert(filed_client_create(&runtime,&request,bytes,sizeof(bytes),&fd,&id)==-EINVAL);
    assert(!calls && !channels && runtime.client_sequence==1);
    request.capabilities&=~(UINT64_C(1)<<40);
    assert(!filed_client_create(&runtime,&request,bytes,sizeof(bytes),&fd,&id));
    assert(fd==101 && id==2 && runtime.clients && runtime.clients->fd==100);
    assert(observed.client==2 && observed.generation==21 && observed.sequence==1);
    assert(runtime.clients->pid==1234 && runtime.clients->subject_generation==555);
    runtime.actor=runtime.clients;
    assert(filed_client_create(&runtime,&request,bytes,sizeof(bytes),&fd,&id)==-EPERM);
    request.opcode=KB2_FILESYSTEM_OP_CLIENT_CREDENTIALS;request.client=2;
    assert(filed_client_credentials(&runtime,&request,bytes,sizeof(bytes))==-EPERM);
    runtime.actor=NULL;
    assert(filed_client_credentials(&runtime,&request,bytes,sizeof(bytes))==-EPERM);
    request.credential_generation=2;request.offset=1235;
    assert(filed_client_credentials(&runtime,&request,bytes,sizeof(bytes))==-EPERM);
    request.offset=1234;request.capabilities|=FILED_RIGHT_WRITE;
    service_error=-EINVAL;
    assert(filed_client_credentials(&runtime,&request,bytes,sizeof(bytes))==-EINVAL);
    assert(runtime.clients->credential_generation==1);
    assert(!(runtime.clients->rights & FILED_RIGHT_WRITE));
    service_error=0;
    assert(!filed_client_credentials(&runtime,&request,bytes,sizeof(bytes)));
    assert(runtime.clients->credential_generation==2);
    assert(runtime.clients->rights & FILED_RIGHT_WRITE);
    assert(filed_client_credentials(&runtime,&request,bytes,sizeof(bytes))==-EPERM);
    request.client=999;
    assert(filed_client_credentials(&runtime,&request,bytes,sizeof(bytes))==-ESRCH);
    request.opcode=KB2_FILESYSTEM_OP_CLIENT_REGISTER;request.client=0;
    service_error=-ENOMEM;
    assert(filed_client_create(&runtime,&request,bytes,sizeof(bytes),&fd,&id)==-ENOMEM);
    assert(runtime.client_sequence==3 && closes==2);
    service_error=0;
    assert(!filed_client_create(&runtime,&request,bytes,sizeof(bytes),&fd,&id));
    assert(id==4 && fd==105 && runtime.clients->next->id==2);
    unsigned prior=calls;
    bytes[KB2_FILESYSTEM_REQUEST_SIZE+KB2_FILESYSTEM_CREDENTIALS_RESERVED_OFFSET]=1;
    assert(filed_client_create(&runtime,&request,bytes,sizeof(bytes),&fd,&id)==-EINVAL);
    assert(calls==prior);
    bytes[KB2_FILESYSTEM_REQUEST_SIZE+KB2_FILESYSTEM_CREDENTIALS_RESERVED_OFFSET]=0;
    assert(!filed_client_release(&runtime,runtime.clients));
    assert(released==1 && observed.opcode==KB2_FILESYSTEM_OP_CLIENT_UNREGISTER && observed.client==4);
    assert(runtime.clients->id==2 && runtime.storage_generation==21);
    runtime.actor=runtime.clients;service_error=-EIO;
    assert(filed_client_release(&runtime,runtime.clients)==-EIO);
    assert(runtime.clients && runtime.clients->retiring && !runtime.clients->revoked);
    assert(!runtime.actor && !runtime.storage_generation);
    assert(released==1 && closes==3);
    assert(filed_client_release(&runtime,runtime.clients)==-EIO);
    assert(released==1 && closes==3);
    /* Test-only disposal after the simulated sandbox has stopped; production
     * retains this inventory while its retirement remains uncertain. */
    free(runtime.clients);
    runtime=(filed_runtime_t){.storage_generation=21,.client_sequence=1};
    request.opcode=KB2_FILESYSTEM_OP_CLIENT_REGISTER;
    request.client=0;request.credential_generation=1;
    request.capabilities=FILED_RIGHT_LOOKUP|FILED_RIGHT_READ;
    service_error=0;
    assert(!filed_client_create(&runtime,&request,bytes,sizeof(bytes),&fd,&id));
    request.opcode=KB2_FILESYSTEM_OP_CLIENT_CREDENTIALS;
    request.client=id;request.credential_generation=2;request.capabilities|=FILED_RIGHT_WRITE;
    transport_error=-EIO;
    assert(filed_client_credentials(&runtime,&request,bytes,sizeof(bytes))==-EIO);
    assert(!runtime.storage_generation && runtime.clients->credential_generation==1 &&
        !(runtime.clients->rights & FILED_RIGHT_WRITE));
    free(runtime.clients); /* Simulated generation stopped; no real capabilities exist. */
    puts("filed trusted credentials / atomic rights updates / nonreused client IDs / retirement: PASS");
}
