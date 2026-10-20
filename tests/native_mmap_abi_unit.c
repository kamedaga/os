/* SPDX-License-Identifier: MIT */
/* Keep the real native transport enabled: replacing fs_call would miss a
 * swapped VMO/address ABI in precisely the production paths under test. */
#define PACHAOS_NATIVE_RAW_TEST
#include "../musl/pachaos/syscall/native.c"
#include <assert.h>
#include <stdio.h>

static void *memory[4096];
static size_t sizes[4096];
static unsigned char occupied[4096];
static uint64_t native_flags[4096];
static unsigned bootstrap_maps,request_maps,file_maps,exits,writes;
static struct __pachaos_ipc_msg pending;
static int pending_vmo,returned_vmo;
static int fail_bootstrap_map,expected_exit_status;
static int allocate(size_t size)
{
    for (int fd=16;fd<4096;++fd) if (!occupied[fd]) {
        occupied[fd]=1;
        if (size) { memory[fd]=calloc(1,size); assert(memory[fd]); sizes[fd]=size; }
        return fd;
    }
    abort();
}
long native_test_raw(long n,long a,long b,long c,long d,long e,long f)
{
    (void)d;(void)e;(void)f;
    switch(n) {
    case PACHAOS_SYSCALL_FD_TABLE:
        ((uint64_t *)b)[0]=4096;((uint64_t *)b)[1]=4096;((uint64_t *)b)[2]=4000;return 0;
    case PACHAOS_SYSCALL_VMO_CREATE:return allocate((size_t)a);
    case PACHAOS_SYSCALL_EVENTFD_CREATE: {
        int fd=allocate(0);native_flags[fd]=(uint64_t)c;return fd;
    }
    case PACHAOS_SYSCALL_FD_FCNTL:assert(b==1);return (long)native_flags[a];
    case PACHAOS_SYSCALL_MMAP:
        /* Native kernel ABI is fd, requested_va, length, prot, flags, offset. */
        if (e&PACHAOS_MMAP_ANONYMOUS) {
            assert(a==0);
            return b ? b : (long)UINT64_C(0x10000000000);
        }
        assert(a>=16 && a<4096 && memory[a] && (size_t)c<=sizes[a]);
        if (a==BOOTSTRAP_FD) { assert(b==0);++bootstrap_maps;if(fail_bootstrap_map)return 1; }
        else if (a==returned_vmo) { assert(b==0x40000000);++file_maps; }
        else { assert(b==0);++request_maps; }
        return (long)memory[a];
    case PACHAOS_SYSCALL_MUNMAP:return 0;
    case PACHAOS_SYSCALL_FD_GET_INFO: {
        assert(a>=16 && a<4096 && occupied[a]);
        struct {uint64_t kind,rights,flags,size,extra;} *info=(void *)b;
        *info=(typeof(*info)){memory[a] ? 4 : 5,
            PACHAOS_FD_RIGHT_INSPECT|PACHAOS_FD_RIGHT_TRANSFER|PACHAOS_FD_RIGHT_MAP_READ|PACHAOS_FD_RIGHT_READ,
            0,sizes[a],0};return 0;
    }
    case PACHAOS_SYSCALL_FD_CLOSE:
        assert(a>=16 && a<4096 && occupied[a]);occupied[a]=0;
        if(a==returned_vmo)returned_vmo=-1;
        free(memory[a]);memory[a]=NULL;sizes[a]=0;return 0;
    case PACHAOS_SYSCALL_IPC_CALL: {
        assert(a==FILE_ENDPOINT);
        struct __pachaos_ipc_msg *message=(void *)b;
        pending=*message;pending_vmo=(int)message->fds[0].fd;
        kb2_fs_request_t request;
        assert(kb2_fs_request_decode(memory[pending_vmo],KB2_FILESYSTEM_REQUEST_SIZE,message->word1,&request)==0);
        kb2_fs_response_t response={.opcode=request.opcode,.generation=request.generation,.sequence=request.sequence};
        returned_vmo=-1;
        if(request.opcode==KB2_FILESYSTEM_OP_OPENAT2) response.handle=77;
        else if(request.opcode==KB2_FILESYSTEM_OP_FILE_VMO) {
            assert(request.handle==77);response.result=4096;returned_vmo=allocate(4096);
        } else if(request.opcode==KB2_FILESYSTEM_OP_EXEC_PATH) {
            assert(message->fd_count==3);
            int bootstrap=(int)message->fds[2].fd;
            assert(__pachaos_rd64(memory[bootstrap])==NATIVE_STATE_MAGIC);
            response.result=-EACCES;
        } else assert(request.opcode==KB2_FILESYSTEM_OP_CLOSE);
        assert(kb2_fs_response_encode(memory[pending_vmo],KB2_FILESYSTEM_RESPONSE_SIZE,&response)==0);
        pending.word1=(uint64_t)response.result;
        assert(d == -1 && e == PACHAOS_WAIT_UNINTERRUPTIBLE);
        struct __pachaos_ipc_msg *reply=(void *)c;
        reply->word0=pending.word0;reply->word1=pending.word1;
        reply->word2=KB2_FILESYSTEM_RESPONSE_SIZE;reply->word3=pending.word3;
        reply->fd_count=returned_vmo>=0;
        if(reply->fd_count) reply->fds[0].fd=returned_vmo;
        return 0;
    }
    case PACHAOS_SYSCALL_FD_WRITE:
        assert(a==1 || a==2);assert(b && c>0);++writes;return c;
    case PACHAOS_SYSCALL_GETTID:return 123;
    case PACHAOS_SYSCALL_PROCESS_EXIT:assert(a==expected_exit_status);++exits;return 0;
    default:fprintf(stderr,"unexpected raw syscall %ld\n",n);abort();
    }
}
static long call(long n,long a,long b,long c,long d,long e,long f)
{return __pachaos_native_syscall(n,a,b,c,d,e,f);}
int main(void)
{
    occupied[BOOTSTRAP_FD]=1;memory[BOOTSTRAP_FD]=calloc(1,4096);sizes[BOOTSTRAP_FD]=4096;
    occupied[FILE_ENDPOINT]=1;
    /* Even a service-specific (non-native-state) bootstrap must map correctly:
     * the old inversion persisted ENOMEM, blocking both stdio and exit. */
    assert(call(SYS_set_tid_address,0,0,0,0,0,0)==123 && bootstrap_maps==1);
    struct __pachaos_iovec iov[2]={{(void *)"hello",5},{(void *)" world",6}};
    assert(call(SYS_writev,1,(long)iov,2,0,0,0)==11 && writes==1);
    assert(call(SYS_writev,2,(long)iov,2,0,0,0)==11 && writes==2);
    assert(call(SYS_mmap,0x40000000,4096,PROT_READ|PROT_WRITE,
        MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0)==0x40000000);
    assert(call(SYS_mmap,0x50000000,4096,PROT_READ|PROT_WRITE,
        MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)==0x50000000);
    int native_vmo=allocate(4096);
    assert(call(SYS_mmap,0,4096,PROT_READ,MAP_PRIVATE,native_vmo,0)==(long)memory[native_vmo]);
    assert(call(SYS_close,native_vmo,0,0,0,0,0)==0);
    long fd=call(SYS_open,(long)"/file",O_RDONLY|O_CLOEXEC,0,0,0,0);assert(fd>=16);
    assert(call(SYS_mmap,0x40000000,4096,PROT_READ,MAP_PRIVATE,fd,0)>=4096 && file_maps==1);
    char *argv[]={"child",NULL};char *envp[]={NULL};
    assert(call(SYS_execve,(long)"/child",(long)argv,(long)envp,0,0,0)==-EACCES);
    assert(call(SYS_close,fd,0,0,0,0,0)==0 && request_maps>=4);
    assert(call(SYS_exit_group,0,0,0,0,0,0)==0 && exits==1);
    initialized=0;fail_bootstrap_map=1;expected_exit_status=7;
    assert(call(SYS_gettid,0,0,0,0,0,0)==-ENOMEM);
    assert(call(SYS_open,(long)"/file",O_RDONLY,0,0,0,0)==-ENOMEM);
    assert(call(SYS_exit_group,7,0,0,0,0,0)==0 && exits==2);
    initialized=0;fail_bootstrap_map=0;expected_exit_status=9;
    wr64(memory[BOOTSTRAP_FD],NATIVE_STATE_MAGIC);wr64((unsigned char *)memory[BOOTSTRAP_FD]+8,50000);
    assert(call(SYS_gettid,0,0,0,0,0,0)==-EPROTO);
    assert(call(SYS_open,(long)"/file",O_RDONLY,0,0,0,0)==-EPROTO);
    assert(call(SYS_exit,9,0,0,0,0,0)==0 && exits==3);
    for(int i=16;i<4096;++i) if(occupied[i]) {free(memory[i]);memory[i]=NULL;}
    puts("native mmap ABI: actual codec transport / bootstrap / exec VMO / low fixed addresses / stdio / exit PASS");
    return 0;
}
