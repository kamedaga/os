/* SPDX-License-Identifier: MIT */
#include "filed/snapshot_cache.h"
#include "filed/pager.h"
#include <pacha/ipc.h>
#include <stdlib.h>

struct filed_snapshot_inode {
    struct filed_snapshot_inode *next;
    struct filed_snapshot_entry *first;
    uint64_t filesystem, inode;
};
struct filed_snapshot_entry {
    struct filed_snapshot_entry *previous, *next;
    struct filed_snapshot_entry *inode_previous, *inode_next;
    struct filed_snapshot_inode *owner;
    filed_storage_stat_t stat;
    uint64_t offset, length;
    pacha_fd_t fd;
    struct filed_pager_record *pager;
    int rx;
};
static uint64_t entry_charge(const struct filed_snapshot_entry *e)
{
    return e->pager ? filed_pager_resident_bytes(e->pager) : e->length;
}
static void refresh_bytes(filed_snapshot_cache_t *cache)
{
    uint64_t bytes=0;
    for (struct filed_snapshot_entry *e=cache->first;e;e=e->next)
        bytes+=entry_charge(e);
    cache->bytes=bytes;
}
/* Mix both identity components before masking; device numbers and sequential
 * inode numbers must not leave most of the bounded index unused. Buckets only
 * link inode groups, while ranges and global LRU order have independent links. */
static size_t inode_bucket(uint64_t filesystem, uint64_t inode)
{
    uint64_t key=inode ^ (filesystem*UINT64_C(0x9e3779b97f4a7c15));
    key=(key^(key>>30))*UINT64_C(0xbf58476d1ce4e5b9);
    key=(key^(key>>27))*UINT64_C(0x94d049bb133111eb);
    return (size_t)(key^(key>>31)) & (FILED_SNAPSHOT_BUCKETS-1);
}
static struct filed_snapshot_inode *find_inode(filed_snapshot_cache_t *cache,
    const filed_storage_stat_t *stat)
{
    for (struct filed_snapshot_inode *i=cache->buckets[inode_bucket(stat->filesystem,stat->inode)];i;i=i->next)
        if (i->filesystem==stat->filesystem && i->inode==stat->inode) return i;
    return NULL;
}
static void remove_entry(filed_snapshot_cache_t *cache, struct filed_snapshot_entry *e)
{
    if (e->previous) e->previous->next=e->next;
    else cache->first=e->next;
    if (e->next) e->next->previous=e->previous;
    else cache->last=e->previous;
    struct filed_snapshot_inode *owner=e->owner;
    if (e->inode_previous) e->inode_previous->inode_next=e->inode_next;
    else owner->first=e->inode_next;
    if (e->inode_next) e->inode_next->inode_previous=e->inode_previous;
    if (!owner->first) {
        struct filed_snapshot_inode **slot=&cache->buckets[inode_bucket(owner->filesystem,owner->inode)];
        while (*slot!=owner) slot=&(*slot)->next;
        *slot=owner->next;
        free(owner);
    }
    cache->bytes-=entry_charge(e);
    --cache->count;
    (void)pacha_fd_close(e->fd);
    free(e);
}
void filed_snapshot_cache_clear(filed_snapshot_cache_t *cache)
{
    refresh_bytes(cache);
    while (cache->last) remove_entry(cache,cache->last);
}
void filed_snapshot_cache_reserve_fds(filed_snapshot_cache_t *cache)
{
    refresh_bytes(cache);
    while (cache->last && cache->bytes>cache->limit) {
        ++cache->evictions;remove_entry(cache,cache->last);
    }
    enum { control_headroom=256 };
    struct pacha_fd_table_info info;
    /* Cache references must not consume the receive/control/exec headroom.
     * Keep a complete initial FD table available to existing workloads. Grow
     * through the existing FD_TABLE ABI before receiving another message;
     * on allocation pressure, surrender cache ownership instead of killing
     * filed or retaining references that starve its management channel. */
    if (pacha_fd_table(0,&info) || !info.capacity ||
        info.capacity>info.maximum || info.maximum>PACHA_THREAD_SELF_FD) {
        filed_snapshot_cache_clear(cache);cache->limit=0;return;
    }
    if (info.free_slots>=control_headroom) return;
    if (info.capacity<info.maximum) {
        uint64_t target=info.capacity>info.maximum-info.capacity ?
            info.maximum : info.capacity*2;
        struct pacha_fd_table_info grown;
        if (!pacha_fd_table(target,&grown)) info=grown;
    }
    while (cache->last && info.free_slots<control_headroom) {
        ++cache->evictions;remove_entry(cache,cache->last);++info.free_slots;
    }
    if (info.free_slots<control_headroom) cache->limit=0;
}
void filed_snapshot_cache_observe(filed_snapshot_cache_t *cache, uint64_t generation,
    const filed_storage_stat_t *stat)
{
    refresh_bytes(cache);
    if (cache->generation!=generation) {
        filed_snapshot_cache_clear(cache);
        cache->generation=generation;
    }
    struct filed_snapshot_inode *inode=find_inode(cache,stat);
    for (struct filed_snapshot_entry *e=inode ? inode->first : NULL,*next;e;e=next) {
        next=e->inode_next;
        if (!stat->snapshot_valid || !stat->nlink ||
            e->stat.inode_generation!=stat->inode_generation ||
            e->stat.change_cookie!=stat->change_cookie || e->stat.size!=stat->size)
            remove_entry(cache,e);
    }
}
int filed_snapshot_cache_get_variant(filed_snapshot_cache_t *cache,
    const filed_storage_stat_t *stat,uint64_t offset,uint64_t length,
    uint64_t rights,int rx,pacha_fd_t *fd,uint64_t *vmo_offset,int *pager)
{
    refresh_bytes(cache);
    while (cache->last && cache->bytes>cache->limit) {
        ++cache->evictions;remove_entry(cache,cache->last);
    }
    const uint64_t allowed=PACHA_FD_RIGHT_INSPECT|PACHA_FD_RIGHT_CLOSE|
        PACHA_FD_RIGHT_TRANSFER|PACHA_FD_RIGHT_DUP|PACHA_FD_RIGHT_MAP_READ|
        PACHA_FD_RIGHT_MAP_EXEC;
    const uint64_t required=allowed&~(uint64_t)PACHA_FD_RIGHT_MAP_EXEC;
    if (rights & ~allowed || (rights&required)!=required) return -22;
    if (!cache->limit || !stat->snapshot_valid || !stat->nlink) { ++cache->bypasses;return 0; }
    struct filed_snapshot_inode *inode=find_inode(cache,stat);
    for (struct filed_snapshot_entry *e=inode ? inode->first : NULL;e;e=e->inode_next) {
        if (e->rx!=rx || e->stat.inode_generation!=stat->inode_generation ||
            e->stat.change_cookie!=stat->change_cookie || e->stat.size!=stat->size ||
            offset<e->offset || offset-e->offset>e->length ||
            length>e->length-(offset-e->offset)) continue;
        long copy=pacha_fd_fcntl(e->fd,PACHA_FD_FCNTL_DUP,16,rights);
        if (copy<16) return copy<0 ? (int)copy : -5;
        if (e!=cache->first) {
            e->previous->next=e->next;
            if (e->next) e->next->previous=e->previous;
            else cache->last=e->previous;
            e->previous=NULL;e->next=cache->first;
            cache->first->previous=e;cache->first=e;
        }
        *fd=(pacha_fd_t)copy;*vmo_offset=offset-e->offset;
        if (pager) *pager=e->pager!=NULL;
        ++cache->hits;
        return 1;
    }
    ++cache->misses;
    return 0;
}
int filed_snapshot_cache_get(filed_snapshot_cache_t *cache,const filed_storage_stat_t *stat,
    uint64_t offset,uint64_t length,uint64_t rights,pacha_fd_t *fd,uint64_t *vmo_offset)
{
    return filed_snapshot_cache_get_variant(cache,stat,offset,length,rights,0,
        fd,vmo_offset,NULL);
}
static void cache_put(filed_snapshot_cache_t *cache,const filed_storage_stat_t *stat,
    uint64_t offset,uint64_t length,pacha_fd_t source,
    struct filed_pager_record *pager,int rx)
{
    refresh_bytes(cache);
    uint64_t charge=pager ? filed_pager_resident_bytes(pager) : length;
    if (!stat->snapshot_valid || !stat->nlink || !length || charge>cache->limit) return;
    struct filed_snapshot_entry *e=malloc(sizeof(*e));
    if (!e) return;
    const uint64_t rights=PACHA_FD_RIGHT_INSPECT|PACHA_FD_RIGHT_CLOSE|
        PACHA_FD_RIGHT_TRANSFER|PACHA_FD_RIGHT_DUP|PACHA_FD_RIGHT_MAP_READ|
        PACHA_FD_RIGHT_MAP_EXEC;
    long copy=pacha_fd_fcntl(source,PACHA_FD_FCNTL_DUP,16,rights);
    if (copy<16) { free(e);return; }
    /* Account the complete VMO, including zero EOF padding. A containment
     * hit does not charge a second copy. Bound metadata/FDs as well as RAM. */
    while (cache->last && (cache->bytes>cache->limit-charge || cache->count>=FILED_SNAPSHOT_MAX_ENTRIES)) {
        ++cache->evictions;remove_entry(cache,cache->last);
    }
    /* Eviction may have removed this inode's last range. Resolve its group
     * after eviction so no entry borrows a freed group. Failure to allocate
     * the index drops only our new reference, never the caller's snapshot. */
    struct filed_snapshot_inode *inode=find_inode(cache,stat);
    if (!inode) {
        inode=malloc(sizeof(*inode));
        if (!inode) { (void)pacha_fd_close((pacha_fd_t)copy);free(e);return; }
        size_t bucket=inode_bucket(stat->filesystem,stat->inode);
        *inode=(struct filed_snapshot_inode){.filesystem=stat->filesystem,
            .inode=stat->inode,.next=cache->buckets[bucket]};
        cache->buckets[bucket]=inode;
    }
    *e=(struct filed_snapshot_entry){.stat=*stat,.offset=offset,.length=length,
        .fd=(pacha_fd_t)copy,.pager=pager,.rx=rx,.next=cache->first,.owner=inode,.inode_next=inode->first};
    if (inode->first) inode->first->inode_previous=e;
    inode->first=e;
    if (cache->first) cache->first->previous=e;
    else cache->last=e;
    cache->first=e;cache->bytes+=charge;++cache->count;
}
void filed_snapshot_cache_put(filed_snapshot_cache_t *cache,const filed_storage_stat_t *stat,
    uint64_t offset,uint64_t length,pacha_fd_t source)
{
    cache_put(cache,stat,offset,length,source,NULL,0);
}
void filed_snapshot_cache_put_variant(filed_snapshot_cache_t *cache,
    const filed_storage_stat_t *stat,uint64_t offset,uint64_t length,
    pacha_fd_t source,int rx)
{
    cache_put(cache,stat,offset,length,source,NULL,rx);
}
void filed_snapshot_cache_put_pager(filed_snapshot_cache_t *cache,
    const filed_storage_stat_t *stat,uint64_t offset,uint64_t length,
    pacha_fd_t source,struct filed_pager_record *pager,int rx)
{
    cache_put(cache,stat,offset,length,source,pager,rx);
}
uint64_t filed_snapshot_cache_resident_bytes(filed_snapshot_cache_t *cache)
{
    refresh_bytes(cache);
    return cache->bytes;
}
