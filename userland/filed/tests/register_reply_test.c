/* SPDX-License-Identifier: MIT */
#include "../src/relay.c"
#include "no_audio_control.h"
#define FILED_TEST_SHARED_CONTROL
#include "no_session_control.h"
#include "filed/client_rights.h"
#include <assert.h>
#include <stdio.h>

static uint8_t memory[KB2_FILESYSTEM_REQUEST_SIZE];
static unsigned created, replies, closed_request, closed_reply, closed_endpoint;
static int reply_error;
static int64_t expect_result, shared_result=8192;
static kb2_fs_response_t observed;
static uint32_t opcode=KB2_FILESYSTEM_OP_CLIENT_REGISTER;
static uint64_t shared_rights;
static unsigned idle_waits, expired_refs;static int idle_test;
static int cache_reply_test, exec_error;static unsigned cache_dups, cache_closes[4];
static uint64_t cache_flags, cache_rights;
static const filed_storage_stat_t cache_stat={.inode=7,.filesystem=1,.size=8192,
    .mode=0100644,.inode_generation=0,.change_cookie=0,.snapshot_valid=1,.nlink=1};

int pacha_ipc_recv(pacha_fd_t fd, struct pacha_ipc_msg *message)
{
    assert(fd==60 && message->fd_capacity>=2);
    message->word0=opcode;
    message->word1=sizeof(memory);message->word2=KB2_FILESYSTEM_RESPONSE_SIZE;
    message->word3=7;message->fd_count=1;
    message->fds[0]=(struct pacha_ipc_fd){.fd=80};
    return 0;
}
int pacha_fd_get_info(pacha_fd_t fd, struct pacha_fd_info *info)
{
    if (fd==90) {
        assert(opcode==KB2_FILESYSTEM_OP_SHARED_FILE_VMO);
        *info=(struct pacha_fd_info){.kind=PACHA_FD_KIND_VMO,
            .size=8192,.rights=shared_rights,.flags=PACHA_FD_FLAG_CLOEXEC};
        return 0;
    }
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
    assert(message->word0==opcode && message->word3==7);
    assert(message->word2==KB2_FILESYSTEM_RESPONSE_SIZE);
    assert((int64_t)message->word1==expect_result);
    assert(!kb2_fs_response_decode(memory,KB2_FILESYSTEM_RESPONSE_SIZE,&observed));
    assert(observed.opcode==opcode && observed.generation==1);
    assert(observed.sequence==7 && observed.result==expect_result);
    if (opcode==KB2_FILESYSTEM_OP_FILE_VMO) {
        if (expect_result<0) assert(!message->fd_count && !observed.handle);
        else {
            assert(message->fd_count==1 && observed.handle==4096);
            assert(message->fds[0].fd==90+cache_dups && message->fds[0].rights==cache_rights);
            assert(!(message->fds[0].rights&PACHA_FD_RIGHT_MAP_WRITE));
        }
    } else if(opcode==KB2_FILESYSTEM_OP_SHARED_FILE_VMO) {
        assert(expect_result==shared_result && message->fd_count==1 && !observed.handle);
        assert(message->fds[0].fd==90 && message->fds[0].rights==shared_rights);
        assert(!(message->fds[0].rights & (PACHA_FD_RIGHT_REVOKE|PACHA_FD_RIGHT_WAIT|PACHA_FD_RIGHT_KILL)));
        assert(message->fds[0].transfer_flags==(PACHA_IPC_TRANSFER_MOVE|PACHA_IPC_TRANSFER_CLOEXEC));
    } else if(expect_result) {
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
    if (cache_reply_test && fd>=91 && fd<=93) { ++cache_closes[fd-91];return 0; }
    if (fd==91) { assert(idle_test);++expired_refs;return 0; }
    if(fd==70) ++closed_reply;
    else if(fd==80) ++closed_request;
    else { assert(fd==90);++closed_endpoint; }
    return 0;
}

/* Unexpected FS/exec/session activity is fatal. Shared exports below exercise
 * the real response capability branch as well as the control dispatch. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
int filed_storage_exchange(filed_runtime_t *a,const uint8_t *b,size_t c,uint8_t *d,size_t e,size_t *f) { abort(); }
int filed_storage_request(filed_runtime_t *a,kb2_fs_request_t *b,const void *c,size_t d,kb2_fs_response_t *e,void *f,size_t g) { abort(); }
int filed_storage_stat(filed_runtime_t *a,uint64_t b,filed_storage_stat_t *c) { assert(cache_reply_test && a && b==90);*c=cache_stat;return 0; }
int filed_storage_pread(filed_runtime_t *a,uint64_t b,uint64_t c,void *d,size_t e,size_t *f) { abort(); }
int filed_storage_close(filed_runtime_t *a,uint64_t b) { abort(); }
int filed_storage_executable_mapping(filed_runtime_t *a,uint64_t b) { assert(cache_reply_test && a && b==90);return exec_error; }
int filed_storage_exec_open(filed_runtime_t *a,uint64_t b,const char *c,uint64_t *d) { abort(); }
int filed_storage_exec_close(filed_runtime_t *a,uint64_t b) { abort(); }
int filed_client_credentials(filed_runtime_t *a,const kb2_fs_request_t *b,const uint8_t *c,size_t d) { abort(); }
int filed_client_release(filed_runtime_t *a,struct filed_client *b) { abort(); }
int filed_owner_control(filed_runtime_t *a,kb2_fs_request_t *b,const uint8_t *c,size_t d,kb2_fs_response_t *e) {
    assert(opcode==KB2_FILESYSTEM_OP_SHARED_FILE_VMO && a->actor && !c && d==KB2_FILESYSTEM_REQUEST_SIZE);
    assert(b->opcode==opcode && b->client==9 && b->credential_generation==7);
    assert(b->handle==90 && !b->offset && b->length==8192 && b->flags==3 && b->mask==1);
    b->sequence=101;*e=(kb2_fs_response_t){.result=shared_result,.handle=55};return 0;
}
int filed_storage_owner_take_shared(struct filed_storage_owner *a,uint64_t id,
    uint64_t sequence,uint64_t length,pacha_fd_t *fd) {
    assert(opcode==KB2_FILESYSTEM_OP_SHARED_FILE_VMO && id==55 && sequence==101 && length==8192);
    *fd=90;return 0;
}
int filed_storage_owner_cancel_shared(struct filed_storage_owner *a) { abort(); }
pacha_fd_t pacha_vmo_create(uint64_t a,uint64_t b,uint32_t c) { abort(); }
long pacha_fd_fcntl(pacha_fd_t a,uint64_t b,uint64_t c,uint64_t d) {
    if (cache_reply_test) {
        assert((a==90 || a==91) && b==PACHA_FD_FCNTL_DUP && c==16 &&
            !(d&PACHA_FD_RIGHT_MAP_WRITE));return 91+cache_dups++;
    }
    assert(idle_test && a==90 && b==PACHA_FD_FCNTL_DUP && c==16 &&
        !(d&PACHA_FD_RIGHT_MAP_WRITE));return 91;
}
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
    if(opcode==KB2_FILESYSTEM_OP_SHARED_FILE_VMO)
        request=(kb2_fs_request_t){.opcode=opcode,.generation=1,.sequence=7,
            .client=9,.credential_generation=7,.handle=90,.length=8192,.flags=3,.mask=1};
    if(opcode==KB2_FILESYSTEM_OP_FILE_VMO)
        request=(kb2_fs_request_t){.opcode=opcode,.generation=1,.sequence=7,
            .client=9,.credential_generation=7,.handle=90,.offset=4096,.length=4096,.flags=cache_flags};
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
    opcode=KB2_FILESYSTEM_OP_SHARED_FILE_VMO;expect_result=8192;reply_error=0;
    client=(struct filed_client){.id=9,.credential_generation=7,.rights=1023};
    runtime.storage_generation=31;
    const uint64_t base=PACHA_FD_RIGHT_INSPECT|PACHA_FD_RIGHT_CLOSE|
        PACHA_FD_RIGHT_TRANSFER|PACHA_FD_RIGHT_DUP|PACHA_FD_RIGHT_MAP_READ;
    shared_rights=base|PACHA_FD_RIGHT_MAP_WRITE|PACHA_FD_RIGHT_MAP_EXEC;
    encode_request();assert(!filed_receive(&runtime,60));
    shared_rights=base; // Linux seals/noexec attenuated both requested ceilings.
    encode_request();assert(!filed_receive(&runtime,60));
    encode_request();reply_error=-EIO;assert(filed_receive(&runtime,60)==-EIO);
    assert(closed_endpoint==2);
    reply_error=0;
    const int64_t sizes[]={INT64_C(2147483647),INT64_C(2147483648),
        INT64_C(3221225472),INT64_C(4294971392)};
    for(size_t i=0;i<sizeof(sizes)/sizeof(sizes[0]);++i) {
        shared_result=expect_result=sizes[i];
        encode_request();assert(!filed_receive(&runtime,60));
    }
    puts("filed 64-bit VMO reply 2GiB-1/2GiB/3GiB/4GiB+4KiB: PASS");
    cache_reply_test=1;cache_flags=FILED_FILE_VMO_EXEC|FILED_FILE_VMO_WRITE;
    opcode=KB2_FILESYSTEM_OP_FILE_VMO;reply_error=0;expect_result=4096;
    runtime.snapshots.limit=8192;runtime.snapshots.generation=runtime.storage_generation;
    filed_snapshot_cache_put_variant(&runtime.snapshots,&cache_stat,0,8192,90,1);
    assert(runtime.snapshots.count==1 && cache_dups==1);
    cache_rights=base|PACHA_FD_RIGHT_MAP_EXEC;
    encode_request();assert(!filed_receive(&runtime,60));
    assert(runtime.snapshots.hits==1 && cache_dups==2);
    exec_error=-EACCES;expect_result=-EACCES;
    encode_request();assert(!filed_receive(&runtime,60));
    assert(runtime.snapshots.hits==1 && cache_dups==2);
    exec_error=0;
    expect_result=4096;reply_error=-EIO;
    encode_request();assert(filed_receive(&runtime,60)==-EIO);
    assert(cache_dups==3 && cache_closes[2]==1 && !cache_closes[0]);
    filed_snapshot_cache_clear(&runtime.snapshots);pacha_fd_close(92);
    assert(cache_closes[0]==1 && cache_closes[1]==1 && cache_closes[2]==1);
    puts("filed cached FILE_VMO reply: containment offset, per-call noexec, no MAP_WRITE/unchecked EXEC, failed MOVE cleanup: PASS");
    cache_reply_test=0;idle_test=1;
    filed_runtime_t idle={.storage_generation=1,.client_endpoint_fd=60,.unix_path_fd=-1};
    idle.snapshots.limit=4096;
    const filed_storage_stat_t cached={.inode=7,.filesystem=1,.size=4096,
        .inode_generation=1,.change_cookie=1,.snapshot_valid=1,.nlink=1};
    filed_snapshot_cache_put(&idle.snapshots,&cached,0,4096,90);
    assert(idle.snapshots.count==1);
    assert(filed_runtime_serve(&idle)==-EINTR && idle_waits==1 && idle.snapshots.count==1 && !expired_refs);
    filed_snapshot_cache_clear(&idle.snapshots);
    assert(expired_refs==1);
    puts("filed idle waits forever with retained snapshots and no clock read: PASS");
    puts("filed actual SHARED_FILE_VMO reply: VMO rights, Linux attenuation, MOVE/CLOEXEC and failed-send cleanup PASS");
}

long pacha_clock_read(uint64_t id,uint64_t *ns)
{ (void)id;(void)ns;abort(); }
long pacha_fd_wait_many_batched(struct pacha_pollfd *fds,uint64_t count,uint64_t timeout)
{
    assert(idle_test && count==1 && fds[0].fd==60);
    ++idle_waits;assert(timeout==PACHA_FD_WAIT_FOREVER && !expired_refs);return -EINTR;
}
long pacha_fd_wait_many_retained(struct pacha_fd_wait_retained *retained,
    struct pacha_pollfd *fds,uint64_t count,uint64_t timeout)
{
    assert(retained != NULL);
    return pacha_fd_wait_many_batched(fds,count,timeout);
}
int filed_unix_path_receive(filed_runtime_t *rt) { (void)rt;abort(); }

int filed_storage_snapshot_stat(filed_runtime_t *rt,uint64_t handle,filed_storage_stat_t *out)
{ return filed_storage_stat(rt,handle,out); }
int filed_storage_snapshot_pread(filed_runtime_t *rt,uint64_t handle,uint64_t offset,
    void *data,size_t length,size_t *got,const filed_storage_stat_t *expected,int *valid)
{ (void)expected;*valid=0;return filed_storage_pread(rt,handle,offset,data,length,got); }

int pacha_fd_table(uint64_t minimum,struct pacha_fd_table_info *out)
{ assert(!minimum && idle_test);*out=(struct pacha_fd_table_info){.capacity=512,.maximum=4096,.free_slots=256};return 0; }
