/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "../src/snapshot_cache.c"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>

uint64_t filed_pager_resident_bytes(const struct filed_pager_record *record)
{
    return record ? __atomic_load_n((const uint64_t *)record,__ATOMIC_RELAXED) : 0;
}

static struct { unsigned references; } objects[16];
static struct { unsigned object;uint64_t rights;int live; } fds[8192];
static unsigned next_fd=16;
static int duplicate_error, allocation_error;
static struct pacha_fd_table_info table={256,4096,200};
static int query_error,grow_error;static unsigned grows;
int pacha_fd_table(uint64_t minimum,struct pacha_fd_table_info *out) {
    if (!minimum) { *out=table;return query_error; }
    ++grows;if (grow_error) return grow_error;
    table.free_slots+=minimum-table.capacity;table.capacity=minimum;*out=table;return 0;
}
void *__real_malloc(size_t);
void *__wrap_malloc(size_t n) { return allocation_error && !--allocation_error ? NULL : __real_malloc(n); }
static const uint64_t base=PACHA_FD_RIGHT_INSPECT|PACHA_FD_RIGHT_CLOSE|
    PACHA_FD_RIGHT_TRANSFER|PACHA_FD_RIGHT_DUP|PACHA_FD_RIGHT_MAP_READ;

long pacha_fd_fcntl(pacha_fd_t fd,uint64_t command,uint64_t minimum,uint64_t rights)
{
    assert(fd>=16 && fds[fd].live && command==PACHA_FD_FCNTL_DUP && minimum==16);
    assert(!(rights&~fds[fd].rights));
    if (duplicate_error) return duplicate_error;
    assert(next_fd<8192);
    fds[next_fd]=fds[fd];fds[next_fd].rights=rights;
    ++objects[fds[fd].object].references;
    return next_fd++;
}
int pacha_fd_close(pacha_fd_t fd)
{
    assert(fd>=16 && fds[fd].live);
    --objects[fds[fd].object].references;fds[fd].live=0;
    return 0;
}
static pacha_fd_t source(unsigned object)
{
    pacha_fd_t fd=(int)next_fd++;
    fds[fd].object=object;fds[fd].live=1;
    fds[fd].rights=base|PACHA_FD_RIGHT_MAP_EXEC|PACHA_FD_RIGHT_MAP_WRITE;
    ++objects[object].references;
    return fd;
}
static pthread_mutex_t dispatcher=PTHREAD_MUTEX_INITIALIZER;
static filed_snapshot_cache_t concurrent;
static filed_storage_stat_t stat={.inode=8,.filesystem=3,.size=16384,
    .inode_generation=19,.change_cookie=100,.snapshot_valid=1,.nlink=1};
static void *requests(void *unused)
{
    (void)unused;
    for (unsigned i=0;i<500;++i) {
        /* As in filed_runtime_serve, one dispatcher owns a complete lookup
         * and duplicate. Concurrent client producers cannot borrow an FD
         * across eviction or interleave an actor/credential assignment. */
        assert(!pthread_mutex_lock(&dispatcher));
        pacha_fd_t fd=-1;uint64_t offset=99;
        assert(filed_snapshot_cache_get(&concurrent,&stat,4096,4096,base,&fd,&offset)==1);
        assert(offset==4096 && fds[fd].rights==base);
        assert(!pacha_fd_close(fd));
        assert(!pthread_mutex_unlock(&dispatcher));
    }
    return NULL;
}
int main(void)
{
    filed_snapshot_cache_t cache={.limit=32768};
    filed_snapshot_cache_observe(&cache,1,&stat);
    pacha_fd_t a=source(1),b=source(2),fd=-1;uint64_t offset=99;
    filed_snapshot_cache_put(&cache,&stat,0,16384,a);
    assert(cache.count==1 && cache.bytes==16384 && objects[1].references==2);
    assert(filed_snapshot_cache_get(&cache,&stat,4096,4096,base,&fd,&offset)==1);
    assert(offset==4096 && fds[fd].object==1 && fds[fd].rights==base);
    assert(!pacha_fd_close(fd));
    assert(filed_snapshot_cache_get(&cache,&stat,0,4096,base|PACHA_FD_RIGHT_MAP_WRITE,&fd,&offset)==-EINVAL);
    assert(filed_snapshot_cache_get(&cache,&stat,0,4096,base|PACHA_FD_RIGHT_MAP_EXEC,&fd,&offset)==1);
    assert(!(fds[fd].rights&PACHA_FD_RIGHT_MAP_WRITE));pacha_fd_close(fd);
    assert(!filed_snapshot_cache_get(&cache,&stat,12288,8192,base,&fd,&offset));
    duplicate_error=-EMFILE;
    assert(filed_snapshot_cache_get(&cache,&stat,0,4096,base,&fd,&offset)==-EMFILE);
    filed_snapshot_cache_put(&cache,&stat,0,16384,b);
    assert(objects[2].references==1 && cache.count==1);
    duplicate_error=0;
    allocation_error=1;
    filed_snapshot_cache_put(&cache,&stat,0,16384,b);
    assert(objects[2].references==1 && cache.count==1);
    allocation_error=0;
    filed_storage_stat_t newer=stat;++newer.change_cookie;
    filed_snapshot_cache_observe(&cache,1,&newer);
    assert(!cache.count && objects[1].references==1);
    filed_snapshot_cache_put(&cache,&newer,0,16384,b);
    filed_snapshot_cache_put(&cache,&stat,0,16384,a);
    filed_storage_stat_t other=stat;++other.inode;
    pacha_fd_t c=source(3);
    filed_snapshot_cache_put(&cache,&other,0,16384,c);
    assert(cache.count==2 && cache.bytes==32768 && cache.evictions==1);
    assert(objects[2].references==1);
    newer=stat;newer.nlink=0;
    filed_snapshot_cache_observe(&cache,1,&newer);
    assert(cache.count==1 && objects[1].references==1);
    newer=other;newer.snapshot_valid=0;
    filed_snapshot_cache_observe(&cache,1,&newer);
    assert(!cache.count && !cache.bytes && objects[3].references==1);
    filed_snapshot_cache_put(&cache,&newer,0,16384,c);
    assert(!cache.count);
    filed_snapshot_cache_put(&cache,&stat,0,16384,a);
    newer=stat;++newer.inode_generation;
    filed_snapshot_cache_observe(&cache,1,&newer);assert(!cache.count);
    filed_snapshot_cache_put(&cache,&stat,0,16384,a);
    filed_snapshot_cache_observe(&cache,2,&other);assert(!cache.count);
    filed_storage_stat_t zero=stat;zero.inode_generation=0;zero.change_cookie=0;
    filed_snapshot_cache_put(&cache,&zero,0,16384,a);
    assert(filed_snapshot_cache_get(&cache,&zero,0,4096,base,&fd,&offset)==1);
    pacha_fd_close(fd);filed_snapshot_cache_clear(&cache);
    concurrent=(filed_snapshot_cache_t){.limit=16384};
    filed_snapshot_cache_put(&concurrent,&stat,0,16384,a);
    pthread_t threads[4];
    for (unsigned i=0;i<4;++i) assert(!pthread_create(&threads[i],NULL,requests,NULL));
    for (unsigned i=0;i<4;++i) assert(!pthread_join(threads[i],NULL));
    assert(concurrent.hits==2000);
    /* There is no clock/expiry API: idle ownership persists until bounded
     * LRU eviction, invalidation or explicit teardown. */
    assert(concurrent.count==1 && concurrent.bytes==16384);
    filed_snapshot_cache_clear(&concurrent);
    filed_snapshot_cache_clear(&cache);
    cache.limit=UINT64_C(1)<<30;
    for(unsigned i=0;i<4097;++i) {
        other.inode=i+100;filed_snapshot_cache_put(&cache,&other,0,4096,c);
    }
    assert(cache.count==4096 && cache.bytes==UINT64_C(4096)*4096);
    filed_snapshot_cache_clear(&cache);
    for (unsigned i=0;i<FILED_SNAPSHOT_BUCKETS;++i) assert(!cache.buckets[i]);
    /* Force a collision in the inode index. Invalidation must remove every
     * range of one identity while preserving its colliding neighbour. */
    filed_storage_stat_t collision=stat;
    do { ++collision.filesystem; } while (inode_bucket(collision.filesystem,collision.inode)!=inode_bucket(stat.filesystem,stat.inode));
    filed_snapshot_cache_put(&cache,&stat,0,4096,a);
    filed_snapshot_cache_put(&cache,&stat,8192,4096,a);
    filed_snapshot_cache_put(&cache,&collision,0,4096,b);
    assert(cache.count==3);
    assert(filed_snapshot_cache_get(&cache,&stat,8192,4096,base,&fd,&offset)==1);
    assert(fds[fd].object==1);pacha_fd_close(fd);
    newer=stat;newer.nlink=0;filed_snapshot_cache_observe(&cache,cache.generation,&newer);
    assert(cache.count==1 && !find_inode(&cache,&stat));
    assert(filed_snapshot_cache_get(&cache,&collision,0,4096,base,&fd,&offset)==1);
    assert(fds[fd].object==2);pacha_fd_close(fd);
    filed_snapshot_cache_clear(&cache);
    allocation_error=2; /* entry succeeds, inode group fails after DUP */
    filed_snapshot_cache_put(&cache,&stat,0,4096,a);
    assert(!cache.count && !find_inode(&cache,&stat) && objects[1].references==1);
    cache.limit=8192;
    filed_snapshot_cache_put(&cache,&stat,0,4096,a);
    filed_snapshot_cache_put(&cache,&collision,0,4096,b);
    assert(filed_snapshot_cache_get(&cache,&stat,0,4096,base,&fd,&offset)==1);
    pacha_fd_close(fd); /* hit promotes the global LRU across inode groups */
    filed_snapshot_cache_put(&cache,&other,0,4096,c);
    assert(!find_inode(&cache,&collision) && find_inode(&cache,&stat));
    filed_snapshot_cache_clear(&cache);
    cache.limit=4096;
    filed_snapshot_cache_put(&cache,&stat,0,4096,a);
    filed_snapshot_cache_put(&cache,&stat,4096,4096,a); /* evict its own last range */
    assert(cache.count==1 && find_inode(&cache,&stat)->first->offset==4096);
    filed_snapshot_cache_clear(&cache);
    filed_snapshot_cache_put(&cache,&stat,0,4096,a);
    table=(struct pacha_fd_table_info){256,4096,1};
    filed_snapshot_cache_reserve_fds(&cache);
    assert(grows==1 && table.capacity==512 && cache.count==1);
    table=(struct pacha_fd_table_info){512,512,255};
    filed_snapshot_cache_reserve_fds(&cache);
    assert(!cache.count && cache.limit && objects[1].references==1);
    filed_snapshot_cache_put(&cache,&stat,0,4096,a);
    table=(struct pacha_fd_table_info){512,4096,255};grow_error=-ENOMEM;
    filed_snapshot_cache_reserve_fds(&cache);
    assert(!cache.count && cache.limit);grow_error=0;
    filed_snapshot_cache_put(&cache,&stat,0,4096,a);query_error=-EIO;
    filed_snapshot_cache_reserve_fds(&cache);
    assert(!cache.count && !cache.limit && objects[1].references==1);
    cache.limit=4096;
    uint64_t pager_resident=0;
    filed_snapshot_cache_put_pager(&cache,&stat,0,1024*1024,a,
        (struct filed_pager_record *)&pager_resident,0);
    assert(cache.count==1 && filed_snapshot_cache_resident_bytes(&cache)==0);
    __atomic_store_n(&pager_resident,4096,__ATOMIC_RELAXED);
    assert(filed_snapshot_cache_resident_bytes(&cache)==4096);
    assert(filed_snapshot_cache_get(&cache,&stat,0,4096,base,&fd,&offset)==1);
    pacha_fd_close(fd);
    __atomic_store_n(&pager_resident,8192,__ATOMIC_RELAXED);
    assert(filed_snapshot_cache_get(&cache,&stat,0,4096,base,&fd,&offset)==0);
    assert(!cache.count && !cache.bytes);
    cache.limit=16384;
    filed_snapshot_cache_put_variant(&cache,&stat,0,4096,a,1);
    int pager_kind=-1;
    assert(!filed_snapshot_cache_get_variant(&cache,&stat,0,4096,base,0,
        &fd,&offset,&pager_kind));
    assert(filed_snapshot_cache_get_variant(&cache,&stat,0,4096,base,1,
        &fd,&offset,&pager_kind)==1 && pager_kind==0);
    pacha_fd_close(fd);filed_snapshot_cache_clear(&cache);
    pacha_fd_close(a);pacha_fd_close(b);pacha_fd_close(c);
    for (unsigned i=0;i<16;++i) assert(!objects[i].references);
    puts("snapshot cache: containment/hit, cookie/generation/unlink, hash collisions/range invalidation, LRU, rights, concurrent dispatch and failed DUP cleanup: PASS");
}
