/* SPDX-License-Identifier: MIT */
#include "../src/relay.c"
#include <assert.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/stat.h>

static filed_runtime_t runtime;
static struct filed_client actor, other;
static struct ph_fs_channel_config configured;
static uint64_t next_id;
static unsigned reservations, binds, releases, closes[100], mapped_control, release_step;
static int bind_error, release_error, close_phase, reply_error;
static kb2_fs_request_t incoming_request;
static unsigned char control[4096];
static size_t incoming_sources;
static int replied;
static unsigned route_calls, route_closes;
static int route_error;
static uint32_t route_major;

int filed_storage_owner_client_config(struct filed_storage_owner *owner, uint64_t client,
    uint64_t minimum, struct ph_fs_channel_config *config)
{
    assert(owner == (void *)1234 && client == actor.id);
    ++reservations;
    configured = (struct ph_fs_channel_config){.generation=77,.channel_id=++next_id,
        .queue_size=8,.arena_bytes=minimum>4096 ? minimum : 4096};
    *config=configured; return 0;
}
int filed_storage_owner_client_bind(struct filed_storage_owner *owner,
    const struct ph_fs_channel_config *config,int vmo,int doorbell)
{
    assert(owner == (void *)1234 && config->generation==77 && config->channel_id==next_id);
    assert(vmo==90 && doorbell==91 && !release_step);
    ++binds; return bind_error;
}
int filed_storage_owner_client_release(struct filed_storage_owner *owner,uint64_t id)
{
    assert(owner==(void *)1234 && id==configured.channel_id && release_step==0);
    ++releases; ++release_step; return release_error;
}
int pacha_fd_get_info(int fd,struct pacha_fd_info *info)
{
    if(fd==70) { *info=(struct pacha_fd_info){.kind=PACHA_FD_KIND_REPLY}; return 0; }
    if(fd==80) { *info=(struct pacha_fd_info){.kind=PACHA_FD_KIND_VMO,.size=sizeof(control)}; return 0; }
    assert(fd==90 || fd==91);
    size_t size,offset;
    assert(!ph_fs_channel_layout(&configured,&size,&offset));
    *info=(struct pacha_fd_info){.kind=fd==90 ? PACHA_FD_KIND_VMO : PACHA_FD_KIND_CHANNEL,
        .size=size,.rights=PACHA_FD_RIGHT_CLOSE|PACHA_FD_RIGHT_TRANSFER|
        (fd==90 ? PACHA_FD_RIGHT_INSPECT|PACHA_FD_RIGHT_MAP_READ|PACHA_FD_RIGHT_MAP_WRITE :
        PH_FS_CLIENT_DOORBELL_RIGHTS)};
    return 0;
}
void *pacha_mmap(int fd,uint64_t size,uint64_t prot,uint64_t flags,uint64_t offset)
{
    /* filed must never map the client queue payload, only the control VMO. */
    assert(fd==80 && size==sizeof(control) && !offset);
    assert(prot==(PACHA_PROT_READ|PACHA_PROT_WRITE) && flags==PACHA_MMAP_SHARED);
    ++mapped_control; return control;
}
int pacha_munmap(void *p,uint64_t size)
{ assert(p==control && size==sizeof(control)); return 0; }
int pacha_fd_close(int fd)
{
    assert(fd==70 || fd==80 || fd==90 || fd==91);
    ++closes[fd];
    if(fd==90 || fd==91) {
        /* Local backing cannot be released until the remote quiescence ACK. */
        assert(release_step==(unsigned)(fd==90 ? 1 : 2));
        ++release_step;
        if(close_phase==(fd==90 ? 2 : 3)) return -EBUSY;
    }
    return 0;
}
int pacha_ipc_recv(int fd,struct pacha_ipc_msg *message)
{
    assert(fd==60 && message->fd_capacity>=4);
    message->word0=incoming_request.opcode;message->word1=KB2_FILESYSTEM_REQUEST_SIZE;
    message->word2=KB2_FILESYSTEM_RESPONSE_SIZE;message->word3=incoming_request.sequence;
    message->fd_count=incoming_sources+2;
    message->fds[0]=(struct pacha_ipc_fd){.fd=80};
    for(size_t i=0;i<incoming_sources;++i) message->fds[i+1]=(struct pacha_ipc_fd){.fd=90+i};
    message->fds[message->fd_count-1]=(struct pacha_ipc_fd){.fd=70};
    return 0;
}
int pacha_ipc_reply(int fd,const struct pacha_ipc_msg *message)
{
    assert(fd==70 && !message->fd_count && message->word3==incoming_request.sequence);
    ++replied; return reply_error;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
int filed_storage_exchange(filed_runtime_t *a,const uint8_t *b,size_t c,uint8_t *d,size_t e,size_t *f) { abort(); }
int filed_storage_request(filed_runtime_t *a,kb2_fs_request_t *b,const void *c,size_t d,kb2_fs_response_t *e,void *f,size_t g) { abort(); }
int filed_storage_stat(filed_runtime_t *a,uint64_t b,filed_storage_stat_t *c) { abort(); }
int filed_storage_device_route(filed_runtime_t *a,kb2_fs_request_t *b,const void *c,size_t d,filed_storage_stat_t *e,uint64_t *f,uint8_t *g)
{
    assert(a==&runtime && b->opcode==KB2_FILESYSTEM_OP_DEVICE_ROUTE &&
        b->flags==(O_CREAT|O_TRUNC|O_RDWR) && b->mode==0600);
    assert(d==sizeof("/alias") && !memcmp(c,"/alias",d));
    ++route_calls;
    if(route_error) return route_error;
    *e=(filed_storage_stat_t){.mode=S_IFCHR|0600,.rdev_major=route_major,.rdev_minor=128};
    *f=432;
    assert(g);memset(g,0,KB2_FILESYSTEM_STATX_SIZE);
    wr32(g+KB2_FILESYSTEM_STATX_RDEV_MAJOR_OFFSET,route_major);
    wr32(g+KB2_FILESYSTEM_STATX_RDEV_MINOR_OFFSET,128);
    return 0;
}
int filed_storage_pread(filed_runtime_t *a,uint64_t b,uint64_t c,void *d,size_t e,size_t *f) { abort(); }
int filed_storage_close(filed_runtime_t *a,uint64_t b) { assert(a==&runtime && b==432);++route_closes;return 0; }
int filed_storage_executable_mapping(filed_runtime_t *a,uint64_t b) { abort(); }
int filed_storage_exec_open(filed_runtime_t *a,uint64_t b,const char *c,uint64_t *d) { abort(); }
int filed_storage_exec_close(filed_runtime_t *a,uint64_t b) { abort(); }
int filed_client_create(filed_runtime_t *a,const kb2_fs_request_t *b,const uint8_t *c,size_t d,int *e,uint64_t *f) { abort(); }
int filed_client_credentials(filed_runtime_t *a,const kb2_fs_request_t *b,const uint8_t *c,size_t d) { abort(); }
int filed_client_release(filed_runtime_t *a,struct filed_client *b) { abort(); }
int filed_owner_control(filed_runtime_t *a,kb2_fs_request_t *b,const uint8_t *c,size_t d,kb2_fs_response_t *e) { abort(); }
int pacha_vmo_create(uint64_t a,uint64_t b,uint32_t c) { abort(); }
long pacha_fd_fcntl(int a,uint64_t b,uint64_t c,uint64_t d) { abort(); }
long pacha_syscall2(uint64_t a,uint64_t b,uint64_t c) { abort(); }
int filed_exec_handle(filed_runtime_t *a,filed_handle_id_t b,const filed_exec_path_t *c,const int *d,uint64_t e,int f,int *g,int *h) { abort(); }
int filed_exec_linux_lpr_prepare_self(filed_runtime_t *a,filed_handle_id_t b,const filed_exec_path_t *c,int *d,int *e) { abort(); }
#pragma GCC diagnostic pop

static kb2_fs_response_t dispatch(kb2_fs_request_t *r,const int *sources,size_t count)
{
    uint8_t bytes[KB2_FILESYSTEM_REQUEST_SIZE]={0},out[128];size_t used=0,fd_count=0;int fds[3];
    assert(!kb2_fs_request_encode(bytes,sizeof(bytes),sizeof(bytes),r));
    assert(!dispatch_wire(&runtime,r,bytes,sizeof(bytes),out,sizeof(out),&used,sources,count,fds,&fd_count));
    kb2_fs_response_t response;
    assert(!fd_count && !kb2_fs_response_decode(out,used,&response));
    assert(kb2_fs_response_matches(&response,r));return response;
}
static void setup(void)
{
    assert(!runtime.sessions);
    actor=(struct filed_client){.id=9,.credential_generation=7,.rights=1023,.fd=60};
    other=(struct filed_client){.id=10,.rights=1023};
    runtime=(filed_runtime_t){.actor=&actor,.clients=&actor,.storage=(void *)1234,
        .storage_generation=77};
    next_id=99;reservations=binds=releases=release_step=mapped_control=0;
    bind_error=release_error=close_phase=reply_error=replied=0;
    memset(closes,0,sizeof(closes));
}
static kb2_fs_request_t reserve(uint64_t minimum)
{
    kb2_fs_request_t hello={.opcode=KB2_FILESYSTEM_OP_HELLO,.generation=1,.sequence=1,
        .flags=1,.length=minimum};
    kb2_fs_response_t response=dispatch(&hello,NULL,0);
    assert(!response.result && response.data_length==32 && response.handle==100);
    assert(runtime.sessions && !runtime.sessions->attached && !mapped_control && !binds);
    return (kb2_fs_request_t){.opcode=KB2_FILESYSTEM_OP_SESSION_ATTACH,.generation=77,
        .sequence=2,.second_handle=100,.mask=8,.length=configured.arena_bytes};
}
static kb2_fs_request_t detach_request(void)
{
    return (kb2_fs_request_t){.opcode=KB2_FILESYSTEM_OP_SESSION_DETACH,.generation=77,
        .sequence=3,.second_handle=100,.client=UINT64_MAX,.credential_generation=UINT64_MAX};
}
static void cleanup(void)
{
    while(runtime.sessions) { struct filed_session *s=runtime.sessions;
        runtime.sessions=s->next;free(s); }
}
static void native_receive(kb2_fs_request_t request,size_t sources)
{
    incoming_request=request;incoming_sources=sources;
    assert(!kb2_fs_request_encode(control,KB2_FILESYSTEM_REQUEST_SIZE,KB2_FILESYSTEM_REQUEST_SIZE,&request));
    assert(filed_receive(&runtime,60)==reply_error);
    assert(replied==1 && closes[70]==1 && closes[80]==1 && mapped_control==1);
}
static void test_device_route(void)
{
    setup();runtime.gpud_drm_endpoint_fd=55;route_major=226;
    kb2_fs_request_t r={.opcode=KB2_FILESYSTEM_OP_DEVICE_ROUTE,.generation=77,.sequence=30,
        .flags=O_CREAT|O_TRUNC|O_RDWR,.mode=0600,.path={KB2_FILESYSTEM_REQUEST_SIZE,sizeof("/alias")}};
    uint8_t bytes[KB2_FILESYSTEM_REQUEST_SIZE+sizeof("/alias")],out[KB2_FILESYSTEM_RESPONSE_SIZE+KB2_FILESYSTEM_STATX_SIZE];
    assert(!kb2_fs_request_encode(bytes,KB2_FILESYSTEM_REQUEST_SIZE,sizeof(bytes),&r));
    memcpy(bytes+KB2_FILESYSTEM_REQUEST_SIZE,"/alias",sizeof("/alias"));
    size_t used=0,count=0;int fds[3];kb2_fs_response_t response;
    assert(!dispatch_wire(&runtime,&r,bytes,sizeof(bytes),out,sizeof(out),&used,NULL,0,fds,&count));
    assert(!kb2_fs_response_decode(out,used,&response) && !response.result && !count &&
        response.handle==432 && response.service==FILED_SERVICE_GPUD_DRM &&
        response.data_length==KB2_FILESYSTEM_STATX_SIZE && used==sizeof(out));
    assert(out[KB2_FILESYSTEM_RESPONSE_SIZE+KB2_FILESYSTEM_STATX_RDEV_MAJOR_OFFSET]==226 && route_calls==1);
    actor.rights=FILED_RIGHT_LOOKUP|FILED_RIGHT_READ;
    assert(!dispatch_wire(&runtime,&r,bytes,sizeof(bytes),out,sizeof(out),&used,NULL,0,fds,&count));
    assert(!kb2_fs_response_decode(out,used,&response) && response.result==-EACCES && route_calls==1);
    actor.rights=1023;route_major=250;
    assert(!dispatch_wire(&runtime,&r,bytes,sizeof(bytes),out,sizeof(out),&used,NULL,0,fds,&count));
    assert(!kb2_fs_response_decode(out,used,&response) && response.result==-ENODEV &&
        !response.handle && !response.data_length && route_calls==2 && route_closes==1);
    route_error=-EACCES;
    assert(!dispatch_wire(&runtime,&r,bytes,sizeof(bytes),out,sizeof(out),&used,NULL,0,fds,&count));
    assert(!kb2_fs_response_decode(out,used,&response) && response.result==-EACCES && route_calls==3);
    route_error=0;
}
int main(void)
{
    int sources[2]={90,91};
    setup(); kb2_fs_request_t attach=reserve(65536);
    assert(attach.length==65536 && !dispatch(&attach,sources,2).result && binds==1);
    assert(!mapped_control && runtime.sessions->attached);
    assert(dispatch(&attach,sources,2).result==-EEXIST);
    kb2_fs_request_t detach=detach_request();
    runtime.actor=&other; assert(dispatch(&detach,NULL,0).result==-ENOENT && !releases);
    runtime.actor=&actor;detach.generation=78;
    assert(dispatch(&detach,NULL,0).result==-ESTALE && !releases);detach.generation=77;
#define BAD(field) do { kb2_fs_request_t bad=detach;bad.field=1; \
    assert(dispatch(&bad,NULL,0).result==-EINVAL && !releases); } while(0)
    BAD(handle);BAD(offset);BAD(length);BAD(flags);BAD(mode);BAD(resolve);
    BAD(mask);BAD(uid);BAD(gid);BAD(umask);BAD(capabilities);
#undef BAD
    assert(dispatch(&detach,NULL,1).result==-EINVAL && !releases);
    assert(!dispatch(&detach,NULL,0).result && !runtime.sessions && release_step==3);
    assert(dispatch(&detach,NULL,0).result==-ENOENT);
    /* Ordinary client RPC cannot re-enter filed's VFS relay. */
    const uint32_t ops[]={KB2_FILESYSTEM_OP_OPENAT2,KB2_FILESYSTEM_OP_STATX,
        KB2_FILESYSTEM_OP_PREAD,KB2_FILESYSTEM_OP_PWRITE,KB2_FILESYSTEM_OP_CLOSE,
        KB2_FILESYSTEM_OP_GETDENTS64,KB2_FILESYSTEM_OP_SYNC};
    for(size_t i=0;i<sizeof(ops)/sizeof(*ops);++i) {
        kb2_fs_request_t r={.opcode=ops[i],.generation=77,.sequence=4};
        assert(dispatch(&r,NULL,0).result==-EPERM);
    }
    setup();attach=reserve(0);native_receive(attach,2);
    assert(!closes[90] && !closes[91] && binds==1 && runtime.sessions->attached);cleanup();
    setup();attach=reserve(0);bind_error=-EIO;native_receive(attach,2);
    assert(!runtime.storage_generation && runtime.sessions && !closes[90] && !closes[91]);cleanup();
    setup();attach=reserve(0);reply_error=-EPIPE;native_receive(attach,2);
    assert(runtime.sessions && !closes[90] && !closes[91]);cleanup();
    for(int phase=1;phase<=3;++phase) {
        setup();attach=reserve(0);assert(!dispatch(&attach,sources,2).result);
        if(phase==1) release_error=-EBUSY;else close_phase=phase;
        detach=detach_request();
        assert(dispatch(&detach,NULL,0).result==-EBUSY);
        assert(!runtime.storage_generation && runtime.sessions && release_step==(unsigned)phase);
        assert(runtime.sessions->remote_released==(phase!=1));
        assert(runtime.sessions->vmo_fd==(phase<=2 ? 90 : -1));cleanup();
    }
    /* An unbound reservation also retires, without touching any queue VMO. */
    setup();(void)reserve(0);detach=detach_request();assert(!dispatch(&detach,NULL,0).result);
    assert(releases==1 && release_step==1 && !runtime.sessions && !mapped_control);
    test_device_route();
    puts("filed direct session control: no payload mapping/relay, registered binding, sandbox ACK before local close, source retention/lost reply PASS");
    return 0;
}
