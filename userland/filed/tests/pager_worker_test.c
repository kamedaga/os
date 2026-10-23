/* SPDX-License-Identifier: MIT */
#include "../src/pager.c"
#include <assert.h>

static uint64_t file_size=400*1024;
static uint8_t file_byte=0x5a;
static int rx_mode;
static uint8_t rx_file[400*1024];
static uint8_t *saved_manifest;
static size_t saved_manifest_length;
static unsigned save_count,remove_count;
static uint8_t *source_pages;
static pacha_fd_t source_fd=16;
static uint64_t supplied[8];
static unsigned supply_count,fail_count,close_count,wait_count;
static struct pacha_pager_event release_event;

pacha_fd_t pacha_vmo_create(uint64_t size,uint64_t rights,uint32_t flags)
{
    assert(size && size<=PACHA_PAGER_MAX_SUPPLY_BYTES && !flags);
    assert((rights&PACHA_FD_RIGHT_MAP_WRITE)!=0 && !source_pages);
    source_pages=calloc(1,(size_t)size);
    assert(source_pages);
    return source_fd++;
}
void *pacha_mmap(pacha_fd_t fd,uint64_t size,uint64_t prot,uint64_t flags,uint64_t offset)
{
    (void)size;(void)prot;(void)flags;(void)offset;
    assert(fd==source_fd-1 && source_pages);
    return source_pages;
}
int pacha_munmap(void *address,uint64_t size)
{
    (void)size;
    assert(address==source_pages);
    return 0;
}
int pacha_fd_close(pacha_fd_t fd)
{
    assert(fd==source_fd-1 && source_pages);
    free(source_pages);source_pages=NULL;
    return 0;
}
long pacha_syscall2(uint64_t nr,uint64_t a0,uint64_t a1)
{
    if (nr==PACHA_RUNTIME_SYSCALL_FUTEX_WAKE) return 0;
    assert(nr==PACHA_PAGER_SYSCALL_SUPPLY && a0==50);
    const struct pacha_pager_supply *s=(const void *)(uintptr_t)a1;
    assert(s->source_fd==(uint64_t)(source_fd-1));
    assert(s->length && s->length<=PACHA_PAGER_MAX_SUPPLY_BYTES);
    if (rx_mode) {
        assert(source_pages[0]==0x90);
        assert(source_pages[100]==0xff && source_pages[101]==0xd0);
    } else assert(source_pages[0]==file_byte);
    supplied[supply_count++]=s->length;
    return 0;
}
long pacha_syscall3(uint64_t nr,uint64_t a0,uint64_t a1,uint64_t a2)
{
    assert(nr==PACHA_PAGER_SYSCALL_WAIT && a0==50 && !a2);
    if (wait_count++) return PACHA_SYSCALL_ERR_CLOSED;
    *(struct pacha_pager_event *)(uintptr_t)a1=release_event;
    return 0;
}
long pacha_syscall5(uint64_t nr,uint64_t a0,uint64_t a1,uint64_t a2,
    uint64_t a3,uint64_t a4)
{
    (void)a1;(void)a2;(void)a3;
    assert(nr==PACHA_PAGER_SYSCALL_FAIL && a0==50 &&
        a4==PACHA_PAGER_FAIL_EOF_OR_TRUNCATE);
    ++fail_count;
    return 0;
}
long pacha_syscall1(uint64_t nr,uint64_t a0)
{ (void)nr;(void)a0;return 50; }
long pacha_syscall4(uint64_t nr,uint64_t a0,uint64_t a1,uint64_t a2,
    uint64_t a3)
{ (void)nr;(void)a0;(void)a1;(void)a2;(void)a3;assert(0);return 1; }
long pacha_pager_attach(uint64_t port,uint64_t fd,uint64_t key,
    uint64_t flags,uint64_t *generation)
{ (void)port;(void)fd;(void)key;(void)flags;*generation=3;return 0; }
int filed_native_thread_start(void (*entry)(void *),void *context,pacha_fd_t *fd)
{ (void)entry;(void)context;*fd=51;return 0; }
int filed_storage_request(filed_runtime_t *rt,kb2_fs_request_t *request,
    const void *tail,size_t tail_size,kb2_fs_response_t *response,
    void *data,size_t capacity)
{
    (void)rt;(void)tail;(void)tail_size;(void)data;(void)capacity;
    assert(request->opcode==KB2_FILESYSTEM_OP_DUP);
    response->handle=11;
    return 0;
}
int filed_owner_control(filed_runtime_t *rt,kb2_fs_request_t *request,
    const uint8_t *tail,size_t size,kb2_fs_response_t *response)
{
    (void)rt;(void)tail;
    assert(size==KB2_FILESYSTEM_REQUEST_SIZE);
    assert(request->opcode==KB2_FILESYSTEM_OP_TRANSFER_DUP);
    assert(request->second_handle==1);
    response->handle=11;
    return 0;
}
int filed_storage_close(filed_runtime_t *rt,uint64_t handle)
{ (void)rt;(void)handle;return 0; }
int filed_storage_pager_request(filed_runtime_t *rt,uint64_t client,
    uint64_t credential,kb2_fs_request_t *request,kb2_fs_response_t *response,
    void *data,size_t capacity)
{
    (void)rt;(void)client;(void)credential;(void)capacity;
    if (request->opcode==KB2_FILESYSTEM_OP_CLOSE) {
        ++close_count;return 0;
    }
    assert(request->opcode==KB2_FILESYSTEM_OP_STATX);
    uint8_t *stat=data;
    size_t stat_size=request->mode==KB2_FILESYSTEM_SNAPSHOT_QUERY ?
        KB2_FILESYSTEM_SNAPSHOT_STAT_SIZE : KB2_FILESYSTEM_STATX_SIZE;
    size_t stat_offset=request->mode==KB2_FILESYSTEM_SNAPSHOT_QUERY ?
        KB2_FILESYSTEM_SNAPSHOT_STAT_STATX_OFFSET : 0;
    memset(stat,0,stat_size);
    if (stat_offset) stat[KB2_FILESYSTEM_SNAPSHOT_METADATA_FLAGS_OFFSET]=
        KB2_FILESYSTEM_SNAPSHOT_VALID;
    for (unsigned i=0;i<8;++i)
        stat[stat_offset+KB2_FILESYSTEM_STATX_SIZE_OFFSET+i]=
            (uint8_t)(file_size>>(i*8));
    response->data_length=stat_size;
    return 0;
}
int filed_storage_pager_pread(filed_runtime_t *rt,uint64_t client,
    uint64_t credential,uint64_t handle,uint64_t offset,void *data,
    size_t length,size_t *got)
{
    (void)rt;(void)client;(void)credential;(void)handle;
    assert(offset+length<=file_size);
    if (rx_mode) memcpy(data,rx_file+offset,length);
    else memset(data,file_byte,length);
    *got=length;
    return 0;
}
int filed_manifest_storage_load(filed_runtime_t *rt,const uint8_t key[32],
    uint8_t **out,size_t *length)
{
    (void)rt;(void)key;
    if (!saved_manifest) return -ENOENT;
    *out=malloc(saved_manifest_length);
    assert(*out);
    memcpy(*out,saved_manifest,saved_manifest_length);
    *length=saved_manifest_length;
    return 0;
}
int filed_manifest_storage_save(filed_runtime_t *rt,const uint8_t key[32],
    const uint8_t *bytes,size_t length)
{
    (void)rt;(void)key;
    free(saved_manifest);
    saved_manifest=malloc(length);
    assert(saved_manifest);
    memcpy(saved_manifest,bytes,length);
    saved_manifest_length=length;
    ++save_count;
    return 0;
}
void filed_manifest_storage_remove(filed_runtime_t *rt,const uint8_t key[32])
{
    (void)rt;(void)key;
    free(saved_manifest);saved_manifest=NULL;saved_manifest_length=0;
    ++remove_count;
}

int main(void)
{
    filed_runtime_t rt={0};
    struct filed_pager pager={.runtime=&rt,.port_fd=50};
    rt.pager=&pager;
    struct filed_pager_record record={.key=7,.generation=3,
        .owner_handle=11,.client=42,.credential=1,
        .mapped_length=512*1024};
    struct pacha_pager_event event={.kind=PACHA_PAGER_EVENT_FAULT,
        .key=7,.vmo_generation=3,.request_id=1,.page_offset=0};
    assert(!pager_fill(&pager,&record,&event));
    assert(supplied[0]==64*1024 && record.next_window==128*1024);
    event.page_offset=64*1024;event.request_id=2;
    assert(!pager_fill(&pager,&record,&event));
    assert(supplied[1]==128*1024 && record.next_window==256*1024);
    file_byte=0x6b; /* A later non-RX fault sees the current file contents. */
    event.page_offset=320*1024;event.request_id=3;
    assert(!pager_fill(&pager,&record,&event));
    assert(supplied[2]==64*1024 && record.next_window==128*1024);
    event.page_offset=448*1024;event.request_id=4;
    assert(!pager_fill(&pager,&record,&event));
    assert(fail_count==1 && supply_count==3);
    assert(record.pread_bytes==256*1024 &&
        filed_pager_resident_bytes(&record)==256*1024);
    struct filed_pager_record *owned=malloc(sizeof(*owned));
    assert(owned);
    *owned=record;
    pager.buckets[7 & (pager_bucket_count-1)]=owned;
    release_event=(struct pacha_pager_event){.kind=PACHA_PAGER_EVENT_VMO_RELEASE,
        .key=7,.vmo_generation=3};
    pager_worker(&pager);
    assert(close_count==1 && !pager.buckets[7 & (pager_bucket_count-1)]);
    rx_mode=1;
    memset(rx_file,0x90,sizeof(rx_file));
    rx_file[100]=0x0f;rx_file[101]=0x05;
    rx_file[64*1024+100]=0x0f;rx_file[64*1024+101]=0x05;
    struct filed_pager_record *rx=calloc(1,sizeof(*rx));
    assert(rx);
    *rx=(struct filed_pager_record){.key=9,.generation=3,
        .owner_handle=11,.client=42,.credential=1,
        .mapped_length=512*1024,.rx_length=file_size,.rx=1};
    event=(struct pacha_pager_event){.kind=PACHA_PAGER_EVENT_FAULT,
        .key=9,.vmo_generation=3,.request_id=5,.page_offset=0};
    assert(!pager_fill(&pager,rx,&event));
    assert(save_count==1 && rx->manifest);
    /* Simulate same-cookie content replacement in a not-yet-supplied page.
     * The guard digest must reject the old manifest and rescan. */
    rx_file[64*1024+200]=0x91;
    event.request_id=6;event.page_offset=64*1024;
    assert(!pager_fill(&pager,rx,&event));
    assert(remove_count==1 && save_count==2);
    pager.buckets[9 & (pager_bucket_count-1)]=rx;
    release_event=(struct pacha_pager_event){.kind=PACHA_PAGER_EVENT_VMO_RELEASE,
        .key=9,.vmo_generation=3};
    wait_count=0;
    pager_worker(&pager);
    assert(close_count==2);
    free(saved_manifest);
    puts("pager worker: window growth, EOF, resident bytes, release close: PASS");
}
