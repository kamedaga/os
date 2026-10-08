/* SPDX-License-Identifier: MIT */
#include "filed_client/client.h"
#include "pacha/ipc.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *memory;
static size_t size;
static int failure;
static unsigned reply_closes;
static struct pacha_ipc_msg sent;
int pacha_vmo_create(uint64_t bytes,uint64_t rights,uint32_t flags)
{
    assert(!flags && rights&PACHA_FD_RIGHT_MAP_WRITE);
    if(failure==1) return -ENOMEM;
    size=(size_t)bytes;memory=calloc(1,size);assert(memory);return 80;
}
void *pacha_mmap(int fd,uint64_t bytes,uint64_t prot,uint64_t flags,uint64_t offset)
{
    assert(fd==80 && bytes==size && prot==(PACHA_PROT_READ|PACHA_PROT_WRITE));
    assert(flags==PACHA_MMAP_SHARED && !offset);return memory;
}
int pacha_munmap(void *mapping,uint64_t bytes)
{
    assert(mapping==memory && bytes==size);return 0;
}
int pacha_fd_close(int fd)
{
    if(fd==80) { free(memory);memory=NULL; }
    else if(fd==82) ++reply_closes;
    else assert(fd==81);
    return 0;
}
int pacha_fd_get_info(int fd,struct pacha_fd_info *info)
{
    (void)fd;(void)info;assert(0);return -EINVAL;
}
int pacha_ipc_call(int endpoint,const struct pacha_ipc_msg *message)
{
    assert(endpoint==40 && message->fd_count==1 && message->fds[0].fd==80);
    sent=*message;return failure==2 ? -ENOMEM : 81;
}
int pacha_ipc_recv_wait(int fd,struct pacha_ipc_msg *reply,uint64_t timeout)
{
    assert(fd==81 && timeout==PACHA_FD_WAIT_FOREVER);
    if(failure==3) return -EIO;
    kb2_fs_request_t request;
    assert(!kb2_fs_request_decode(memory,(size_t)sent.word1,sent.word1,&request));
    kb2_fs_response_t response={.opcode=request.opcode,.generation=request.generation,
        .sequence=request.sequence,.result=20000,.data_length=20000};
    assert(sent.word2==KB2_FILESYSTEM_RESPONSE_SIZE+20000);
    assert(!kb2_fs_response_encode(memory,size,&response));
    memset(memory+KB2_FILESYSTEM_RESPONSE_SIZE,'Q',20000);
    *reply=(struct pacha_ipc_msg){.word0=sent.word0,.word1=20000,
        .word2=sent.word2,.word3=sent.word3,.fds=reply->fds,.fd_capacity=reply->fd_capacity};
    if(failure==4) ++reply->word3;
    if(failure==5) { reply->fd_count=1;reply->fds[0].fd=82; }
    return 0;
}
int main(void)
{
    kb2_fs_request_t request={.opcode=KB2_FILESYSTEM_OP_PREAD,.generation=1,.sequence=7,
        .handle=90,.length=20000};
    kb2_fs_response_t response;
    uint8_t output[20000];int published=9;
    assert(!filed_client_call_published(40,&request,NULL,0,output,sizeof(output),
        &response,NULL,0,NULL,NULL,&published));
    assert(published==1 && response.result==20000 && response.data_length==sizeof(output));
    for(size_t i=0;i<sizeof(output);++i) assert(output[i]=='Q');
    for(failure=1;failure<=5;++failure) {
        published=9;
        int status=filed_client_call_published(40,&request,NULL,0,output,sizeof(output),
            &response,NULL,0,NULL,NULL,&published);
        assert(status==(failure<=2 ? -ENOMEM : failure==3 ? -EIO : -EPROTO));
        assert(published==(failure<=2 ? 0 : 1));
        assert(!memory);
    }
    assert(reply_closes==1);
    puts("filed native client raw 20KB / publication ambiguity / malformed reply FD cleanup: PASS");
}
