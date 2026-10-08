/* SPDX-License-Identifier: MIT */
/* PachaOS-specific translation; upstream musl remains an unmodified consumer. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include "native_raw.h"
#include "kobox2/filesystem.h"
#include "filed/payload.h"

#define FILE_ENDPOINT 240
#define BOOTSTRAP_FD 255
#define NATIVE_STATE_MAGIC UINT64_C(0x326466657669746e)

unsigned char *__pachaos_brk_base, *__pachaos_brk_cur, *__pachaos_brk_limit;
int __pachaos_brk_lock;

struct native_file {
    int fd;
    uint64_t handle;
    struct native_file *next;
};
static struct native_file *files;
static int files_lock;
static uint32_t creation_mask = 0022;
static uint64_t current_directory;
static void lock_files(void)
{
    while (__sync_lock_test_and_set(&files_lock, 1)) __asm__ volatile("pause");
}
static void unlock_files(void) { __sync_lock_release(&files_lock); }
static struct native_file *lookup(long fd)
{
    for (struct native_file *p=files;p;p=p->next) if (p->fd==fd) return p;
    return NULL;
}
static long fd_status(long s) { return s>=16 ? s : s<0 ? s : -EIO; }
static long native_status(long s) { return s ? s<0 ? s : -EIO : 0; }
static int descriptor_room(size_t minimum,size_t spare)
{
    struct { uint64_t capacity,maximum,free_slots; } info;
    long s=__pachaos_raw2(PACHAOS_SYSCALL_FD_TABLE,0,(long)&info);
    if (s) return (int)native_status(s);
    if (minimum<=info.capacity && spare<=info.free_slots) return 0;
    uint64_t wanted=info.capacity;
    uint64_t extra=spare>info.free_slots ? spare-info.free_slots : 0;
    if (wanted>info.maximum || extra>info.maximum-wanted || minimum>info.maximum) return -EMFILE;
    wanted+=extra;
    if (wanted<minimum) wanted=minimum;
    if (info.capacity<=info.maximum/2 && wanted<info.capacity*2) wanted=info.capacity*2;
    if (wanted>info.maximum) wanted=info.maximum;
    return (int)native_status(__pachaos_raw2(PACHAOS_SYSCALL_FD_TABLE,(long)wanted,(long)&info));
}

/* Every request gets its own VMO: concurrent IPC never shares a mutable page,
 * and byte capacity follows the actual operation rather than an inline cap. */
#ifndef PACHAOS_NATIVE_TEST
static int fs_call(kb2_fs_request_t *r,const char *path,const char *second,
    const void *data,size_t data_size,void *out,size_t capacity,
    kb2_fs_response_t *response,const int *sources,size_t source_count,
    int *received,size_t *received_count)
{
    static uint64_t sequence;
    size_t n1=path ? strlen(path)+1 : 0,n2=second ? strlen(second)+1 : 0;
    if (n1>SIZE_MAX-n2 || data_size>SIZE_MAX-n1-n2 ||
        n1+n2+data_size>SIZE_MAX-KB2_FILESYSTEM_REQUEST_SIZE ||
        capacity>SIZE_MAX-KB2_FILESYSTEM_RESPONSE_SIZE || source_count>17)
        return -EOVERFLOW;
    size_t request_size=KB2_FILESYSTEM_REQUEST_SIZE+n1+n2+data_size;
    size_t response_size=KB2_FILESYSTEM_RESPONSE_SIZE+capacity;
    size_t size=request_size>response_size ? request_size : response_size;
    if (size>SIZE_MAX-4095) return -EOVERFLOW;
    size=(size+4095)&~(size_t)4095;
    int room=descriptor_room(0,3);
    if (room) return room;
    r->generation=1;
    r->sequence=__atomic_add_fetch(&sequence,1,__ATOMIC_RELAXED);
    if (!r->sequence) return -EOVERFLOW;
    uint64_t rights=PACHAOS_FD_RIGHT_INSPECT|PACHAOS_FD_RIGHT_CLOSE|
        PACHAOS_FD_RIGHT_MAP_READ|PACHAOS_FD_RIGHT_MAP_WRITE;
    int vmo=(int)fd_status(__pachaos_raw3(PACHAOS_SYSCALL_VMO_CREATE,size,
        rights|PACHAOS_FD_RIGHT_TRANSFER,PACHAOS_FD_FLAG_CLOEXEC));
    if (vmo<0) return vmo;
    uint8_t *p=(void *)__pachaos_raw6(PACHAOS_SYSCALL_MMAP,vmo,0,size,
        PACHAOS_PROT_READ|PACHAOS_PROT_WRITE,PACHAOS_MMAP_SHARED,0);
    if ((uintptr_t)p<4096 || (intptr_t)p<0) {
        (void)__pachaos_raw1(PACHAOS_SYSCALL_FD_CLOSE,vmo); return -ENOMEM;
    }
    size_t used=KB2_FILESYSTEM_REQUEST_SIZE;
    r->path=r->second_path=r->data=(kb2_fs_span_t){0};
    if (n1) { r->path=(kb2_fs_span_t){used,n1}; memcpy(p+used,path,n1); used+=n1; }
    if (n2) { r->second_path=(kb2_fs_span_t){used,n2}; memcpy(p+used,second,n2); used+=n2; }
    if (data_size) { r->data=(kb2_fs_span_t){used,data_size}; memcpy(p+used,data,data_size); }
    int status=kb2_fs_request_encode(p,KB2_FILESYSTEM_REQUEST_SIZE,request_size,r);
    struct __pachaos_ipc_fd send[19]={{.fd=vmo,.rights=rights,.transfer_flags=2}};
    struct { uint64_t kind,rights,flags,size,extra; } info;
    for (size_t i=0;!status && i<source_count;++i) {
        status=(int)native_status(__pachaos_raw2(PACHAOS_SYSCALL_FD_GET_INFO,sources[i],(long)&info));
        if (!status) send[i+1]=(struct __pachaos_ipc_fd){.fd=sources[i],.rights=info.rights,.transfer_flags=2};
    }
    struct __pachaos_ipc_msg message={.word0=r->opcode,.word1=request_size,
        .word2=response_size,.word3=r->sequence,.fds=send,.fd_count=source_count+1};
    struct __pachaos_ipc_fd got[19]={{0}};
    struct __pachaos_ipc_msg reply={.fds=got,.fd_capacity=19};
    if (!status) {
        int reply_fd=(int)fd_status(__pachaos_raw2(PACHAOS_SYSCALL_IPC_CALL,FILE_ENDPOINT,(long)&message));
        if (reply_fd<0) status=reply_fd;
        else {
            long s;
            do { s=__pachaos_raw4(PACHAOS_SYSCALL_IPC_RECV_WAIT,reply_fd,(long)&reply,-1,0); } while (s==2);
            status=(int)native_status(s);
            (void)__pachaos_raw1(PACHAOS_SYSCALL_FD_CLOSE,reply_fd);
        }
    }
    if (!status && (reply.word0!=r->opcode || reply.word3!=r->sequence ||
        reply.word2<KB2_FILESYSTEM_RESPONSE_SIZE || reply.word2>response_size ||
        kb2_fs_response_decode(p,reply.word2,response) ||
        !kb2_fs_response_matches(response,r) ||
        reply.word1!=(uint64_t)response->result || response->data_length>capacity)) status=-EPROTO;
    if (!status && response->result<0 && reply.fd_count) status=-EPROTO;
    size_t count=received_count && received ? *received_count : 0;
    if (!status && reply.fd_count>count) status=-EPROTO;
    if (count>reply.fd_count) count=reply.fd_count;
    if (status) count=0;
    for (size_t i=0;i<reply.fd_count;++i) {
        if (i<count) received[i]=(int)got[i].fd;
        else (void)__pachaos_raw1(PACHAOS_SYSCALL_FD_CLOSE,got[i].fd);
    }
    if (received_count) *received_count=count;
    if (!status && response->data_length) memcpy(out,p+KB2_FILESYSTEM_RESPONSE_SIZE,response->data_length);
    (void)__pachaos_raw2(PACHAOS_SYSCALL_MUNMAP,(long)p,size);
    (void)__pachaos_raw1(PACHAOS_SYSCALL_FD_CLOSE,vmo);
    return status ? status : response->result<0 ? (int)response->result : 0;
}
#else
extern int fs_call(kb2_fs_request_t *,const char *,const char *,const void *,size_t,
    void *,size_t,kb2_fs_response_t *,const int *,size_t,int *,size_t *);
#endif
static long request(kb2_fs_request_t *r,const char *a,const char *b,
                     const void *data,size_t size,void *out,size_t capacity)
{
    kb2_fs_response_t reply;
    int s=fs_call(r,a,b,data,size,out,capacity,&reply,NULL,0,NULL,NULL);
    return s ? s : reply.result;
}
static long directory(long fd,uint64_t *handle)
{
    if (fd==AT_FDCWD) { *handle=current_directory; return 0; }
    struct native_file *p=lookup(fd);
    if (!p) return -EBADF;
    *handle=p->handle; return 0;
}
static long path_directory(long fd,const char *path,uint64_t *handle)
{
    if (!path) return -EFAULT;
    if (path[0]=='/') { *handle=0; return 0; }
    return directory(fd,handle);
}
static long close_handle(uint64_t handle)
{
    kb2_fs_request_t r={.opcode=KB2_FILESYSTEM_OP_CLOSE,.handle=handle};
    return request(&r,NULL,NULL,NULL,0,NULL,0);
}
static int reserve_fd(int minimum,int cloexec)
{
    /* A real native descriptor reserves the number against capability grants.
     * A synthetic high-number table would collide with future IPC descriptors. */
    uint64_t rights=PACHAOS_FD_RIGHT_INSPECT|PACHAOS_FD_RIGHT_CLOSE|(1ull<<1)|PACHAOS_FD_RIGHT_SET_FLAGS;
    int room=descriptor_room((size_t)minimum+1,3);
    if (room) return room;
    int fd=(int)fd_status(__pachaos_raw3(PACHAOS_SYSCALL_EVENTFD_CREATE,0,rights,
        cloexec ? PACHAOS_FD_FLAG_CLOEXEC : 0));
    if (fd>=0 && fd<minimum) {
        int copy=(int)fd_status(__pachaos_raw4(PACHAOS_SYSCALL_FD_DUP,fd,minimum,rights,
            cloexec ? PACHAOS_FD_FLAG_CLOEXEC : 0));
        (void)__pachaos_raw1(PACHAOS_SYSCALL_FD_CLOSE,fd); fd=copy;
    }
    return fd;
}
static long install(uint64_t handle,int minimum,int cloexec)
{
    struct native_file *p=malloc(sizeof(*p));
    if (!p) { (void)close_handle(handle); return -ENOMEM; }
    int fd=reserve_fd(minimum,cloexec);
    if (fd<0) { free(p); (void)close_handle(handle); return fd; }
    *p=(struct native_file){fd,handle,files}; files=p; return fd;
}
static long open_file(long dir,const char *path,long flags,long mode)
{
    kb2_fs_request_t r={.opcode=KB2_FILESYSTEM_OP_OPENAT2,.flags=(uint64_t)flags,
        .mode=(flags & (O_CREAT|O_TMPFILE)) ? (uint64_t)mode : 0,.umask=creation_mask};
    long s=path_directory(dir,path,&r.handle);
    if (s) return s;
    kb2_fs_response_t reply;
    s=fs_call(&r,path,NULL,NULL,0,NULL,0,&reply,NULL,0,NULL,NULL);
    return s ? s : install(reply.handle,16,!!(flags&O_CLOEXEC));
}
static long close_file(long fd)
{
    struct native_file **p=&files;
    while (*p && (*p)->fd!=fd) p=&(*p)->next;
    if (!*p) return native_status(__pachaos_raw1(PACHAOS_SYSCALL_FD_CLOSE,fd));
    struct native_file *old=*p; *p=old->next;
    long s=old->handle ? close_handle(old->handle) : 0;
    (void)__pachaos_raw1(PACHAOS_SYSCALL_FD_CLOSE,fd); free(old); return s;
}
static long io_file(long fd,void *buffer,long length,long offset,int write,int positioned,int dents)
{
    if (length<0) return -EINVAL;
    if (length && !buffer) return -EFAULT;
    struct native_file *p=lookup(fd);
    if (!p) {
        if (positioned || dents) return -EBADF;
        return __pachaos_raw3(write ? PACHAOS_SYSCALL_FD_WRITE : PACHAOS_SYSCALL_FD_READ,fd,(long)buffer,length);
    }
    kb2_fs_request_t r={.opcode=dents ? KB2_FILESYSTEM_OP_GETDENTS64 :
        write ? positioned ? KB2_FILESYSTEM_OP_PWRITE : KB2_FILESYSTEM_OP_WRITE :
        positioned ? KB2_FILESYSTEM_OP_PREAD : KB2_FILESYSTEM_OP_READ,
        .handle=p->handle,.offset=offset,.length=(uint64_t)length};
    kb2_fs_response_t reply;
    long s=fs_call(&r,NULL,NULL,write ? buffer : NULL,write ? (size_t)length : 0,
        write ? NULL : buffer,write ? 0 : (size_t)length,&reply,NULL,0,NULL,NULL);
    if (s) return s;
    if (reply.result<0 || (uint64_t)reply.result>(uint64_t)length ||
        (!write && reply.data_length!=(uint64_t)reply.result)) return -EPROTO;
    return reply.result;
}
static long vector_io(long fd,const struct __pachaos_iovec *iov,long count,int write)
{
    if (count<0 || count>1024 || (count && !iov)) return -EINVAL;
    size_t total=0;
    for (long i=0;i<count;++i) {
        if (iov[i].len>(size_t)LONG_MAX-total) return -EINVAL;
        if (iov[i].len && !iov[i].base) return -EFAULT;
        total+=iov[i].len;
    }
    if (!total) return 0;
    void *data=malloc(total);
    if (!data) return -ENOMEM;
    size_t at=0;
    if (write) for (long i=0;i<count;++i) { memcpy((char *)data+at,iov[i].base,iov[i].len); at+=iov[i].len; }
    long n=io_file(fd,data,(long)total,0,write,0,0);
    if (!write && n>0) {
        at=0;
        for (long i=0;i<count && at<(size_t)n;++i) {
            size_t size=iov[i].len; if (size>(size_t)n-at) size=(size_t)n-at;
            memcpy(iov[i].base,(char *)data+at,size); at+=size;
        }
    }
    free(data); return n;
}
static long stat_file(long fd,const char *path,struct __pachaos_kstat *st,long flags)
{
    if (!st) return -EFAULT;
    if ((flags&AT_EMPTY_PATH) && (!path || !path[0]) && fd!=AT_FDCWD && !lookup(fd)) {
        /* FD_STAT reports one generic native error for bad descriptors and
         * copyout faults. Check descriptor existence separately so omitted
         * CLOEXEC descriptors retain Linux fstat's EBADF contract. */
        struct {uint64_t kind,rights,flags,size,extra;} info;
        if(__pachaos_raw2(PACHAOS_SYSCALL_FD_GET_INFO,fd,(long)&info))return -EBADF;
        return native_status(__pachaos_raw2(PACHAOS_SYSCALL_FD_STAT,fd,(long)st));
    }
    kb2_fs_request_t r={.opcode=KB2_FILESYSTEM_OP_STATX,.flags=flags,.mask=0x7ff};
    long s=path ? path_directory(fd,path,&r.handle) : directory(fd,&r.handle);
    if (s) return s;
    unsigned char raw[KB2_FILESYSTEM_STATX_SIZE];
    kb2_fs_response_t reply;
    s=fs_call(&r,path,NULL,NULL,0,raw,sizeof(raw),&reply,NULL,0,NULL,NULL);
    if (s) return s;
    if (reply.data_length!=sizeof(raw)) return -EPROTO;
    memset(st,0,sizeof(*st));
#define R32(f) __pachaos_rd32(raw+KB2_FILESYSTEM_STATX_##f##_OFFSET)
#define R64(f) __pachaos_rd64(raw+KB2_FILESYSTEM_STATX_##f##_OFFSET)
    st->st_ino=R64(INO); st->st_size=R64(SIZE); st->st_blocks=R64(BLOCKS);
    st->st_nlink=R32(NLINK); st->st_uid=R32(UID); st->st_gid=R32(GID);
    st->st_mode=__pachaos_rd16(raw+KB2_FILESYSTEM_STATX_MODE_OFFSET);
    st->st_blksize=R32(BLKSIZE);
    uint64_t major=R32(DEV_MAJOR),minor=R32(DEV_MINOR);
    st->st_dev=(minor&255)|((major&4095)<<8)|((minor&~255ull)<<12)|((major&~4095ull)<<32);
    major=R32(RDEV_MAJOR); minor=R32(RDEV_MINOR);
    st->st_rdev=(minor&255)|((major&4095)<<8)|((minor&~255ull)<<12)|((major&~4095ull)<<32);
    st->st_atime_sec=(int64_t)R64(ATIME); st->st_atime_nsec=__pachaos_rd32(raw+KB2_FILESYSTEM_STATX_ATIME_OFFSET+8);
    st->st_mtime_sec=(int64_t)R64(MTIME); st->st_mtime_nsec=__pachaos_rd32(raw+KB2_FILESYSTEM_STATX_MTIME_OFFSET+8);
    st->st_ctime_sec=(int64_t)R64(CTIME); st->st_ctime_nsec=__pachaos_rd32(raw+KB2_FILESYSTEM_STATX_CTIME_OFFSET+8);
#undef R32
#undef R64
    return 0;
}
static long fcntl_file(long fd,long command,long value)
{
    struct native_file *p=lookup(fd);
    if (command==F_GETFD || command==F_SETFD) {
        long n=__pachaos_raw4(PACHAOS_SYSCALL_FD_FCNTL,fd,command==F_GETFD ? 1 : 2,
            command==F_SETFD ? (value&FD_CLOEXEC ? PACHAOS_FD_FLAG_CLOEXEC : 0) : 0,
            command==F_SETFD ? PACHAOS_FD_FLAG_CLOEXEC : 0);
        return command==F_GETFD && n>=0 ? !!(n&PACHAOS_FD_FLAG_CLOEXEC) : native_status(n);
    }
    if (command==F_DUPFD || command==F_DUPFD_CLOEXEC) {
        if (value<0 || value>INT_MAX) return -EINVAL;
        if (!p) {
            struct { uint64_t kind,rights,flags,size,extra; } info;
            long s=native_status(__pachaos_raw2(PACHAOS_SYSCALL_FD_GET_INFO,fd,(long)&info));
            return s ? s : fd_status(__pachaos_raw4(PACHAOS_SYSCALL_FD_DUP,fd,value,info.rights,
                command==F_DUPFD_CLOEXEC ? PACHAOS_FD_FLAG_CLOEXEC : 0));
        }
        kb2_fs_request_t r={.opcode=KB2_FILESYSTEM_OP_DUP,.handle=p->handle};
        kb2_fs_response_t reply;
        long s=fs_call(&r,NULL,NULL,NULL,0,NULL,0,&reply,NULL,0,NULL,NULL);
        return s ? s : install(reply.handle,(int)value,command==F_DUPFD_CLOEXEC);
    }
    if (!p) return -EOPNOTSUPP;
    kb2_fs_request_t r={.opcode=KB2_FILESYSTEM_OP_FCNTL,.handle=p->handle,
        .flags=(uint64_t)command,.length=(uint64_t)value};
    return request(&r,NULL,NULL,NULL,0,NULL,0);
}
static long poll_native(struct __pachaos_linux_pollfd *fds,long count,long timeout)
{
    if (count<0 || (size_t)count>SIZE_MAX/sizeof(struct __pachaos_pollfd)) return -EINVAL;
    if (count && !fds) return -EFAULT;
    struct { uint64_t capacity,maximum,free_slots; } table;
    long status=native_status(__pachaos_raw2(PACHAOS_SYSCALL_FD_TABLE,0,(long)&table));
    if (status) return status;
    if ((uint64_t)count>table.maximum) return -EINVAL;
    struct __pachaos_pollfd *native=calloc(count ? (size_t)count : 1,sizeof(*native));
    if (!native) return -ENOMEM;
    size_t active=0;
    for (long i=0;i<count;++i) {
        fds[i].revents=0;
        if (fds[i].fd<0) continue;
        /* Filesystem polling needs the Linux waitqueue export, not the
         * reservation eventfd: reporting that event's readiness is wrong. */
        if (lookup(fds[i].fd)) { free(native); return -EOPNOTSUPP; }
        native[active].fd=fds[i].fd;
        native[active++].events=__pachaos_poll_events_from_linux(fds[i].events);
    }
    status=__pachaos_raw2(PACHAOS_SYSCALL_FD_POLL,(long)native,(long)active);
    if (!status && timeout) {
        do {
            status=__pachaos_raw4(PACHAOS_SYSCALL_FD_WAIT_MANY,(long)native,
                (long)active,timeout<0 ? -1 : timeout,0);
            if (status==2) status=__pachaos_raw2(PACHAOS_SYSCALL_FD_POLL,(long)native,(long)active);
        } while (!status && timeout<0);
    }
    active=0;
    long ready=0;
    for (long i=0;i<count;++i) if (fds[i].fd>=0) {
        fds[i].revents=__pachaos_poll_events_to_linux(native[active++].revents);
        if (fds[i].revents) ++ready;
    }
    free(native); return status<0 ? status : ready ? ready : status ? -EIO : 0;
}

static long mmap_file(long addr,long len,long prot,long flags,long fd,long offset)
{
    if (len<=0 || offset<0 || (offset&4095)) return -EINVAL;
    struct native_file *p=flags&MAP_ANONYMOUS ? NULL : lookup(fd);
    if (!p) return __pachaos_mmap_result(__pachaos_raw6(PACHAOS_SYSCALL_MMAP,
        flags&MAP_ANONYMOUS ? 0 : fd,addr,len,__pachaos_prot(prot),__pachaos_mmap_flags(flags),offset));
    kb2_fs_request_t r={.opcode=flags&MAP_SHARED ? KB2_FILESYSTEM_OP_SHARED_FILE_VMO : KB2_FILESYSTEM_OP_FILE_VMO,
        .handle=p->handle,.offset=offset,.length=(uint64_t)len,
        .flags=(prot&PROT_EXEC ? FILED_FILE_VMO_EXEC : 0)|(prot&PROT_WRITE ? FILED_FILE_VMO_WRITE : 0)};
    kb2_fs_response_t reply;
    int vmo=-1; size_t count=1;
    long s=fs_call(&r,NULL,NULL,NULL,0,NULL,0,&reply,NULL,0,&vmo,&count);
    if (s) return s;
    if (count!=1 || vmo<16) return -EPROTO;
    s=__pachaos_mmap_result(__pachaos_raw6(PACHAOS_SYSCALL_MMAP,vmo,addr,len,
        __pachaos_prot(prot),__pachaos_mmap_flags(flags),0));
    (void)__pachaos_raw1(PACHAOS_SYSCALL_FD_CLOSE,vmo); return s;
}

static void wr64(unsigned char *p,uint64_t v)
{
    for (unsigned i=0;i<8;++i) p[i]=(unsigned char)(v>>(i*8));
}
static void close_files(void)
{
    while (files) (void)close_file(files->fd);
    if (current_directory) { (void)close_handle(current_directory); current_directory=0; }
}
static long initialized;
static long inherit_state(void)
{
    if (initialized) return initialized<0 ? initialized : 0;
    initialized=1;
    struct { uint64_t kind,rights,flags,size,extra; } info;
    if (__pachaos_raw2(PACHAOS_SYSCALL_FD_GET_INFO,BOOTSTRAP_FD,(long)&info) || info.kind!=4 || info.size<32)
        return 0;
    unsigned char *bytes=(void *)__pachaos_raw6(PACHAOS_SYSCALL_MMAP,BOOTSTRAP_FD,0,
        info.size,PACHAOS_PROT_READ,PACHAOS_MMAP_SHARED,0);
    if ((uintptr_t)bytes<4096 || (intptr_t)bytes<0) return initialized=-ENOMEM;
    long status=0;
    if (__pachaos_rd64(bytes)==NATIVE_STATE_MAGIC) {
        uint64_t count=__pachaos_rd64(bytes+8);
        if (count>(info.size-32)/16) status=-EPROTO;
        else {
            current_directory=__pachaos_rd64(bytes+16);
            creation_mask=(uint32_t)__pachaos_rd64(bytes+24)&0777;
            for (uint64_t i=0;!status && i<count;++i) {
                uint64_t fd=__pachaos_rd64(bytes+32+i*16),handle=__pachaos_rd64(bytes+40+i*16);
                if (fd<16 || fd>INT_MAX || !handle || lookup((long)fd)) { status=-EPROTO; break; }
                long installed=install(handle,(int)fd,0);
                if (installed!=(long)fd) { if (installed>=0) (void)close_file(installed); status=-EPROTO; }
            }
        }
    }
    (void)__pachaos_raw2(PACHAOS_SYSCALL_MUNMAP,(long)bytes,info.size);
    if (status) initialized=status;
    return status;
}
static long exec_file(const char *path,char *const *argv,char *const *envp)
{
    if (!path) return -EFAULT;
    filed_exec_path_t exec={.flags=FILED_EXEC_INHERIT_FDS|FILED_EXEC_BOOTSTRAP_FD|
        FILED_EXEC_INHERIT_HANDLES|FILED_EXEC_PATCH_BOOTSTRAP_FDS,.path=(char *)path};
    size_t string_size=0;
    char *const *lists[2]={argv,envp};
    uint64_t counts[2]={0};
    for (unsigned list=0;list<2;++list) if (lists[list])
        for (;lists[list][counts[list]];++counts[list]) {
            size_t n=strlen(lists[list][counts[list]])+1;
            /* Linux's per-string limit is architectural (32 pages), not the
             * old filed argv/environment/string-array assumption. */
            if (n>32*4096 || n>SIZE_MAX-string_size || counts[list]>=SIZE_MAX/16) return -E2BIG;
            string_size+=n;
        }
    exec.argc=counts[0]; exec.envc=counts[1]; exec.string_bytes=string_size;
    exec.argv=calloc(exec.argc,sizeof(*exec.argv)); exec.envp=calloc(exec.envc,sizeof(*exec.envp));
    exec.strings=string_size ? malloc(string_size) : NULL;
    if ((exec.argc && !exec.argv) || (exec.envc && !exec.envp) || (string_size && !exec.strings)) {
        free(exec.argv); free(exec.envp); free(exec.strings); return -ENOMEM;
    }
    size_t pos=0;
    for (unsigned list=0;list<2;++list) {
        filed_exec_string_ref_t *refs=list ? exec.envp : exec.argv;
        for (uint64_t i=0;i<counts[list];++i) {
            size_t n=strlen(lists[list][i])+1;
            refs[i]=(filed_exec_string_ref_t){pos,n}; memcpy(exec.strings+pos,lists[list][i],n); pos+=n;
        }
    }
    size_t count=0;
    for (struct native_file *p=files;p;p=p->next) {
        long flags=fcntl_file(p->fd,F_GETFD,0);
        if (flags<0) { free(exec.argv); free(exec.envp); free(exec.strings); return flags; }
        if (!(flags&FD_CLOEXEC)) ++count;
    }
    if (count>(SIZE_MAX-32)/16 || count==SIZE_MAX) {
        free(exec.argv); free(exec.envp); free(exec.strings); return -EOVERFLOW;
    }
    size_t state_size=32+count*16;
    exec.inherit_handle_count=count+!!current_directory;
    exec.fd_patch_count=exec.inherit_handle_count;
    exec.inherit_handles=calloc(exec.inherit_handle_count,sizeof(*exec.inherit_handles));
    exec.fd_patches=calloc(exec.fd_patch_count,sizeof(*exec.fd_patches));
    unsigned char *state=calloc(1,state_size);
    long status=0;
    uint8_t *payload=NULL; size_t payload_size=0;
    int bootstrap=-1; unsigned char *mapping=NULL; size_t mapped_size=0;
    if (!state || (exec.inherit_handle_count && (!exec.inherit_handles || !exec.fd_patches))) { status=-ENOMEM; goto done; }
    wr64(state,NATIVE_STATE_MAGIC); wr64(state+8,count); wr64(state+24,creation_mask);
    pos=0;
    for (struct native_file *p=files;p;p=p->next) if (!(fcntl_file(p->fd,F_GETFD,0)&FD_CLOEXEC)) {
        wr64(state+32+pos*16,p->fd);
        exec.inherit_handles[pos]=p->handle;
        exec.fd_patches[pos]=(filed_exec_fd_patch_t){FILED_EXEC_PATCH_INHERIT_HANDLE,pos,40+pos*16,0}; ++pos;
    }
    if (current_directory) {
        exec.inherit_handles[pos]=current_directory;
        exec.fd_patches[pos]=(filed_exec_fd_patch_t){FILED_EXEC_PATCH_INHERIT_HANDLE,pos,16,0};
    }
    /* filed grants the native child READ as well as MAP_READ; the source
     * capability must authorize that grant before PROCESS_CREATE. */
    uint64_t rights=PACHAOS_FD_RIGHT_INSPECT|PACHAOS_FD_RIGHT_CLOSE|PACHAOS_FD_RIGHT_TRANSFER|
        PACHAOS_FD_RIGHT_READ|PACHAOS_FD_RIGHT_MAP_READ|PACHAOS_FD_RIGHT_MAP_WRITE;
    mapped_size=(state_size+4095)&~(size_t)4095;
    status=descriptor_room(0,3);
    if (status) goto done;
    bootstrap=(int)fd_status(__pachaos_raw3(PACHAOS_SYSCALL_VMO_CREATE,mapped_size,rights,PACHAOS_FD_FLAG_CLOEXEC));
    if (bootstrap<0) { status=bootstrap; goto done; }
    mapping=(void *)__pachaos_raw6(PACHAOS_SYSCALL_MMAP,bootstrap,0,mapped_size,
        PACHAOS_PROT_READ|PACHAOS_PROT_WRITE,PACHAOS_MMAP_SHARED,0);
    if ((uintptr_t)mapping<4096 || (intptr_t)mapping<0) { mapping=NULL; status=-ENOMEM; goto done; }
    memcpy(mapping,state,state_size);
    struct { uint64_t kind,rights,flags,size,extra; } info;
    status=native_status(__pachaos_raw2(PACHAOS_SYSCALL_FD_GET_INFO,FILE_ENDPOINT,(long)&info));
    if (status) goto done;
    filed_exec_fd_grant_t grant={.target=FILE_ENDPOINT,.rights=info.rights};
    exec.inherit_fd_count=1; exec.fd_grants=&grant;
    status=filed_exec_payload_encode(&exec,&payload,&payload_size);
    if (status) goto done;
    kb2_fs_request_t r={.opcode=KB2_FILESYSTEM_OP_EXEC_PATH,.flags=exec.flags};
    status=path_directory(AT_FDCWD,path,&r.handle);
    if (status) goto done;
    int sources[2]={FILE_ENDPOINT,bootstrap},received[2]={-1,-1}; size_t received_count=2;
    kb2_fs_response_t reply;
    status=fs_call(&r,path,NULL,payload,payload_size,NULL,0,&reply,sources,2,received,&received_count);
    if (!status && received_count!=2) status=-EPROTO;
    for (size_t i=0;i<received_count;++i)
        if (received[i]>=16) (void)__pachaos_raw1(PACHAOS_SYSCALL_FD_CLOSE,received[i]);
done:
    if (mapping) (void)__pachaos_raw2(PACHAOS_SYSCALL_MUNMAP,(long)mapping,mapped_size);
    if (bootstrap>=16) (void)__pachaos_raw1(PACHAOS_SYSCALL_FD_CLOSE,bootstrap);
    free(payload); free(state); free(exec.argv); free(exec.envp); free(exec.strings);
    free(exec.inherit_handles); free(exec.fd_patches);
    /* This retains the native port's existing spawn-and-exit exec contract;
     * same-PID POSIX replacement is a separate process-runtime change. */
    if (!status) { close_files(); (void)__pachaos_raw1(PACHAOS_SYSCALL_PROCESS_EXIT,0); for (;;) __asm__ volatile("pause"); }
    return status;
}

static long dispatch(long n,long a,long b,long c,long d,long e,long f)
{
    kb2_fs_request_t r={0};
    struct native_file *p;
    long s;
    switch (n) {
    case SYS_open: return open_file(AT_FDCWD,(const char *)a,b,c);
    case SYS_openat: return open_file(a,(const char *)b,c,d);
    case SYS_close: return close_file(a);
    case SYS_read: return io_file(a,(void *)b,c,0,0,0,0);
    case SYS_write: return io_file(a,(void *)b,c,0,1,0,0);
    case SYS_pread64: return io_file(a,(void *)b,c,d,0,1,0);
    case SYS_pwrite64: return io_file(a,(void *)b,c,d,1,1,0);
    case SYS_readv: return vector_io(a,(const void *)b,c,0);
    case SYS_writev: return vector_io(a,(const void *)b,c,1);
    case SYS_getdents64: return io_file(a,(void *)b,c,0,0,0,1);
    case SYS_getdents: return -EOPNOTSUPP;
    case SYS_fcntl: return fcntl_file(a,b,c);
    case SYS_dup: return fcntl_file(a,F_DUPFD,0);
    case SYS_sync:
        r.opcode=KB2_FILESYSTEM_OP_SYNC;
        return request(&r,NULL,NULL,NULL,0,NULL,0);
    case SYS_lseek:
    case SYS_ftruncate:
    case SYS_fsync:
    case SYS_fdatasync:
    case SYS_syncfs:
        p=lookup(a); if (!p) return -EBADF;
        r.opcode=n==SYS_lseek ? KB2_FILESYSTEM_OP_LSEEK : n==SYS_ftruncate ? KB2_FILESYSTEM_OP_FTRUNCATE :
            n==SYS_syncfs ? KB2_FILESYSTEM_OP_SYNCFS : KB2_FILESYSTEM_OP_FSYNC;
        r.handle=p->handle; r.offset=b; r.length=b;
        r.flags=n==SYS_lseek ? c : n==SYS_fdatasync;
        return request(&r,NULL,NULL,NULL,0,NULL,0);
    case SYS_fstat: return stat_file(a,"",(void *)b,AT_EMPTY_PATH);
    case SYS_stat: return stat_file(AT_FDCWD,(const char *)a,(void *)b,0);
    case SYS_lstat: return stat_file(AT_FDCWD,(const char *)a,(void *)b,AT_SYMLINK_NOFOLLOW);
    case SYS_newfstatat: return stat_file(a,(const char *)b,(void *)c,d);
    case SYS_truncate:
        s=open_file(AT_FDCWD,(const char *)a,O_WRONLY|O_CLOEXEC,0);
        if (s<0) return s;
        p=lookup(s); r.opcode=KB2_FILESYSTEM_OP_FTRUNCATE; r.handle=p->handle; r.offset=b;
        b=request(&r,NULL,NULL,NULL,0,NULL,0); (void)close_file(s); return b;
    case SYS_execve: return exec_file((const char *)a,(void *)b,(void *)c);
    case SYS_access: c=b; b=a; a=AT_FDCWD; /* fall through */
    case SYS_faccessat:
    case SYS_faccessat2:
        r.opcode=KB2_FILESYSTEM_OP_FACCESSAT2; r.mode=c; r.flags=n==SYS_faccessat2 ? d : 0;
        s=path_directory(a,(const char *)b,&r.handle); if (s) return s;
        return request(&r,(const char *)b,NULL,NULL,0,NULL,0);
    case SYS_mkdir: d=c; c=b; b=a; a=AT_FDCWD; /* fall through */
    case SYS_mkdirat:
    case SYS_unlinkat:
    case SYS_fchmodat:
        r.opcode=n==SYS_unlinkat ? KB2_FILESYSTEM_OP_UNLINKAT : n==SYS_fchmodat ? KB2_FILESYSTEM_OP_FCHMODAT : KB2_FILESYSTEM_OP_MKDIRAT;
        r.mode=c; r.flags=n==SYS_unlinkat ? c : 0; r.umask=creation_mask;
        s=path_directory(a,(const char *)b,&r.handle); if (s) return s;
        return request(&r,(const char *)b,NULL,NULL,0,NULL,0);
    case SYS_unlink:
    case SYS_rmdir:
        r.opcode=KB2_FILESYSTEM_OP_UNLINKAT; r.flags=n==SYS_rmdir ? AT_REMOVEDIR : 0;
        s=path_directory(AT_FDCWD,(const char *)a,&r.handle); if (s) return s;
        return request(&r,(const char *)a,NULL,NULL,0,NULL,0);
    case SYS_link: d=b; b=a; a=c=AT_FDCWD; /* fall through */
    case SYS_linkat:
        r.opcode=KB2_FILESYSTEM_OP_LINKAT;r.flags=n==SYS_linkat ? e : 0;
        s=path_directory(a,(const char *)b,&r.handle);if(s)return s;
        s=path_directory(c,(const char *)d,&r.second_handle);if(s)return s;
        return request(&r,(const char *)b,(const char *)d,NULL,0,NULL,0);
    case SYS_symlink: c=b;b=AT_FDCWD; /* fall through */
    case SYS_symlinkat:
        if(!a)return -EFAULT;
        r.opcode=KB2_FILESYSTEM_OP_SYMLINKAT;
        /* The target is stored verbatim; only the new link's pathname is
         * relative to dirfd. The service wire places target before new name. */
        s=path_directory(b,(const char *)c,&r.handle);if(s)return s;
        return request(&r,(const char *)a,(const char *)c,NULL,0,NULL,0);
    case SYS_rename: d=b; b=a; a=c=AT_FDCWD; /* fall through */
    case SYS_renameat:
    case SYS_renameat2:
        r.opcode=KB2_FILESYSTEM_OP_RENAMEAT2; r.flags=n==SYS_renameat2 ? e : 0;
        s=path_directory(a,(const char *)b,&r.handle); if (s) return s;
        s=path_directory(c,(const char *)d,&r.second_handle); if (s) return s;
        return request(&r,(const char *)b,(const char *)d,NULL,0,NULL,0);
    case SYS_readlink: d=c; c=b; b=a; a=AT_FDCWD; /* fall through */
    case SYS_readlinkat:
        if (d<=0 || !c) return -EINVAL;
        r.opcode=KB2_FILESYSTEM_OP_READLINKAT; r.length=d;
        s=path_directory(a,(const char *)b,&r.handle); if (s) return s;
        return request(&r,(const char *)b,NULL,NULL,0,(void *)c,d);
    case SYS_umask: s=creation_mask; creation_mask=(uint32_t)a&0777; return s;
    case SYS_chdir:
        s=open_file(AT_FDCWD,(const char *)a,O_PATH|O_DIRECTORY|O_CLOEXEC,0);
        if (s<0) return s;
        p=lookup(s);
        r.opcode=KB2_FILESYSTEM_OP_FACCESSAT2; r.handle=p->handle;
        r.mode=X_OK; r.flags=AT_EMPTY_PATH|AT_EACCESS;
        b=request(&r,"",NULL,NULL,0,NULL,0);
        if (b<0) { (void)close_file(s); return b; }
        if (current_directory) (void)close_handle(current_directory);
        current_directory=p->handle;
        p->handle=0;
        (void)close_file(s); return 0;
    case SYS_getcwd:
        if (!a) return -EFAULT;
        if (b<=0) return -EINVAL;
        if (!current_directory) {
            if (b<2) return -ERANGE;
            memcpy((void *)a,"/",2); return 2;
        }
        r.opcode=KB2_FILESYSTEM_OP_READLINK_HANDLE; r.handle=current_directory; r.length=(uint64_t)b-1; r.flags=1;
        s=request(&r,NULL,NULL,NULL,0,(void *)a,(size_t)b-1);
        if (s<0) return s;
        if (s>=b-1) return -ERANGE;
        ((char *)a)[s]=0; return s+1;
    case SYS_mmap: return mmap_file(a,b,c,d,e,f);
    case SYS_munmap: return native_status(__pachaos_raw2(PACHAOS_SYSCALL_MUNMAP,a,b));
    case SYS_mprotect: return native_status(__pachaos_raw3(PACHAOS_SYSCALL_MPROTECT,a,b,__pachaos_prot(c)));
    case SYS_madvise: return __pachaos_madvise(a,b,c);
    case SYS_mremap: return __pachaos_mremap(a,b,c,d,e);
    case SYS_brk: return __pachaos_brk(a);
    case SYS_getpid: return __pachaos_raw0(PACHAOS_SYSCALL_GETPID);
    case SYS_gettid:
    case SYS_set_tid_address: return __pachaos_raw0(PACHAOS_SYSCALL_GETTID);
    case SYS_sched_yield: return native_status(__pachaos_raw0(PACHAOS_SYSCALL_YIELD));
    case SYS_clock_gettime: return native_status(__pachaos_raw2(PACHAOS_SYSCALL_CLOCK_GETTIME,a,b));
    case SYS_clock_getres: return native_status(__pachaos_raw2(PACHAOS_SYSCALL_CLOCK_GETRES,a,b));
    case SYS_nanosleep: return native_status(__pachaos_raw2(PACHAOS_SYSCALL_NANOSLEEP,a,b));
    case SYS_clock_nanosleep:
        if (b) return -ENOSYS;
        return native_status(__pachaos_raw2(PACHAOS_SYSCALL_NANOSLEEP,c,d));
    case SYS_futex:
        if ((b&127)==0) return native_status(__pachaos_raw4(PACHAOS_SYSCALL_FUTEX_WAIT,a,c,d,0));
        if ((b&127)==1) return __pachaos_raw2(PACHAOS_SYSCALL_FUTEX_WAKE,a,c);
        if ((b&127)==3) return __pachaos_raw4(PACHAOS_SYSCALL_FUTEX_REQUEUE,a,c,d,e);
        return -ENOSYS;
    case SYS_ioctl: return native_status(__pachaos_raw3(PACHAOS_SYSCALL_FD_IOCTL,a,b,c));
    case SYS_poll: return poll_native((void *)a,b,c);
    case SYS_eventfd2:
        if (b&~(O_CLOEXEC|O_NONBLOCK)) return -EINVAL;
        return fd_status(__pachaos_raw3(PACHAOS_SYSCALL_EVENTFD_CREATE,a,
            PACHAOS_FD_RIGHT_INSPECT|PACHAOS_FD_RIGHT_CLOSE|PACHAOS_FD_RIGHT_WAIT|
            PACHAOS_FD_RIGHT_POLL|PACHAOS_FD_RIGHT_READ|PACHAOS_FD_RIGHT_WRITE,
            __pachaos_fd_flags_from_linux(b,O_CLOEXEC,O_NONBLOCK)));
    case SYS_timerfd_create:
        if (a!=1 || (b&~(O_CLOEXEC|O_NONBLOCK))) return -EINVAL;
        return fd_status(__pachaos_raw6(PACHAOS_SYSCALL_TIMERFD_CREATE,a,0,0,0,
            PACHAOS_FD_RIGHT_INSPECT|PACHAOS_FD_RIGHT_CLOSE|PACHAOS_FD_RIGHT_WAIT|
            PACHAOS_FD_RIGHT_POLL|PACHAOS_FD_RIGHT_READ|PACHAOS_FD_RIGHT_WRITE,
            __pachaos_fd_flags_from_linux(b,O_CLOEXEC,O_NONBLOCK)));
    case SYS_timerfd_settime: return native_status(__pachaos_raw4(PACHAOS_SYSCALL_TIMERFD_SETTIME,a,b,c,d));
    case SYS_timerfd_gettime: return native_status(__pachaos_raw2(PACHAOS_SYSCALL_TIMERFD_GETTIME,a,b));
    case SYS_exit:
    case SYS_exit_group: close_files(); return __pachaos_raw1(PACHAOS_SYSCALL_PROCESS_EXIT,a);
    default: return -ENOSYS;
    }
}
long __pachaos_native_syscall(long n,long a,long b,long c,long d,long e,long f)
{
    /* malloc itself enters mmap/brk; locking those recursion paths would
     * deadlock descriptor installation and variable-length request allocation. */
    if (n==SYS_mmap && (d&MAP_ANONYMOUS)) return dispatch(n,a,b,c,d,e,f);
    if (n==SYS_brk || n==SYS_munmap || n==SYS_mprotect || n==SYS_madvise ||
        n==SYS_mremap || n==SYS_futex || n==SYS_sched_yield)
        return dispatch(n,a,b,c,d,e,f);
    lock_files();
    /* A malformed or unavailable bootstrap must fail filesystem operations
     * closed, but cannot prevent process termination (musl retries exit). */
    if (n==SYS_exit || n==SYS_exit_group) {
        long result=dispatch(n,a,b,c,d,e,f);
        unlock_files();return result;
    }
    long result=inherit_state();
    if (!result) result=dispatch(n,a,b,c,d,e,f);
    unlock_files(); return result;
}
