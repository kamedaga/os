/* SPDX-License-Identifier: MIT */
#include "../src/relay.c"
#include "no_session_control.h"
#include <assert.h>
#include <stdio.h>

static uint8_t memory[1024];
static size_t request_size;
static unsigned prepared, guards_opened, guards_closed, duplicated, replied, killed;
static unsigned closed[100];
static uint64_t source_rights[100];
static int expect_result, duplicate_error, reply_error, close_error_fd;
static int close_error, kill_error;
static const uint64_t manifest_recipient=PACHA_FD_RIGHT_INSPECT|PACHA_FD_RIGHT_CLOSE|
    PACHA_FD_RIGHT_MAP_READ|PACHA_FD_RIGHT_DUP|PACHA_FD_RIGHT_SET_FLAGS;

/* Model the actual native rules, not POSIX DUP's inherit-all convention.
 * A zero-rights duplicate is legal; IPC requires TRANSFER on its source and
 * forbids granting any bit the source does not possess. */
static long native_duplicate(uint64_t source,uint64_t requested,uint64_t *copy)
{
    if (!(source&PACHA_FD_RIGHT_DUP) || (requested&~source)) return 1;
    *copy=requested;return 96;
}
static int native_grant(uint64_t source,uint64_t requested)
{
    return !(source&PACHA_FD_RIGHT_TRANSFER) || (requested&~source) ? -1 : 0;
}
int pacha_ipc_recv(pacha_fd_t fd,struct pacha_ipc_msg *message)
{
    assert(fd==60 && message->fd_capacity>=3);
    message->word0=KB2_FILESYSTEM_OP_EXEC_SELF;
    message->word1=request_size;message->word2=KB2_FILESYSTEM_RESPONSE_SIZE;
    message->word3=7;message->fd_count=2;
    message->fds[0]=(struct pacha_ipc_fd){.fd=80};
    message->fds[1]=(struct pacha_ipc_fd){.fd=81};
    return 0;
}
int pacha_fd_get_info(pacha_fd_t fd,struct pacha_fd_info *info)
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
int filed_storage_exec_open(filed_runtime_t *rt,uint64_t dir,const char *path,uint64_t *handle)
{
    assert(!rt->actor && dir==123 && !strcmp(path,"/bin/true"));
    ++guards_opened;*handle=42;return 0;
}
int filed_storage_exec_close(filed_runtime_t *rt,uint64_t handle)
{
    assert(!rt->actor && handle==42);++guards_closed;return 0;
}
int filed_exec_linux_lpr_prepare_self(filed_runtime_t *rt,filed_handle_id_t handle,
    const filed_exec_path_t *request,pacha_fd_t *process,pacha_fd_t *thread)
{
    assert(!rt->actor && handle==42 && !strcmp(request->path,"/bin/true"));
    assert(request->flags==(FILED_EXEC_LINUX_LPR|FILED_EXEC_BOOTSTRAP_FD|FILED_EXEC_SELF));
    assert(!request->inherit_fd_count && !request->inherit_handle_count && !request->fd_patch_count);
    /* The real process/thread creators possess these source rights. The
     * relay must attenuate them rather than assume a process-shaped VMO. */
    source_rights[94]=PACHA_FD_RIGHT_INSPECT|PACHA_FD_RIGHT_TRANSFER|
        PACHA_FD_RIGHT_CLOSE|PACHA_FD_RIGHT_WAIT|PACHA_FD_RIGHT_POLL|
        PACHA_FD_RIGHT_KILL|PACHA_FD_RIGHT_SPAWN|PACHA_FD_RIGHT_MAP_INTO|
        PACHA_FD_RIGHT_SET_CONTEXT;
    source_rights[95]=PACHA_FD_RIGHT_INSPECT|PACHA_FD_RIGHT_TRANSFER|
        PACHA_FD_RIGHT_CLOSE|PACHA_FD_RIGHT_WAIT|PACHA_FD_RIGHT_KILL|
        PACHA_FD_RIGHT_START|PACHA_FD_RIGHT_SET_CONTEXT;
    ++prepared;*process=94;*thread=95;return 0;
}
long pacha_fd_fcntl(pacha_fd_t fd,uint64_t command,uint64_t minimum,uint64_t rights)
{
    assert(fd==81 && command==PACHA_FD_FCNTL_DUP && minimum==16);
    assert(rights==(manifest_recipient|PACHA_FD_RIGHT_TRANSFER));
    assert(!(rights&(PACHA_FD_RIGHT_MAP_WRITE|PACHA_FD_RIGHT_MAP_EXEC)));
    ++duplicated;
    if(duplicate_error) return duplicate_error;
    return native_duplicate(source_rights[81],rights,&source_rights[96]);
}
int pacha_ipc_reply(pacha_fd_t fd,const struct pacha_ipc_msg *message)
{
    if (reply_error && !message->word0) return reply_error;
    assert(fd==PACHA_IPC_REPLY_IMPLICIT);++replied;
    assert(message->word0==KB2_FILESYSTEM_OP_EXEC_SELF && message->word3==7);
    assert(message->word2==KB2_FILESYSTEM_RESPONSE_SIZE);
    assert((int64_t)message->word1==expect_result);
    kb2_fs_response_t decoded;
    assert(!kb2_fs_response_decode(memory,KB2_FILESYSTEM_RESPONSE_SIZE,&decoded));
    assert(decoded.opcode==KB2_FILESYSTEM_OP_EXEC_SELF && decoded.generation==1);
    assert(decoded.sequence==7 && decoded.result==expect_result && !decoded.handle);
    if(expect_result) {
        assert(!message->fd_count);return reply_error;
    }
    assert(message->fd_count==3);
    const uint64_t common=PACHA_FD_RIGHT_INSPECT|PACHA_FD_RIGHT_CLOSE|
        PACHA_FD_RIGHT_WAIT|PACHA_FD_RIGHT_SET_CONTEXT;
    const uint64_t expected[3]={common|PACHA_FD_RIGHT_TRANSFER|PACHA_FD_RIGHT_POLL|
        PACHA_FD_RIGHT_KILL|PACHA_FD_RIGHT_MAP_INTO,
        common|PACHA_FD_RIGHT_KILL,manifest_recipient};
    for(unsigned i=0;i<3;++i) {
        const struct pacha_ipc_fd *cap=&message->fds[i];
        assert(cap->fd==94+i && cap->rights==expected[i] && !cap->flags);
        assert(cap->transfer_flags==(PACHA_IPC_TRANSFER_MOVE|PACHA_IPC_TRANSFER_CLOEXEC));
        assert(!native_grant(source_rights[cap->fd],cap->rights));
    }
    assert(!(message->fds[2].rights&PACHA_FD_RIGHT_TRANSFER));
    if(reply_error) return reply_error;
    for(unsigned i=0;i<3;++i) source_rights[94+i]=0;
    return 0;
}
int pacha_fd_close(pacha_fd_t fd)
{
    assert(fd==70 || fd==80 || fd==81 || fd==94 || fd==95 || fd==96);
    ++closed[fd];
    if(fd==close_error_fd) return close_error;
    source_rights[fd]=0;return 0;
}
long pacha_syscall2(uint64_t syscall,uint64_t fd,uint64_t status)
{
    assert(syscall==PACHA_PROCESS_SYSCALL_KILL && fd==94 && status==1);
    ++killed;return kill_error;
}

/* The real relay dispatch is linked; any unrelated branch is forbidden. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
int filed_storage_exchange(filed_runtime_t *a,const uint8_t *b,size_t c,uint8_t *d,size_t e,size_t *f) { abort(); }
int filed_storage_request(filed_runtime_t *a,kb2_fs_request_t *b,const void *c,size_t d,kb2_fs_response_t *e,void *f,size_t g) { abort(); }
int filed_storage_stat(filed_runtime_t *a,uint64_t b,filed_storage_stat_t *c) { abort(); }
int filed_storage_pread(filed_runtime_t *a,uint64_t b,uint64_t c,void *d,size_t e,size_t *f) { abort(); }
int filed_storage_close(filed_runtime_t *a,uint64_t b) { abort(); }
int filed_storage_executable_mapping(filed_runtime_t *a,uint64_t b) { abort(); }
int filed_client_create(filed_runtime_t *a,const kb2_fs_request_t *b,const uint8_t *c,size_t d,pacha_fd_t *e,uint64_t *f) { abort(); }
int filed_client_credentials(filed_runtime_t *a,const kb2_fs_request_t *b,const uint8_t *c,size_t d) { abort(); }
int filed_client_release(filed_runtime_t *a,struct filed_client *b) { abort(); }
int filed_owner_control(filed_runtime_t *a,kb2_fs_request_t *b,const uint8_t *c,size_t d,kb2_fs_response_t *e) { abort(); }
pacha_fd_t pacha_vmo_create(uint64_t a,uint64_t b,uint32_t c) { abort(); }
int ph_fs_channel_layout(const struct ph_fs_channel_config *a,size_t *b,size_t *c) { abort(); }
int ph_fs_channel_bind(struct ph_fs_channel *a,void *b,size_t c,const struct ph_fs_channel_config *d,kb2_vq_side_t e,const kb2_vq_atomic_ops_t *f,kb2_vq_chain_t **g,uint16_t *h) { abort(); }
const kb2_vq_atomic_ops_t kb2_vq_x86_64_atomics={0};
int filed_exec_handle(filed_runtime_t *a,filed_handle_id_t b,const filed_exec_path_t *c,const pacha_fd_t *d,uint64_t e,pacha_fd_t f,pacha_fd_t *g,pacha_fd_t *h) { abort(); }
#pragma GCC diagnostic pop

static void setup(void)
{
    memset(memory,0,sizeof(memory));memset(closed,0,sizeof(closed));
    memset(source_rights,0,sizeof(source_rights));
    prepared=guards_opened=guards_closed=duplicated=replied=killed=0;
    expect_result=duplicate_error=reply_error=close_error_fd=close_error=kill_error=0;
    source_rights[81]=manifest_recipient|PACHA_FD_RIGHT_TRANSFER;
    const char path[]="/bin/true";
    filed_exec_path_t exec={.flags=FILED_EXEC_LINUX_LPR|FILED_EXEC_BOOTSTRAP_FD|FILED_EXEC_SELF};
    uint8_t *payload=NULL;size_t payload_size=0;
    assert(!filed_exec_payload_encode(&exec,&payload,&payload_size));
    const size_t path_offset=KB2_FILESYSTEM_REQUEST_SIZE;
    const size_t payload_offset=path_offset+sizeof(path);
    request_size=payload_offset+payload_size;assert(request_size<=sizeof(memory));
    kb2_fs_request_t request={.opcode=KB2_FILESYSTEM_OP_EXEC_SELF,
        .generation=1,.sequence=7,.handle=123,.flags=exec.flags,
        .path={path_offset,sizeof(path)},.data={payload_offset,payload_size}};
    assert(!kb2_fs_request_encode(memory,request_size,request_size,&request));
    memcpy(memory+path_offset,path,sizeof(path));
    memcpy(memory+payload_offset,payload,payload_size);free(payload);
}
static void check_consumed(void)
{
    assert(prepared==1 && guards_opened==1 && guards_closed==1 && duplicated==1 && replied==1);
    assert(closed[70]==0 && closed[80]==1 && closed[81]==1);
}
static void check_unwound(void)
{
    check_consumed();assert(killed==1 && closed[94]==1 && closed[95]==1 && !closed[96]);
}
int main(void)
{
    uint64_t copy=UINT64_MAX;
    assert(native_duplicate(manifest_recipient|PACHA_FD_RIGHT_TRANSFER,0,&copy)==96 && !copy);
    assert(native_grant(copy,manifest_recipient)==-1); /* Previous zero-DUP defect. */
    assert(native_grant(manifest_recipient,manifest_recipient)==-1); /* Missing source TRANSFER. */
    assert(native_grant(manifest_recipient|PACHA_FD_RIGHT_TRANSFER,
        manifest_recipient|PACHA_FD_RIGHT_MAP_WRITE)==-1); /* Extra recipient authority. */
    filed_runtime_t runtime={0};
    setup();assert(!filed_receive(&runtime,60));check_consumed();
    assert(!killed && !closed[94] && !closed[95] && !closed[96]);
    setup();source_rights[81]|=PACHA_FD_RIGHT_MAP_WRITE|PACHA_FD_RIGHT_MAP_EXEC;
    assert(!filed_receive(&runtime,60));check_consumed(); /* Source extras are attenuated. */
    const uint64_t missing[]={PACHA_FD_RIGHT_DUP,PACHA_FD_RIGHT_TRANSFER,
        PACHA_FD_RIGHT_INSPECT,PACHA_FD_RIGHT_MAP_READ,PACHA_FD_RIGHT_SET_FLAGS};
    for(unsigned i=0;i<sizeof(missing)/sizeof(missing[0]);++i) {
        setup();source_rights[81]&=~missing[i];expect_result=-EIO;
        assert(!filed_receive(&runtime,60));check_unwound();
        assert(!source_rights[94] && !source_rights[95] && !source_rights[96]);
    }
    setup();duplicate_error=3;expect_result=-EIO;
    assert(!filed_receive(&runtime,60));check_unwound();
    setup();duplicate_error=-ENOMEM;expect_result=-ENOMEM;
    assert(!filed_receive(&runtime,60));check_unwound();
    setup();duplicate_error=-ENOMEM;kill_error=1;expect_result=-EIO;
    assert(!filed_receive(&runtime,60));check_unwound(); /* Positive cleanup rejection surfaced. */
    setup();duplicate_error=-ENOMEM;close_error_fd=94;close_error=-EBADF;expect_result=-EBADF;
    assert(!filed_receive(&runtime,60));check_unwound();
    assert(source_rights[94] && !source_rights[95]); /* Failed close is NOT claimed clean. */
    setup();reply_error=-EIO;assert(filed_receive(&runtime,60)==-EIO);check_consumed();
    assert(!killed && closed[94]==1 && closed[95]==1 && closed[96]==1);
    assert(!source_rights[94] && !source_rights[95] && !source_rights[96]);
    puts("filed actual EXEC_SELF reply: explicit native DUP / exact source and recipient rights / negative errors / no partial replacement / failed cleanup / failed-send unwind PASS");
}

int filed_storage_snapshot_stat(filed_runtime_t *rt,uint64_t handle,filed_storage_stat_t *out)
{ return filed_storage_stat(rt,handle,out); }
int filed_storage_snapshot_pread(filed_runtime_t *rt,uint64_t handle,uint64_t offset,
    void *data,size_t length,size_t *got,const filed_storage_stat_t *expected,int *valid)
{ (void)expected;*valid=0;return filed_storage_pread(rt,handle,offset,data,length,got); }
