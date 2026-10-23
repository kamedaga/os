#include "lpr_ipc_mock.h"
/* Exercise public LPR mmap and the actual FILE_VMO RPC/codec. The native VM
 * mock retains the real capability ceiling: private COW permits WRITE from
 * MAP_READ, but neither mmap nor mprotect may invent EXEC authority. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../userland/personality/linux/runtime/lpr_filed_internal.h"
#include "../userland/personality/linux/runtime/lpr_vfs/session.h"
#include "../userland/personality/linux/runtime/lpr_common/boot_diagnostic.h"
#include <personality/zpoline.h>
#include <pacha/status.h>
#include "../userland/personality/linux/runtime/lpr_linux_syscall.h"

int64_t lpr_linux_mmap(uint64_t addr,uint64_t len,uint64_t prot,
                       uint64_t flags,uint64_t fd,uint64_t offset, unsigned *wait_not_ready __attribute__((unused)));

lpr_state_t lpr_state;
#if defined(LPR_BOOT_DIAGNOSTICS) && LPR_BOOT_DIAGNOSTICS
lpr_boot_diagnostic_state_t lpr_boot_diagnostic_state;
#endif
static lpr_filed_backend_t file = {.active=1,.handle=123};
static uint64_t file_bytes = 0x49000;
static int64_t stat_error, snapshot_error, map_error;
static unsigned snapshots, exec_snapshots, patches, anon_maps, fallback_reads;
static unsigned pins, unpins, logs;
static int allow_fallback;
static int64_t log_result;
static uint64_t snapshot_rights, local_rights, local_bytes, expected_flags;
static uint64_t expected_snapshot_offset, expected_snapshot_bytes, snapshot_bytes;
static unsigned char *rpc_memory;
static size_t rpc_size;
static struct pacha_ipc_msg completion;
struct mapping { uint64_t base, size, max_prot, prot; };
static struct mapping mappings[32];
static unsigned mapping_count;
static uint64_t next_va = 0x10000000;

int lpr_fs_session_terminal(void) { return 0; }
int lpr_fs_session_call(kb2_fs_request_t *request,const struct lpr_fs_buffer *input,unsigned input_count,
    kb2_fs_response_t *response,const struct lpr_fs_buffer *output,unsigned output_count,size_t *used)
{ (void)request;(void)input;(void)input_count;(void)response;(void)output;(void)output_count;(void)used;assert(0);return -1; }
int64_t lpr_filed_endpoint_ready(void)
{ lpr_filed_client_fd=50; lpr_filed_client_id=2; lpr_filed_endpoint_checked=1; return 0; }
int64_t lpr_filed_fd_binding(uint64_t fd) { assert(fd==3); return lpr_filed_endpoint_ready(); }
lpr_filed_backend_t *lpr_filed_backend(uint64_t fd) { return fd==3 ? &file : NULL; }
int lpr_linux_filed_fd_active(uint64_t fd) { return fd==3; }
int lpr_linux_dmabuf_fd_active(uint64_t fd) { (void)fd; return 0; }
int lpr_linux_drm_fd_active(uint64_t fd) { (void)fd; return 0; }
int64_t lpr_dmabuf_mmap(uint64_t f,uint64_t a,uint64_t n,uint64_t p,uint64_t g,uint64_t o)
{ (void)f;(void)a;(void)n;(void)p;(void)g;(void)o;assert(0);return -1; }
int64_t lpr_drm_mmap(uint64_t f,uint64_t a,uint64_t n,uint64_t p,uint64_t g,uint64_t o)
{ (void)f;(void)a;(void)n;(void)p;(void)g;(void)o;assert(0);return -1; }
int lpr_memfd_write_is_sealed(uint8_t flags) { (void)flags; return 0; }
/* Stat is an explicit operation-local fixture input, not an image cache. */
int64_t lpr_linux_fstat(uint64_t fd,uint64_t out, unsigned *wait_not_ready __attribute__((unused)))
{ assert(fd==3); if(stat_error)return stat_error; *(lpr_linux_stat_t *)(uintptr_t)out=(lpr_linux_stat_t){.st_size=(int64_t)file_bytes}; return 0; }
int lpr_fd_table_pin(lpr_fd_table_t *t,lpr_linux_fd_t fd,lpr_fd_pin_t *p)
{ assert(t==&lpr_control_fd_table && fd==3); *p=(lpr_fd_pin_t){.fd=fd,.ops_id=LPR_FD_OPS_FILED,.effective_rights=LPR_FD_RIGHT_MMAP}; ++pins; return 0; }
int lpr_fd_table_unpin(lpr_fd_table_t *t,const lpr_fd_pin_t *p,lpr_fd_drop_t *d)
{ assert(t==&lpr_control_fd_table && p->fd==3); ++unpins; *d=(lpr_fd_drop_t){0}; return 0; }
/* The pinned fixture never drops its backend. Keep real dispatch/unpin code
 * linked, and fail if it unexpectedly tries unrelated service cleanup. */
int64_t lpr_filed_close_handle(uint64_t h) { (void)h;assert(0);return -1; }
int64_t lpr_close_native_fd_if_open(uint64_t f) { (void)f;assert(0);return -1; }
int64_t lpr_input_close_handle(uint64_t h) { (void)h;assert(0);return -1; }
int64_t lpr_block_close_state(const void *s) { (void)s;assert(0);return -1; }
int64_t lpr_termd_call_handle(uint32_t op,uint64_t h,uint64_t *r) { (void)op;(void)h;(void)r;assert(0);return -1; }
int64_t lpr_drm_close_handle(uint64_t h) { (void)h;assert(0);return -1; }
int64_t lpr_socket_close_backend(void *s) { (void)s;assert(0);return -1; }
int64_t lpr_device_description_close(lpr_device_backend_t *s) { (void)s;assert(0);return -1; }
int64_t lpr_unix_socket_close(void *s) { (void)s;assert(0);return -1; }
int64_t lpr_backend_state_free(void *s,uint64_t n) { (void)s;(void)n;assert(0);return -1; }
void lpr_signalfd_refresh_mask(void) { assert(0); }
int64_t lpr_linux_sync_native_signal_mask(void) { assert(0);return -1; }
int64_t lpr_drm_prime_ref(uint32_t op,uint64_t t) { (void)op;(void)t;assert(0);return -1; }
uint64_t pacha_trace_name_id(const char *name) { (void)name; return 0; }
void pacha_trace_emit(uint32_t component,uint32_t event,uint32_t event_class,
                      uint32_t count,uint64_t a,uint64_t b,uint64_t c,
                      uint64_t d,uint64_t e,uint64_t f)
{ (void)component;(void)event;(void)event_class;(void)count;(void)a;(void)b;(void)c;(void)d;(void)e;(void)f; }
void *lpr_memcpy(void *d,const void *s,size_t n) { return memcpy(d,s,n); }
size_t lpr_strnlen(const char *s,size_t n) { return strnlen(s,n); }
uint64_t lpr_page_align_up(uint64_t n) { return n>UINT64_MAX-4095 ? 0 : (n+4095)&~4095ull; }
uint64_t lpr_next_request_id(volatile uint64_t *v) { return ++*v; }
int64_t lpr_pacha_status_to_errno(int64_t s) { return pacha_kernel_status_to_errno(s); }

int64_t lpr_pacha_syscall3(uint64_t nr,uint64_t a,uint64_t b,uint64_t c)
{
    if(nr==PACHAOS_SYSCALL_VMO_CREATE) {
        assert(b & PACHA_FD_RIGHT_MAP_WRITE);
        if (!(b & PACHA_FD_RIGHT_INSPECT)) {
            /* 0: the eagerly filled local-copy fallback. Zero tails and
             * reservations must be zero-on-demand; an eager VMO would
             * allocate and zero the whole span up front. */
            assert(c==0 || c==(PACHA_FD_FLAG_PRIVATE|PACHA_FD_FLAG_CLOEXEC|
                PACHAOS_VMO_CREATE_ZERO_ON_DEMAND));
            local_rights=b; local_bytes=a; return 70;
        }
        assert(c==0 || c==(PACHA_FD_FLAG_PRIVATE|PACHA_FD_FLAG_CLOEXEC));
        assert(!rpc_memory); rpc_size=a; return 40;
    }
    assert(nr==PACHAOS_SYSCALL_MPROTECT);
    for(unsigned i=0;i<mapping_count;++i) {
        struct mapping *m=&mappings[i];
        if(a>=m->base && a+b<=m->base+m->size) {
            if(c & ~m->max_prot)return 1; /* native syscall_err_invalid */
            m->prot=c; return 0;
        }
    }
    return 1; /* A range spanning two distinct VMAs is not one VMA. */
}
int64_t lpr_pacha_syscall6(uint64_t nr,uint64_t fd,uint64_t addr,uint64_t n,uint64_t p,uint64_t f,uint64_t o)
{
    assert(nr==PACHAOS_SYSCALL_MMAP && fd==40 && !addr && !o);
    assert(n==rpc_size && p==(PACHAOS_PROT_READ|PACHAOS_PROT_WRITE) && f==PACHAOS_MMAP_SHARED);
    rpc_memory=calloc(1,n); assert(rpc_memory); return (int64_t)(uintptr_t)rpc_memory;
}
int64_t lpr_pacha_syscall1(uint64_t nr,uint64_t fd)
{ assert(nr==PACHAOS_SYSCALL_FD_CLOSE && (fd==40 || fd==41 || fd==60 || fd==70)); return 0; }
static int64_t unit_ipc_call(uint64_t fd,struct pacha_ipc_msg *msg);
static int64_t unit_ipc_recv(uint64_t fd,struct pacha_ipc_msg *msg);
static int64_t fixture_prepare_request(uint64_t nr,uint64_t a,uint64_t b)
{
    if(nr==PACHAOS_SYSCALL_LOG) { assert(b<=256); ++logs; return log_result; }
    if(nr==PACHAOS_SYSCALL_IPC_CALL)
        return unit_ipc_call(a,(void *)(uintptr_t)b);
    assert(nr==PACHAOS_SYSCALL_MUNMAP);
    if(a==(uint64_t)(uintptr_t)rpc_memory) { assert(b==rpc_size); free(rpc_memory); rpc_memory=NULL; return 0; }
    for(unsigned i=0;i<mapping_count;++i) if(mappings[i].base==a) { mappings[i].size=0; return 0; }
    return 0;
}
static int64_t unit_ipc_call(uint64_t fd,struct pacha_ipc_msg *msg)
{
    assert(fd==50 && msg->fd_count==1 && msg->fds[0].fd==40);
    kb2_fs_request_t r;
    assert(!kb2_fs_request_decode(rpc_memory,msg->word1,msg->word1,&r));
    assert(r.opcode==KB2_FILESYSTEM_OP_FILE_VMO && r.handle==123);
    assert((uint64_t)r.offset==expected_snapshot_offset && r.length==expected_snapshot_bytes &&
        !r.path.length && !r.data.length);
    snapshot_bytes=r.length;
    assert(r.flags==expected_flags);
    ++snapshots;
    if(r.flags & FILED_FILE_VMO_EXEC)++exec_snapshots;
    snapshot_rights=PACHA_FD_RIGHT_MAP_READ;
    if(r.flags & FILED_FILE_VMO_EXEC)snapshot_rights|=PACHA_FD_RIGHT_MAP_EXEC;
    kb2_fs_response_t response={.opcode=r.opcode,.generation=r.generation,.sequence=r.sequence,
        .result=snapshot_error ? snapshot_error :
            (int64_t)(r.length < file_bytes-(uint64_t)r.offset ? r.length : file_bytes-(uint64_t)r.offset)};
    assert(!kb2_fs_response_encode(rpc_memory,rpc_size,&response));
    completion=(struct pacha_ipc_msg){.word0=r.opcode,.word1=response.result,
        .word2=KB2_FILESYSTEM_RESPONSE_SIZE,.word3=r.sequence};
    return 41;
}
static int64_t unit_ipc_recv(uint64_t fd,struct pacha_ipc_msg *msg)
{
    assert(fd==41 && msg->fd_capacity==3);
    struct pacha_ipc_fd *caps=msg->fds;
    *msg=completion; msg->fds=caps; msg->fd_capacity=3;
    if(!snapshot_error) { msg->fd_count=1; caps[0]=(struct pacha_ipc_fd){.fd=60,.rights=snapshot_rights}; }
    return 0;
}
static int64_t fixture_finish_request(uint64_t nr,uint64_t fd,uint64_t raw,uint64_t timeout,uint64_t flags)
{ assert(nr==PACHAOS_SYSCALL_IPC_RECV_WAIT && timeout==UINT64_MAX && flags == PACHA_WAIT_UNINTERRUPTIBLE); return unit_ipc_recv(fd,(void *)(uintptr_t)raw); }

int64_t lpr_drm_native_mmap(uint64_t fd,uint64_t addr,uint64_t n,uint64_t p,uint64_t f,uint64_t o)
{
    assert(!(n & 4095) && n);
    uint64_t max=PACHAOS_PROT_READ|PACHAOS_PROT_WRITE|PACHAOS_PROT_EXEC;
    if(f & PACHAOS_MMAP_ANONYMOUS) { assert(!fd); ++anon_maps; }
    else {
        assert((fd==60 && !o && n<=lpr_page_align_up(snapshot_bytes)) ||
            (fd==70 && o+n<=local_bytes));
        max=PACHAOS_PROT_READ;
        if((f & PACHAOS_MMAP_PRIVATE) && !(f & PACHAOS_MMAP_SHARED))max|=PACHAOS_PROT_WRITE;
        if((fd==60 ? snapshot_rights : local_rights) & PACHA_FD_RIGHT_MAP_EXEC)max|=PACHAOS_PROT_EXEC;
    }
    if(map_error)return map_error;
    if(p & ~max)return 1;
    if(!addr) { addr=next_va; next_va+=n+0x10000; }
    if(f & PACHAOS_MMAP_FIXED) {
        for(unsigned i=0;i<mapping_count;++i) {
            struct mapping *m=&mappings[i];
            if(addr>=m->base && addr+n<=m->base+m->size) {
                const uint64_t old_end=m->base+m->size;
                if(addr+n<old_end)mappings[mapping_count++]=(struct mapping){addr+n,old_end-addr-n,m->max_prot,m->prot};
                m->size=addr-m->base;
            }
        }
    }
    assert(mapping_count<32);
    mappings[mapping_count++]=(struct mapping){addr,n,max,p};
    return (int64_t)addr;
}
int64_t lpr_linux_pread_to_vmo(uint64_t f,uint64_t v,uint64_t vo,uint64_t n,uint64_t o)
{ assert(allow_fallback && f==3 && v==70 && !vo); ++fallback_reads;
  uint64_t available=file_bytes>o ? file_bytes-o : 0; return (int64_t)(n<available ? n : available); }
int64_t lpr_patch_mapping(const struct lpr_patch_mapping_request *r,struct lpr_patch_mapping_result *s)
{ assert(r->flags==(LPR_PATCH_FLAG_EXECUTABLE|LPR_PATCH_FLAG_PRIVATE) && r->size_bytes); ++patches; *s=(struct lpr_patch_mapping_result){0}; return PERSONALITY_STATUS_OK; }

static void reset(void)
{
    assert(!rpc_memory && pins==unpins);
    mapping_count=snapshots=exec_snapshots=patches=anon_maps=fallback_reads=0;
    stat_error=snapshot_error=map_error=0; expected_flags=0; file_bytes=0x49000; allow_fallback=0;
}

static int64_t checked_mmap(uint64_t address,uint64_t length,uint64_t prot,
    uint64_t flags,uint64_t fd,uint64_t offset, unsigned *wait_not_ready)
{
    (void)wait_not_ready;
    expected_snapshot_offset=offset<file_bytes ? offset : 0;
    const uint64_t image=lpr_page_align_up(file_bytes);
    expected_snapshot_bytes=offset<file_bytes ? lpr_page_align_up(length) :
        (file_bytes<4096 ? file_bytes : 4096);
    if(offset<file_bytes && expected_snapshot_bytes>image-offset)
        expected_snapshot_bytes=image-offset;
    return lpr_linux_mmap(address,length,prot,flags,fd,offset, 0);
}
#define lpr_linux_mmap checked_mmap
int main(void)
{
    reset();
    /* musl maps a read-only whole image, then remaps a nonzero-offset RX
     * PT_LOAD with MAP_FIXED. The first mapping must not acquire EXEC. */
    int64_t base=lpr_linux_mmap(0,0x4a000,1,2,3,0, 0);
    assert(base>=4096 && !exec_snapshots);
#ifndef LPR_MMAP_OLD_SOURCE
    /* The zero tail may not turn a read-only file reservation into an
     * executable anonymous region behind the filesystem's policy. */
    assert(lpr_pacha_syscall3(PACHAOS_SYSCALL_MPROTECT,base+file_bytes,4096,5)==1);
#endif
    expected_flags=FILED_FILE_VMO_EXEC;
#ifdef LPR_MMAP_OLD_SOURCE
    expected_flags=0;
#endif
    int64_t text=lpr_linux_mmap((uint64_t)base+0xe000,0x25000,5,0x12,3,0xe000, 0);
    fprintf(stderr,"LPR_PRIVATE_RX_RESULT=%lld EXEC_GRANTS=%u ANON=%u\n",(long long)text,exec_snapshots,anon_maps);
    assert(text==base+0xe000 && patches==1 && exec_snapshots==1 && !fallback_reads);
    reset();
    assert(lpr_linux_mmap(0,4096,3,2,3,0, 0)>=4096);
    assert(!exec_snapshots && !patches && !anon_maps);
    assert(snapshot_bytes==4096);
    reset(); file_bytes=256ull*1024*1024;
    assert(lpr_linux_mmap(0,8192,1,2,3,128ull*1024*1024, 0)>=4096);
    assert(snapshots==1 && snapshot_bytes==8192 && !exec_snapshots);
    for(unsigned scenario=0;scenario<5;++scenario) {
        reset(); expected_flags=FILED_FILE_VMO_EXEC;
        snapshot_error=scenario==0 ? -LPR_LINUX_EACCES : -LPR_LINUX_EIO;
        uint64_t offset=scenario==2 ? file_bytes : 0;
        uint64_t length=scenario==3 ? file_bytes+4096 : 4096;
        if(scenario==4)stat_error=-LPR_LINUX_EIO;
        int64_t result=lpr_linux_mmap(0,length,5,2,3,offset, 0);
        assert(result==(scenario==0 ? -LPR_LINUX_EACCES : -LPR_LINUX_EIO));
        assert(!anon_maps && !patches && !fallback_reads);
    }
    reset(); expected_flags=FILED_FILE_VMO_EXEC; map_error=3;
    assert(lpr_linux_mmap(0,4096,5,2,3,0, 0)==-LPR_LINUX_ENOMEM);
    assert(exec_snapshots==1 && !anon_maps && !fallback_reads);
    reset(); expected_flags=FILED_FILE_VMO_EXEC;
    assert(lpr_linux_mmap(0,4096,5,2,3,file_bytes, 0)>=4096);
    assert(exec_snapshots==1 && !anon_maps && !patches);
    reset(); expected_flags=FILED_FILE_VMO_EXEC;
    base=lpr_linux_mmap(0,file_bytes+4096,5,2,3,0, 0);
    assert(base>=4096 && exec_snapshots==1 && !anon_maps && patches==1);
    assert(!lpr_pacha_syscall3(PACHAOS_SYSCALL_MPROTECT,base,file_bytes,5));
    assert(!lpr_pacha_syscall3(PACHAOS_SYSCALL_MPROTECT,base+file_bytes,4096,5));
    reset();
    base=lpr_linux_mmap(0,4096,1,2,3,file_bytes, 0);
    assert(base>=4096 && !exec_snapshots && !anon_maps);
    assert(lpr_pacha_syscall3(PACHAOS_SYSCALL_MPROTECT,base,4096,5)==1);
    reset(); snapshot_error=-LPR_LINUX_EIO; allow_fallback=1;
    base=lpr_linux_mmap(0,4096,1,2,3,0, 0);
    assert(base>=4096 && !exec_snapshots && !anon_maps && fallback_reads==1);
    assert(!(local_rights & PACHA_FD_RIGHT_MAP_EXEC));
    assert(lpr_pacha_syscall3(PACHAOS_SYSCALL_MPROTECT,base,4096,5)==1);
    reset(); snapshot_error=-LPR_LINUX_EIO; allow_fallback=1; map_error=3;
    assert(lpr_linux_mmap(0,4096,1,2,3,0, 0)==-LPR_LINUX_ENOMEM);
    assert(!anon_maps && fallback_reads==1);
    reset(); file_bytes=0;
    assert(lpr_linux_mmap(0,4096,5,2,3,0, 0)==-LPR_LINUX_EINVAL && !snapshots && !anon_maps);
    reset();
    assert(lpr_linux_mmap(0,4096,5,2,3,UINT64_MAX-4095, 0)==-LPR_LINUX_EOVERFLOW && !snapshots && !anon_maps);
    reset(); expected_flags=FILED_FILE_VMO_EXEC; log_result=-19;
    assert(lpr_linux_mmap(0,4096,5,2,3,0, 0)>=4096 && exec_snapshots==1);
    assert(pins==unpins);
    puts("LPR_PRIVATE_MMAP_AUTHORITY_UNIT_PASS");
}

/* Request preparation and reply rendering are fixture phases within one
 * synchronous syscall. Their transaction tags never become caller FDs. */
int64_t lpr_pacha_syscall2(uint64_t nr, uint64_t a, uint64_t b)
{
    assert(nr != PACHAOS_SYSCALL_IPC_CALL);
    return fixture_prepare_request(nr, a, b);
}
int64_t lpr_pacha_syscall4(uint64_t nr, uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{
    return fixture_finish_request(nr, a, b, c, d);
}
int64_t lpr_ipc_message(uint64_t nr, uint64_t endpoint,
    uint64_t request, uint64_t reply, uint64_t timeout, uint64_t flags, int *sent)
{
    if (nr != PACHA_IPC_SYSCALL_CALL) return test_lpr_ipc_noncall(nr, endpoint, request, reply, timeout, flags, sent);

    assert(nr == PACHAOS_SYSCALL_IPC_CALL && request && reply);
    assert(timeout == UINT64_MAX && flags == PACHA_WAIT_UNINTERRUPTIBLE);
    *sent = 0;
    int64_t transaction = fixture_prepare_request(nr, endpoint, request);
    if (transaction < 16) return transaction;
    *sent = 1;
    int64_t status = fixture_finish_request(PACHAOS_SYSCALL_IPC_RECV_WAIT,
        (uint64_t)transaction, reply, timeout, flags);

    return status;
}
