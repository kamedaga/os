#define _GNU_SOURCE
#define __pachaos__
#define PACHAOS_NATIVE_TEST
#define main fixture_entry
#include "../musl/pachaos/smoke/libc_vfs_exec.c"
#undef main
#include <assert.h>
#include <sys/syscall.h>
#include <sys/wait.h>

struct resource {
    struct resource *next;
    long fd;
    unsigned char *bytes;
    size_t size;
    struct __pachaos_ipc_msg reply;
};
struct handle {
    struct handle *next;
    uint64_t id;
    int fd;
};
static struct resource *resources;
static struct handle *handles;
static long next_resource=10000;
static uint64_t next_handle=1;
static size_t largest_request,largest_path,largest_io;
static int corrupt_sequence,corrupt_reserved;
static struct resource *new_resource(void)
{
    struct resource *p=calloc(1,sizeof(*p)); assert(p);
    p->fd=next_resource++; p->next=resources; resources=p; return p;
}
static struct resource *resource(long fd)
{
    for (struct resource *p=resources;p;p=p->next) if (p->fd==fd) return p;
    abort();
}
static uint64_t handle_add(int fd)
{
    struct handle *p=malloc(sizeof(*p)); assert(p);
    *p=(struct handle){handles,next_handle++,fd}; handles=p; return p->id;
}
static int handle_fd(uint64_t id)
{
    for (struct handle *p=handles;p;p=p->next) if (p->id==id) return p->fd;
    return -1;
}
static int handle_close(uint64_t id)
{
    struct handle **p=&handles;
    while (*p && (*p)->id!=id) p=&(*p)->next;
    if (!*p) return -EBADF;
    struct handle *old=*p; *p=old->next; int result=close(old->fd); free(old); return result;
}
static void write64(unsigned char *p,uint64_t value)
{
    for (unsigned i=0;i<8;++i) p[i]=(unsigned char)(value>>(i*8));
}
static void dispatch_mock(struct resource *vmo,const struct __pachaos_ipc_msg *message,
                         struct resource *reply)
{
    assert(message->word1<=vmo->size && message->word2<=vmo->size);
    if (message->word1>largest_request) largest_request=(size_t)message->word1;
    kb2_fs_request_t r;
    assert(kb2_fs_request_decode(vmo->bytes,KB2_FILESYSTEM_REQUEST_SIZE,message->word1,&r)==0);
    assert(kb2_fs_request_validate_paths(vmo->bytes,message->word1,&r)==0);
    assert(message->word0==r.opcode && message->word3==r.sequence);
    const char *path=r.path.length ? (char *)vmo->bytes+r.path.offset : NULL;
    const char *second=r.second_path.length ? (char *)vmo->bytes+r.second_path.offset : NULL;
    if (path && strlen(path)>largest_path) largest_path=strlen(path);
    unsigned char *output=vmo->bytes+KB2_FILESYSTEM_RESPONSE_SIZE;
    size_t capacity=(size_t)message->word2-KB2_FILESYSTEM_RESPONSE_SIZE;
    kb2_fs_response_t response={.opcode=r.opcode,.generation=r.generation,.sequence=r.sequence};
    int fd=handle_fd(r.handle);
    errno=0;
    switch (r.opcode) {
    case KB2_FILESYSTEM_OP_OPENAT2:
        fd=open(path,(int)r.flags,(mode_t)r.mode);
        if (fd<0) response.result=-errno;
        else response.handle=handle_add(fd);
        break;
    case KB2_FILESYSTEM_OP_DUP:
        fd=dup(fd);
        if (fd<0) response.result=-errno;
        else response.handle=handle_add(fd);
        break;
    case KB2_FILESYSTEM_OP_CLOSE: response.result=handle_close(r.handle); break;
    case KB2_FILESYSTEM_OP_PWRITE:
    case KB2_FILESYSTEM_OP_WRITE:
        assert(r.data.length==r.length);
        response.result=r.opcode==KB2_FILESYSTEM_OP_PWRITE ?
            pwrite(fd,vmo->bytes+r.data.offset,r.length,r.offset) :
            write(fd,vmo->bytes+r.data.offset,r.length);
        if (r.length>largest_io) largest_io=(size_t)r.length;
        break;
    case KB2_FILESYSTEM_OP_PREAD:
    case KB2_FILESYSTEM_OP_READ:
    case KB2_FILESYSTEM_OP_GETDENTS64:
        assert(r.length<=capacity);
        response.result=r.opcode==KB2_FILESYSTEM_OP_PREAD ? pread(fd,output,r.length,r.offset) :
            r.opcode==KB2_FILESYSTEM_OP_READ ? read(fd,output,r.length) :
            syscall(SYS_getdents64,fd,output,r.length);
        if (response.result>=0) response.data_length=(uint64_t)response.result;
        break;
    case KB2_FILESYSTEM_OP_LSEEK: response.result=lseek(fd,r.offset,(int)r.flags); break;
    case KB2_FILESYSTEM_OP_FTRUNCATE: response.result=ftruncate(fd,r.offset); break;
    case KB2_FILESYSTEM_OP_STATX: {
        struct stat stat;
        if (fstat(fd,&stat)) response.result=-errno;
        else {
            assert(capacity>=KB2_FILESYSTEM_STATX_SIZE);
            memset(output,0,KB2_FILESYSTEM_STATX_SIZE);
            write64(output+KB2_FILESYSTEM_STATX_SIZE_OFFSET,(uint64_t)stat.st_size);
            response.data_length=KB2_FILESYSTEM_STATX_SIZE;
        }
        break;
    }
    case KB2_FILESYSTEM_OP_MKDIRAT: response.result=mkdir(path,(mode_t)r.mode); break;
    case KB2_FILESYSTEM_OP_UNLINKAT: response.result=unlinkat(AT_FDCWD,path,(int)r.flags); break;
    case KB2_FILESYSTEM_OP_RENAMEAT2: response.result=rename(path,second); break;
    default: abort();
    }
    if (response.result==-1 && errno) response.result=-errno;
    assert(kb2_fs_response_encode(vmo->bytes,message->word2,&response)==0);
    if (corrupt_reserved) vmo->bytes[KB2_FILESYSTEM_RESPONSE_RESERVED_OFFSET]=1;
    reply->reply=(struct __pachaos_ipc_msg){.word0=r.opcode,
        .word1=(uint64_t)response.result,
        .word2=KB2_FILESYSTEM_RESPONSE_SIZE+response.data_length,
        .word3=r.sequence+(uint64_t)corrupt_sequence};
}
long native_test_raw(long n,long a,long b,long c,long d,long e,long f)
{
    (void)d; (void)e; (void)f;
    switch (n) {
    case PACHAOS_SYSCALL_VMO_CREATE: {
        struct resource *p=new_resource(); p->size=(size_t)a;
        p->bytes=calloc(1,p->size); assert(p->bytes); return p->fd;
    }
    case PACHAOS_SYSCALL_MMAP: assert(a==0 && (size_t)c==resource(b)->size); return (long)resource(b)->bytes;
    case PACHAOS_SYSCALL_MUNMAP: return 0;
    case PACHAOS_SYSCALL_FD_CLOSE: {
        struct resource **p=&resources; while (*p && (*p)->fd!=a) p=&(*p)->next;
        assert(*p); struct resource *old=*p; *p=old->next; free(old->bytes); free(old); return 0;
    }
    case PACHAOS_SYSCALL_IPC_CALL: {
        assert(a==SMOKE_FILED_ENDPOINT);
        struct __pachaos_ipc_msg *message=(void *)b;
        assert(message->fd_count==1 && message->fds);
        struct resource result={0};
        dispatch_mock(resource((long)message->fds[0].fd),message,&result);
        assert(d == -1 && e == PACHAOS_WAIT_UNINTERRUPTIBLE);
        struct __pachaos_ipc_msg *reply=(void *)c;
        struct __pachaos_ipc_fd *fds=reply->fds;
        uint64_t capacity=reply->fd_capacity;
        *reply=result.reply; reply->fds=fds; reply->fd_capacity=capacity;
        return 0;
    }
    default: abort();
    }
}
int getdents(int fd,struct dirent *out,size_t size)
{
    return (int)syscall(SYS_getdents64,fd,out,size);
}
int main(int argc,char **argv,char **envp)
{
    if (argc>1) return fixture_entry(argc,argv,envp);
    char fixture[]="/tmp/native-fs-fixture-XXXXXX";
    int fd=mkstemp(fixture); assert(fd>=0);
    assert(write(fd,"seed",4)==4 && close(fd)==0);
    assert(run_filed_raw_operation_bench(fixture)==0);
    assert(largest_io==16384 && largest_request>16384 && largest_path>800);
    assert(resources==NULL && handles==NULL);
    uint64_t h=0;
    assert(raw_open(fixture,O_RDONLY,0,&h)==0);
    corrupt_sequence=1;
    assert(raw_simple(KB2_FILESYSTEM_OP_LSEEK,h,0,SEEK_SET)==-EPROTO);
    corrupt_sequence=0; corrupt_reserved=1;
    assert(raw_simple(KB2_FILESYSTEM_OP_LSEEK,h,0,SEEK_SET)==-EPROTO);
    corrupt_reserved=0;
    assert(raw_simple(KB2_FILESYSTEM_OP_CLOSE,h,0,0)==0);
    assert(resources==NULL && handles==NULL);
    assert(unlink(fixture)==0);
    char inherited_path[]="/tmp/native-exec-fixture-XXXXXX";
    fd=mkstemp(inherited_path); assert(fd>=0);
    assert(write(fd,"abcdefghij",10)==10 && lseek(fd,2,SEEK_SET)==2);
    assert(unlink(inherited_path)==0);
    int shared=dup(fd),cloexec=fcntl(fd,F_DUPFD_CLOEXEC,0);
    assert(shared>=0 && cloexec>=0);
    char descriptor_text[3][24],time_text[96];
    snprintf(descriptor_text[0],sizeof(descriptor_text[0]),"%d",fd);
    snprintf(descriptor_text[1],sizeof(descriptor_text[1]),"%d",shared);
    snprintf(descriptor_text[2],sizeof(descriptor_text[2]),"%d",cloexec);
    snprintf(time_text,sizeof(time_text),"PACHA_LIBC_VFS_EXEC_START_NS=%llu",now_ns());
    char *child_argv[301]={argv[0],"--child",descriptor_text[0],descriptor_text[1],descriptor_text[2]};
    char *child_envp[81]={"PACHA_LIBC_VFS_EXEC_CHILD=1",time_text};
    for (unsigned i=5;i<300;++i) child_argv[i]="argument-crossing-the-former-fixed-string-payload-budget";
    child_argv[300]=NULL;
    for (unsigned i=2;i<80;++i) child_envp[i]="FIXTURE_LARGE_ENV=value";
    child_envp[80]=NULL;
    pid_t pid=fork(); assert(pid>=0);
    if (!pid) { execve(argv[0],child_argv,child_envp); _exit(127); }
    int status;
    assert(waitpid(pid,&status,0)==pid && WIFEXITED(status) && WEXITSTATUS(status)==0);
    assert(lseek(fd,0,SEEK_CUR)==8);
    assert(close(fd)==0 && close(shared)==0 && close(cloexec)==0);
    puts("native raw fixture: shared codec, long spans, 16KiB data, real Linux offsets/metadata/getdents/rename, malformed replies PASS");
    puts("native exec fixture: 300argv / 80env / unlinked references / shared offsets / CLOEXEC PASS");
    return 0;
}
