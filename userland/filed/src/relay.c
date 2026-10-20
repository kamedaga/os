/* SPDX-License-Identifier: MIT */
#include "filed/runtime.h"
#include "filed/exec.h"
#include "filed/exec_linux_lpr.h"
#include "filed/ipc_protocol.h"
#include "filed/client_rights.h"
#include "filed/storage_owner.h"
#include "filed/payload.h"
#include "pacha/ipc.h"
#include "pacha/syscall.h"
#include "../../kobox2_owner/fs_channel.h"
#include "filed/unix_path.h"
#include <kobox2/virtqueue_x86_64.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#ifdef FILED_INTERNAL_MICROBENCH
#include "../tests/internal_microbench_clock.h"
#endif
#ifdef PH_FS_CHANNEL_PROFILE
#include "../tests/ring_profile.h"
#endif

struct filed_session {
    struct filed_session *next;
    struct filed_client *client;
    struct ph_fs_channel_config config;
    pacha_fd_t vmo_fd, doorbell_fd;
    int attached, remote_released;
};
struct filed_lease {
    struct filed_lease *next;
    uint64_t id, handle, client;
    pacha_fd_t fd;
};
static int authorize(filed_runtime_t *rt,const kb2_fs_request_t *r)
{
    if (!rt->actor) return 0;
    if (rt->actor->retiring) return -EACCES;
    uint64_t required;
    kb2_fs_operation_class_t class=kb2_fs_request_policy(r,&required);
    if (class==KB2_FS_OPERATION_INVALID) return -EOPNOTSUPP;
    if (class==KB2_FS_OPERATION_OWNER) return -EPERM;
    return required & ~rt->actor->rights ? -EACCES : 0;
}
static int relay(filed_runtime_t *rt,const kb2_fs_request_t *input,
    const uint8_t *bytes,size_t size,uint8_t *output,size_t capacity,size_t *used)
{
    /* Ordinary authenticated clients own a direct sandbox queue. The native
     * manager still uses this private backend for bootstrap/control work. */
    if (rt->actor) return -EPERM;
    uint8_t *copy=malloc(size);
    if (!copy) return -ENOMEM;
    memcpy(copy,bytes,size);
    kb2_fs_request_t request=*input;
    request.generation=rt->storage_generation;
    request.client=rt->actor ? rt->actor->id : 1;
    request.credential_generation=rt->actor ? rt->actor->credential_generation : 1;
    request.capabilities=0;
    request.groups=(kb2_fs_span_t){0};
    if (rt->request_sequence==UINT64_MAX) { free(copy); return -EOVERFLOW; }
    request.sequence=++rt->request_sequence;
    int status=kb2_fs_request_encode(copy,KB2_FILESYSTEM_REQUEST_SIZE,size,&request);
    if (!status) status=filed_storage_exchange(rt,copy,size,output,capacity,used);
    kb2_fs_response_t response;
    if (!status && (kb2_fs_response_decode(output,*used,&response) ||
                    !kb2_fs_response_matches(&response,&request))) status=-EPROTO;
    if (!status) {
        response.generation=input->generation;
        response.sequence=input->sequence;
        status=kb2_fs_response_encode(output,capacity,&response);
    }
    free(copy);
    return status;
}
static int make_file_vmo(filed_runtime_t *rt,const kb2_fs_request_t *r,
                         kb2_fs_response_t *response,pacha_fd_t *fd)
{
    if (r->flags & ~(uint64_t)(FILED_FILE_VMO_WRITE|FILED_FILE_VMO_EXEC) ||
        r->offset < 0 || !r->length || r->length > SIZE_MAX-4095)
        return -EINVAL;
    if(r->length>INT64_MAX) return -EOVERFLOW;
    if(r->flags & FILED_FILE_VMO_EXEC) {
        int status=filed_storage_executable_mapping(rt,r->handle);
        if(status) return status;
    }
    filed_storage_stat_t stat;
#ifdef FILED_INTERNAL_MICROBENCH
    uint64_t bench_stage=filed_bench_collect ? filed_bench_tsc() : 0;
#endif
    int status=filed_storage_stat(rt,r->handle,&stat);
    if (status) return status;
    if ((stat.mode & 0170000) != 0100000) return -ENODEV;
    uint64_t length=r->length;
    if ((uint64_t)r->offset > stat.size) return -EINVAL;
    if (length > stat.size-(uint64_t)r->offset)
        length=stat.size-(uint64_t)r->offset;
    if (!length) return -EINVAL;
    size_t mapped=((size_t)r->length+4095)&~(size_t)4095;
    uint64_t rights=PACHA_FD_RIGHT_INSPECT|PACHA_FD_RIGHT_CLOSE|
        PACHA_FD_RIGHT_TRANSFER|PACHA_FD_RIGHT_DUP|
        PACHA_FD_RIGHT_MAP_READ|PACHA_FD_RIGHT_MAP_WRITE;
    if (r->flags & FILED_FILE_VMO_EXEC) rights|=PACHA_FD_RIGHT_MAP_EXEC;
#ifdef FILED_INTERNAL_MICROBENCH
    FILED_BENCH_STAGE(filed_bench_snapshot_cycles,0,bench_stage);
#endif
    pacha_fd_t vmo=pacha_vmo_create(mapped,rights,0);
    if (vmo<16) return vmo;
#ifdef FILED_INTERNAL_MICROBENCH
    FILED_BENCH_STAGE(filed_bench_snapshot_cycles,1,bench_stage);
#endif
    uint8_t *memory=pacha_mmap(vmo,mapped,PACHA_PROT_READ|PACHA_PROT_WRITE,
                               PACHA_MMAP_SHARED,0);
    if (!memory) { (void)pacha_fd_close(vmo); return -ENOMEM; }
#ifdef FILED_INTERNAL_MICROBENCH
    FILED_BENCH_STAGE(filed_bench_snapshot_cycles,2,bench_stage);
#endif
    /* VMO_CREATE eagerly supplies zeroed pages. Preserve its untouched EOF
     * tail instead of writing the whole image a second time before PREAD. */
#ifdef FILED_INTERNAL_MICROBENCH
    FILED_BENCH_STAGE(filed_bench_snapshot_cycles,3,bench_stage);
#endif
    size_t done=0;
    while (!status && done<length) {
        size_t got=0;
        status=filed_storage_pread(rt,r->handle,(uint64_t)r->offset+done,
                                    memory+done,(size_t)length-done,&got);
        if (!status && !got) break;
        done+=got;
    }
#ifdef FILED_INTERNAL_MICROBENCH
    FILED_BENCH_STAGE(filed_bench_snapshot_cycles,4,bench_stage);
#endif
    (void)pacha_munmap(memory,mapped);
#ifdef FILED_INTERNAL_MICROBENCH
    FILED_BENCH_STAGE(filed_bench_snapshot_cycles,5,bench_stage);
#endif
    if (status) { (void)pacha_fd_close(vmo); return status; }
    response->result=(int64_t)done;
    *fd=vmo;
    return 0;
}
static void wr64(uint8_t *p,uint64_t value)
{
    for (unsigned i=0;i<8;++i) p[i]=(uint8_t)(value>>(8*i));
}
static void wr32(uint8_t *p,uint32_t value)
{
    for (unsigned i=0;i<4;++i) p[i]=(uint8_t)(value>>(8*i));
}
static int route_device(filed_runtime_t *rt,const kb2_fs_request_t *request,
    const uint8_t *bytes,size_t size,kb2_fs_response_t *response,uint8_t *statx)
{
    kb2_fs_request_t probe=*request;
    probe.opcode=KB2_FILESYSTEM_OP_DEVICE_ROUTE;
    uint64_t handle=0;
    filed_storage_stat_t stat;
    int status=filed_storage_device_route(rt,&probe,bytes+KB2_FILESYSTEM_REQUEST_SIZE,
        size-KB2_FILESYSTEM_REQUEST_SIZE,&stat,&handle,statx);
    if(status) return status;
    uint32_t service=0;
    if(!status && (stat.mode&0170000)==0020000) {
        if(stat.rdev_major==226 && rt->gpud_drm_endpoint_fd>=16)
            service=FILED_SERVICE_GPUD_DRM;
        else if(stat.rdev_major==13 && rt->inputd_input_endpoint_fd>=16)
            service=FILED_SERVICE_INPUTD_INPUT;
        else if((stat.rdev_major==4 || stat.rdev_major==5) &&
            rt->termd_tty_endpoint_fd>=16) service=FILED_SERVICE_TERMD_TTY;
    }
    if(status || !service) {
        int closed=filed_storage_close(rt,handle);
        if (closed) return closed;
        return status ? status : -ENODEV;
    }
    response->result=0;response->handle=handle;response->service=service;
    response->data_length=statx ? KB2_FILESYSTEM_STATX_SIZE : 0;
    return 0;
}
static int exec_request(filed_runtime_t *rt,const kb2_fs_request_t *r,
    const uint8_t *bytes,const pacha_fd_t *sources,size_t source_count,
    pacha_fd_t *fds,size_t *fd_count)
{
    if (!r->path.length || r->data.length < KB2_FILESYSTEM_EXEC_PAYLOAD_SIZE)
        return -EINVAL;
    filed_exec_path_t request;
    int status=filed_exec_payload_decode(bytes+r->data.offset,(size_t)r->data.length,
        (const char *)bytes+r->path.offset,r->handle,r->flags,&request);
    if (status) return status;
    uint64_t known=(1ull<<12)-1;
    size_t expected=(size_t)request.inherit_fd_count+
        !!(request.flags & FILED_EXEC_BOOTSTRAP_FD);
    if (request.flags & ~known || expected != source_count ||
        request.inherit_fd_count > PACHA_PROCESS_CREATE_MAX_GRANTS-7 ||
        (request.inherit_fd_count && !(request.flags & FILED_EXEC_INHERIT_FDS)) ||
        (request.fd_patch_count && !(request.flags & FILED_EXEC_PATCH_BOOTSTRAP_FDS)) ||
        (request.inherit_handle_count && !(request.flags & FILED_EXEC_INHERIT_HANDLES))) {
        status=-EINVAL; goto done;
    }
    pacha_fd_t bootstrap=request.flags & FILED_EXEC_BOOTSTRAP_FD ?
        sources[request.inherit_fd_count] : -1;
    /* Inherited handles are references in this authenticated Linux client.
     * Duplicates share struct file offsets; there is no userspace f_pos. */
    uint64_t *handles=request.inherit_handle_count ?
        calloc((size_t)request.inherit_handle_count,sizeof(*handles)) : NULL;
    if (request.inherit_handle_count && !handles) { status=-ENOMEM; goto done; }
    for (uint64_t i=0; i<request.inherit_handle_count; ++i) {
        kb2_fs_request_t dup={.opcode=KB2_FILESYSTEM_OP_DUP,
            .handle=request.inherit_handles[i]};
        kb2_fs_response_t response;
        status=filed_storage_request(rt,&dup,NULL,0,&response,NULL,0);
        if (status) break;
        handles[i]=response.handle;
    }
    if (!status && request.fd_patch_count) {
        struct pacha_fd_info info;
        if (bootstrap<16 || pacha_fd_get_info(bootstrap,&info) ||
            info.kind != PACHA_FD_KIND_VMO || info.size < 8 ||
            info.size > SIZE_MAX) status=-EINVAL;
        void *mapping=!status ? pacha_mmap(bootstrap,info.size,
            PACHA_PROT_READ|PACHA_PROT_WRITE,PACHA_MMAP_SHARED,0) : NULL;
        if (!status && !mapping) status=-ENOMEM;
        for (uint64_t i=0; !status && i<request.fd_patch_count; ++i) {
            const filed_exec_fd_patch_t *p=&request.fd_patches[i];
            uint64_t value=0;
            if (p->offset > info.size-8) { status=-EINVAL; break; }
            if (p->kind==FILED_EXEC_PATCH_INHERIT_FD && p->index<request.inherit_fd_count)
                value=request.fd_grants[p->index].target;
            else if (p->kind==FILED_EXEC_PATCH_BOOTSTRAP_FD && !p->index)
                value=request.flags & FILED_EXEC_LINUX_LPR ?
                    FILED_EXEC_LPR_BOOTSTRAP_FD : FILED_EXEC_NATIVE_BOOTSTRAP_FD;
            else if (p->kind==FILED_EXEC_PATCH_INHERIT_HANDLE && p->index<request.inherit_handle_count)
                value=handles[p->index];
            else { status=-EINVAL; break; }
            wr64((uint8_t *)mapping+p->offset,value);
        }
        if (mapping) (void)pacha_munmap(mapping,info.size);
    }
    uint64_t handle=0;
    if (!status) status=filed_storage_exec_open(rt,r->handle,
        (const char *)bytes+r->path.offset,&handle);
    pacha_fd_t process=-1,thread=-1;
    if (!status && r->opcode==KB2_FILESYSTEM_OP_EXEC_SELF) {
        if (!(request.flags & FILED_EXEC_LINUX_LPR) || source_count!=1 ||
            !(request.flags & FILED_EXEC_BOOTSTRAP_FD) ||
            request.inherit_fd_count || request.inherit_handle_count ||
            request.fd_patch_count)
            status=-EINVAL;
        else status=filed_exec_linux_lpr_prepare_self(rt,handle,&request,&process,&thread);
    } else if (!status) status=filed_exec_handle(rt,handle,&request,sources,
        request.inherit_fd_count,bootstrap,&process,&thread);
    if (handle) (void)filed_storage_exec_close(rt,handle);
    pacha_fd_t bootstrap_copy=-1;
    int bootstrap_copy_failed=0;
    if (!status && r->opcode==KB2_FILESYSTEM_OP_EXEC_SELF) {
        /* DUP attenuates explicitly: zero is not inherited authority. The
         * source also needs TRANSFER for IPC_REPLY; the recipient retains
         * SET_FLAGS for CLOEXEC but does not receive TRANSFER. */
        uint64_t rights=PACHA_FD_RIGHT_INSPECT|PACHA_FD_RIGHT_CLOSE|
            PACHA_FD_RIGHT_MAP_READ|PACHA_FD_RIGHT_DUP|
            PACHA_FD_RIGHT_SET_FLAGS|PACHA_FD_RIGHT_TRANSFER;
        long copy=pacha_fd_fcntl(bootstrap,PACHA_FD_FCNTL_DUP,16,rights);
        /* Native rejection codes are positive, not successful Linux results. */
        if (copy<16) {
            status=copy<0 ? (int)copy : -EIO;
            bootstrap_copy_failed=1;
        }
        else bootstrap_copy=(int)copy;
    }
    if (!status) {
        fds[0]=process; fds[1]=thread; *fd_count=2;
        if (bootstrap_copy>=16) { fds[2]=bootstrap_copy; *fd_count=3; }
    } else {
        int cleanup_status=0;
        if (process>=16) {
            long killed=pacha_syscall2(PACHA_PROCESS_SYSCALL_KILL,(uint64_t)process,1);
            if (killed) cleanup_status=killed<0 ? (int)killed : -EIO;
            int closed=pacha_fd_close(process);
            if (closed && !cleanup_status) cleanup_status=closed<0 ? closed : -EIO;
        }
        if (thread>=16) {
            int closed=pacha_fd_close(thread);
            if (closed && !cleanup_status) cleanup_status=closed<0 ? closed : -EIO;
        }
        /* Never claim the failed-copy unwind succeeded if a native cleanup
         * operation rejected it; still attempt every owned descriptor. */
        if (bootstrap_copy_failed && cleanup_status) status=cleanup_status;
    }
    if (status)
        for (uint64_t i=0;i<request.inherit_handle_count;++i)
            if (handles[i]) (void)filed_storage_close(rt,handles[i]);
    free(handles);
done:
    filed_exec_payload_destroy(&request);
    return status;
}
static int reserve_session(filed_runtime_t *rt,uint64_t minimum,
                          struct filed_session **out)
{
    if (!rt->actor) return -EPERM;
    struct filed_session *s=calloc(1,sizeof(*s));
    if (!s) return -ENOMEM;
    s->vmo_fd=s->doorbell_fd=-1;
    s->client=rt->actor;
    int status=filed_storage_owner_client_config(rt->storage,rt->actor->id,
        minimum,&s->config);
    if (status) {
        free(s);
        /* Pre-publication resource pressure is not a sandbox generation fault.
         * Existing clients continue and a released lease restores admission. */
        if (status!=-EINVAL && status!=-EOVERFLOW && status!=-ENOMEM && status!=-EAGAIN)
            rt->storage_generation=0;
        return status;
    }
    s->next=rt->sessions; rt->sessions=s;
    *out=s;
    return 0;
}
static int attach_session(filed_runtime_t *rt,const kb2_fs_request_t *r,
                           const pacha_fd_t *sources,size_t count)
{
    if (!rt->actor) return -EPERM;
    if (count!=2 || !r->second_handle || sources[0]==sources[1]) return -EINVAL;
    struct filed_session *s=rt->sessions;
    while (s && (s->client!=rt->actor || s->config.channel_id!=r->second_handle)) s=s->next;
    if (!s) return -ENOENT;
    if (s->attached) return -EEXIST;
    if (r->generation!=s->config.generation) return -ESTALE;
    if (r->mask!=s->config.queue_size || r->length!=s->config.arena_bytes) return -EINVAL;
    size_t size,offset;
    int status=ph_fs_channel_layout(&s->config,&size,&offset);
    if (status) return status;
    struct pacha_fd_info info;
    if (pacha_fd_get_info(sources[0],&info) || info.kind!=PACHA_FD_KIND_VMO ||
        info.size!=size || (info.rights & (PACHA_FD_RIGHT_INSPECT|PACHA_FD_RIGHT_TRANSFER|
        PACHA_FD_RIGHT_MAP_READ|PACHA_FD_RIGHT_MAP_WRITE)) !=
        (PACHA_FD_RIGHT_INSPECT|PACHA_FD_RIGHT_TRANSFER|PACHA_FD_RIGHT_MAP_READ|PACHA_FD_RIGHT_MAP_WRITE))
        return -EINVAL;
    if (pacha_fd_get_info(sources[1],&info) || info.kind!=PACHA_FD_KIND_CHANNEL ||
        (info.rights & (PH_FS_CLIENT_DOORBELL_RIGHTS|PACHA_FD_RIGHT_TRANSFER)) !=
        (PH_FS_CLIENT_DOORBELL_RIGHTS|PACHA_FD_RIGHT_TRANSFER)) return -EACCES;
    /* Record ambiguous transferred inventory before publication. There is no
     * filed-side mapping, descriptor consumption or payload rewriting. */
    s->attached=1; s->vmo_fd=sources[0]; s->doorbell_fd=sources[1];
    status=filed_storage_owner_client_bind(rt->storage,&s->config,sources[0],sources[1]);
    if (status) rt->storage_generation=0;
    return status;
}
static int release_session(filed_runtime_t *rt,struct filed_session *s)
{
    int status=0;
    if (!s->remote_released) {
        status=filed_storage_owner_client_release(rt->storage,s->config.channel_id);
        if (!status) s->remote_released=1;
    }
    if (!status && s->vmo_fd>=16) {
        status=pacha_fd_close(s->vmo_fd);
        if (!status) s->vmo_fd=-1;
    }
    if (!status && s->doorbell_fd>=16) {
        status=pacha_fd_close(s->doorbell_fd);
        if (!status) s->doorbell_fd=-1;
    }
    if (status) rt->storage_generation=0;
    return status;
}
static int detach_session(filed_runtime_t *rt,const kb2_fs_request_t *r,
                          size_t size,size_t source_count)
{
    if (size!=KB2_FILESYSTEM_REQUEST_SIZE || source_count || r->handle ||
        r->offset || r->length || r->flags || r->mode || r->resolve ||
        r->mask || r->uid || r->gid || r->umask || r->capabilities ||
        r->path.offset || r->path.length || r->second_path.offset ||
        r->second_path.length || r->groups.offset || r->groups.length ||
        r->data.offset || r->data.length) return -EINVAL;
    if (!rt->actor) return -EPERM;
    struct filed_session **cursor=&rt->sessions;
    while (*cursor && ((*cursor)->client!=rt->actor ||
        (*cursor)->config.channel_id!=r->second_handle)) cursor=&(*cursor)->next;
    if (!*cursor) return -ENOENT;
    struct filed_session *s=*cursor;
    if (r->generation!=s->config.generation) return -ESTALE;
    int status=release_session(rt,s);
    if (status) return status;
    *cursor=s->next;
    free(s);
    return 0;
}
static int dispatch_wire(filed_runtime_t *rt,const kb2_fs_request_t *r,
    const uint8_t *bytes,size_t size,uint8_t *out,size_t capacity,size_t *used,
    const pacha_fd_t *sources,size_t source_count,pacha_fd_t *fds,size_t *fd_count)
{
    kb2_fs_response_t response={.opcode=r->opcode,.generation=r->generation,
        .sequence=r->sequence};
    int status=authorize(rt,r);
    *fd_count=0;
    if (!status) switch(r->opcode) {
    case KB2_FILESYSTEM_OP_HELLO:
        if(r->flags==1) {
            if(capacity < KB2_FILESYSTEM_RESPONSE_SIZE+KB2_FILESYSTEM_QUEUE_CONFIG_SIZE)
                status=-ENOSPC;
            else {
                struct filed_session *s=NULL;
                status=reserve_session(rt,r->length,&s);
                if (status) break;
                uint8_t *config=out+KB2_FILESYSTEM_RESPONSE_SIZE;
                memset(config,0,KB2_FILESYSTEM_QUEUE_CONFIG_SIZE);
                wr64(config+KB2_FILESYSTEM_QUEUE_CONFIG_GENERATION_OFFSET,s->config.generation);
                wr64(config+KB2_FILESYSTEM_QUEUE_CONFIG_CHANNEL_ID_OFFSET,
                    s->config.channel_id);
                wr64(config+KB2_FILESYSTEM_QUEUE_CONFIG_ARENA_BYTES_OFFSET,
                    s->config.arena_bytes);
                wr32(config+KB2_FILESYSTEM_QUEUE_CONFIG_QUEUE_SIZE_OFFSET,
                    s->config.queue_size);
                response.data_length=KB2_FILESYSTEM_QUEUE_CONFIG_SIZE;
                response.handle=s->config.channel_id;
            }
        } else if(r->flags) status=-EINVAL;
        break;
    case KB2_FILESYSTEM_OP_CLIENT_REGISTER:
        status=filed_client_create(rt,r,bytes,size,&fds[0],&response.handle);
        if (!status) *fd_count=1;
        break;
    case KB2_FILESYSTEM_OP_CLIENT_CREDENTIALS:
        status=filed_client_credentials(rt,r,bytes,size); break;
    case KB2_FILESYSTEM_OP_CLIENT_UNREGISTER: {
        struct filed_client *client=rt->clients;
        while(client && client->id!=r->client) client=client->next;
        if (!client) status=-ESRCH;
        else status=filed_client_release(rt,client);
        break;
    }
    case KB2_FILESYSTEM_OP_SERVICE_BIND: {
        pacha_fd_t *target=NULL;
        switch(r->mode) {
        case FILED_SERVICE_NETD_SOCKET: target=&rt->netd_socket_endpoint_fd; break;
        case FILED_SERVICE_TERMD_TTY: target=&rt->termd_tty_endpoint_fd; break;
        case FILED_SERVICE_GPUD_DRM: target=&rt->gpud_drm_endpoint_fd; break;
        case FILED_SERVICE_INPUTD_INPUT: target=&rt->inputd_input_endpoint_fd; break;
        default: status=-EINVAL; break;
        }
        if (!status && source_count!=1) status=-EINVAL;
        if (!status) {
            if (*target>=16) (void)pacha_fd_close(*target);
            *target=sources[0];
        }
        break;
    }
    case KB2_FILESYSTEM_OP_SESSION_ATTACH:
        status=attach_session(rt,r,sources,source_count);
        if (!status) response.handle=r->second_handle;
        break;
    case KB2_FILESYSTEM_OP_SESSION_DETACH:
        status=detach_session(rt,r,size,source_count);
        if (!status) response.handle=r->second_handle;
        break;
    case KB2_FILESYSTEM_OP_EXEC_PATH: case KB2_FILESYSTEM_OP_EXEC_SELF:
        status=exec_request(rt,r,bytes,sources,source_count,fds,fd_count); break;
    case KB2_FILESYSTEM_OP_FILE_VMO:
        if (source_count) status=-EINVAL;
        else status=make_file_vmo(rt,r,&response,&fds[0]);
        if (!status) *fd_count=1;
        break;
    case KB2_FILESYSTEM_OP_SHARED_FILE_VMO:
        /* A private pread snapshot cannot implement MAP_SHARED coherency.
         * Linux-backed page export belongs to the shmem integration phase. */
        status=-EOPNOTSUPP; break;
    case KB2_FILESYSTEM_OP_TRANSFER_DUP: {
        if (source_count!=1) { status=-EINVAL; break; }
        struct filed_lease *lease=calloc(1,sizeof(*lease));
        if (!lease) { status=-ENOMEM; break; }
        kb2_fs_request_t control={.opcode=KB2_FILESYSTEM_OP_TRANSFER_DUP,
            .client=rt->actor ? rt->actor->id : 1,.second_handle=1,
            .handle=r->handle,.credential_generation=rt->actor ?
                rt->actor->credential_generation : 1};
        kb2_fs_response_t transferred;
        status=filed_owner_control(rt,&control,NULL,KB2_FILESYSTEM_REQUEST_SIZE,&transferred);
        if (status) { free(lease); break; }
        *lease=(struct filed_lease){.next=rt->leases,.id=transferred.handle,
            .handle=transferred.handle,.client=1,.fd=sources[0]};
        rt->leases=lease; response.handle=lease->id;
        break;
    }
    case KB2_FILESYSTEM_OP_DEVICE_ROUTE:
        if(source_count) status=-EINVAL;
        else if(capacity<KB2_FILESYSTEM_RESPONSE_SIZE+KB2_FILESYSTEM_STATX_SIZE) status=-ENOSPC;
        else status=route_device(rt,r,bytes,size,&response,out+KB2_FILESYSTEM_RESPONSE_SIZE);
        break;
    case KB2_FILESYSTEM_OP_OPENAT2:
        if(rt->actor) { status=-EPERM; break; }
        if(source_count) { status=-EINVAL; break; }
        status=relay(rt,r,bytes,size,out,capacity,used);
        if(status) return status;
        if(kb2_fs_response_decode(out,*used,&response)) return -EPROTO;
        if(response.result==-ENXIO || response.result==-ENODEV) {
            int routed=route_device(rt,r,bytes,size,&response,NULL);
            if(!routed) {
                if(kb2_fs_response_encode(out,capacity,&response)) return -EPROTO;
                *used=KB2_FILESYSTEM_RESPONSE_SIZE;
            }
        }
        return 0;
    default:
        if (source_count) status=-EINVAL;
        else if (rt->actor) status=-EPERM;
        else return relay(rt,r,bytes,size,out,capacity,used);
    }
    if(status) response.result=status;
    if (status) { response.handle=0; response.service=0; response.data_length=0; }
    if (kb2_fs_response_encode(out,capacity,&response)) return -EPROTO;
    *used=KB2_FILESYSTEM_RESPONSE_SIZE+(size_t)response.data_length;
    return 0;
}
static int retained_source(uint32_t opcode,int status)
{
    return !status && (opcode==KB2_FILESYSTEM_OP_SERVICE_BIND ||
        opcode==KB2_FILESYSTEM_OP_SESSION_ATTACH ||
        opcode==KB2_FILESYSTEM_OP_TRANSFER_DUP);
}
int filed_receive(filed_runtime_t *rt,pacha_fd_t endpoint)
{
    struct pacha_ipc_fd incoming[PACHA_IPC_MAX_TRANSFER_FDS]={{0}};
    struct pacha_ipc_msg message={.fds=incoming,.fd_capacity=PACHA_IPC_MAX_TRANSFER_FDS};
    int status=pacha_ipc_recv(endpoint,&message);
    if (status) return status;
    pacha_fd_t reply=PACHA_IPC_REPLY_IMPLICIT;
    struct pacha_fd_info info;
    void *mapping=NULL;
    uint8_t *snapshot=NULL,*output=NULL;
    size_t mapped=0,used=0;
    pacha_fd_t fds[3]={-1,-1,-1}; size_t fd_count=0;
    kb2_fs_request_t request={0};
    status=-EINVAL;
    if (message.fd_count>=1 &&
        message.word1>=KB2_FILESYSTEM_REQUEST_SIZE &&
        message.word2>=KB2_FILESYSTEM_RESPONSE_SIZE &&
        message.word1<=SIZE_MAX && message.word2<=SIZE_MAX &&
        !pacha_fd_get_info((pacha_fd_t)incoming[0].fd,&info) &&
        info.kind==PACHA_FD_KIND_VMO &&
        info.size>=message.word1 && info.size>=message.word2 &&
        info.size<=SIZE_MAX) {
        mapped=(size_t)info.size;
        mapping=pacha_mmap((pacha_fd_t)incoming[0].fd,mapped,PACHA_PROT_READ|PACHA_PROT_WRITE,
                            PACHA_MMAP_SHARED,0);
        snapshot=malloc((size_t)message.word1);
        output=malloc((size_t)message.word2);
        if (mapping && snapshot && output) {
            memcpy(snapshot,mapping,(size_t)message.word1);
            if (!kb2_fs_request_decode(snapshot,(size_t)message.word1,message.word1,&request) &&
                request.opcode==message.word0 && request.sequence==message.word3 &&
                !kb2_fs_request_validate_paths(snapshot,(size_t)message.word1,&request)) {
                pacha_fd_t sources[PACHA_IPC_MAX_TRANSFER_FDS];
                size_t source_count=(size_t)message.fd_count-1;
                for (size_t i=0;i<source_count;++i) sources[i]=(pacha_fd_t)incoming[i+1].fd;
                status=dispatch_wire(rt,&request,snapshot,(size_t)message.word1,
                    output,(size_t)message.word2,&used,sources,source_count,fds,&fd_count);
                if (!status) memcpy(mapping,output,used);
            }
        } else status=-ENOMEM;
    }
    int result=status;
    if (!status) {
        kb2_fs_response_t response;
        if (kb2_fs_response_decode(output,used,&response)) result=-EPROTO;
        else result=(int)response.result;
    }
    {
        struct pacha_ipc_fd outbound[3]={{0}};
        for (size_t i=0;i<fd_count;++i) {
            uint64_t rights=0;
            if (request.opcode==KB2_FILESYSTEM_OP_CLIENT_REGISTER)
                rights=FILED_CLIENT_REGISTER_RIGHTS;
            else if (request.opcode==KB2_FILESYSTEM_OP_FILE_VMO) {
                rights=PACHA_FD_RIGHT_INSPECT|PACHA_FD_RIGHT_CLOSE|
                    PACHA_FD_RIGHT_TRANSFER|PACHA_FD_RIGHT_DUP|PACHA_FD_RIGHT_MAP_READ;
                if (request.flags & FILED_FILE_VMO_WRITE) rights|=PACHA_FD_RIGHT_MAP_WRITE;
                if (request.flags & FILED_FILE_VMO_EXEC) rights|=PACHA_FD_RIGHT_MAP_EXEC;
            } else if (request.opcode==KB2_FILESYSTEM_OP_EXEC_SELF) {
                rights=PACHA_FD_RIGHT_INSPECT|PACHA_FD_RIGHT_CLOSE|
                    PACHA_FD_RIGHT_WAIT|PACHA_FD_RIGHT_SET_CONTEXT;
                if (!i) rights|=PACHA_FD_RIGHT_TRANSFER|PACHA_FD_RIGHT_POLL|
                    PACHA_FD_RIGHT_KILL|PACHA_FD_RIGHT_MAP_INTO;
                else if (i==1) rights|=PACHA_FD_RIGHT_KILL;
                else rights=PACHA_FD_RIGHT_INSPECT|PACHA_FD_RIGHT_CLOSE|
                    PACHA_FD_RIGHT_MAP_READ|PACHA_FD_RIGHT_DUP|PACHA_FD_RIGHT_SET_FLAGS;
            } else {
                rights=PACHA_FD_RIGHT_INSPECT|PACHA_FD_RIGHT_CLOSE|PACHA_FD_RIGHT_WAIT|
                    PACHA_FD_RIGHT_KILL;
                if (!i) {
                    rights|=PACHA_FD_RIGHT_POLL;
                    if (request.flags & FILED_EXEC_TRANSFER_PROCESS_FD)
                        rights|=PACHA_FD_RIGHT_TRANSFER;
                } else if (request.flags & FILED_EXEC_DEFER_START)
                    rights|=PACHA_FD_RIGHT_START;
            }
            outbound[i]=(struct pacha_ipc_fd){.fd=(uint64_t)fds[i],.rights=rights,
                .transfer_flags=PACHA_IPC_TRANSFER_MOVE|PACHA_IPC_TRANSFER_CLOEXEC};
        }
        struct pacha_ipc_msg response={.word0=message.word0,
            .word1=(uint64_t)(int64_t)result,.word2=status ? 0 : used,
            .word3=message.word3,.fds=outbound,.fd_count=fd_count};
        int reply_status=pacha_ipc_reply(reply,&response);
        if (reply_status) {
            const struct pacha_ipc_msg failed = {0};
            (void)pacha_ipc_reply(PACHA_IPC_REPLY_IMPLICIT, &failed);
            for(size_t i=0;i<fd_count;++i)
                if (fds[i]>=16) (void)pacha_fd_close(fds[i]);
            status=reply_status;
        }
    }
    if (mapping) (void)pacha_munmap(mapping,mapped);
    free(snapshot); free(output);
    int keep=retained_source(request.opcode,result) ||
        (request.opcode==KB2_FILESYSTEM_OP_SESSION_ATTACH && !rt->storage_generation);
    for(size_t i=0;i<message.fd_count;++i)
        if (!(keep && i>0))
            (void)pacha_fd_close((pacha_fd_t)incoming[i].fd);
    return status;
}
int filed_sessions_release_client(filed_runtime_t *rt,struct filed_client *client)
{
    struct filed_session **cursor=&rt->sessions;
    while (*cursor) {
        struct filed_session *s=*cursor;
        if (s->client!=client) { cursor=&s->next; continue; }
        int status=release_session(rt,s);
        if (status) return status;
        *cursor=s->next;
        free(s);
    }
    return 0;
}
static int lease_receive(filed_runtime_t *rt,struct filed_lease *lease)
{
    struct pacha_ipc_fd fds[PACHA_IPC_MAX_TRANSFER_FDS]={{0}};
    struct pacha_ipc_msg request={.fds=fds,.fd_capacity=PACHA_IPC_MAX_TRANSFER_FDS};
    int status=pacha_ipc_recv(lease->fd,&request);
    if(status) return status;
    pacha_fd_t reply=PACHA_IPC_REPLY_IMPLICIT;
    uint64_t handle=0;
    status=-EINVAL;
    struct filed_client *client=rt->clients;
    while(client && client->id!=request.word2) client=client->next;
    if(request.fd_count==0 && client && request.word0==FILED_LEASE_MAGIC && request.word1==lease->id) {
        if(lease->client==client->id) { handle=lease->handle; status=0; }
        else if(lease->client!=1) status=-EPERM;
        else {
            kb2_fs_request_t control={.opcode=KB2_FILESYSTEM_OP_TRANSFER_DUP,
                .client=1,.second_handle=client->id,.handle=lease->handle,
                .credential_generation=1};
            kb2_fs_response_t response;
            status=filed_owner_control(rt,&control,NULL,KB2_FILESYSTEM_REQUEST_SIZE,&response);
            if(!status) {
                handle=response.handle;
                rt->actor=NULL;
                int close=filed_storage_close(rt,lease->handle);
                if(close) {
                    /* Duplication already published a Linux reference. An
                     * ambiguous escrow close cannot leave this owner alive
                     * with two competing lease states. Still reply and drop
                     * received capabilities before retiring the generation. */
                    rt->storage_generation=0;
                    status=close;handle=0;
                } else {
                    lease->handle=handle; lease->client=client->id;
                }
            }
        }
    }
    {
        struct pacha_ipc_msg response={.word0=FILED_LEASE_MAGIC,
            .word1=(uint64_t)(int64_t)status,.word2=handle,.word3=request.word3};
        (void)pacha_ipc_reply(reply,&response);
    }
    for(size_t i=0;i<request.fd_count;++i) (void)pacha_fd_close((pacha_fd_t)fds[i].fd);
    return 0;
}
static int retire_lease(filed_runtime_t *rt,struct filed_lease *lease)
{
    if (!rt->storage_generation) return -EIO;
    int status=0;
    if (lease->client==1 && lease->handle) {
        status=filed_storage_close(rt,lease->handle);
        if (!status) lease->handle=0;
    }
    if (!status) status=pacha_fd_close(lease->fd);
    /* Escrow close and endpoint close are separate ownership transitions.
     * Failure retains the lease, including a still-owned FD, without repeating
     * an already acknowledged Linux close or discarding an uncertain one. */
    if (status) rt->storage_generation=0;
    else lease->fd=-1;
    return status;
}
int filed_runtime_serve(filed_runtime_t *rt)
{
    if(!rt || rt->client_endpoint_fd<16) return -EINVAL;
    for(;;) {
        size_t count=1;
        for(struct filed_client *c=rt->clients;c;c=c->next) ++count;
        for(struct filed_lease *l=rt->leases;l;l=l->next) ++count;
        if(rt->unix_path_fd>=16) ++count;
        if(count>SIZE_MAX/sizeof(struct pacha_pollfd)) return -EOVERFLOW;
        struct pacha_pollfd *poll=calloc(count,sizeof(*poll));
        if(!poll) return -ENOMEM;
        size_t at=0;
        uint64_t events=PACHA_FD_EVENT_READABLE|PACHA_FD_EVENT_HANGUP;
        poll[at++]=(struct pacha_pollfd){.fd=rt->client_endpoint_fd,.events=events};
        if(rt->unix_path_fd>=16) poll[at++]=(struct pacha_pollfd){.fd=rt->unix_path_fd,.events=events};
        for(struct filed_client *c=rt->clients;c;c=c->next)
            poll[at++]=(struct pacha_pollfd){.fd=c->fd,.events=events};
        for(struct filed_lease *l=rt->leases;l;l=l->next)
            poll[at++]=(struct pacha_pollfd){.fd=l->fd,.events=events};
        long waited=pacha_fd_wait_many_batched(poll,count,PACHA_FD_WAIT_FOREVER);
        if(waited<0) { free(poll); return (int)waited; }
        int status=0;
        /* Resolve each ready descriptor against the live lists again: an
         * earlier manager request may have removed a client and its sessions. */
        for(size_t i=0;i<count;++i) {
            if(!poll[i].revents) continue;
            rt->actor=NULL;
            if(poll[i].fd==(uint64_t)rt->client_endpoint_fd) status=filed_receive(rt,poll[i].fd);
            else if(poll[i].fd==(uint64_t)rt->unix_path_fd) status=filed_unix_path_receive(rt);
            else {
                struct filed_client *client=rt->clients;
                while(client && (uint64_t)client->fd!=poll[i].fd) client=client->next;
                if(client) {
                    if(poll[i].revents & PACHA_FD_EVENT_HANGUP) status=filed_client_release(rt,client);
                    else { rt->actor=client; status=filed_receive(rt,client->fd); }
                } else {
                    {
                        struct filed_lease **cursor=&rt->leases;
                        while(*cursor && (uint64_t)(*cursor)->fd!=poll[i].fd) cursor=&(*cursor)->next;
                        if(*cursor) {
                            struct filed_lease *lease=*cursor;
                            if(poll[i].revents & PACHA_FD_EVENT_HANGUP) {
                                status=retire_lease(rt,lease);
                                if(!status) { *cursor=lease->next;free(lease); }
                            } else status=lease_receive(rt,lease);
                        }
                    }
                }
            }
            /* A control reply can report a partial queue release while receive
             * itself succeeds. Never touch another ready fd in that retired
             * storage generation, including entries from this same batch. */
            if(!rt->storage_generation) break;
            if(status && status!=PACHA_ERR_EMPTY && status!=PACHA_ERR_NOT_READY &&
                status!=PACHA_ERR_CLOSED) break;
            status=0;
        }
        rt->actor=NULL;
        free(poll);
        if(status) return status;
        if(!rt->storage_generation) return -EIO;
    }
}
#ifdef FILED_INTERNAL_MICROBENCH
/* Include beside the static dispatch functions so the test exercises them
 * without adding a public entry point or a diagnostic wire operation. */
#include "../tests/internal_microbench.inc"
#endif
