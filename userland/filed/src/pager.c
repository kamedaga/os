/* SPDX-License-Identifier: MIT */
#include "filed/pager.h"
#include "filed/runtime.h"
#include "filed/native_thread.h"
#include "filed/manifest.h"
#include <pacha/syscall.h>
#include <pacha/ipc.h>
#include <kobox2/sha256.h>
#include <elf.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { pager_bucket_count = 8192, pager_base_window = 64 * 1024 };

struct filed_pager_record {
    struct filed_pager_record *next;
    uint64_t key, generation, owner_handle, client, credential;
    uint64_t file_offset, mapped_length, rx_length, last_window_end;
    int rx;
    uint32_t next_window, window_counts[4];
    uint64_t faults, pread_bytes;
    uint64_t resident_bytes;
    filed_rx_manifest_t *manifest;
    int skip_persistent_once;
};

struct filed_pager {
    filed_runtime_t *runtime;
    pacha_fd_t worker;
    filed_mutex_t records_lock;
    struct filed_pager_record *buckets[pager_bucket_count];
    uint64_t next_key;
    uint8_t scanner_hash[32];
    pacha_fd_t port_fd;
    uint64_t faults, supplies, pread_bytes, supplied_bytes;
    uint64_t windows[5];
};

static int scanner_code_hash(const filed_runtime_t *rt,uint8_t digest[32])
{
    if (!rt->executable_phdr || !rt->executable_phnum ||
        rt->executable_phnum>128) return -EINVAL;
    const Elf64_Phdr *phdr=(const void *)rt->executable_phdr;
    uintptr_t base=0;
    for (uint64_t i=0;i<rt->executable_phnum;++i)
        if (phdr[i].p_type==PT_PHDR) {
            base=rt->executable_phdr-phdr[i].p_vaddr;
            break;
        }
    if (!base) return -EINVAL;
    kb2_sha256_context_t hash;
    kb2_sha256_initialize(&hash);
    unsigned count=0;
    for (uint64_t i=0;i<rt->executable_phnum;++i) {
        if (phdr[i].p_type!=PT_LOAD || !(phdr[i].p_flags&PF_X)) continue;
        if (!phdr[i].p_filesz || phdr[i].p_memsz<phdr[i].p_filesz ||
            phdr[i].p_filesz>SIZE_MAX || phdr[i].p_vaddr>UINTPTR_MAX-base)
            return -EINVAL;
        /* Hash actual loaded executable code, including the scanner and
         * Zydis decoder. Other code changes can only invalidate more hits. */
        kb2_sha256_update(&hash,(const void *)(base+phdr[i].p_vaddr),
            (size_t)phdr[i].p_filesz);
        ++count;
    }
    if (!count) return -EINVAL;
    kb2_sha256_finish(&hash,digest);
    return 0;
}

static struct filed_pager_record **pager_slot(struct filed_pager *pager,uint64_t key)
{
    struct filed_pager_record **slot=&pager->buckets[key & (pager_bucket_count-1)];
    while (*slot && (*slot)->key!=key) slot=&(*slot)->next;
    return slot;
}

static int pager_request(filed_runtime_t *rt,const struct filed_pager_record *record,
    uint32_t opcode,uint64_t handle,void *data,size_t length,
    kb2_fs_response_t *reply)
{
    kb2_fs_request_t request={.opcode=opcode,.handle=handle};
    if (opcode==KB2_FILESYSTEM_OP_STATX) {
        request.flags=0x1000;
        request.mask=0x7ff;
    }
    return filed_storage_pager_request(rt,record->client,record->credential,
        &request,reply,data,length);
}

static void pager_close_owner_handle(filed_runtime_t *rt,uint64_t handle)
{
    kb2_fs_request_t close={.opcode=KB2_FILESYSTEM_OP_CLOSE,.handle=handle};
    kb2_fs_response_t reply;
    (void)filed_storage_pager_request(rt,1,1,&close,&reply,NULL,0);
}

static int pager_file_size(filed_runtime_t *rt,const struct filed_pager_record *record,
    uint64_t *size)
{
    uint8_t stat[KB2_FILESYSTEM_STATX_SIZE];
    kb2_fs_response_t reply;
    int status=pager_request(rt,record,KB2_FILESYSTEM_OP_STATX,
        record->owner_handle,stat,sizeof(stat),&reply);
    if (status) return status;
    if (reply.data_length!=sizeof(stat)) return -EPROTO;
    *size=0;
    for (unsigned i=0;i<8;++i)
        *size|=(uint64_t)stat[KB2_FILESYSTEM_STATX_SIZE_OFFSET+i]<<(i*8);
    return 0;
}

static uint64_t pager_le64(const uint8_t *bytes)
{
    uint64_t value=0;
    for (unsigned i=0;i<8;++i) value|=(uint64_t)bytes[i]<<(i*8);
    return value;
}
static uint32_t pager_le32(const uint8_t *bytes)
{
    uint32_t value=0;
    for (unsigned i=0;i<4;++i) value|=(uint32_t)bytes[i]<<(i*8);
    return value;
}
static int pager_identity(filed_runtime_t *rt,
    const struct filed_pager_record *record,
    uint8_t identity[KB2_FILESYSTEM_SNAPSHOT_STAT_SIZE])
{
    kb2_fs_request_t request={.opcode=KB2_FILESYSTEM_OP_STATX,
        .handle=record->owner_handle,.flags=0x1000,
        .mode=KB2_FILESYSTEM_SNAPSHOT_QUERY,.mask=0x7ff};
    kb2_fs_response_t response;
    int status=filed_storage_pager_request(rt,record->client,
        record->credential,&request,&response,identity,
        KB2_FILESYSTEM_SNAPSHOT_STAT_SIZE);
    if (status) return status;
    return response.data_length==KB2_FILESYSTEM_SNAPSHOT_STAT_SIZE ? 0 : -EPROTO;
}
static int pager_read_exact(filed_runtime_t *rt,
    struct filed_pager_record *record,uint64_t file_offset,
    uint8_t *destination,size_t length)
{
    size_t done=0;
    while (done<length) {
        size_t got=0;
        int status=filed_storage_pager_pread(rt,record->client,
            record->credential,record->owner_handle,file_offset+done,
            destination+done,length-done,&got);
        if (status) return status;
        if (!got) return -EIO;
        done+=got;
        __atomic_add_fetch(&record->pread_bytes,got,__ATOMIC_RELAXED);
        rt->pager->pread_bytes+=got;
    }
    return 0;
}

static int pager_manifest_refresh(struct filed_pager *pager,
    struct filed_pager_record *record)
{
    filed_runtime_t *rt=pager->runtime;
    for (;;) {
        uint8_t before[KB2_FILESYSTEM_SNAPSHOT_STAT_SIZE],
            after[KB2_FILESYSTEM_SNAPSHOT_STAT_SIZE],key[32];
        int status=pager_identity(rt,record,before);
        if (status) return status;
        uint64_t size=pager_le64(before+
            KB2_FILESYSTEM_SNAPSHOT_STAT_STATX_OFFSET+
            KB2_FILESYSTEM_STATX_SIZE_OFFSET);
        if (size<=record->file_offset) return -EIO;
        uint64_t available=size-record->file_offset;
        if (available>record->rx_length) available=record->rx_length;
        if (!available || available>SIZE_MAX) return -EIO;
        filed_rx_manifest_key(rt->bootstrap.root_uuid,before,
            record->file_offset,available,pager->scanner_hash,key);
        if (record->manifest && record->manifest->length==available &&
            !memcmp(record->manifest->key,key,32)) return 0;
        if (record->manifest) {
            filed_rx_manifest_free(record->manifest);
            free(record->manifest);record->manifest=NULL;
        }
        filed_rx_manifest_t *manifest=calloc(1,sizeof(*manifest));
        if (!manifest) return -ENOMEM;
        uint8_t *cached=NULL;size_t cached_size=0;
        int cache_status=record->skip_persistent_once ? -ENOENT :
            filed_manifest_storage_load(rt,key,&cached,&cached_size);
        if (!cache_status) {
            status=filed_rx_manifest_decode(cached,cached_size,key,
                pager->scanner_hash,manifest);
            free(cached);
            if (!status && manifest->file_offset==record->file_offset &&
                manifest->length==available) {
                record->manifest=manifest;
                return 0;
            }
            filed_rx_manifest_free(manifest);
            filed_manifest_storage_remove(rt,key);
        } else if (cache_status==-EINVAL || cache_status==-EPROTO ||
            cache_status==-EIO) filed_manifest_storage_remove(rt,key);
        record->skip_persistent_once=0;
        uint8_t *raw=malloc((size_t)available);
        if (!raw) { free(manifest);return -ENOMEM; }
        status=pager_read_exact(rt,record,record->file_offset,raw,
            (size_t)available);
        int after_status=pager_identity(rt,record,after);
        if (after_status) { free(raw);free(manifest);return after_status; }
        if (memcmp(before,after,sizeof(before))) {
            free(raw);free(manifest);continue;
        }
        if (status) { free(raw);free(manifest);return status; }
        /* With no trustworthy change cookie, two complete reads must agree
         * before the scanner records any site. The per-page guard remains
         * the final content check at supply time. */
        if (!(pager_le32(before+KB2_FILESYSTEM_SNAPSHOT_METADATA_FLAGS_OFFSET)&
                KB2_FILESYSTEM_SNAPSHOT_VALID)) {
            uint8_t *again=malloc((size_t)available);
            if (!again) { free(raw);free(manifest);return -ENOMEM; }
            status=pager_read_exact(rt,record,record->file_offset,again,
                (size_t)available);
            if (!status) status=pager_identity(rt,record,after);
            if (status) { free(again);free(raw);free(manifest);return status; }
            const int different=memcmp(before,after,sizeof(before)) ||
                memcmp(raw,again,(size_t)available);
            free(again);
            if (different) { free(raw);free(manifest);continue; }
        }
        status=filed_rx_manifest_build(raw,(size_t)available,
            record->file_offset,rt->bootstrap.root_uuid,before,
            pager->scanner_hash,manifest);
        free(raw);
        if (status) { free(manifest);return status; }
        status=pager_identity(rt,record,after);
        if (status) {
            filed_rx_manifest_free(manifest);free(manifest);return status;
        }
        if (memcmp(before,after,sizeof(before))) {
            filed_rx_manifest_free(manifest);free(manifest);continue;
        }
        uint8_t *encoded=NULL;size_t encoded_length=0;
        if (!filed_rx_manifest_encode(manifest,&encoded,&encoded_length)) {
            (void)filed_manifest_storage_save(rt,key,encoded,encoded_length);
            free(encoded);
        }
        record->manifest=manifest;
        return 0;
    }
}

static int pager_patch_rx(struct filed_pager *pager,
    struct filed_pager_record *record,uint64_t base,uint8_t *memory,
    uint64_t supply_bytes)
{
    int status=pager_manifest_refresh(pager,record);
    if (status) return status;
    filed_rx_manifest_t *manifest=record->manifest;
    if (base>=manifest->length) return -EAGAIN;
    const uint64_t first=base<FILED_RX_GUARD_BYTES ? 0 :
        base-FILED_RX_GUARD_BYTES;
    uint64_t last=base+supply_bytes+FILED_RX_GUARD_BYTES;
    if (last>manifest->length) last=manifest->length;
    uint8_t *guarded=malloc((size_t)(last-first));
    if (!guarded) return -ENOMEM;
    const size_t prefix=(size_t)(base-first);
    const size_t center=(size_t)(manifest->length-base<supply_bytes ?
        manifest->length-base : supply_bytes);
    if (prefix) status=pager_read_exact(pager->runtime,record,
        record->file_offset+first,guarded,prefix);
    if (!status) memcpy(guarded+prefix,memory,center);
    const size_t suffix=(size_t)(last-base-center);
    if (!status && suffix) status=pager_read_exact(pager->runtime,record,
        record->file_offset+base+center,guarded+prefix+center,suffix);
    /* A truncate or replacement during the guard reads is a retryable RX
     * identity change. Only a stable identity with a failed read is IO. */
    uint8_t after[KB2_FILESYSTEM_SNAPSHOT_STAT_SIZE];
    const int identity_status=pager_identity(pager->runtime,record,after);
    if (!identity_status &&
        memcmp(after,manifest->identity,sizeof(after))) status=-EAGAIN;
    else if (!status && identity_status) status=identity_status;
    if (!status) status=filed_rx_manifest_apply(manifest,base,memory,
        (size_t)supply_bytes,first,guarded,(size_t)(last-first));
    free(guarded);
    if (status==-EAGAIN) {
        filed_manifest_storage_remove(pager->runtime,manifest->key);
        filed_rx_manifest_free(manifest);free(manifest);
        record->manifest=NULL;
        record->skip_persistent_once=1;
    }
    return status;
}

static int pager_fail(struct filed_pager *pager,const struct pacha_pager_event *event,
    uint32_t reason)
{
    fprintf(stderr,"[filed-pager] FAIL key=%llu offset=%llu reason=%u\n",
        (unsigned long long)event->key,
        (unsigned long long)event->page_offset,reason);
    long status=pacha_syscall5(PACHA_PAGER_SYSCALL_FAIL,pager->port_fd,
        event->key,event->vmo_generation,event->request_id,reason);
    return status==0 ? 0 : -EIO;
}

static int pager_fill(struct filed_pager *pager,struct filed_pager_record *record,
    const struct pacha_pager_event *event)
{
    filed_runtime_t *rt=pager->runtime;
    const uint64_t base=event->page_offset & ~(uint64_t)(pager_base_window-1);
    if (event->page_offset>=record->mapped_length || base>=record->mapped_length) {
        fprintf(stderr,"[filed-pager] mapped EOF key=%llu event=%llu mapped=%llu\n",
            (unsigned long long)record->key,
            (unsigned long long)event->page_offset,
            (unsigned long long)record->mapped_length);
        return pager_fail(pager,event,PACHA_PAGER_FAIL_EOF_OR_TRUNCATE);
    }
    unsigned window=record->next_window ? record->next_window : pager_base_window;
    if (base!=record->last_window_end) window=pager_base_window;
    if (window>record->mapped_length-base) window=(unsigned)(record->mapped_length-base);
    window&=~4095u;
    if (!window) return pager_fail(pager,event,PACHA_PAGER_FAIL_EOF_OR_TRUNCATE);
    uint64_t size_before=0,size_after=0;
    int status=0;
    pacha_fd_t source=-1;
    uint8_t *memory=NULL;
    uint64_t supply_bytes=0;
    for (;;) {
        status=pager_file_size(rt,record,&size_before);
        if (status) break;
        const uint64_t file_start=record->file_offset+base;
        if (file_start>=size_before ||
            record->file_offset+event->page_offset>=((size_before+4095)&~UINT64_C(4095))) {
            fprintf(stderr,"[filed-pager] file EOF key=%llu event=%llu file_offset=%llu file_size=%llu\n",
                (unsigned long long)record->key,
                (unsigned long long)event->page_offset,
                (unsigned long long)record->file_offset,
                (unsigned long long)size_before);
            return pager_fail(pager,event,PACHA_PAGER_FAIL_EOF_OR_TRUNCATE);
        }
        uint64_t read_bytes=size_before-file_start;
        if (read_bytes>window) read_bytes=window;
        supply_bytes=(read_bytes+4095)&~UINT64_C(4095);
        if (supply_bytes>window) supply_bytes=window;
        const uint64_t rights=PACHA_FD_RIGHT_INSPECT|PACHA_FD_RIGHT_CLOSE|
            PACHA_FD_RIGHT_MAP_READ|PACHA_FD_RIGHT_MAP_WRITE;
        source=pacha_vmo_create(supply_bytes,rights,0);
        if (source<16) { status=-ENOMEM; break; }
        memory=pacha_mmap(source,supply_bytes,PACHA_PROT_READ|PACHA_PROT_WRITE,
            PACHA_MMAP_SHARED,0);
        if (!memory) { status=-ENOMEM; break; }
        uint64_t done=0;
        while (done<read_bytes) {
            size_t got=0;
            status=filed_storage_pager_pread(rt,record->client,record->credential,
                record->owner_handle,file_start+done,memory+done,
                (size_t)(read_bytes-done),&got);
            if (status || !got) { if (!status) status=-EIO; break; }
            done+=got;
        }
        if (!status) status=pager_file_size(rt,record,&size_after);
        if (status || size_before==size_after) {
            if (!status) {
                __atomic_add_fetch(&record->pread_bytes,done,__ATOMIC_RELAXED);
                pager->pread_bytes+=done;
                if (record->rx) status=pager_patch_rx(pager,record,base,
                    memory,supply_bytes);
            }
            if (status==-EAGAIN) {
                (void)pacha_munmap(memory,supply_bytes);
                (void)pacha_fd_close(source);
                memory=NULL;source=-1;status=0;
                continue;
            }
            break;
        }
        (void)pacha_munmap(memory,supply_bytes);
        (void)pacha_fd_close(source);
        memory=NULL;source=-1;
        /* Truncate/write raced the read. Retry until a stable size is seen. */
    }
    if (!status) {
        struct pacha_pager_supply supply={.key=event->key,
            .vmo_generation=event->vmo_generation,.request_id=event->request_id,
            .source_fd=(uint64_t)source,.source_offset=0,
            .target_offset=base,.length=supply_bytes};
        status=(int)pacha_syscall2(PACHA_PAGER_SYSCALL_SUPPLY,
            pager->port_fd,(uintptr_t)&supply);
        if (status==PACHA_SYSCALL_ERR_INVALID && supply_bytes>4096) {
            /* A previous out-of-order window may already cover part of
             * this one. Publish the requested page alone in that case. */
            supply.source_offset=event->page_offset-base;
            supply.target_offset=event->page_offset;
            supply.length=4096;
            status=(int)pacha_syscall2(PACHA_PAGER_SYSCALL_SUPPLY,
                pager->port_fd,(uintptr_t)&supply);
        }
        if (!status) {
            __atomic_add_fetch(&record->resident_bytes,supply.length,__ATOMIC_RELAXED);
            ++pager->supplies;
            pager->supplied_bytes+=supply.length;
            ++pager->windows[supply.length==4096 ? 0 :
                supply.length<=65536 ? 1 : supply.length<=131072 ? 2 :
                supply.length<=262144 ? 3 : 4];
            ++record->window_counts[supply.length<=65536 ? 0 :
                supply.length<=131072 ? 1 : supply.length<=262144 ? 2 : 3];
            record->last_window_end=supply.target_offset+supply.length;
            record->next_window=supply.length==4096 ? pager_base_window :
                window<512*1024 ? window*2 : window;
            if (record->next_window>512*1024) record->next_window=512*1024;
        }
    }
    /* Keep the staging mapping resident until SUPPLY has copied its pages.
     * Closing the mapping first can reclaim a sparse anonymous page even
     * while the VMO descriptor itself is still live. */
    if (memory && pacha_munmap(memory,supply_bytes) && !status) status=-EIO;
    if (source>=16) (void)pacha_fd_close(source);
    if (status) {
        fprintf(stderr,"[filed-pager] fill status=%d key=%llu handle=%llu client=%llu credential=%llu rx=%d base=%llu size_before=%llu size_after=%llu window=%u\n",
            status,(unsigned long long)record->key,
            (unsigned long long)record->owner_handle,
            (unsigned long long)record->client,
            (unsigned long long)record->credential,record->rx,
            (unsigned long long)base,(unsigned long long)size_before,
            (unsigned long long)size_after,window);
        return pager_fail(pager,event,PACHA_PAGER_FAIL_IO);
    }
    return 0;
}

static void pager_worker(void *context)
{
    struct filed_pager *pager=context;
    for (;;) {
        struct pacha_pager_event event={0};
        long status=pacha_syscall3(PACHA_PAGER_SYSCALL_WAIT,pager->port_fd,
            (uintptr_t)&event,0);
        if (status==PACHA_PAGER_WAIT_RETRY_STATUS) continue;
        if (status) break;
        filed_mutex_lock(&pager->records_lock);
        struct filed_pager_record **slot=pager_slot(pager,event.key);
        struct filed_pager_record *record=*slot;
        if (!record || record->generation!=event.vmo_generation) {
            filed_mutex_unlock(&pager->records_lock);
            continue;
        }
        if (event.kind==PACHA_PAGER_EVENT_VMO_RELEASE) {
            *slot=record->next;
            filed_mutex_unlock(&pager->records_lock);
            kb2_fs_response_t reply;
            (void)pager_request(pager->runtime,record,KB2_FILESYSTEM_OP_CLOSE,
                record->owner_handle,NULL,0,&reply);
            if (record->manifest) {
                filed_rx_manifest_free(record->manifest);
                free(record->manifest);
            }
            free(record);
            continue;
        }
        filed_mutex_unlock(&pager->records_lock);
        if (event.kind==PACHA_PAGER_EVENT_FAULT) {
            ++record->faults;
            ++pager->faults;
            if (pager_fill(pager,record,&event))
                fprintf(stderr,"[filed-pager] supply failed key=%llu offset=%llu\n",
                    (unsigned long long)event.key,
                    (unsigned long long)event.page_offset);
            if ((pager->faults&127)==0)
                fprintf(stderr,"[filed-pager] STATS faults=%llu supplies=%llu supplied=%llu pread=%llu w4=%llu w64=%llu w128=%llu w256=%llu w512=%llu\n",
                    (unsigned long long)pager->faults,
                    (unsigned long long)pager->supplies,
                    (unsigned long long)pager->supplied_bytes,
                    (unsigned long long)pager->pread_bytes,
                    (unsigned long long)pager->windows[0],
                    (unsigned long long)pager->windows[1],
                    (unsigned long long)pager->windows[2],
                    (unsigned long long)pager->windows[3],
                    (unsigned long long)pager->windows[4]);
        }
    }
}

int filed_pager_start(filed_runtime_t *rt)
{
    if (!rt || rt->pager) return -EINVAL;
    struct filed_pager *pager=calloc(1,sizeof(*pager));
    if (!pager) return -ENOMEM;
    if (scanner_code_hash(rt,pager->scanner_hash)) {
        free(pager);return -EINVAL;
    }
    pager->runtime=rt;
    pager->port_fd=(pacha_fd_t)pacha_syscall1(PACHA_PAGER_SYSCALL_PORT_CREATE,0);
    if (pager->port_fd<16) { free(pager); return -EIO; }
    rt->pager=pager;
    if (filed_native_thread_start(pager_worker,pager,
            &pager->worker)) {
        rt->pager=NULL;
        (void)pacha_fd_close(pager->port_fd);
        free(pager);
        return -EIO;
    }
    return 0;
}

int filed_pager_create(filed_runtime_t *rt,uint64_t handle,
    uint64_t file_offset,uint64_t mapped_length,uint64_t rx_length,int rx,
    const filed_storage_stat_t *stat,
    uint64_t rights,pacha_fd_t *vmo_fd,struct filed_pager_record **out)
{
    if (!rt || !rt->pager || !stat || !vmo_fd || !out ||
        (file_offset|mapped_length)&4095 || !mapped_length) {
        fprintf(stderr,"[filed-pager] create invalid runtime=%d pager=%d offset=%llu mapped=%llu\n",
            rt!=NULL,rt && rt->pager!=NULL,
            (unsigned long long)file_offset,(unsigned long long)mapped_length);
        return -EINVAL;
    }
    struct filed_pager *pager=rt->pager;
    /* The mapping may outlive its requesting client. TRANSFER_DUP puts the
     * retained handle into filed's permanent owner client (1), so client
     * unregister cannot revoke the worker's read authority. */
    kb2_fs_request_t dup={.opcode=KB2_FILESYSTEM_OP_TRANSFER_DUP,
        .handle=handle,.client=rt->actor ? rt->actor->id : 1,
        .credential_generation=rt->actor ?
            rt->actor->credential_generation : 1,.second_handle=1};
    kb2_fs_response_t reply;
    int status=filed_owner_control(rt,&dup,NULL,
        KB2_FILESYSTEM_REQUEST_SIZE,&reply);
    if (status) {
        fprintf(stderr,"[filed-pager] DUP failed handle=%llu status=%d\n",
            (unsigned long long)handle,status);
        return status;
    }
    struct filed_pager_record *record=calloc(1,sizeof(*record));
    if (!record) { pager_close_owner_handle(rt,reply.handle);return -ENOMEM; }
    /* PAGER_FAULT is a port right. The kernel rejects it on a pager VMO. */
    uint64_t owner_rights=rights|PACHA_FD_RIGHT_PAGER_ATTACH|
        PACHA_FD_RIGHT_MAP_WRITE;
    pacha_fd_t fd=pacha_vmo_create(mapped_length,owner_rights,
        PACHA_VMO_CREATE_PAGER);
    if (fd<16) { free(record);pager_close_owner_handle(rt,reply.handle);return -EIO; }
    record->key=++pager->next_key;
    record->owner_handle=reply.handle;
    record->client=1;
    record->credential=1;
    record->file_offset=file_offset;
    record->mapped_length=mapped_length;
    record->rx_length=rx_length;
    record->rx=rx;
    status=(int)pacha_pager_attach(pager->port_fd,fd,record->key,0,
        &record->generation);
    if (status) {
        (void)pacha_fd_close(fd);
        pager_close_owner_handle(rt,reply.handle);
        free(record);
        return -EIO;
    }
    filed_mutex_lock(&pager->records_lock);
    struct filed_pager_record **slot=pager_slot(pager,record->key);
    record->next=*slot;*slot=record;
    filed_mutex_unlock(&pager->records_lock);
    *vmo_fd=fd;*out=record;
    return 0;
}

uint64_t filed_pager_resident_bytes(const struct filed_pager_record *record)
{
    return record ? __atomic_load_n(&record->resident_bytes,__ATOMIC_RELAXED) : 0;
}
