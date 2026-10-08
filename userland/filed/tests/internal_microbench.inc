/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <time.h>
#include "filed/storage_owner.h"

int filed_bench_collect;
#ifdef FILED_INTERNAL_MMAP_PREFAULT
int filed_bench_prefault=1;
#else
int filed_bench_prefault;
#endif
uint64_t filed_bench_exchange_cycles, filed_bench_exchange_count;
uint64_t filed_bench_snapshot_cycles[10], filed_bench_storage_cycles[4];
uint64_t filed_bench_channel_cycles[3];
#ifdef FILED_INTERNAL_RING_ONLY
uint64_t filed_bench_wait_cycles[2], filed_bench_wait_counts[3];
#endif
static double bench_ns_per_cycle;
static uint64_t bench_seq;
static volatile unsigned bench_touch;
static uint8_t bench_output[KB2_FILESYSTEM_RESPONSE_SIZE + 32768];
static uint8_t bench_request[KB2_FILESYSTEM_REQUEST_SIZE + 4096];

static void bench_check(int status)
{
    if (status) {
        printf("FILED_INTERNAL_BENCH_FAIL status=%d\n", status);
        fflush(stdout);
        exit(1);
    }
}
static kb2_fs_response_t bench_call(filed_runtime_t *rt, uint32_t opcode,
    uint64_t handle, const char *path, uint64_t flags, uint64_t offset,
    uint64_t length, const void *data)
{
    kb2_fs_request_t r={.opcode=opcode,.generation=1,.sequence=++bench_seq,
        .handle=handle,.flags=flags,.offset=(int64_t)offset,.length=length,
        .mode=opcode==KB2_FILESYSTEM_OP_MKDIRAT ? 0755 :
            opcode==KB2_FILESYSTEM_OP_OPENAT2 && (flags&0100) ? 0644 : 0,
        .umask=0022,.mask=opcode==KB2_FILESYSTEM_OP_STATX ? 0x7ff : 0};
    size_t tail=path ? strlen(path)+1 : data ? (size_t)length : 0;
    if(tail>4096) bench_check(-EOVERFLOW);
    if(path) r.path=(kb2_fs_span_t){KB2_FILESYSTEM_REQUEST_SIZE,tail};
    if(data) r.data=(kb2_fs_span_t){KB2_FILESYSTEM_REQUEST_SIZE,tail};
    bench_check(kb2_fs_request_encode(bench_request,KB2_FILESYSTEM_REQUEST_SIZE,
        KB2_FILESYSTEM_REQUEST_SIZE+tail,&r));
    if(tail) memcpy(bench_request+KB2_FILESYSTEM_REQUEST_SIZE,path ? (const void *)path : data,tail);
    size_t used=0,fd_count=0;int fds[3];
    size_t capacity=KB2_FILESYSTEM_RESPONSE_SIZE;
    if(opcode==KB2_FILESYSTEM_OP_STATX) capacity+=KB2_FILESYSTEM_STATX_SIZE;
    if(opcode==KB2_FILESYSTEM_OP_PREAD || opcode==KB2_FILESYSTEM_OP_GETDENTS64) capacity+=(size_t)length;
    bench_check(dispatch_wire(rt,&r,bench_request,KB2_FILESYSTEM_REQUEST_SIZE+tail,
        bench_output,capacity,&used,NULL,0,fds,&fd_count));
    kb2_fs_response_t response;
    bench_check(kb2_fs_response_decode(bench_output,used,&response));
    if(response.result<0) bench_check((int)response.result);
    if(fd_count) bench_check(-EPROTO);
    return response;
}
static uint64_t bench_open(filed_runtime_t *rt,const char *path,uint64_t flags)
{
    return bench_call(rt,KB2_FILESYSTEM_OP_OPENAT2,0,path,flags,0,0,NULL).handle;
}
static void bench_close(filed_runtime_t *rt,uint64_t handle)
{
    (void)bench_call(rt,KB2_FILESYSTEM_OP_CLOSE,handle,NULL,0,0,0,NULL);
}
static void bench_empty(filed_runtime_t *rt,const char *path)
{
    bench_close(rt,bench_open(rt,path,0100|0200|02000000|1));
}
static void bench_read4k(filed_runtime_t *rt,uint64_t handle,uint64_t offset)
{
    kb2_fs_response_t r=bench_call(rt,KB2_FILESYSTEM_OP_PREAD,handle,NULL,0,offset,4096,NULL);
    if(r.result!=4096 || r.data_length!=4096) bench_check(-EIO);
    bench_touch+=bench_output[KB2_FILESYSTEM_RESPONSE_SIZE];
}
static void bench_mmap(filed_runtime_t *rt,uint64_t handle)
{
    kb2_fs_request_t r={.handle=handle,.length=1048576};
    kb2_fs_response_t response={0};int vmo=-1;
    bench_check(make_file_vmo(rt,&r,&response,&vmo));
    if(response.result!=1048576) bench_check(-EIO);
    uint64_t stage=filed_bench_tsc();
    unsigned char *map=pacha_mmap(vmo,1048576,PACHA_PROT_READ,PACHA_MMAP_PRIVATE,0);
    if(!map) bench_check(-ENOMEM);
    FILED_BENCH_STAGE(filed_bench_snapshot_cycles,6,stage);
    for(unsigned off=0;off<1048576;off+=4096) bench_touch+=map[off];
    FILED_BENCH_STAGE(filed_bench_snapshot_cycles,7,stage);
    bench_check(pacha_munmap(map,1048576));
    FILED_BENCH_STAGE(filed_bench_snapshot_cycles,8,stage);
    bench_check(pacha_fd_close(vmo));
    FILED_BENCH_STAGE(filed_bench_snapshot_cycles,9,stage);
}
static void bench_directory(filed_runtime_t *rt,const char *wide)
{
    uint64_t h=bench_open(rt,wide,0200000|02000000);
    unsigned entries=0,dots=0;
    for(;;) {
        kb2_fs_response_t r=bench_call(rt,KB2_FILESYSTEM_OP_GETDENTS64,h,NULL,0,0,2048,NULL);
        if(!r.result) break;
        if((uint64_t)r.result!=r.data_length) bench_check(-EIO);
        for(size_t at=0;at<r.data_length;) {
            const uint8_t *p=bench_output+KB2_FILESYSTEM_RESPONSE_SIZE+at;
            if(r.data_length-at<20) bench_check(-EIO);
            unsigned len=p[16]|(unsigned)p[17]<<8;
            if(len<20 || len>r.data_length-at || !memchr(p+19,0,len-19)) bench_check(-EIO);
            const char *name=(const char *)p+19;
            if(!strcmp(name,".") || !strcmp(name,"..")) ++dots;
            else {if(strncmp(name,"entry-",6)) bench_check(-EIO);++entries;}
            at+=len;
        }
    }
    if(entries!=256 || dots!=2) bench_check(-EIO);
    bench_close(rt,h);
}
static void bench_phase(filed_runtime_t *rt,const char *fs,const char *phase,
    unsigned kind,const char *path,const char *wide,uint64_t handle,unsigned trial)
{
    uint8_t block[4096];memset(block,0x5a,sizeof(block));
    uint64_t total=0,count=0,exchanges=0,exchange_cycles=0;
    memset(filed_bench_snapshot_cycles,0,sizeof(filed_bench_snapshot_cycles));
    memset(filed_bench_storage_cycles,0,sizeof(filed_bench_storage_cycles));
    memset(filed_bench_channel_cycles,0,sizeof(filed_bench_channel_cycles));
#ifdef FILED_INTERNAL_RING_ONLY
    memset(filed_ring_storage_cycles,0,sizeof(filed_ring_storage_cycles));
    filed_ring_storage_notifications=filed_ring_storage_waits=0;
    memset(filed_bench_wait_cycles,0,sizeof(filed_bench_wait_cycles));
    memset(filed_bench_wait_counts,0,sizeof(filed_bench_wait_counts));
#endif
    do {
        filed_bench_exchange_cycles=filed_bench_exchange_count=0;
        filed_bench_collect=1;
        uint64_t start=filed_bench_tsc();
        switch(kind) {
        case 0: (void)bench_call(rt,KB2_FILESYSTEM_OP_STATX,0,path,0,0,0,NULL);break;
        case 1: bench_close(rt,bench_open(rt,path,02000000));break;
        case 2: bench_empty(rt,path);break;
        case 3: bench_directory(rt,wide);break;
        case 4: bench_read4k(rt,handle,0);break;
        case 5: case 6:
            if(bench_call(rt,KB2_FILESYSTEM_OP_PWRITE,handle,NULL,0,(count%256)*4096,4096,block).result!=4096) bench_check(-EIO);
            if(kind==6) (void)bench_call(rt,KB2_FILESYSTEM_OP_FSYNC,handle,NULL,0,0,0,NULL);
            break;
        case 7: for(unsigned off=0;off<1048576;off+=4096) bench_read4k(rt,handle,off);break;
        case 8: bench_mmap(rt,handle);break;
        }
        total+=filed_bench_tsc()-start;
        filed_bench_collect=0;
        exchanges+=filed_bench_exchange_count;exchange_cycles+=filed_bench_exchange_cycles;++count;
        /* As in the original create benchmark, deletion is untimed setup. */
        if(kind==2) (void)bench_call(rt,KB2_FILESYSTEM_OP_UNLINKAT,0,path,0,0,0,NULL);
    } while((double)total*bench_ns_per_cycle<1000000000.0);
    printf("FILED_INTERNAL_BENCH fs=%s phase=%s trial=%u ops=%llu total_ns=%.0f ns_per_op=%.3f exchange_ns_per_op=%.3f exchanges_per_op=%.3f\n",
        fs,phase,trial,(unsigned long long)count,total*bench_ns_per_cycle,
        total*bench_ns_per_cycle/count,exchange_cycles*bench_ns_per_cycle/count,(double)exchanges/count);
    fflush(stdout);
#ifdef FILED_INTERNAL_RING_ONLY
    printf("FILED_STORAGE_RING fs=%s phase=%s trial=%u calls=%llu publish_ns=%.3f notify_ns=%.3f wait_ns=%.3f copy_release_ns=%.3f notified=%.6f waits_per_call=%.6f\n",
        fs,phase,trial,(unsigned long long)exchanges,
        filed_ring_storage_cycles[0]*bench_ns_per_cycle/exchanges,
        filed_ring_storage_cycles[1]*bench_ns_per_cycle/exchanges,
        filed_ring_storage_cycles[2]*bench_ns_per_cycle/exchanges,
        filed_ring_storage_cycles[3]*bench_ns_per_cycle/exchanges,
        (double)filed_ring_storage_notifications/exchanges,
        (double)filed_ring_storage_waits/exchanges);
    printf("FILED_STORAGE_WAIT fs=%s phase=%s trial=%u calls=%llu recv_ns=%.3f fd_wait_ns=%.3f receives=%.6f empty=%.6f fd_waits=%.6f\n",
        fs,phase,trial,(unsigned long long)exchanges,
        filed_bench_wait_cycles[0]*bench_ns_per_cycle/exchanges,
        filed_bench_wait_cycles[1]*bench_ns_per_cycle/exchanges,
        (double)filed_bench_wait_counts[0]/exchanges,
        (double)filed_bench_wait_counts[1]/exchanges,
        (double)filed_bench_wait_counts[2]/exchanges);
    fflush(stdout);
#endif
    if(kind==8) {
        static const char *names[]={"stat","vmo_create","writer_map","zero_fill","storage_read","writer_unmap","reader_map","touch","reader_unmap","vmo_close"};
        for(unsigned i=0;i<10;++i) printf("FILED_MMAP_STAGE fs=%s trial=%u stage=%s ns_per_op=%.3f\n",fs,trial,names[i],filed_bench_snapshot_cycles[i]*bench_ns_per_cycle/count);
        static const char *nested[]={"request_alloc_encode","response_decode_copy","request_response_free","response_prefault"};
        for(unsigned i=0;i<4;++i) printf("FILED_MMAP_NESTED fs=%s trial=%u stage=%s ns_per_op=%.3f\n",fs,trial,nested[i],filed_bench_storage_cycles[i]*bench_ns_per_cycle/count);
        static const char *channel[]={"channel_publish","channel_notify_wait","channel_response_copy_release"};
        for(unsigned i=0;i<3;++i) printf("FILED_MMAP_NESTED fs=%s trial=%u stage=%s ns_per_op=%.3f\n",fs,trial,channel[i],filed_bench_channel_cycles[i]*bench_ns_per_cycle/count);
        fflush(stdout);
    }
}
#ifdef FILED_INTERNAL_MMAP_ONLY
static void bench_touch_control(filed_runtime_t *rt,const char *fs,uint64_t handle)
{
    uint64_t cold=0,warm=0;const unsigned samples=32,passes=100;
    for(unsigned i=0;i<samples;++i) {
        kb2_fs_request_t r={.handle=handle,.length=1048576};
        kb2_fs_response_t response={0};int vmo=-1;
        bench_check(make_file_vmo(rt,&r,&response,&vmo));
        unsigned char *map=pacha_mmap(vmo,1048576,PACHA_PROT_READ,PACHA_MMAP_PRIVATE,0);
        if(!map) bench_check(-ENOMEM);
        uint64_t start=filed_bench_tsc();
        for(unsigned off=0;off<1048576;off+=4096) bench_touch+=map[off];
        cold+=filed_bench_tsc()-start;start=filed_bench_tsc();
        for(unsigned pass=0;pass<passes;++pass)
            for(unsigned off=0;off<1048576;off+=4096) bench_touch+=map[off];
        warm+=filed_bench_tsc()-start;
        bench_check(pacha_munmap(map,1048576));bench_check(pacha_fd_close(vmo));
    }
    printf("FILED_MMAP_TOUCH_CONTROL fs=%s samples=%u warm_passes=%u cold_ns=%.3f warm_ns=%.3f\n",fs,samples,passes,cold*bench_ns_per_cycle/samples,warm*bench_ns_per_cycle/samples/passes);fflush(stdout);
}
static void bench_mmap_fs(filed_runtime_t *rt,const char *fs,const char *parent)
{
    char path[256];snprintf(path,sizeof(path),"%s/filed-mmap-breakdown",parent);
    uint64_t h=bench_open(rt,path,0100|0200|02000000|2);
    uint8_t block[4096];memset(block,0x5a,sizeof(block));
    for(unsigned off=0;off<1048576;off+=4096) if(bench_call(rt,KB2_FILESYSTEM_OP_PWRITE,h,NULL,0,off,4096,block).result!=4096) bench_check(-EIO);
    (void)bench_call(rt,KB2_FILESYSTEM_OP_FSYNC,h,NULL,0,0,0,NULL);
    for(unsigned trial=1;trial<=3;++trial) bench_phase(rt,fs,"mmap_1m_touch",8,path,NULL,h,trial);
    bench_touch_control(rt,fs,h);
    /* Outside timing, validate every byte and the zero tail after a partial
     * EOF read. A warm checksum alone would miss a missing page or overcopy. */
    kb2_fs_request_t snapshot={.handle=h,.offset=1048576-1000,.length=5000};
    kb2_fs_response_t response={0};int vmo=-1;
    bench_check(make_file_vmo(rt,&snapshot,&response,&vmo));
    if(response.result!=1000) bench_check(-EIO);
    unsigned char *map=pacha_mmap(vmo,8192,PACHA_PROT_READ,PACHA_MMAP_PRIVATE,0);
    if(!map) bench_check(-ENOMEM);
    for(unsigned i=0;i<8192;++i) if(map[i]!=(i<1000 ? 0x5a : 0)) bench_check(-EIO);
    bench_check(pacha_munmap(map,8192));bench_check(pacha_fd_close(vmo));
    snapshot=(kb2_fs_request_t){.handle=h,.length=1048576};
    bench_check(make_file_vmo(rt,&snapshot,&response,&vmo));
    if(response.result!=1048576) bench_check(-EIO);
    map=pacha_mmap(vmo,1048576,PACHA_PROT_READ,PACHA_MMAP_PRIVATE,0);
    if(!map) bench_check(-ENOMEM);
    for(unsigned i=0;i<1048576;++i) if(map[i]!=0x5a) bench_check(-EIO);
    bench_check(pacha_munmap(map,1048576));bench_check(pacha_fd_close(vmo));
    printf("FILED_MMAP_CONTENT_OK fs=%s full_bytes=1048576 eof_bytes=1000 zero_tail=7192\n",fs);fflush(stdout);
    bench_close(rt,h);(void)bench_call(rt,KB2_FILESYSTEM_OP_UNLINKAT,0,path,0,0,0,NULL);
}
#endif
static void bench_fs(filed_runtime_t *rt,const char *fs,const char *parent)
{
    char base[256],deep[256],shallow[300],leaf[300],wide[300],data[300],path[350];
    snprintf(base,sizeof(base),"%s/filed-internal-bench",parent);
    (void)bench_call(rt,KB2_FILESYSTEM_OP_MKDIRAT,0,base,0,0,0,NULL);
    strcpy(deep,base);
    for(unsigned d=0;d<8;++d) {size_t at=strlen(deep);snprintf(deep+at,sizeof(deep)-at,"/d%u",d);(void)bench_call(rt,KB2_FILESYSTEM_OP_MKDIRAT,0,deep,0,0,0,NULL);}
    snprintf(shallow,sizeof(shallow),"%s/f",base);snprintf(leaf,sizeof(leaf),"%s/f",deep);
    snprintf(wide,sizeof(wide),"%s/wide",base);snprintf(data,sizeof(data),"%s/data",base);
    bench_empty(rt,shallow);bench_empty(rt,leaf);
    (void)bench_call(rt,KB2_FILESYSTEM_OP_MKDIRAT,0,wide,0,0,0,NULL);
    for(unsigned i=0;i<256;i++) {snprintf(path,sizeof(path),"%s/entry-%04u",wide,i);bench_empty(rt,path);}
    uint64_t h=bench_open(rt,data,0100|0200|02000000|2);uint8_t block[4096];memset(block,0x5a,sizeof(block));
    for(unsigned off=0;off<1048576;off+=4096) if(bench_call(rt,KB2_FILESYSTEM_OP_PWRITE,h,NULL,0,off,4096,block).result!=4096) bench_check(-EIO);
    (void)bench_call(rt,KB2_FILESYSTEM_OP_FSYNC,h,NULL,0,0,0,NULL);
    for(unsigned trial=1;trial<=3;trial++) {
        if(!strcmp(fs,"ext4")) {
            bench_phase(rt,fs,"stat_d0",0,shallow,wide,h,trial);
            bench_phase(rt,fs,"open_close_d0",1,shallow,wide,h,trial);
            snprintf(path,sizeof(path),"%s/created",deep);bench_phase(rt,fs,"create_close_d8",2,path,wide,h,trial);
            bench_phase(rt,fs,"readdir_256",3,shallow,wide,h,trial);
            bench_phase(rt,fs,"pwrite_4k",5,shallow,wide,h,trial);
            bench_phase(rt,fs,"pwrite_fsync",6,shallow,wide,h,trial);
            bench_phase(rt,fs,"read_1m_hot",7,shallow,wide,h,trial);
        }
        bench_phase(rt,fs,"stat_d8",0,leaf,wide,h,trial);
        bench_phase(rt,fs,"pread_4k_hot",4,shallow,wide,h,trial);
        bench_phase(rt,fs,"mmap_1m_touch",8,shallow,wide,h,trial);
    }
    bench_close(rt,h);
    const char *files[]={shallow,leaf,data};for(unsigned i=0;i<3;++i)(void)bench_call(rt,KB2_FILESYSTEM_OP_UNLINKAT,0,files[i],0,0,0,NULL);
    for(unsigned i=0;i<256;i++) {snprintf(path,sizeof(path),"%s/entry-%04u",wide,i);(void)bench_call(rt,KB2_FILESYSTEM_OP_UNLINKAT,0,path,0,0,0,NULL);}
    (void)bench_call(rt,KB2_FILESYSTEM_OP_UNLINKAT,0,wide,0x200,0,0,NULL);
    for(unsigned d=0;d<8;++d) {(void)bench_call(rt,KB2_FILESYSTEM_OP_UNLINKAT,0,deep,0x200,0,0,NULL);*strrchr(deep,'/')=0;}
    (void)bench_call(rt,KB2_FILESYSTEM_OP_UNLINKAT,0,base,0x200,0,0,NULL);
}
#ifdef FILED_INTERNAL_RING_ONLY
static void bench_ring_fs(filed_runtime_t *rt,const char *fs,const char *parent)
{
    char path[256];snprintf(path,sizeof(path),"%s/filed-storage-ring-bench",parent);
    uint64_t h=bench_open(rt,path,0100|0200|02000000|2);
    uint8_t block[4096];memset(block,0x5a,sizeof(block));
    for(unsigned off=0;off<1048576;off+=4096)
        if(bench_call(rt,KB2_FILESYSTEM_OP_PWRITE,h,NULL,0,off,4096,block).result!=4096)
            bench_check(-EIO);
    (void)bench_call(rt,KB2_FILESYSTEM_OP_FSYNC,h,NULL,0,0,0,NULL);
    for(unsigned trial=1;trial<=3;++trial) {
        bench_phase(rt,fs,"stat_d0",0,path,NULL,h,trial);
        bench_phase(rt,fs,"pread_4k_hot",4,path,NULL,h,trial);
        bench_phase(rt,fs,"pwrite_4k",5,path,NULL,h,trial);
    }
    /* Timed RPCs must return real contents, not just a short-I/O count. */
    bench_read4k(rt,h,0);
    for(unsigned i=0;i<sizeof(block);++i)
        if(bench_output[KB2_FILESYSTEM_RESPONSE_SIZE+i]!=0x5a) bench_check(-EIO);
    bench_close(rt,h);
    (void)bench_call(rt,KB2_FILESYSTEM_OP_UNLINKAT,0,path,0,0,0,NULL);
}
#endif
int filed_internal_microbench(filed_runtime_t *rt)
{
    struct timespec a,b;bench_check(clock_gettime(CLOCK_MONOTONIC,&a));uint64_t start=filed_bench_tsc(),ns;
    do {bench_check(clock_gettime(CLOCK_MONOTONIC,&b));ns=(uint64_t)(b.tv_sec-a.tv_sec)*1000000000+(uint64_t)(b.tv_nsec-a.tv_nsec);}while(ns<1000000000);
    uint64_t cycles=filed_bench_tsc()-start;bench_ns_per_cycle=(double)ns/cycles;
    printf("FILED_INTERNAL_CLOCK tsc_hz=%.0f clock_ns=%llu cycles=%llu\n",1e9/bench_ns_per_cycle,(unsigned long long)ns,(unsigned long long)cycles);fflush(stdout);
    /* A real ordinary client namespace keeps owner exec handles out of this
     * benchmark; no endpoint or LPR participates in the timed calls. */
    uint8_t bytes[KB2_FILESYSTEM_REQUEST_SIZE+KB2_FILESYSTEM_CREDENTIALS_SIZE]={0};
    kb2_fs_credentials_t cred={.umask=0022,.cap_permitted=KB2_FILESYSTEM_CREDENTIAL_CAP_VALID_MASK,.cap_effective=KB2_FILESYSTEM_CREDENTIAL_CAP_VALID_MASK};
    bench_check(kb2_fs_credentials_encode(bytes+KB2_FILESYSTEM_REQUEST_SIZE,KB2_FILESYSTEM_CREDENTIALS_SIZE,&cred));
    kb2_fs_request_t r={.opcode=KB2_FILESYSTEM_OP_CLIENT_REGISTER,.client=2,.credential_generation=1,.data={KB2_FILESYSTEM_REQUEST_SIZE,KB2_FILESYSTEM_CREDENTIALS_SIZE}};
    kb2_fs_response_t response;bench_check(filed_owner_control(rt,&r,bytes,sizeof(bytes),&response));
    struct filed_client client={.id=2,.credential_generation=1,.rights=1023};rt->actor=&client;
#ifdef FILED_INTERNAL_RING_ONLY
    bench_ring_fs(rt,"ext4","/root");bench_ring_fs(rt,"tmpfs","/tmp");
#elif defined(FILED_INTERNAL_MMAP_ONLY)
    bench_mmap_fs(rt,"ext4","/root");bench_mmap_fs(rt,"tmpfs","/tmp");
#else
    bench_fs(rt,"ext4","/root");bench_fs(rt,"tmpfs","/tmp");
#endif
    (void)bench_call(rt,KB2_FILESYSTEM_OP_SYNC,0,NULL,0,0,0,NULL);
    rt->actor=NULL;r=(kb2_fs_request_t){.opcode=KB2_FILESYSTEM_OP_CLIENT_UNREGISTER,.client=2};bench_check(filed_owner_control(rt,&r,NULL,KB2_FILESYSTEM_REQUEST_SIZE,&response));
#ifdef FILED_INTERNAL_RING_ONLY
    /* Flush sandbox aggregates only after all timings and FS cleanup. */
    bench_check(filed_storage_owner_destroy(rt->storage));rt->storage=NULL;
#endif
    puts("FILED_INTERNAL_BENCH_OK");fflush(stdout);return 0;
}
