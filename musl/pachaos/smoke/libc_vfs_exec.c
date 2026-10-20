#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#ifdef __pachaos__
extern int getdents(int, struct dirent *, size_t);
#define smoke_getdents getdents
#else
extern ssize_t getdents64(int fd, void *buffer, size_t length);
#define smoke_getdents getdents64
#endif

static unsigned long long now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec;
}

static unsigned long long read_tsc(void)
{
#if defined(__x86_64__) || defined(__i386__)
    uint32_t lo = 0;
    uint32_t hi = 0;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((unsigned long long)hi << 32) | (unsigned long long)lo;
#else
    return 0;
#endif
}

static unsigned long long tsc_calibration_cycles;
static unsigned long long tsc_calibration_ns;

static void calibrate_tsc_timebase(void)
{
    const unsigned long long min_calibration_ns = 20000000ull;
    unsigned long long start_ns = now_ns();
    unsigned long long start_cycles = read_tsc();
    unsigned long long end_ns = start_ns;
    unsigned long long end_cycles = start_cycles;

    if (start_ns == 0 || start_cycles == 0) {
        return;
    }

    do {
        end_ns = now_ns();
        end_cycles = read_tsc();
    } while (end_ns >= start_ns && end_ns - start_ns < min_calibration_ns);

    if (end_ns > start_ns && end_cycles > start_cycles) {
        tsc_calibration_ns = end_ns - start_ns;
        tsc_calibration_cycles = end_cycles - start_cycles;
    }
}

static unsigned long long cycles_to_ns(unsigned long long cycles)
{
    if (cycles == 0) return 0;
    if (tsc_calibration_cycles == 0 || tsc_calibration_ns == 0) return 0;
    long double scaled = (long double)cycles * (long double)tsc_calibration_ns;
    scaled /= (long double)tsc_calibration_cycles;
    if (scaled < 0.0L) return 0;
    return (unsigned long long)scaled;
}

static void metric(const char *name, unsigned long long start_ns, unsigned long long end_ns)
{
    printf("[libc-vfs-exec-smoke] metric op=%s ns=%llu\n", name, end_ns - start_ns);
    fflush(stdout);
}

static void metric_avg(
    const char *name,
    unsigned long long start_ns,
    unsigned long long end_ns,
    unsigned long long iterations)
{
    unsigned long long total = end_ns - start_ns;
    unsigned long long avg = iterations > 0 ? total / iterations : 0;
    printf(
        "[libc-vfs-exec-smoke] metric op=%s iterations=%llu total_ns=%llu avg_ns=%llu\n",
        name,
        iterations,
        total,
        avg);
    fflush(stdout);
}

static void metric_total_avg(
    const char *name,
    unsigned long long total_ns,
    unsigned long long iterations)
{
    unsigned long long avg = iterations > 0 ? total_ns / iterations : 0;
    printf(
        "[libc-vfs-exec-smoke] metric op=%s iterations=%llu total_ns=%llu avg_ns=%llu\n",
        name,
        iterations,
        total_ns,
        avg);
    fflush(stdout);
}

static void metric_total_avg_from_cycles(
    const char *name,
    unsigned long long total_cycles,
    unsigned long long iterations)
{
    unsigned long long total_ns = cycles_to_ns(total_cycles);
    unsigned long long avg = iterations > 0 ? total_ns / iterations : 0;
    printf(
        "[libc-vfs-exec-smoke] metric op=%s iterations=%llu total_ns=%llu avg_ns=%llu total_cycles=%llu avg_cycles=%llu\n",
        name,
        iterations,
        total_ns,
        avg,
        total_cycles,
        iterations > 0 ? total_cycles / iterations : 0);
    fflush(stdout);
}

static int bench_done(unsigned long long start_ns, unsigned long long now, unsigned long long iterations)
{
    const unsigned long long min_total_ns = 10000000ull;
    const unsigned long long max_iterations = 1048576ull;
    return (iterations != 0 && now - start_ns >= min_total_ns) || iterations >= max_iterations;
}

static int fail(const char *what)
{
    fprintf(stderr, "[libc-vfs-exec-smoke] %s failed errno=%d\n", what, errno);
    return 1;
}

static int errno_is_unsupported(int value)
{
    return value == ENOSYS || value == ENOTSUP || value == EOPNOTSUPP;
}

#ifdef __pachaos__
#include "../syscall/native_raw.h"
#include "kobox2/filesystem.h"

enum { SMOKE_FILED_ENDPOINT = 240 };
static int raw_fail(const char *what,long status)
{
    fprintf(stderr,"[libc-vfs-exec-smoke] %s failed status=%ld\n",what,status);
    return 1;
}
static long raw_fs_call(kb2_fs_request_t *r,const char *path,const char *second,
    const void *input,size_t input_size,void *output,size_t capacity,
    kb2_fs_response_t *response)
{
    static uint64_t sequence;
    size_t first=path ? strlen(path)+1 : 0,other=second ? strlen(second)+1 : 0;
    if (first>SIZE_MAX-other || input_size>SIZE_MAX-first-other ||
        first+other+input_size>SIZE_MAX-KB2_FILESYSTEM_REQUEST_SIZE ||
        capacity>SIZE_MAX-KB2_FILESYSTEM_RESPONSE_SIZE ||
        (input_size && !input) || (capacity && !output)) return -EINVAL;
    size_t request_size=KB2_FILESYSTEM_REQUEST_SIZE+first+other+input_size;
    size_t response_size=KB2_FILESYSTEM_RESPONSE_SIZE+capacity;
    size_t size=request_size>response_size ? request_size : response_size;
    if (size>SIZE_MAX-4095) return -EOVERFLOW;
    size=(size+4095)&~(size_t)4095;
    r->generation=1;
    r->sequence=++sequence;
    if (!r->sequence) return -EOVERFLOW;
    uint64_t rights=PACHAOS_FD_RIGHT_INSPECT|PACHAOS_FD_RIGHT_CLOSE|
        PACHAOS_FD_RIGHT_MAP_READ|PACHAOS_FD_RIGHT_MAP_WRITE;
    long fd=__pachaos_raw3(PACHAOS_SYSCALL_VMO_CREATE,size,
        rights|PACHAOS_FD_RIGHT_TRANSFER,PACHAOS_FD_FLAG_CLOEXEC);
    if (fd<16) return -ENOMEM;
    unsigned char *bytes=(void *)__pachaos_raw6(PACHAOS_SYSCALL_MMAP,0,fd,size,
        PACHAOS_PROT_READ|PACHAOS_PROT_WRITE,PACHAOS_MMAP_SHARED,0);
    if ((uintptr_t)bytes<4096 || (intptr_t)bytes<0) {
        (void)__pachaos_raw1(PACHAOS_SYSCALL_FD_CLOSE,fd); return -ENOMEM;
    }
    r->path=r->second_path=r->data=(kb2_fs_span_t){0};
    size_t pos=KB2_FILESYSTEM_REQUEST_SIZE;
    if (first) { r->path=(kb2_fs_span_t){pos,first}; memcpy(bytes+pos,path,first); pos+=first; }
    if (other) { r->second_path=(kb2_fs_span_t){pos,other}; memcpy(bytes+pos,second,other); pos+=other; }
    if (input_size) { r->data=(kb2_fs_span_t){pos,input_size}; memcpy(bytes+pos,input,input_size); }
    long status=kb2_fs_request_encode(bytes,KB2_FILESYSTEM_REQUEST_SIZE,request_size,r);
    struct __pachaos_ipc_fd source={.fd=fd,.rights=rights,.transfer_flags=2};
    struct __pachaos_ipc_msg request={.word0=r->opcode,.word1=request_size,
        .word2=response_size,.word3=r->sequence,.fds=&source,.fd_count=1};
    struct __pachaos_ipc_fd received[2]={{0}};
    struct __pachaos_ipc_msg reply={.fds=received,.fd_capacity=2};
    if (!status) {
        status=__pachaos_ipc_message(PACHAOS_SYSCALL_IPC_CALL, SMOKE_FILED_ENDPOINT, (struct __pachaos_ipc_msg *)(uintptr_t)((long)&request), (struct __pachaos_ipc_msg *)(uintptr_t)((long)&reply), -1, PACHAOS_WAIT_UNINTERRUPTIBLE, 0);
        if (status>0) status=-EIO;
    }
    if (!status && (reply.word0!=r->opcode || reply.word3!=r->sequence ||
        reply.word2<KB2_FILESYSTEM_RESPONSE_SIZE || reply.word2>response_size ||
        kb2_fs_response_decode(bytes,reply.word2,response) ||
        !kb2_fs_response_matches(response,r) ||
        reply.word1!=(uint64_t)response->result || response->data_length>capacity ||
        reply.fd_count)) status=-EPROTO;
    for (size_t i=0;i<reply.fd_count && i<2;++i)
        (void)__pachaos_raw1(PACHAOS_SYSCALL_FD_CLOSE,(long)received[i].fd);
    if (!status && response->data_length)
        memcpy(output,bytes+KB2_FILESYSTEM_RESPONSE_SIZE,(size_t)response->data_length);
    (void)__pachaos_raw2(PACHAOS_SYSCALL_MUNMAP,(long)bytes,size);
    (void)__pachaos_raw1(PACHAOS_SYSCALL_FD_CLOSE,fd);
    return status ? status : response->result<0 ? (long)response->result : 0;
}
static long raw_open(const char *path,uint64_t flags,uint64_t mode,uint64_t *handle)
{
    kb2_fs_request_t r={.opcode=KB2_FILESYSTEM_OP_OPENAT2,.flags=flags,.mode=mode,.umask=0022};
    kb2_fs_response_t response;
    long status=raw_fs_call(&r,path,NULL,NULL,0,NULL,0,&response);
    if (!status) { if (!response.handle) return -EPROTO; *handle=response.handle; }
    return status;
}
static long raw_simple(uint32_t operation,uint64_t handle,int64_t offset,uint64_t flags)
{
    kb2_fs_request_t r={.opcode=operation,.handle=handle,.offset=offset,.flags=flags};
    kb2_fs_response_t response;
    long status=raw_fs_call(&r,NULL,NULL,NULL,0,NULL,0,&response);
    return status ? status : response.result;
}
static long raw_io(uint32_t operation,uint64_t handle,int64_t offset,
    void *bytes,size_t size)
{
    int write=operation==KB2_FILESYSTEM_OP_WRITE || operation==KB2_FILESYSTEM_OP_PWRITE;
    kb2_fs_request_t r={.opcode=operation,.handle=handle,.offset=offset,.length=size};
    kb2_fs_response_t response;
    long status=raw_fs_call(&r,NULL,NULL,write ? bytes : NULL,write ? size : 0,
        write ? NULL : bytes,write ? 0 : size,&response);
    if (status) return status;
    if (response.result<0 || (uint64_t)response.result>size ||
        (!write && response.data_length!=(uint64_t)response.result)) return -EPROTO;
    return response.result;
}
static long raw_path(uint32_t operation,const char *first,const char *second,uint64_t flags)
{
    kb2_fs_request_t r={.opcode=operation,.mode=0700,.umask=0022,.flags=flags};
    kb2_fs_response_t response;
    return raw_fs_call(&r,first,second,NULL,0,NULL,0,&response);
}
static int run_filed_raw_operation_bench(const char *path)
{
    uint64_t handle=0,duplicate=0;
    size_t created_directories=0;
    int created_file=0;
    long status=0;
    unsigned long long t0=now_ns(),t1=t0,iterations=0;
    do {
        status=raw_open(path,O_RDONLY,0,&handle);
        if (status) return raw_fail("raw open",status);
        status=raw_simple(KB2_FILESYSTEM_OP_CLOSE,handle,0,0); handle=0;
        if (status) return raw_fail("raw close",status);
        ++iterations; t1=now_ns();
    } while (!bench_done(t0,t1,iterations));
    metric_avg("filed_raw_open_close_roundtrip",t0,t1,iterations);

    /* Private nested fixture crosses both former name and path widths while
     * each component remains valid under the actual Linux NAME_MAX rule. */
    size_t dir_count=5,component_size=120,root_size=80,filename_size=180;
    char **directories=calloc(dir_count+1,sizeof(*directories));
    char *long_path=calloc(1,root_size+dir_count*(component_size+1)+filename_size+16);
    unsigned char *data=malloc(16384),*readback=malloc(16384);
    if (!directories || !long_path || !data || !readback) { status=-ENOMEM; goto done; }
    snprintf(long_path,root_size,"/tmp/libc-fs-raw-%llx",now_ns());
    for (size_t i=0;i<=dir_count;++i) {
        directories[i]=strdup(long_path);
        if (!directories[i]) { status=-ENOMEM; goto done; }
        status=raw_path(KB2_FILESYSTEM_OP_MKDIRAT,long_path,NULL,0);
        if (status) goto done;
        ++created_directories;
        if (i<dir_count) {
            size_t at=strlen(long_path); long_path[at++]='/';
            memset(long_path+at,(int)('a'+i),component_size); long_path[at+component_size]=0;
        }
    }
    size_t filename_offset=strlen(long_path);
    long_path[filename_offset++]='/';
    memset(long_path+filename_offset,'f',filename_size);
    long_path[filename_offset+filename_size]=0;
    status=raw_open(long_path,O_CREAT|O_EXCL|O_RDWR,0600,&handle);
    if (status) goto done;
    created_file=1;
    for (size_t i=0;i<16384;++i) data[i]=(unsigned char)(i*17u);
    if (raw_io(KB2_FILESYSTEM_OP_PWRITE,handle,0,data,16384)!=16384 ||
        raw_io(KB2_FILESYSTEM_OP_PREAD,handle,0,readback,16384)!=16384 ||
        memcmp(data,readback,16384)) { status=-EPROTO; goto done; }
    kb2_fs_request_t dup={.opcode=KB2_FILESYSTEM_OP_DUP,.handle=handle};
    kb2_fs_response_t response;
    status=raw_fs_call(&dup,NULL,NULL,NULL,0,NULL,0,&response);
    if (status) goto done;
    duplicate=response.handle;
    if (!duplicate || raw_io(KB2_FILESYSTEM_OP_READ,handle,0,readback,7)!=7 ||
        memcmp(readback,data,7) ||
        raw_io(KB2_FILESYSTEM_OP_READ,duplicate,0,readback,11)!=11 ||
        memcmp(readback,data+7,11) ||
        raw_simple(KB2_FILESYSTEM_OP_LSEEK,handle,0,SEEK_CUR)!=18) {
        status=-EPROTO; goto done;
    }
    data[0]=91;
    if (raw_io(KB2_FILESYSTEM_OP_PWRITE,duplicate,0,data,1)!=1 ||
        raw_io(KB2_FILESYSTEM_OP_PREAD,handle,0,readback,1)!=1 || readback[0]!=91 ||
        raw_simple(KB2_FILESYSTEM_OP_LSEEK,duplicate,0,SEEK_CUR)!=18) {
        status=-EPROTO; goto done;
    }
    unsigned char statx[KB2_FILESYSTEM_STATX_SIZE];
    kb2_fs_request_t stat={.opcode=KB2_FILESYSTEM_OP_STATX,.handle=handle,
        .flags=0x1000,.mask=0x7ff};
    status=raw_fs_call(&stat,"",NULL,NULL,0,statx,sizeof(statx),&response);
    if (status || response.data_length!=sizeof(statx) ||
        __pachaos_rd64(statx+KB2_FILESYSTEM_STATX_SIZE_OFFSET)!=16384) {
        if (!status) status=-EPROTO;
        goto done;
    }
    status=raw_simple(KB2_FILESYSTEM_OP_FTRUNCATE,handle,13,0);
    if (status) goto done;
    status=raw_fs_call(&stat,"",NULL,NULL,0,statx,sizeof(statx),&response);
    if (status || response.data_length!=sizeof(statx) ||
        __pachaos_rd64(statx+KB2_FILESYSTEM_STATX_SIZE_OFFSET)!=13) {
        if (!status) status=-EPROTO;
        goto done;
    }
    uint64_t directory=0;
    status=raw_open(directories[dir_count],O_RDONLY|O_DIRECTORY,0,&directory);
    if (status) goto done;
    long entries=raw_io(KB2_FILESYSTEM_OP_GETDENTS64,directory,0,readback,16384);
    long close_status=raw_simple(KB2_FILESYSTEM_OP_CLOSE,directory,0,0);
    int saw_file=0;
    if (entries<0) { status=entries; goto done; }
    if (close_status) { status=close_status; goto done; }
    for (size_t at=0;at<(size_t)entries;) {
        if ((size_t)entries-at<19) { status=-EPROTO; goto done; }
        unsigned reclen=__pachaos_rd16(readback+at+16);
        if (reclen<20 || reclen>(size_t)entries-at ||
            !memchr(readback+at+19,0,reclen-19)) { status=-EPROTO; goto done; }
        if (!strcmp((char *)readback+at+19,long_path+filename_offset)) saw_file=1;
        at+=reclen;
    }
    if (!saw_file) { status=-EPROTO; goto done; }
    char *renamed=malloc(strlen(long_path)+9);
    if (!renamed) { status=-ENOMEM; goto done; }
    sprintf(renamed,"%s.renamed",long_path);
    status=raw_path(KB2_FILESYSTEM_OP_RENAMEAT2,long_path,renamed,0);
    if (!status) strcpy(long_path,renamed);
    free(renamed);
    if (status) goto done;
    printf("[libc-vfs-exec-smoke] raw ABI: long_path=%zu io_bytes=16384 shared_offsets=ok fresh_stat=ok\n",strlen(long_path));
done:
    if (duplicate) { long s=raw_simple(KB2_FILESYSTEM_OP_CLOSE,duplicate,0,0); if (!status) status=s; }
    if (handle) { long s=raw_simple(KB2_FILESYSTEM_OP_CLOSE,handle,0,0); if (!status) status=s; }
    if (created_file) (void)raw_path(KB2_FILESYSTEM_OP_UNLINKAT,long_path,NULL,0);
    if (directories) for (size_t i=dir_count+1;i;--i) if (directories[i-1]) {
        if (i<=created_directories)
            (void)raw_path(KB2_FILESYSTEM_OP_UNLINKAT,directories[i-1],NULL,0x200);
        free(directories[i-1]);
    }
    free(directories); free(long_path); free(data); free(readback);
    return status ? raw_fail("raw ABI regression",status) : 0;
}
static int run_ipc_microbench(void)
{
    uint64_t pair[2]={0};
    uint64_t rights=PACHAOS_FD_RIGHT_INSPECT|PACHAOS_FD_RIGHT_CLOSE|
        PACHAOS_FD_RIGHT_SEND|PACHAOS_FD_RIGHT_RECV|PACHAOS_FD_RIGHT_WAIT|
        PACHAOS_FD_RIGHT_POLL;
    long status=__pachaos_raw3(PACHAOS_SYSCALL_IPC_CHANNEL_CREATE,(long)pair,rights,
        PACHAOS_FD_FLAG_CLOEXEC);
    if (status || pair[0]<16 || pair[1]<16) return raw_fail("IPC channel",status ? status : -EIO);
    unsigned long long t0=now_ns(),t1=t0,iterations=0;
    do {
        struct __pachaos_ipc_msg sent={.word0=UINT64_C(0x49504342454e4348),.word1=iterations};
        struct __pachaos_ipc_msg reply={0};
        status=__pachaos_ipc_message(PACHAOS_SYSCALL_IPC_SEND, (long)pair[0], (struct __pachaos_ipc_msg *)(uintptr_t)((long)&sent), 0, 0, 0, 0);
        if (!status) status=__pachaos_ipc_message(PACHAOS_SYSCALL_IPC_RECV, (long)pair[1], 0, (struct __pachaos_ipc_msg *)(uintptr_t)((long)&reply), 0, 0, 0);
        if (!status && (reply.word0!=sent.word0 || reply.word1!=sent.word1)) status=-EPROTO;
        ++iterations; t1=now_ns();
    } while (!status && !bench_done(t0,t1,iterations));
    (void)__pachaos_raw1(PACHAOS_SYSCALL_FD_CLOSE,(long)pair[0]);
    (void)__pachaos_raw1(PACHAOS_SYSCALL_FD_CLOSE,(long)pair[1]);
    if (status) return raw_fail("IPC pair",status);
    metric_avg("ipc_send_recv_pair",t0,t1,iterations);
    uint8_t config[KB2_FILESYSTEM_QUEUE_CONFIG_SIZE];
    t0=now_ns(); iterations=0;
    do {
        kb2_fs_request_t request={.opcode=KB2_FILESYSTEM_OP_HELLO,.flags=1};
        kb2_fs_response_t response;
        status=raw_fs_call(&request,NULL,NULL,NULL,0,config,sizeof(config),&response);
        if (!status && (response.data_length!=sizeof(config) ||
            __pachaos_rd64(config+KB2_FILESYSTEM_QUEUE_CONFIG_GENERATION_OFFSET)!=request.generation ||
            !__pachaos_rd64(config+KB2_FILESYSTEM_QUEUE_CONFIG_CHANNEL_ID_OFFSET) ||
            !__pachaos_rd64(config+KB2_FILESYSTEM_QUEUE_CONFIG_ARENA_BYTES_OFFSET) ||
            !__pachaos_rd32(config+KB2_FILESYSTEM_QUEUE_CONFIG_QUEUE_SIZE_OFFSET) ||
            __pachaos_rd32(config+KB2_FILESYSTEM_QUEUE_CONFIG_RESERVED_OFFSET))) status=-EPROTO;
        ++iterations; t1=now_ns();
    } while (!status && !bench_done(t0,t1,iterations));
    if (status) return raw_fail("raw HELLO",status);
    metric_avg("filed_raw_hello_roundtrip",t0,t1,iterations);
    return 0;
}
#else
static int run_ipc_microbench(void) { return 0; }
static int run_filed_raw_operation_bench(const char *path) { (void)path; return 0; }
#endif

static int run_child(int argc, char **argv, char **envp)
{
    unsigned long long child_start_ns = now_ns();
    printf("[libc-vfs-exec-smoke] child start\n");
    fflush(stdout);
    if (argc != 300 || strcmp(argv[1], "--child") != 0) {
        return fail("child argv");
    }
    for (int i=5;i<argc;++i)
        if (strcmp(argv[i],"argument-crossing-the-former-fixed-string-payload-budget"))
            return fail("child large argv contents");
    int inherited=atoi(argv[2]),shared=atoi(argv[3]),cloexec=atoi(argv[4]);
    char bytes[4]={0};
    struct stat info;
    if (read(inherited,bytes,3)!=3 || memcmp(bytes,"cde",3) ||
        read(shared,bytes,3)!=3 || memcmp(bytes,"fgh",3) ||
        lseek(inherited,0,SEEK_CUR)!=8 || fstat(inherited,&info) || info.st_size!=10 ||
        fstat(cloexec,&info)==0) return fail("child inherited handles / offsets / CLOEXEC");
    if (close(inherited) || close(shared)) return fail("child inherited close");
    int saw_child = 0;
    int saw_exec_start = 0;
    size_t environment_count=0;
    for (char **it = envp; it != NULL && *it != NULL; ++it) {
        ++environment_count;
        const char *prefix = "PACHA_LIBC_VFS_EXEC_START_NS=";
        const size_t prefix_len = strlen(prefix);
        if (strncmp(*it, prefix, prefix_len) == 0) {
            unsigned long long exec_start_ns = strtoull(*it + prefix_len, NULL, 10);
            if (exec_start_ns != 0 && child_start_ns >= exec_start_ns) {
                metric("execve_to_child_start", exec_start_ns, child_start_ns);
                saw_exec_start = 1;
            }
        }
        if (strcmp(*it, "PACHA_LIBC_VFS_EXEC_CHILD=1") == 0) {
            saw_child = 1;
        }
    }
    if (!saw_child || environment_count!=80) return fail("child large env");
    if (!saw_exec_start) {
        printf("[libc-vfs-exec-smoke] metric op=execve_to_child_start ns=missing\n");
        fflush(stdout);
    }
    printf("[libc-vfs-exec-smoke] child ok\n");
    fflush(stdout);
    return 0;
}

static void fill_stress_page(char *page, size_t page_size, unsigned long long file_index, unsigned long long page_index)
{
    for (size_t i = 0; i < page_size; i++) {
        page[i] = (char)('A' + ((file_index + page_index + i) % 26));
    }
}

static int run_fs_write_stress(void)
{
    enum {
        stress_files = 16,
        stress_pages_per_file = 8,
        stress_page_size = 4096,
    };
    char page[stress_page_size];
    char readback[stress_page_size];
    unsigned long long create_ns = 0;
    unsigned long long write_ns = 0;
    unsigned long long read_ns = 0;
    unsigned long long truncate_ns = 0;
    unsigned long long truncate_skipped = 0;
    unsigned long long close_ns = 0;
    unsigned long long rename_ns = 0;
    unsigned long long rename_skipped = 0;
    unsigned long long reopen_ns = 0;
    unsigned long long unlink_ns = 0;
    unsigned long long unlink_skipped = 0;
    unsigned long long checksum = 0;
    const unsigned long long tag = now_ns();
    unsigned long long t0 = now_ns();

    for (unsigned long long file_index = 0; file_index < stress_files; file_index++) {
        char path[128];
        char renamed[128];
        snprintf(path, sizeof(path), "/tmp/libc-vfs-write-stress-%llx-%llu.tmp", tag, file_index);
        snprintf(renamed, sizeof(renamed), "/tmp/libc-vfs-write-stress-%llx-%llu.renamed", tag, file_index);
        (void)unlink(path);
        (void)unlink(renamed);

        unsigned long long part0 = now_ns();
        int fd = open(path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
        unsigned long long part1 = now_ns();
        create_ns += part1 - part0;
        if (fd < 0) return fail("fs write stress create");

        for (unsigned long long page_index = 0; page_index < stress_pages_per_file; page_index++) {
            fill_stress_page(page, sizeof(page), file_index, page_index);
            part0 = now_ns();
            ssize_t wrote = write(fd, page, sizeof(page));
            part1 = now_ns();
            write_ns += part1 - part0;
            if (wrote != (ssize_t)sizeof(page)) {
                (void)close(fd);
                return fail("fs write stress write");
            }
        }

        if (lseek(fd, 0, SEEK_SET) != 0) {
            (void)close(fd);
            return fail("fs write stress seek readback");
        }
        memset(readback, 0, sizeof(readback));
        part0 = now_ns();
        ssize_t got = read(fd, readback, sizeof(readback));
        part1 = now_ns();
        read_ns += part1 - part0;
        if (got != (ssize_t)sizeof(readback)) {
            (void)close(fd);
            return fail("fs write stress readback");
        }
        fill_stress_page(page, sizeof(page), file_index, 0);
        if (memcmp(page, readback, sizeof(page)) != 0) {
            (void)close(fd);
            return fail("fs write stress content");
        }
        checksum += (unsigned char)readback[0];
        checksum += (unsigned char)readback[sizeof(readback) - 1];

        off_t expected_size = (off_t)(stress_page_size * stress_pages_per_file);
        part0 = now_ns();
        if (ftruncate(fd, (off_t)(stress_page_size * 4)) == 0) {
            part1 = now_ns();
            truncate_ns += part1 - part0;
            expected_size = (off_t)(stress_page_size * 4);
        } else if (errno == ENOSYS || errno == ENOTSUP || errno == EOPNOTSUPP) {
            part1 = now_ns();
            truncate_ns += part1 - part0;
            truncate_skipped++;
        } else {
            (void)close(fd);
            return fail("fs write stress truncate");
        }

        part0 = now_ns();
        if (close(fd) != 0) return fail("fs write stress close");
        part1 = now_ns();
        close_ns += part1 - part0;

        const char *final_path = renamed;
        part0 = now_ns();
        if (rename(path, renamed) == 0) {
            part1 = now_ns();
            rename_ns += part1 - part0;
        } else if (errno == ENOSYS || errno == ENOTSUP || errno == EOPNOTSUPP) {
            part1 = now_ns();
            rename_ns += part1 - part0;
            rename_skipped++;
            final_path = path;
        } else {
            return fail("fs write stress rename");
        }

        part0 = now_ns();
        fd = open(final_path, O_RDONLY | O_CLOEXEC);
        part1 = now_ns();
        reopen_ns += part1 - part0;
        if (fd < 0) return fail("fs write stress reopen");
        struct stat st;
        memset(&st, 0, sizeof(st));
        if (fstat(fd, &st) != 0 || st.st_size != expected_size) {
            (void)close(fd);
            return fail("fs write stress stat");
        }
        if (close(fd) != 0) return fail("fs write stress close reopen");

        part0 = now_ns();
        if (unlink(final_path) == 0) {
            part1 = now_ns();
            unlink_ns += part1 - part0;
        } else if (errno == ENOSYS || errno == ENOTSUP || errno == EOPNOTSUPP) {
            part1 = now_ns();
            unlink_ns += part1 - part0;
            unlink_skipped++;
        } else {
            return fail("fs write stress unlink");
        }
    }

    unsigned long long t1 = now_ns();
    metric_avg("fs_write_stress", t0, t1, stress_files);
    metric_total_avg("fs_write_stress.create", create_ns, stress_files);
    metric_total_avg("fs_write_stress.write_page", write_ns, stress_files * stress_pages_per_file);
    metric_total_avg("fs_write_stress.readback", read_ns, stress_files);
    metric_total_avg("fs_write_stress.truncate", truncate_ns, stress_files);
    printf(
        "[libc-vfs-exec-smoke] metric op=fs_write_stress.truncate_skipped iterations=%d total=%llu avg=%llu\n",
        stress_files,
        truncate_skipped,
        truncate_skipped == 0 ? 0ull : 1ull);
    metric_total_avg("fs_write_stress.close", close_ns, stress_files);
    metric_total_avg("fs_write_stress.rename", rename_ns, stress_files);
    printf(
        "[libc-vfs-exec-smoke] metric op=fs_write_stress.rename_skipped iterations=%d total=%llu avg=%llu\n",
        stress_files,
        rename_skipped,
        rename_skipped == 0 ? 0ull : 1ull);
    metric_total_avg("fs_write_stress.reopen", reopen_ns, stress_files);
    metric_total_avg("fs_write_stress.unlink", unlink_ns, stress_files);
    printf(
        "[libc-vfs-exec-smoke] metric op=fs_write_stress.unlink_skipped iterations=%d total=%llu avg=%llu\n",
        stress_files,
        unlink_skipped,
        unlink_skipped == 0 ? 0ull : 1ull);
    printf("[libc-vfs-exec-smoke] metric op=fs_write_stress_checksum total=%llu\n", checksum);
    fflush(stdout);
    return 0;
}

static int run_vfs(void)
{
    const char *path = "/tmp/libc-vfs-exec-smoke.txt";
    const char *first = "alpha\n";
    const char *second = "beta\n";
    char buf[64];
    struct stat st;

    printf("[libc-vfs-exec-smoke] vfs start\n");
    fflush(stdout);
    unsigned long long t0 = now_ns();
    unsigned long long t1 = now_ns();
    metric("clock_pair", t0, t1);

    const char *ipc_bench = getenv("PACHA_LIBC_VFS_IPC_BENCH");
    if (ipc_bench != NULL && strcmp(ipc_bench, "1") == 0) {
        if (run_ipc_microbench() != 0) return 1;
    }

    t0 = now_ns();
    FILE *f = fopen(path, "w+");
    t1 = now_ns();
    metric("fopen_wplus", t0, t1);
    if (f == NULL) return fail("fopen w+");

    t0 = now_ns();
    if (fwrite(first, 1, strlen(first), f) != strlen(first)) return fail("fwrite first");
    t1 = now_ns();
    metric("fwrite_first", t0, t1);

    t0 = now_ns();
    if (fflush(f) != 0) return fail("fflush first");
    t1 = now_ns();
    metric("fflush_first", t0, t1);

    t0 = now_ns();
    if (fseek(f, 0, SEEK_SET) != 0) return fail("fseek set");
    t1 = now_ns();
    metric("fseek_set", t0, t1);

    memset(buf, 0, sizeof(buf));
    t0 = now_ns();
    if (fread(buf, 1, strlen(first), f) != strlen(first)) return fail("fread first");
    t1 = now_ns();
    metric("fread_first", t0, t1);
    if (strcmp(buf, first) != 0) return fail("readback first");

    t0 = now_ns();
    if (fclose(f) != 0) return fail("fclose first");
    t1 = now_ns();
    metric("fclose_first", t0, t1);

    t0 = now_ns();
    f = fopen(path, "a");
    t1 = now_ns();
    metric("fopen_append", t0, t1);
    if (f == NULL) return fail("fopen append");

    t0 = now_ns();
    if (fwrite(second, 1, strlen(second), f) != strlen(second)) return fail("fwrite append");
    t1 = now_ns();
    metric("fwrite_append", t0, t1);

    t0 = now_ns();
    if (fclose(f) != 0) return fail("fclose append");
    t1 = now_ns();
    metric("fclose_append", t0, t1);

    t0 = now_ns();
    int fd = open(path, O_RDWR | O_CLOEXEC);
    t1 = now_ns();
    metric("open_rw_cloexec", t0, t1);
    if (fd < 0) return fail("open rw");

    if (ipc_bench != NULL && strcmp(ipc_bench, "1") == 0) {
        if (run_filed_raw_operation_bench(path) != 0) {
            (void)close(fd);
            return 1;
        }
    }

    t0 = now_ns();
    int fd_flags = fcntl(fd, F_GETFD);
    t1 = now_ns();
    metric("fcntl_getfd", t0, t1);
    if (fd_flags < 0 || (fd_flags & FD_CLOEXEC) == 0) {
        (void)close(fd);
        return fail("fcntl getfd");
    }

    t0 = now_ns();
    if (fcntl(fd, F_SETFD, 0) != 0) {
        (void)close(fd);
        return fail("fcntl setfd");
    }
    t1 = now_ns();
    metric("fcntl_setfd", t0, t1);

    t0 = now_ns();
    fd_flags = fcntl(fd, F_GETFD);
    t1 = now_ns();
    metric("fcntl_getfd_after_set", t0, t1);
    if (fd_flags < 0 || (fd_flags & FD_CLOEXEC) != 0) {
        (void)close(fd);
        return fail("fcntl setfd");
    }

    t0 = now_ns();
    int dup_fd = fcntl(fd, F_DUPFD_CLOEXEC, fd + 3);
    t1 = now_ns();
    metric("fcntl_dupfd_cloexec", t0, t1);
    if (dup_fd < fd + 3) {
        (void)close(fd);
        return fail("fcntl dupfd");
    }

    t0 = now_ns();
    fd_flags = fcntl(dup_fd, F_GETFD);
    t1 = now_ns();
    metric("fcntl_dup_getfd", t0, t1);
    if (fd_flags < 0 || (fd_flags & FD_CLOEXEC) == 0) {
        (void)close(dup_fd);
        (void)close(fd);
        return fail("fcntl dupfd cloexec");
    }

    t0 = now_ns();
    if (close(dup_fd) != 0) {
        (void)close(fd);
        return fail("close dupfd");
    }
    t1 = now_ns();
    metric("close_dupfd", t0, t1);

    t0 = now_ns();
    if (lseek(fd, 0, SEEK_END) != (off_t)(strlen(first) + strlen(second))) {
        (void)close(fd);
        return fail("lseek end");
    }
    t1 = now_ns();
    metric("lseek_end", t0, t1);

    const char *rewrite = "BETA\n";
    t0 = now_ns();
    if (pwrite(fd, rewrite, strlen(rewrite), (off_t)strlen(first)) != (ssize_t)strlen(rewrite)) {
        (void)close(fd);
        return fail("pwrite rewrite");
    }
    t1 = now_ns();
    metric("pwrite_rewrite", t0, t1);

    memset(buf, 0, sizeof(buf));
    t0 = now_ns();
    if (pread(fd, buf, strlen(rewrite), (off_t)strlen(first)) != (ssize_t)strlen(rewrite) ||
        strcmp(buf, rewrite) != 0)
    {
        (void)close(fd);
        return fail("pread rewrite");
    }
    t1 = now_ns();
    metric("pread_rewrite", t0, t1);

    t0 = now_ns();
    if (lseek(fd, 0, SEEK_END) < 0) {
        (void)close(fd);
        return fail("lseek append");
    }
    t1 = now_ns();
    metric("lseek_append", t0, t1);

    const char *vec0 = "vec";
    const char *vec1 = "tor\n";
    struct iovec iov[2];
    iov[0].iov_base = (void *)vec0;
    iov[0].iov_len = strlen(vec0);
    iov[1].iov_base = (void *)vec1;
    iov[1].iov_len = strlen(vec1);
    t0 = now_ns();
    if (writev(fd, iov, 2) != (ssize_t)(strlen(vec0) + strlen(vec1))) {
        (void)close(fd);
        return fail("writev");
    }
    t1 = now_ns();
    metric("writev_append", t0, t1);

    t0 = now_ns();
    if (lseek(fd, 0, SEEK_SET) != 0) {
        (void)close(fd);
        return fail("lseek set");
    }
    t1 = now_ns();
    metric("lseek_set", t0, t1);

    memset(&st, 0, sizeof(st));
    t0 = now_ns();
    if (fstat(fd, &st) != 0 ||
        st.st_size < (off_t)(strlen(first) + strlen(rewrite) + strlen(vec0) + strlen(vec1)))
    {
        (void)close(fd);
        return fail("fstat readback");
    }
    t1 = now_ns();
    metric("fstat_readback", t0, t1);

    memset(buf, 0, sizeof(buf));
    char left[8];
    char right[32];
    memset(left, 0, sizeof(left));
    memset(right, 0, sizeof(right));
    iov[0].iov_base = left;
    iov[0].iov_len = strlen(first);
    iov[1].iov_base = right;
    iov[1].iov_len = sizeof(right) - 1;
    t0 = now_ns();
    ssize_t got = readv(fd, iov, 2);
    t1 = now_ns();
    metric("readv_full", t0, t1);
    if (got < 0) {
        (void)close(fd);
        return fail("read readback");
    }
    snprintf(buf, sizeof(buf), "%s%s", left, right);
    if (strstr(buf, first) == NULL || strstr(buf, rewrite) == NULL || strstr(buf, "vector\n") == NULL) {
        (void)close(fd);
        return fail("content readback");
    }

    t0 = now_ns();
    if (close(fd) != 0) return fail("close readback");
    t1 = now_ns();
    metric("close_readback", t0, t1);

    t0 = now_ns();
    DIR *dir = opendir("/");
    t1 = now_ns();
    metric("opendir_root", t0, t1);
    if (dir == NULL) return fail("opendir /");

    int saw_tmp = 0;
    int entries = 0;
    t0 = now_ns();
    for (;;) {
        struct dirent *de = readdir(dir);
        if (de == NULL) break;
        entries++;
        if (strcmp(de->d_name, "tmp") == 0) saw_tmp = 1;
    }
    t1 = now_ns();
    metric("readdir_root_all", t0, t1);
    printf("[libc-vfs-exec-smoke] metric op=readdir_root_entries count=%d\n", entries);
    fflush(stdout);

    t0 = now_ns();
    if (closedir(dir) != 0) return fail("closedir /");
    t1 = now_ns();
    metric("closedir_root", t0, t1);
    if (!saw_tmp) return fail("readdir tmp");

    unsigned long long open_close_iters = 0;
    unsigned long long open_close_open_ns = 0;
    unsigned long long open_close_close_ns = 0;
    unsigned long long open_close_open_cycles = 0;
    unsigned long long open_close_close_cycles = 0;
    int warm_fd = open(path, O_RDONLY | O_CLOEXEC);
    if (warm_fd < 0) return fail("batch open warmup");
    if (close(warm_fd) != 0) return fail("batch close warmup");
    t0 = now_ns();
    for (;;) {
        unsigned long long part0 = now_ns();
        unsigned long long part0_cycles = read_tsc();
        int loop_fd = open(path, O_RDONLY | O_CLOEXEC);
        unsigned long long part1_cycles = read_tsc();
        unsigned long long part1 = now_ns();
        if (loop_fd < 0) return fail("batch open");
        unsigned long long part2 = now_ns();
        unsigned long long part2_cycles = read_tsc();
        if (close(loop_fd) != 0) return fail("batch close");
        unsigned long long part3_cycles = read_tsc();
        unsigned long long part3 = now_ns();
        open_close_open_ns += part1 - part0;
        open_close_close_ns += part3 - part2;
        if (part1_cycles >= part0_cycles) open_close_open_cycles += part1_cycles - part0_cycles;
        if (part3_cycles >= part2_cycles) open_close_close_cycles += part3_cycles - part2_cycles;
        open_close_iters++;
        t1 = now_ns();
        if (bench_done(t0, t1, open_close_iters)) break;
    }
    metric_avg("batch_open_close", t0, t1, open_close_iters);
    if (tsc_calibration_cycles != 0) {
        metric_total_avg_from_cycles("batch_open_close.open", open_close_open_cycles, open_close_iters);
        metric_total_avg_from_cycles("batch_open_close.close", open_close_close_cycles, open_close_iters);
    } else {
        metric_total_avg("batch_open_close.open", open_close_open_ns, open_close_iters);
        metric_total_avg("batch_open_close.close", open_close_close_ns, open_close_iters);
    }

    int root_dir_fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (root_dir_fd < 0) return fail("batch openat dirfd root");
    unsigned long long openat_dir_iters = 0;
    unsigned long long openat_dir_open_ns = 0;
    unsigned long long openat_dir_close_ns = 0;
    unsigned long long openat_dir_open_cycles = 0;
    unsigned long long openat_dir_close_cycles = 0;
    t0 = now_ns();
    for (;;) {
        unsigned long long part0 = now_ns();
        unsigned long long part0_cycles = read_tsc();
        int tmp_fd = openat(root_dir_fd, "tmp", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        unsigned long long part1_cycles = read_tsc();
        unsigned long long part1 = now_ns();
        if (tmp_fd < 0) {
            (void)close(root_dir_fd);
            return fail("batch openat dirfd tmp");
        }
        unsigned long long part2 = now_ns();
        unsigned long long part2_cycles = read_tsc();
        if (close(tmp_fd) != 0) {
            (void)close(root_dir_fd);
            return fail("batch openat dirfd close");
        }
        unsigned long long part3_cycles = read_tsc();
        unsigned long long part3 = now_ns();
        openat_dir_open_ns += part1 - part0;
        openat_dir_close_ns += part3 - part2;
        if (part1_cycles >= part0_cycles) openat_dir_open_cycles += part1_cycles - part0_cycles;
        if (part3_cycles >= part2_cycles) openat_dir_close_cycles += part3_cycles - part2_cycles;
        openat_dir_iters++;
        t1 = now_ns();
        if (bench_done(t0, t1, openat_dir_iters)) break;
    }
    metric_avg("batch_openat_dirfd_close", t0, t1, openat_dir_iters);
    if (tsc_calibration_cycles != 0) {
        metric_total_avg_from_cycles("batch_openat_dirfd_close.openat", openat_dir_open_cycles, openat_dir_iters);
        metric_total_avg_from_cycles("batch_openat_dirfd_close.close", openat_dir_close_cycles, openat_dir_iters);
    } else {
        metric_total_avg("batch_openat_dirfd_close.openat", openat_dir_open_ns, openat_dir_iters);
        metric_total_avg("batch_openat_dirfd_close.close", openat_dir_close_ns, openat_dir_iters);
    }

    unsigned long long fstatat_dir_iters = 0;
    t0 = now_ns();
    for (;;) {
        if (fstatat(root_dir_fd, "tmp", &st, 0) != 0) {
            (void)close(root_dir_fd);
            return fail("batch fstatat dirfd");
        }
        fstatat_dir_iters++;
        t1 = now_ns();
        if (bench_done(t0, t1, fstatat_dir_iters)) break;
    }
    metric_avg("batch_fstatat_dirfd", t0, t1, fstatat_dir_iters);
    if (close(root_dir_fd) != 0) return fail("batch openat root close");

    int tmp_dir_fd = open("/tmp", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (tmp_dir_fd < 0) return fail("batch openat file tmp dir");
    unsigned long long openat_file_iters = 0;
    unsigned long long openat_file_open_ns = 0;
    unsigned long long openat_file_close_ns = 0;
    unsigned long long openat_file_open_cycles = 0;
    unsigned long long openat_file_close_cycles = 0;
    t0 = now_ns();
    for (;;) {
        unsigned long long part0 = now_ns();
        unsigned long long part0_cycles = read_tsc();
        int tmp_file_fd = openat(tmp_dir_fd, "libc-vfs-exec-smoke.txt", O_RDONLY | O_CLOEXEC);
        unsigned long long part1_cycles = read_tsc();
        unsigned long long part1 = now_ns();
        if (tmp_file_fd < 0) {
            (void)close(tmp_dir_fd);
            return fail("batch openat file");
        }
        unsigned long long part2 = now_ns();
        unsigned long long part2_cycles = read_tsc();
        if (close(tmp_file_fd) != 0) {
            (void)close(tmp_dir_fd);
            return fail("batch openat file close");
        }
        unsigned long long part3_cycles = read_tsc();
        unsigned long long part3 = now_ns();
        openat_file_open_ns += part1 - part0;
        openat_file_close_ns += part3 - part2;
        if (part1_cycles >= part0_cycles) openat_file_open_cycles += part1_cycles - part0_cycles;
        if (part3_cycles >= part2_cycles) openat_file_close_cycles += part3_cycles - part2_cycles;
        openat_file_iters++;
        t1 = now_ns();
        if (bench_done(t0, t1, openat_file_iters)) break;
    }
    metric_avg("batch_openat_file_close", t0, t1, openat_file_iters);
    if (tsc_calibration_cycles != 0) {
        metric_total_avg_from_cycles("batch_openat_file_close.openat", openat_file_open_cycles, openat_file_iters);
        metric_total_avg_from_cycles("batch_openat_file_close.close", openat_file_close_cycles, openat_file_iters);
    } else {
        metric_total_avg("batch_openat_file_close.openat", openat_file_open_ns, openat_file_iters);
        metric_total_avg("batch_openat_file_close.close", openat_file_close_ns, openat_file_iters);
    }
    if (close(tmp_dir_fd) != 0) return fail("batch openat file dir close");

    unsigned long long stat_path_iters = 0;
    t0 = now_ns();
    for (;;) {
        if (fstatat(AT_FDCWD, path, &st, 0) != 0) return fail("batch stat path");
        stat_path_iters++;
        t1 = now_ns();
        if (bench_done(t0, t1, stat_path_iters)) break;
    }
    metric_avg("batch_stat_path", t0, t1, stat_path_iters);

    unsigned long long stdio_iters = 0;
    unsigned long long stdio_fopen_ns = 0;
    unsigned long long stdio_fread_ns = 0;
    unsigned long long stdio_fclose_ns = 0;
    unsigned long long stdio_fopen_cycles = 0;
    unsigned long long stdio_fread_cycles = 0;
    unsigned long long stdio_fclose_cycles = 0;
    t0 = now_ns();
    for (;;) {
        unsigned long long part0 = now_ns();
        unsigned long long part0_cycles = read_tsc();
        FILE *loop_file = fopen(path, "r");
        unsigned long long part1_cycles = read_tsc();
        unsigned long long part1 = now_ns();
        if (loop_file == NULL) return fail("batch stdio fopen");
        memset(buf, 0, sizeof(buf));
        unsigned long long part2 = now_ns();
        unsigned long long part2_cycles = read_tsc();
        size_t read_bytes = fread(buf, 1, strlen(first), loop_file);
        unsigned long long part3_cycles = read_tsc();
        unsigned long long part3 = now_ns();
        if (read_bytes != strlen(first)) {
            (void)fclose(loop_file);
            return fail("batch stdio fread");
        }
        unsigned long long part4 = now_ns();
        unsigned long long part4_cycles = read_tsc();
        if (fclose(loop_file) != 0) return fail("batch stdio fclose");
        unsigned long long part5_cycles = read_tsc();
        unsigned long long part5 = now_ns();
        stdio_fopen_ns += part1 - part0;
        stdio_fread_ns += part3 - part2;
        stdio_fclose_ns += part5 - part4;
        if (part1_cycles >= part0_cycles) stdio_fopen_cycles += part1_cycles - part0_cycles;
        if (part3_cycles >= part2_cycles) stdio_fread_cycles += part3_cycles - part2_cycles;
        if (part5_cycles >= part4_cycles) stdio_fclose_cycles += part5_cycles - part4_cycles;
        stdio_iters++;
        t1 = now_ns();
        if (bench_done(t0, t1, stdio_iters)) break;
    }
    metric_avg("batch_stdio_fopen_fread_fclose", t0, t1, stdio_iters);
    if (tsc_calibration_cycles != 0) {
        metric_total_avg_from_cycles("batch_stdio_fopen_fread_fclose.fopen", stdio_fopen_cycles, stdio_iters);
        metric_total_avg_from_cycles("batch_stdio_fopen_fread_fclose.fread", stdio_fread_cycles, stdio_iters);
        metric_total_avg_from_cycles("batch_stdio_fopen_fread_fclose.fclose", stdio_fclose_cycles, stdio_iters);
    } else {
        metric_total_avg("batch_stdio_fopen_fread_fclose.fopen", stdio_fopen_ns, stdio_iters);
        metric_total_avg("batch_stdio_fopen_fread_fclose.fread", stdio_fread_ns, stdio_iters);
        metric_total_avg("batch_stdio_fopen_fread_fclose.fclose", stdio_fclose_ns, stdio_iters);
    }

    fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) return fail("batch open rw");

    unsigned long long dup_close_iters = 0;
    unsigned long long dup_close_dup_ns = 0;
    unsigned long long dup_close_close_ns = 0;
    unsigned long long dup_close_dup_cycles = 0;
    unsigned long long dup_close_close_cycles = 0;
    t0 = now_ns();
    for (;;) {
        unsigned long long part0 = now_ns();
        unsigned long long part0_cycles = read_tsc();
        int loop_dup_fd = fcntl(fd, F_DUPFD_CLOEXEC, fd + 8);
        unsigned long long part1_cycles = read_tsc();
        unsigned long long part1 = now_ns();
        if (loop_dup_fd < fd + 8) {
            (void)close(fd);
            return fail("batch dup close dupfd");
        }
        unsigned long long part2 = now_ns();
        unsigned long long part2_cycles = read_tsc();
        if (close(loop_dup_fd) != 0) {
            (void)close(fd);
            return fail("batch dup close close");
        }
        unsigned long long part3_cycles = read_tsc();
        unsigned long long part3 = now_ns();
        dup_close_dup_ns += part1 - part0;
        dup_close_close_ns += part3 - part2;
        if (part1_cycles >= part0_cycles) dup_close_dup_cycles += part1_cycles - part0_cycles;
        if (part3_cycles >= part2_cycles) dup_close_close_cycles += part3_cycles - part2_cycles;
        dup_close_iters++;
        t1 = now_ns();
        if (bench_done(t0, t1, dup_close_iters)) break;
    }
    metric_avg("batch_fcntl_dupfd_close", t0, t1, dup_close_iters);
    if (tsc_calibration_cycles != 0) {
        metric_total_avg_from_cycles("batch_fcntl_dupfd_close.dupfd", dup_close_dup_cycles, dup_close_iters);
        metric_total_avg_from_cycles("batch_fcntl_dupfd_close.close", dup_close_close_cycles, dup_close_iters);
    } else {
        metric_total_avg("batch_fcntl_dupfd_close.dupfd", dup_close_dup_ns, dup_close_iters);
        metric_total_avg("batch_fcntl_dupfd_close.close", dup_close_close_ns, dup_close_iters);
    }

    unsigned long long fcntl_iters = 0;
    t0 = now_ns();
    for (;;) {
        if (fcntl(fd, F_GETFD) < 0) {
            (void)close(fd);
            return fail("batch fcntl getfd");
        }
        fcntl_iters++;
        t1 = now_ns();
        if (bench_done(t0, t1, fcntl_iters)) break;
    }
    metric_avg("batch_fcntl_getfd", t0, t1, fcntl_iters);

    unsigned long long lseek_iters = 0;
    t0 = now_ns();
    for (;;) {
        if (lseek(fd, 0, SEEK_SET) != 0) {
            (void)close(fd);
            return fail("batch lseek");
        }
        lseek_iters++;
        t1 = now_ns();
        if (bench_done(t0, t1, lseek_iters)) break;
    }
    metric_avg("batch_lseek_set", t0, t1, lseek_iters);

    unsigned long long lseek_read_iters = 0;
    unsigned long long lseek_read_lseek_ns = 0;
    unsigned long long lseek_read_read_ns = 0;
    unsigned long long lseek_read_lseek_cycles = 0;
    unsigned long long lseek_read_read_cycles = 0;
    t0 = now_ns();
    for (;;) {
        memset(buf, 0, sizeof(buf));
        unsigned long long part0 = now_ns();
        unsigned long long part0_cycles = read_tsc();
        off_t seek_status = lseek(fd, 0, SEEK_SET);
        unsigned long long part1_cycles = read_tsc();
        unsigned long long part1 = now_ns();
        ssize_t got = -1;
        unsigned long long part2 = part1;
        unsigned long long part3 = part1;
        unsigned long long part2_cycles = part1_cycles;
        unsigned long long part3_cycles = part1_cycles;
        if (seek_status == 0) {
            part2 = now_ns();
            part2_cycles = read_tsc();
            got = read(fd, buf, strlen(first));
            part3_cycles = read_tsc();
            part3 = now_ns();
        }
        if (seek_status != 0 || got != (ssize_t)strlen(first)) {
            (void)close(fd);
            return fail("batch lseek read");
        }
        lseek_read_lseek_ns += part1 - part0;
        lseek_read_read_ns += part3 - part2;
        if (part1_cycles >= part0_cycles) lseek_read_lseek_cycles += part1_cycles - part0_cycles;
        if (part3_cycles >= part2_cycles) lseek_read_read_cycles += part3_cycles - part2_cycles;
        lseek_read_iters++;
        t1 = now_ns();
        if (bench_done(t0, t1, lseek_read_iters)) break;
    }
    metric_avg("batch_lseek_read", t0, t1, lseek_read_iters);
    if (tsc_calibration_cycles != 0) {
        metric_total_avg_from_cycles("batch_lseek_read.lseek", lseek_read_lseek_cycles, lseek_read_iters);
        metric_total_avg_from_cycles("batch_lseek_read.read", lseek_read_read_cycles, lseek_read_iters);
    } else {
        metric_total_avg("batch_lseek_read.lseek", lseek_read_lseek_ns, lseek_read_iters);
        metric_total_avg("batch_lseek_read.read", lseek_read_read_ns, lseek_read_iters);
    }

    unsigned long long lseek_write_iters = 0;
    unsigned long long lseek_write_lseek_ns = 0;
    unsigned long long lseek_write_write_ns = 0;
    unsigned long long lseek_write_lseek_cycles = 0;
    unsigned long long lseek_write_write_cycles = 0;
    t0 = now_ns();
    for (;;) {
        unsigned long long part0 = now_ns();
        unsigned long long part0_cycles = read_tsc();
        off_t seek_status = lseek(fd, (off_t)strlen(first), SEEK_SET);
        unsigned long long part1_cycles = read_tsc();
        unsigned long long part1 = now_ns();
        ssize_t got = -1;
        unsigned long long part2 = part1;
        unsigned long long part3 = part1;
        unsigned long long part2_cycles = part1_cycles;
        unsigned long long part3_cycles = part1_cycles;
        if (seek_status == (off_t)strlen(first)) {
            part2 = now_ns();
            part2_cycles = read_tsc();
            got = write(fd, rewrite, strlen(rewrite));
            part3_cycles = read_tsc();
            part3 = now_ns();
        }
        if (seek_status != (off_t)strlen(first) || got != (ssize_t)strlen(rewrite)) {
            (void)close(fd);
            return fail("batch lseek write");
        }
        lseek_write_lseek_ns += part1 - part0;
        lseek_write_write_ns += part3 - part2;
        if (part1_cycles >= part0_cycles) lseek_write_lseek_cycles += part1_cycles - part0_cycles;
        if (part3_cycles >= part2_cycles) lseek_write_write_cycles += part3_cycles - part2_cycles;
        lseek_write_iters++;
        t1 = now_ns();
        if (bench_done(t0, t1, lseek_write_iters)) break;
    }
    metric_avg("batch_lseek_write", t0, t1, lseek_write_iters);
    if (tsc_calibration_cycles != 0) {
        metric_total_avg_from_cycles("batch_lseek_write.lseek", lseek_write_lseek_cycles, lseek_write_iters);
        metric_total_avg_from_cycles("batch_lseek_write.write", lseek_write_write_cycles, lseek_write_iters);
    } else {
        metric_total_avg("batch_lseek_write.lseek", lseek_write_lseek_ns, lseek_write_iters);
        metric_total_avg("batch_lseek_write.write", lseek_write_write_ns, lseek_write_iters);
    }

    unsigned long long fstat_iters = 0;
    t0 = now_ns();
    for (;;) {
        if (fstat(fd, &st) != 0) {
            (void)close(fd);
            return fail("batch fstat");
        }
        fstat_iters++;
        t1 = now_ns();
        if (bench_done(t0, t1, fstat_iters)) break;
    }
    metric_avg("batch_fstat", t0, t1, fstat_iters);

    unsigned long long pread_iters = 0;
    t0 = now_ns();
    for (;;) {
        errno = 0;
        ssize_t got = pread(fd, buf, strlen(first), 0);
        if (got != (ssize_t)strlen(first)) {
            fprintf(
                stderr,
                "[libc-vfs-exec-smoke] batch pread got=%lld want=%llu iter=%llu errno=%d\n",
                (long long)got,
                (unsigned long long)strlen(first),
                pread_iters,
                errno);
            (void)close(fd);
            return fail("batch pread");
        }
        pread_iters++;
        t1 = now_ns();
        if (bench_done(t0, t1, pread_iters)) break;
    }
    metric_avg("batch_pread", t0, t1, pread_iters);

    unsigned long long pwrite_iters = 0;
    t0 = now_ns();
    for (;;) {
        errno = 0;
        ssize_t got = pwrite(fd, rewrite, strlen(rewrite), (off_t)strlen(first));
        if (got != (ssize_t)strlen(rewrite)) {
            fprintf(
                stderr,
                "[libc-vfs-exec-smoke] batch pwrite got=%lld want=%llu iter=%llu errno=%d\n",
                (long long)got,
                (unsigned long long)strlen(rewrite),
                pwrite_iters,
                errno);
            (void)close(fd);
            return fail("batch pwrite");
        }
        pwrite_iters++;
        t1 = now_ns();
        if (bench_done(t0, t1, pwrite_iters)) break;
    }
    metric_avg("batch_pwrite", t0, t1, pwrite_iters);

    {
        char mutation_probe_path[128];
        snprintf(mutation_probe_path, sizeof(mutation_probe_path), "/tmp/libc-vfs-mutation-probe-%llu.tmp", now_ns());
        int mutation_probe_fd = open(mutation_probe_path, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
        int mutation_supported = 0;
        if (mutation_probe_fd >= 0) {
            if (close(mutation_probe_fd) != 0) {
                (void)close(fd);
                return fail("batch create unlink probe close");
            }
            if (unlink(mutation_probe_path) == 0) {
                mutation_supported = 1;
            } else if (!errno_is_unsupported(errno)) {
                (void)close(fd);
                return fail("batch create unlink probe unlink");
            }
        } else if (!errno_is_unsupported(errno)) {
            (void)close(fd);
            return fail("batch create unlink probe create");
        }

        if (!mutation_supported) {
            printf("[libc-vfs-exec-smoke] metric op=batch_create_close_unlink_skipped iterations=1 total=1 avg=1\n");
            fflush(stdout);
        } else {
        unsigned long long create_unlink_iters = 0;
        unsigned long long create_unlink_create_ns = 0;
        unsigned long long create_unlink_close_ns = 0;
        unsigned long long create_unlink_unlink_ns = 0;
        unsigned long long create_unlink_create_cycles = 0;
        unsigned long long create_unlink_close_cycles = 0;
        unsigned long long create_unlink_unlink_cycles = 0;
        const unsigned long long create_unlink_tag = now_ns();
        t0 = now_ns();
        for (;;) {
            char mutation_path[128];
            snprintf(
                mutation_path,
                sizeof(mutation_path),
                "/tmp/libc-vfs-mutation-%llx-%llu.tmp",
                create_unlink_tag,
                create_unlink_iters);
            unsigned long long part0 = now_ns();
            unsigned long long part0_cycles = read_tsc();
            int mutation_fd = open(mutation_path, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
            unsigned long long part1_cycles = read_tsc();
            unsigned long long part1 = now_ns();
            if (mutation_fd < 0) {
                (void)close(fd);
                return fail("batch create unlink create");
            }
            unsigned long long part2 = now_ns();
            unsigned long long part2_cycles = read_tsc();
            if (close(mutation_fd) != 0) {
                (void)close(fd);
                return fail("batch create unlink close");
            }
            unsigned long long part3_cycles = read_tsc();
            unsigned long long part3 = now_ns();
            unsigned long long part4 = now_ns();
            unsigned long long part4_cycles = read_tsc();
            if (unlink(mutation_path) != 0) {
                (void)close(fd);
                return fail("batch create unlink unlink");
            }
            unsigned long long part5_cycles = read_tsc();
            unsigned long long part5 = now_ns();
            create_unlink_create_ns += part1 - part0;
            create_unlink_close_ns += part3 - part2;
            create_unlink_unlink_ns += part5 - part4;
            if (part1_cycles >= part0_cycles) create_unlink_create_cycles += part1_cycles - part0_cycles;
            if (part3_cycles >= part2_cycles) create_unlink_close_cycles += part3_cycles - part2_cycles;
            if (part5_cycles >= part4_cycles) create_unlink_unlink_cycles += part5_cycles - part4_cycles;
            create_unlink_iters++;
            t1 = now_ns();
            if (bench_done(t0, t1, create_unlink_iters)) break;
        }
        metric_avg("batch_create_close_unlink", t0, t1, create_unlink_iters);
        if (tsc_calibration_cycles != 0) {
            metric_total_avg_from_cycles("batch_create_close_unlink.create", create_unlink_create_cycles, create_unlink_iters);
            metric_total_avg_from_cycles("batch_create_close_unlink.close", create_unlink_close_cycles, create_unlink_iters);
            metric_total_avg_from_cycles("batch_create_close_unlink.unlink", create_unlink_unlink_cycles, create_unlink_iters);
        } else {
            metric_total_avg("batch_create_close_unlink.create", create_unlink_create_ns, create_unlink_iters);
            metric_total_avg("batch_create_close_unlink.close", create_unlink_close_ns, create_unlink_iters);
            metric_total_avg("batch_create_close_unlink.unlink", create_unlink_unlink_ns, create_unlink_iters);
        }
    }
    }

    unsigned long long readv_iters = 0;
    unsigned long long readv_lseek_ns = 0;
    unsigned long long readv_readv_ns = 0;
    unsigned long long readv_lseek_cycles = 0;
    unsigned long long readv_readv_cycles = 0;
    t0 = now_ns();
    for (;;) {
        memset(left, 0, sizeof(left));
        memset(right, 0, sizeof(right));
        iov[0].iov_base = left;
        iov[0].iov_len = strlen(first);
        iov[1].iov_base = right;
        iov[1].iov_len = sizeof(right) - 1;
        errno = 0;
        unsigned long long part0 = now_ns();
        unsigned long long part0_cycles = read_tsc();
        off_t seek_status = lseek(fd, 0, SEEK_SET);
        unsigned long long part1_cycles = read_tsc();
        unsigned long long part1 = now_ns();
        ssize_t got = -1;
        unsigned long long part2 = part1;
        unsigned long long part3 = part1;
        unsigned long long part2_cycles = part1_cycles;
        unsigned long long part3_cycles = part1_cycles;
        if (seek_status == 0) {
            part2 = now_ns();
            part2_cycles = read_tsc();
            got = readv(fd, iov, 2);
            part3_cycles = read_tsc();
            part3 = now_ns();
        }
        if (seek_status != 0 || got < 0) {
            fprintf(
                stderr,
                "[libc-vfs-exec-smoke] batch readv seek=%lld got=%lld iter=%llu errno=%d\n",
                (long long)seek_status,
                (long long)got,
                readv_iters,
                errno);
            (void)close(fd);
            return fail("batch readv");
        }
        readv_lseek_ns += part1 - part0;
        readv_readv_ns += part3 - part2;
        if (part1_cycles >= part0_cycles) readv_lseek_cycles += part1_cycles - part0_cycles;
        if (part3_cycles >= part2_cycles) readv_readv_cycles += part3_cycles - part2_cycles;
        readv_iters++;
        t1 = now_ns();
        if (bench_done(t0, t1, readv_iters)) break;
    }
    metric_avg("batch_lseek_readv", t0, t1, readv_iters);
    if (tsc_calibration_cycles != 0) {
        metric_total_avg_from_cycles("batch_lseek_readv.lseek", readv_lseek_cycles, readv_iters);
        metric_total_avg_from_cycles("batch_lseek_readv.readv", readv_readv_cycles, readv_iters);
    } else {
        metric_total_avg("batch_lseek_readv.lseek", readv_lseek_ns, readv_iters);
        metric_total_avg("batch_lseek_readv.readv", readv_readv_ns, readv_iters);
    }

    if (close(fd) != 0) return fail("batch close fd");

    unsigned long long dir_iters = 0;
    unsigned long long dir_opendir_ns = 0;
    unsigned long long dir_readdir_ns = 0;
    unsigned long long dir_closedir_ns = 0;
    unsigned long long dir_opendir_cycles = 0;
    unsigned long long dir_readdir_cycles = 0;
    unsigned long long dir_closedir_cycles = 0;
    t0 = now_ns();
    for (;;) {
        unsigned long long part0 = now_ns();
        unsigned long long part0_cycles = read_tsc();
        DIR *loop_dir = opendir("/");
        unsigned long long part1_cycles = read_tsc();
        unsigned long long part1 = now_ns();
        if (loop_dir == NULL) return fail("batch opendir");
        unsigned long long part2 = now_ns();
        unsigned long long part2_cycles = read_tsc();
        for (;;) {
            struct dirent *de = readdir(loop_dir);
            if (de == NULL) break;
        }
        unsigned long long part3_cycles = read_tsc();
        unsigned long long part3 = now_ns();
        unsigned long long part3_close_cycles = read_tsc();
        if (closedir(loop_dir) != 0) return fail("batch closedir");
        unsigned long long part4_cycles = read_tsc();
        unsigned long long part4 = now_ns();
        dir_opendir_ns += part1 - part0;
        dir_readdir_ns += part3 - part2;
        dir_closedir_ns += part4 - part3;
        if (part1_cycles >= part0_cycles) dir_opendir_cycles += part1_cycles - part0_cycles;
        if (part3_cycles >= part2_cycles) dir_readdir_cycles += part3_cycles - part2_cycles;
        if (part4_cycles >= part3_close_cycles) dir_closedir_cycles += part4_cycles - part3_close_cycles;
        dir_iters++;
        t1 = now_ns();
        if (bench_done(t0, t1, dir_iters)) break;
    }
    metric_avg("batch_opendir_readdir_closedir", t0, t1, dir_iters);
    if (tsc_calibration_cycles != 0) {
        metric_total_avg_from_cycles("batch_opendir_readdir_closedir.opendir", dir_opendir_cycles, dir_iters);
        metric_total_avg_from_cycles("batch_opendir_readdir_closedir.readdir_all", dir_readdir_cycles, dir_iters);
        metric_total_avg_from_cycles("batch_opendir_readdir_closedir.closedir", dir_closedir_cycles, dir_iters);
    } else {
        metric_total_avg("batch_opendir_readdir_closedir.opendir", dir_opendir_ns, dir_iters);
        metric_total_avg("batch_opendir_readdir_closedir.readdir_all", dir_readdir_ns, dir_iters);
        metric_total_avg("batch_opendir_readdir_closedir.closedir", dir_closedir_ns, dir_iters);
    }

    unsigned long long raw_dir_iters = 0;
    unsigned long long raw_dir_open_cycles = 0;
    unsigned long long raw_dir_getdents_first_cycles = 0;
    unsigned long long raw_dir_scan_cycles = 0;
    unsigned long long raw_dir_getdents_eof_cycles = 0;
    unsigned long long raw_dir_close_cycles = 0;
    unsigned long long raw_dir_entries = 0;
    unsigned long long raw_dir_bytes = 0;
    t0 = now_ns();
    for (;;) {
        unsigned char dents[4096];
        unsigned long long part0_cycles = read_tsc();
        int raw_dir_fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        unsigned long long part1_cycles = read_tsc();
        if (raw_dir_fd < 0) return fail("raw dir open");

        int dent_bytes = (int)smoke_getdents(raw_dir_fd, (struct dirent *)dents, sizeof(dents));
        unsigned long long part2_cycles = read_tsc();
        if (dent_bytes < 0) {
            (void)close(raw_dir_fd);
            return fail("raw dir getdents first");
        }

        int offset = 0;
        while (offset < dent_bytes) {
            const struct dirent *de = (const struct dirent *)(const void *)(dents + offset);
            if (de->d_reclen == 0 || offset + de->d_reclen > dent_bytes) {
                (void)close(raw_dir_fd);
                return fail("raw dir scan");
            }
            raw_dir_entries++;
            offset += de->d_reclen;
        }
        unsigned long long part3_cycles = read_tsc();

        int eof_bytes = (int)smoke_getdents(raw_dir_fd, (struct dirent *)dents, sizeof(dents));
        unsigned long long part4_cycles = read_tsc();
        if (eof_bytes != 0) {
            (void)close(raw_dir_fd);
            return fail("raw dir getdents eof");
        }

        if (close(raw_dir_fd) != 0) return fail("raw dir close");
        unsigned long long part5_cycles = read_tsc();

        if (part1_cycles >= part0_cycles) raw_dir_open_cycles += part1_cycles - part0_cycles;
        if (part2_cycles >= part1_cycles) raw_dir_getdents_first_cycles += part2_cycles - part1_cycles;
        if (part3_cycles >= part2_cycles) raw_dir_scan_cycles += part3_cycles - part2_cycles;
        if (part4_cycles >= part3_cycles) raw_dir_getdents_eof_cycles += part4_cycles - part3_cycles;
        if (part5_cycles >= part4_cycles) raw_dir_close_cycles += part5_cycles - part4_cycles;
        raw_dir_bytes += (unsigned long long)dent_bytes;
        raw_dir_iters++;
        t1 = now_ns();
        if (bench_done(t0, t1, raw_dir_iters)) break;
    }
    metric_avg("batch_dir_raw", t0, t1, raw_dir_iters);
    metric_total_avg_from_cycles("batch_dir_raw.open", raw_dir_open_cycles, raw_dir_iters);
    metric_total_avg_from_cycles("batch_dir_raw.getdents_first", raw_dir_getdents_first_cycles, raw_dir_iters);
    metric_total_avg_from_cycles("batch_dir_raw.scan", raw_dir_scan_cycles, raw_dir_iters);
    metric_total_avg_from_cycles("batch_dir_raw.getdents_eof", raw_dir_getdents_eof_cycles, raw_dir_iters);
    metric_total_avg_from_cycles("batch_dir_raw.close", raw_dir_close_cycles, raw_dir_iters);
    printf(
        "[libc-vfs-exec-smoke] metric op=batch_dir_raw.entries iterations=%llu total=%llu avg=%llu\n",
        raw_dir_iters,
        raw_dir_entries,
        raw_dir_iters > 0 ? raw_dir_entries / raw_dir_iters : 0);
    printf(
        "[libc-vfs-exec-smoke] metric op=batch_dir_raw.bytes iterations=%llu total=%llu avg=%llu\n",
        raw_dir_iters,
        raw_dir_bytes,
        raw_dir_iters > 0 ? raw_dir_bytes / raw_dir_iters : 0);
    fflush(stdout);

    if (run_filed_raw_operation_bench(path) != 0) {
        return 1;
    }

    if (run_fs_write_stress() != 0) {
        return 1;
    }

    printf("[libc-vfs-exec-smoke] vfs ok\n");
    fflush(stdout);
    return 0;
}

int main(int argc, char **argv, char **envp)
{
    calibrate_tsc_timebase();

    if (argc >= 2 && strcmp(argv[1], "--child") == 0) {
        return run_child(argc, argv, envp);
    }

    if (run_vfs() != 0) {
        return 1;
    }

    printf("[libc-vfs-exec-smoke] exec child\n");
    fflush(stdout);
    /* An unlinked open file proves exec preserves live Linux references,
     * rather than reopening saved names or serializing cached offsets. */
    char inherit_path[128];
    snprintf(inherit_path,sizeof(inherit_path),"/tmp/libc-exec-inherit-%llx",now_ns());
    int inherited=open(inherit_path,O_CREAT|O_EXCL|O_RDWR,0600);
    if (inherited<0) return fail("exec inherited open");
    if (write(inherited,"abcdefghij",10)!=10 || lseek(inherited,2,SEEK_SET)!=2 ||
        unlink(inherit_path)) { (void)close(inherited); return fail("exec inherited prepare"); }
    int shared=fcntl(inherited,F_DUPFD,0);
    int cloexec=fcntl(inherited,F_DUPFD_CLOEXEC,0);
    if (shared<0 || cloexec<0) {
        if (shared>=0) (void)close(shared);
        if (cloexec>=0) (void)close(cloexec);
        (void)close(inherited); return fail("exec inherited duplicate");
    }
    char fd_text[3][24];
    snprintf(fd_text[0],sizeof(fd_text[0]),"%d",inherited);
    snprintf(fd_text[1],sizeof(fd_text[1]),"%d",shared);
    snprintf(fd_text[2],sizeof(fd_text[2]),"%d",cloexec);
    char **child_argv=calloc(301,sizeof(*child_argv));
    char **child_envp=calloc(81,sizeof(*child_envp));
    if (!child_argv || !child_envp) {
        free(child_argv); free(child_envp);
        (void)close(inherited); (void)close(shared); (void)close(cloexec);
        return fail("exec dynamic vectors");
    }
    child_argv[0]=
#ifdef __linux__
        argv[0];
#else
        "/cmd/libc_vfs_exec_smoke.elf";
#endif
    child_argv[1]="--child";
    child_argv[2]=fd_text[0]; child_argv[3]=fd_text[1]; child_argv[4]=fd_text[2];
    for (size_t i=5;i<300;++i)
        child_argv[i]="argument-crossing-the-former-fixed-string-payload-budget";
    char exec_start_env[96];
    unsigned long long exec_start_ns = now_ns();
    snprintf(exec_start_env, sizeof(exec_start_env), "PACHA_LIBC_VFS_EXEC_START_NS=%llu", exec_start_ns);
    child_envp[0]="PACHA_LIBC_VFS_EXEC_CHILD=1";
    child_envp[1]=exec_start_env;
    for (size_t i=2;i<80;++i) child_envp[i]="FIXTURE_LARGE_ENV=value";
    execve(child_argv[0], child_argv, child_envp);
    free(child_argv); free(child_envp);
    (void)close(inherited); (void)close(shared); (void)close(cloexec);
    return fail("execve child");
}
