#define PACHAOS_NATIVE_TEST
#include "../musl/pachaos/syscall/native.c"
#include <assert.h>
#include <stdio.h>
#include "filed/exec.h"
#include "filed/runtime.h"

static unsigned char used[4096];
static uint64_t fd_flags[4096];
static uint64_t capacity_now=256;
static void *vmo_data[4096];
static size_t vmo_sizes[4096];
static uint64_t vmo_rights[4096];
static uint64_t next_handle=1;
static uint64_t positions[4096],objects[4096],status_flags[4096];
static unsigned char contents[32768];
static size_t content_size;
static unsigned read_calls,stat_calls;
static uint32_t mode_seen,mask_seen;
static size_t path_seen,largest_io;
static size_t exec_argc,exec_envc,exec_handles;
static uint64_t namespace_first,namespace_second,namespace_flags;
static unsigned namespace_opcode,namespace_calls;
static unsigned sync_calls;
static char namespace_path[128],namespace_other[128];
static int mock_process_create(const struct pacha_process_fd_grant *grants,uint64_t count)
{
    unsigned bootstrap_grants=0;
    for (uint64_t i=0;i<count;++i) {
        if (grants[i].target_fd!=FILED_EXEC_NATIVE_BOOTSTRAP_FD) continue;
        ++bootstrap_grants;
        assert(grants[i].source_fd<4096 && vmo_data[grants[i].source_fd]);
        if (grants[i].rights&~vmo_rights[grants[i].source_fd]) return -EPERM;
    }
    assert(bootstrap_grants==1);
    return 0;
}
long native_test_raw(long n,long a,long b,long c,long d,long e,long f)
{
    (void)e; (void)f;
    switch (n) {
    case PACHAOS_SYSCALL_FD_TABLE: {
        struct { uint64_t capacity,maximum,free_slots; } *info=(void *)b;
        if (a>4096) return 1;
        if ((uint64_t)a>capacity_now) capacity_now=(uint64_t)a;
        size_t free_slots=0;
        for (unsigned i=16;i<capacity_now;++i) free_slots+=!used[i];
        *info=(typeof(*info)){capacity_now,4096,free_slots}; return 0;
    }
    case PACHAOS_SYSCALL_EVENTFD_CREATE:
        for (int i=16;i<(int)capacity_now;++i) if (!used[i]) { used[i]=1; fd_flags[i]=(uint64_t)c; return i; }
        return 3;
    case PACHAOS_SYSCALL_FD_CLOSE:
        assert(a>=16 && a<4096 && used[a]); used[a]=0;
        free(vmo_data[a]); vmo_data[a]=NULL; vmo_sizes[a]=0; vmo_rights[a]=0; return 0;
    case 41:
        if (a==FILE_ENDPOINT) {
            struct { uint64_t kind,rights,flags,size,extra; } *info=(void *)b;
            *info=(typeof(*info)){5,PACHAOS_FD_RIGHT_CALL|PACHAOS_FD_RIGHT_INSPECT|PACHAOS_FD_RIGHT_TRANSFER,0,0,0};
            return 0;
        }
        if(a>=16 && a<4096 && used[a]) {
            struct {uint64_t kind,rights,flags,size,extra;} *info=(void *)b;
            *info=(typeof(*info)){14,PACHAOS_FD_RIGHT_INSPECT,fd_flags[a],0,0};return 0;
        }
        return 1; /* No inherited bootstrap, or a closed descriptor. */
    case PACHAOS_SYSCALL_VMO_CREATE:
        for (int i=16;i<(int)capacity_now;++i) if (!used[i]) {
            used[i]=1; vmo_data[i]=calloc(1,(size_t)a); assert(vmo_data[i]);
            vmo_sizes[i]=(size_t)a; vmo_rights[i]=(uint64_t)b; return i;
        }
        return 3;
    case PACHAOS_SYSCALL_MMAP: assert(a>=16 && a<4096 && vmo_data[a]); return (long)vmo_data[a];
    case PACHAOS_SYSCALL_MUNMAP: return 0;
    case PACHAOS_SYSCALL_FD_STAT:
        assert(a>=16 && a<4096 && used[a]);
        memset((void *)b,0,sizeof(struct __pachaos_kstat));
        ((struct __pachaos_kstat *)b)->st_mode=0020000; return 0;
    case PACHAOS_SYSCALL_FD_POLL: {
        struct __pachaos_pollfd *fds=(void *)a;
        assert(b==72);
        fds[0].revents=PACHAOS_POLL_READABLE; return 1;
    }
    case 40:
        assert(a>=16 && a<4096 && used[a]);
        for (long i=b;i<(long)capacity_now;++i) if (i>=16 && !used[i]) { used[i]=1; fd_flags[i]=(uint64_t)d; return i; }
        return 3;
    case PACHAOS_SYSCALL_FD_FCNTL:
        if (a<16 || a>=4096 || !used[a]) return 1;
        if (b==1) return (long)fd_flags[a];
        assert(b==2); fd_flags[a]=(fd_flags[a]&~(uint64_t)d)|((uint64_t)c&(uint64_t)d); return 0;
    default: fprintf(stderr,"unexpected native syscall %ld\n",n); abort();
    }
}
int fs_call(kb2_fs_request_t *r,const char *path,const char *second,
    const void *data,size_t data_size,void *out,size_t capacity,
    kb2_fs_response_t *reply,const int *sources,size_t source_count,
    int *received,size_t *received_count)
{
    (void)second; (void)received; (void)received_count;
    *reply=(kb2_fs_response_t){.opcode=r->opcode};
    if (path) path_seen=strlen(path);
    uint64_t h=r->handle,o=h<4096 ? objects[h] : 0;
    switch (r->opcode) {
    case KB2_FILESYSTEM_OP_SYNC:
        assert(!path && !second && !data && !data_size && !out && !capacity);
        assert(!r->handle && !r->second_handle && !r->flags && !r->offset && !r->length && !r->mode && !r->umask && !r->mask);
        ++sync_calls;return 0;
    case KB2_FILESYSTEM_OP_LINKAT:
    case KB2_FILESYSTEM_OP_SYMLINKAT:
        ++namespace_calls;namespace_opcode=r->opcode;
        namespace_first=r->handle;namespace_second=r->second_handle;namespace_flags=r->flags;
        assert(path && second && strlen(path)<sizeof(namespace_path) && strlen(second)<sizeof(namespace_other));
        strcpy(namespace_path,path);strcpy(namespace_other,second);
        return r->opcode==KB2_FILESYSTEM_OP_LINKAT && (r->flags&~(uint64_t)(AT_SYMLINK_FOLLOW|AT_EMPTY_PATH)) ? -EINVAL : 0;
    case KB2_FILESYSTEM_OP_EXEC_PATH: {
        assert(source_count==2 && sources[0]==FILE_ENDPOINT && vmo_data[sources[1]]);
        filed_exec_path_t exec;
        assert(filed_exec_payload_decode(data,data_size,path,r->handle,r->flags,&exec)==0);
        exec_argc=exec.argc; exec_envc=exec.envc; exec_handles=exec.inherit_handle_count;
        assert(exec.inherit_fd_count==1 && exec.fd_grants[0].target==FILE_ENDPOINT);
        /* Exercise filed's actual grant policy, then emulate PROCESS_CREATE's
         * authority subset check rather than mirroring its bootstrap rights. */
        filed_runtime_t runtime={0};
        struct pacha_process_fd_grant grants[PACHA_PROCESS_CREATE_MAX_GRANTS];
        uint64_t grant_count=0;
        assert(filed_exec_build_grants(&runtime,&exec,sources,exec.inherit_fd_count,
            sources[1],grants,&grant_count)==0);
        assert(mock_process_create(grants,grant_count)==0);
        uint64_t bootstrap_rights=vmo_rights[sources[1]];
        vmo_rights[sources[1]]&=~PACHAOS_FD_RIGHT_READ;
        assert(mock_process_create(grants,grant_count)==-EPERM);
        vmo_rights[sources[1]]=bootstrap_rights;
        assert(exec.fd_patch_count==exec.inherit_handle_count);
        for (uint64_t i=0;i<exec.inherit_handle_count;++i) {
            assert(exec.fd_patches[i].kind==FILED_EXEC_PATCH_INHERIT_HANDLE);
            assert(exec.fd_patches[i].index==i && exec.fd_patches[i].offset==40+i*16);
        }
        assert(__pachaos_rd64((unsigned char *)vmo_data[sources[1]]+8)==exec_handles);
        filed_exec_payload_destroy(&exec); return -EACCES;
    }
    case KB2_FILESYSTEM_OP_OPENAT2:
        assert(r->flags==(uint64_t)(O_RDWR|O_CREAT|O_CLOEXEC) || r->flags==(uint64_t)O_RDONLY);
        mode_seen=(uint32_t)r->mode; mask_seen=r->umask;
        assert(next_handle<4096); h=next_handle++; objects[h]=h; positions[h]=0;
        status_flags[h]=r->flags&~(uint64_t)O_CLOEXEC;
        reply->handle=h; return 0;
    case KB2_FILESYSTEM_OP_CLOSE: assert(o); objects[h]=0; return 0;
    case KB2_FILESYSTEM_OP_DUP:
        assert(o && next_handle<4096); reply->handle=next_handle++;
        objects[reply->handle]=o; return 0;
    case KB2_FILESYSTEM_OP_FCNTL:
        assert(o);
        if (r->flags==F_GETFL) reply->result=(int64_t)status_flags[o];
        else { assert(r->flags==F_SETFL); status_flags[o]=r->length; }
        return 0;
    case KB2_FILESYSTEM_OP_LSEEK:
        assert(o && r->flags==SEEK_SET && r->offset>=0);
        positions[o]=(uint64_t)r->offset; reply->result=r->offset; return 0;
    case KB2_FILESYSTEM_OP_FTRUNCATE:
        assert(o && r->offset>=0 && (uint64_t)r->offset<sizeof(contents));
        content_size=(size_t)r->offset; return 0;
    case KB2_FILESYSTEM_OP_WRITE:
    case KB2_FILESYSTEM_OP_PWRITE: {
        assert(o && r->length==data_size && data_size<=sizeof(contents));
        size_t offset=r->opcode==KB2_FILESYSTEM_OP_WRITE ? (size_t)positions[o] : (size_t)r->offset;
        if (status_flags[o]&O_APPEND) offset=content_size;
        assert(offset<=sizeof(contents)-data_size);
        memcpy(contents+offset,data,data_size);
        if (offset+data_size>content_size) content_size=offset+data_size;
        if (r->opcode==KB2_FILESYSTEM_OP_WRITE) positions[o]=offset+data_size;
        reply->result=(int64_t)data_size;
        if (data_size>largest_io) largest_io=data_size;
        return 0;
    }
    case KB2_FILESYSTEM_OP_READ:
    case KB2_FILESYSTEM_OP_PREAD: {
        ++read_calls; assert(o && r->length<=capacity);
        size_t offset=r->opcode==KB2_FILESYSTEM_OP_READ ? (size_t)positions[o] : (size_t)r->offset;
        size_t got=offset<content_size ? content_size-offset : 0;
        if (got>r->length) got=(size_t)r->length;
        memcpy(out,contents+offset,got);
        if (r->opcode==KB2_FILESYSTEM_OP_READ) positions[o]+=got;
        reply->result=(int64_t)got; reply->data_length=got;
        return 0;
    }
    case KB2_FILESYSTEM_OP_STATX: {
        ++stat_calls; assert(o && capacity==KB2_FILESYSTEM_STATX_SIZE);
        memset(out,0,capacity); wr64((unsigned char *)out+KB2_FILESYSTEM_STATX_SIZE_OFFSET,content_size);
        reply->data_length=capacity; return 0;
    }
    default: fprintf(stderr,"unexpected filesystem opcode %u\n",r->opcode); abort();
    }
}
static long call(long n,long a,long b,long c,long d)
{
    return __pachaos_native_syscall(n,a,b,c,d,0,0);
}
int main(void)
{
    assert(call(SYS_sync,0,0,0,0)==0 && sync_calls==1);
    char path[701]; memset(path,'x',sizeof(path)-1); path[0]='/'; path[sizeof(path)-1]=0;
    long fd=call(SYS_open,(long)path,O_RDWR|O_CREAT|O_CLOEXEC,0640,0);
    assert(fd>=16 && path_seen==700 && mode_seen==0640 && mask_seen==0022);
    assert(call(SYS_fcntl,fd,F_GETFD,0,0)==FD_CLOEXEC);
    assert(call(SYS_fcntl,fd,F_SETFD,0,0)==0);
    assert(call(SYS_fcntl,fd,F_GETFD,0,0)==0);
    unsigned char data[16384],readback[16384];
    for (size_t i=0;i<sizeof(data);++i) data[i]=(unsigned char)i;
    assert(call(SYS_write,fd,(long)data,sizeof(data),0)==sizeof(data));
    assert(largest_io==16384);
    assert(call(SYS_lseek,fd,0,SEEK_SET,0)==0);
    long duplicate=call(SYS_fcntl,fd,F_DUPFD,500,0); assert(duplicate==500);
    assert(call(SYS_read,fd,(long)readback,17,0)==17);
    assert(call(SYS_read,duplicate,(long)readback,17,0)==17 && !memcmp(readback,data+17,17));
    assert(call(SYS_pread64,fd,(long)readback,sizeof(readback),0)==sizeof(readback));
    contents[0]=99;
    assert(call(SYS_pread64,fd,(long)readback,1,0)==1 && readback[0]==99 && read_calls==4);
    struct __pachaos_kstat st;
    assert(call(SYS_fstat,fd,(long)&st,0,0)==0 && st.st_size==16384);
    assert(call(SYS_ftruncate,fd,7,0,0)==0);
    assert(call(SYS_fstat,fd,(long)&st,0,0)==0 && st.st_size==7 && stat_calls==2);
    assert(call(SYS_fcntl,duplicate,F_SETFL,O_RDWR|O_APPEND,0)==0);
    assert(call(SYS_fcntl,fd,F_GETFL,0,0)==(O_RDWR|O_APPEND));
    assert(call(SYS_write,fd,(long)"!",1,0)==1 && content_size==8 && contents[7]=='!');
    assert(__pachaos_native_syscall(SYS_linkat,fd,(long)"old",duplicate,(long)"new",
        AT_SYMLINK_FOLLOW|AT_EMPTY_PATH,0)==0);
    assert(namespace_opcode==KB2_FILESYSTEM_OP_LINKAT && namespace_first==lookup(fd)->handle &&
        namespace_second==lookup(duplicate)->handle && namespace_flags==(AT_SYMLINK_FOLLOW|AT_EMPTY_PATH) &&
        !strcmp(namespace_path,"old") && !strcmp(namespace_other,"new"));
    assert(__pachaos_native_syscall(SYS_linkat,-1,(long)"/old",-1,(long)"/new",0,0)==0);
    assert(namespace_first==0 && namespace_second==0);
    assert(__pachaos_native_syscall(SYS_linkat,fd,(long)"old",duplicate,(long)"new",0x80000000,0)==-EINVAL);
    unsigned before=namespace_calls;
    assert(__pachaos_native_syscall(SYS_linkat,-1,(long)"old",duplicate,(long)"new",0,0)==-EBADF);
    assert(namespace_calls==before);
    current_directory=99;
    assert(call(SYS_link,(long)"old",(long)"new",0,0)==0 && namespace_first==99 && namespace_second==99 && !namespace_flags);
    assert(call(SYS_symlinkat,(long)"../verbatim-target",fd,(long)"link",0)==0 &&
        namespace_opcode==KB2_FILESYSTEM_OP_SYMLINKAT && namespace_first==lookup(fd)->handle &&
        !strcmp(namespace_path,"../verbatim-target") && !strcmp(namespace_other,"link"));
    assert(call(SYS_symlinkat,(long)"relative-target",-1,(long)"/link",0)==0 && namespace_first==0);
    assert(call(SYS_symlink,(long)"/absolute-target",(long)"link",0,0)==0 && namespace_first==99 &&
        !strcmp(namespace_path,"/absolute-target") && !strcmp(namespace_other,"link"));
    before=namespace_calls;
    assert(call(SYS_symlinkat,0,fd,(long)"link",0)==-EFAULT);
    assert(call(SYS_symlinkat,(long)"target",-1,(long)"link",0)==-EBADF && namespace_calls==before);
    current_directory=0;
    assert(call(SYS_close,fd,0,0,0)==0);
    assert(call(SYS_close,duplicate,0,0,0)==0);
    long descriptors[300];
    for (unsigned i=0;i<300;++i) { descriptors[i]=call(SYS_open,(long)"/file",O_RDONLY,0,0); assert(descriptors[i]>=16); }
    assert(capacity_now>256);
    char *argv[301],*envp[81];
    for (unsigned i=0;i<300;++i) argv[i]="argument-long-enough-to-overflow-the-old-six-kilobyte-string-budget";
    argv[300]=NULL;
    for (unsigned i=0;i<80;++i) envp[i]="KEY=environment";
    envp[80]=NULL;
    assert(call(SYS_execve,(long)"/child",(long)argv,(long)envp,0)==-EACCES);
    assert(exec_argc==300 && exec_envc==80 && exec_handles==300);
    for (unsigned i=0;i<300;++i) assert(call(SYS_close,descriptors[i],0,0,0)==0);
    assert(files==NULL);
    struct __pachaos_linux_pollfd pollfds[72];
    for (unsigned i=0;i<72;++i) pollfds[i]=(struct __pachaos_linux_pollfd){reserve_fd(16,0),LINUX_POLLIN,0};
    assert(call(SYS_fstat,pollfds[0].fd,(long)&st,0,0)==0 && st.st_mode==0020000);
    assert(call(SYS_poll,(long)pollfds,72,0,0)==1 && pollfds[0].revents==LINUX_POLLIN);
    for (unsigned i=0;i<72;++i) assert(call(SYS_close,pollfds[i].fd,0,0,0)==0);
    assert(call(SYS_fstat,pollfds[0].fd,(long)&st,0,0)==-EBADF);
    puts("native filesystem port: large paths / 16KiB I/O / 300 descriptors / shared offsets / cacheless metadata / dynamic exec inheritance PASS");
    return 0;
}
