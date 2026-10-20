/* SPDX-License-Identifier: MIT */
#include "../src/relay.c"
#include "no_session_control.h"
#include "filed/client_rights.h"
#include <assert.h>
#include <stdio.h>

static uint8_t memory[KB2_FILESYSTEM_REQUEST_SIZE];
static unsigned created, replies, closed_request, closed_reply, closed_endpoint;
static int reply_error, expect_result;
static kb2_fs_response_t observed;

int pacha_ipc_recv(pacha_fd_t fd, struct pacha_ipc_msg *message)
{
    assert(fd==60 && message->fd_capacity>=2);
    message->word0=KB2_FILESYSTEM_OP_CLIENT_REGISTER;
    message->word1=sizeof(memory);message->word2=KB2_FILESYSTEM_RESPONSE_SIZE;
    message->word3=7;message->fd_count=1;
    message->fds[0]=(struct pacha_ipc_fd){.fd=80};
    return 0;
}
int pacha_fd_get_info(pacha_fd_t fd, struct pacha_fd_info *info)
{
    assert(fd==70 || fd==80);
    *info=(struct pacha_fd_info){.kind=fd==70 ? PACHA_FD_KIND_REPLY : PACHA_FD_KIND_VMO,
        .size=sizeof(memory)};return 0;
}
void *pacha_mmap(pacha_fd_t fd,uint64_t size,uint64_t protection,uint64_t flags,uint64_t offset)
{
    assert(fd==80 && size==sizeof(memory) && !offset);
    assert(protection==(PACHA_PROT_READ|PACHA_PROT_WRITE) && flags==PACHA_MMAP_SHARED);
    return memory;
}
int pacha_munmap(void *address,uint64_t size)
{
    assert(address==memory && size==sizeof(memory));return 0;
}
int filed_client_create(filed_runtime_t *rt,const kb2_fs_request_t *request,
    const uint8_t *bytes,size_t size,pacha_fd_t *fd,uint64_t *id)
{
    assert(!rt->actor && size==sizeof(memory));
    kb2_fs_request_t decoded;
    assert(!kb2_fs_request_decode(bytes,size,size,&decoded));
    assert(decoded.opcode==KB2_FILESYSTEM_OP_CLIENT_REGISTER && decoded.sequence==7);
    assert(request->offset==1234 && request->second_handle==555);
    ++created;*fd=90;*id=42;return 0;
}
int pacha_ipc_reply(pacha_fd_t fd,const struct pacha_ipc_msg *message)
{
    if (reply_error && !message->word0) return reply_error;
    assert(fd==PACHA_IPC_REPLY_IMPLICIT);++replies;
    assert(message->word0==KB2_FILESYSTEM_OP_CLIENT_REGISTER && message->word3==7);
    assert(message->word2==KB2_FILESYSTEM_RESPONSE_SIZE);
    assert((int64_t)message->word1==expect_result);
    assert(!kb2_fs_response_decode(memory,KB2_FILESYSTEM_RESPONSE_SIZE,&observed));
    assert(observed.opcode==KB2_FILESYSTEM_OP_CLIENT_REGISTER && observed.generation==1);
    assert(observed.sequence==7 && observed.result==expect_result);
    if(expect_result) {
        assert(!message->fd_count && !observed.handle);
    } else {
        assert(observed.handle==42 && message->fd_count==1);
        const struct pacha_ipc_fd *cap=&message->fds[0];
        const uint64_t expected=PACHA_FD_RIGHT_INSPECT|PACHA_FD_RIGHT_CLOSE|
            PACHA_FD_RIGHT_CALL|PACHA_FD_RIGHT_WAIT|PACHA_FD_RIGHT_POLL|
            PACHA_FD_RIGHT_DUP|PACHA_FD_RIGHT_TRANSFER;
        assert(cap->fd==90 && cap->rights==expected);
        assert(cap->rights==FILED_CLIENT_REGISTER_RIGHTS);
        assert(!(cap->rights&(PACHA_FD_RIGHT_SET_FLAGS|PACHA_FD_RIGHT_RECV|PACHA_FD_RIGHT_SEND)));
        assert(!cap->flags && cap->transfer_flags==
            (PACHA_IPC_TRANSFER_MOVE|PACHA_IPC_TRANSFER_CLOEXEC));
    }
    return reply_error;
}
int pacha_fd_close(pacha_fd_t fd)
{
    if(fd==70) ++closed_reply;
    else if(fd==80) ++closed_request;
    else { assert(fd==90);++closed_endpoint; }
    return 0;
}

/* These branches are linked from the real dispatch switch, but this test
 * admits only CLIENT_REGISTER. Unexpected FS/exec/session activity is fatal. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
int filed_storage_exchange(filed_runtime_t *a,const uint8_t *b,size_t c,uint8_t *d,size_t e,size_t *f) { abort(); }
int filed_storage_request(filed_runtime_t *a,kb2_fs_request_t *b,const void *c,size_t d,kb2_fs_response_t *e,void *f,size_t g) { abort(); }
int filed_storage_stat(filed_runtime_t *a,uint64_t b,filed_storage_stat_t *c) { abort(); }
int filed_storage_pread(filed_runtime_t *a,uint64_t b,uint64_t c,void *d,size_t e,size_t *f) { abort(); }
int filed_storage_close(filed_runtime_t *a,uint64_t b) { abort(); }
int filed_storage_executable_mapping(filed_runtime_t *a,uint64_t b) { abort(); }
int filed_storage_exec_open(filed_runtime_t *a,uint64_t b,const char *c,uint64_t *d) { abort(); }
int filed_storage_exec_close(filed_runtime_t *a,uint64_t b) { abort(); }
int filed_client_credentials(filed_runtime_t *a,const kb2_fs_request_t *b,const uint8_t *c,size_t d) { abort(); }
int filed_client_release(filed_runtime_t *a,struct filed_client *b) { abort(); }
int filed_owner_control(filed_runtime_t *a,kb2_fs_request_t *b,const uint8_t *c,size_t d,kb2_fs_response_t *e) { abort(); }
pacha_fd_t pacha_vmo_create(uint64_t a,uint64_t b,uint32_t c) { abort(); }
long pacha_fd_fcntl(pacha_fd_t a,uint64_t b,uint64_t c,uint64_t d) { abort(); }
long pacha_syscall2(uint64_t a,uint64_t b,uint64_t c) { abort(); }
int ph_fs_channel_layout(const struct ph_fs_channel_config *a,size_t *b,size_t *c) { abort(); }
int ph_fs_channel_bind(struct ph_fs_channel *a,void *b,size_t c,const struct ph_fs_channel_config *d,kb2_vq_side_t e,const kb2_vq_atomic_ops_t *f,kb2_vq_chain_t **g,uint16_t *h) { abort(); }
const kb2_vq_atomic_ops_t kb2_vq_x86_64_atomics={0};
int filed_exec_handle(filed_runtime_t *a,filed_handle_id_t b,const filed_exec_path_t *c,const pacha_fd_t *d,uint64_t e,pacha_fd_t f,pacha_fd_t *g,pacha_fd_t *h) { abort(); }
int filed_exec_linux_lpr_prepare_self(filed_runtime_t *a,filed_handle_id_t b,const filed_exec_path_t *c,pacha_fd_t *d,pacha_fd_t *e) { abort(); }
#pragma GCC diagnostic pop

static void encode_request(void)
{
    kb2_fs_request_t request={.opcode=KB2_FILESYSTEM_OP_CLIENT_REGISTER,
        .generation=1,.sequence=7,.offset=1234,.second_handle=555};
    assert(!kb2_fs_request_encode(memory,sizeof(memory),sizeof(memory),&request));
}
int main(void)
{
    filed_runtime_t runtime={0};
    encode_request();assert(!filed_receive(&runtime,60));
    assert(created==1 && replies==1 && closed_request==1 && closed_reply==0 && !closed_endpoint);
    encode_request();reply_error=-EIO;assert(filed_receive(&runtime,60)==-EIO);
    assert(created==2 && replies==2 && closed_request==2 && closed_reply==0 && closed_endpoint==1);
    encode_request();reply_error=0;expect_result=-EPERM;
    struct filed_client client={0};runtime.actor=&client;
    assert(!filed_receive(&runtime,60));
    assert(created==2 && replies==3 && closed_request==3 && closed_reply==0 && closed_endpoint==1);
    puts("filed actual CLIENT_REGISTER reply: exact least-privilege rights / CLOEXEC / failed-send cleanup / private authority PASS");
}
