/* SPDX-License-Identifier: GPL-2.0-only */
/* Exercise the actual i915 adapter's final-release and quarantine paths.
 * Driver wait/cache calls are modeled; no hardware completion is asserted. */
#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
struct list_head {struct list_head *next,*prev;};
#define LIST_HEAD(name) struct list_head name={&name,&name}
#define DEFINE_MUTEX(name) int name
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define list_entry(p,t,m) container_of(p,t,m)
#define list_for_each_entry_safe(pos,n,head,member) \
 for(pos=(head)->next!=(head)?list_entry((head)->next,struct mapped_bo,member):NULL, \
     n=pos && pos->member.next!=(head)?list_entry(pos->member.next,struct mapped_bo,member):NULL; \
     pos;pos=n,n=pos && pos->member.next!=(head)?list_entry(pos->member.next,struct mapped_bo,member):NULL)
static void list_del(struct list_head *p) {p->prev->next=p->next;p->next->prev=p->prev;}
static void list_add_tail(struct list_head *p,struct list_head *head) {p->next=head;p->prev=head->prev;head->prev->next=p;head->prev=p;}
static void mutex_lock(int *m) {assert(!*m);*m=1;}
static void mutex_unlock(int *m) {assert(*m);*m=0;}
struct page {int tag;};
struct drm_i915_gem_object {struct {void *resv;} base;};
struct kobox_cache_lease {int restored;};
#define DMA_RESV_USAGE_BOOKKEEP 7
#define MAX_SCHEDULE_TIMEOUT 999
static int wait_calls,restore_calls,destroy_calls,freed;
static long wait_result=1;
static int restore_error,destroy_error;
static long dma_resv_wait_timeout(void *r,int usage,bool intr,long timeout)
{assert(r && usage==7 && intr && timeout==999);++wait_calls;return wait_result;}
static int kobox_cache_lease_restore_wb(struct kobox_cache_lease *p)
{assert(p);++restore_calls;if(restore_error)return restore_error;p->restored=1;return 0;}
static int kobox_cache_lease_destroy(struct kobox_cache_lease **p)
{assert(p && *p && (*p)->restored);++destroy_calls;if(destroy_error)return destroy_error;free(*p);*p=NULL;return 0;}
static void kvfree(void *p) {assert(p);free(p);}
static void kfree(void *p) {assert(p);++freed;free(p);}
#include "i915_mapping_lifetime_actual.inc"
static struct drm_i915_gem_object object={.base={.resv=&object}};
static struct mapped_bo *make(size_t users)
{
    struct mapped_bo *bo=calloc(1,sizeof(*bo));assert(bo);
    bo->pages=calloc(3,sizeof(*bo->pages));bo->lease=calloc(1,sizeof(*bo->lease));assert(bo->pages && bo->lease);
    bo->object=&object;bo->users=users;bo->count=3;list_add_tail(&bo->link,&mapped_bos);return bo;
}
int main(void)
{
    struct mapped_bo *bo=make(2);void *first=bo,*last=bo;
    assert(!release(&first) && !first && bo->users==1 && !wait_calls && !restore_calls && !freed);
    restore_error=-EIO;
    assert(release(&last)==-EIO && last==bo && bo->users==1 && !freed && !destroy_calls);
    assert(quiesce()==-EBUSY && last==bo && !freed);
    restore_error=0;destroy_error=-EBUSY;
    assert(release(&last)==-EBUSY && last==bo && bo->users==1 && !freed);
    destroy_error=0;
    assert(!release(&last) && !last && freed==1 && mapped_bos.next==&mapped_bos);
    bo=make(0);restore_error=-EIO;
    assert(quiesce()==-EIO && !bo->users && freed==1);
    restore_error=0;assert(!quiesce() && freed==2 && mapped_bos.next==&mapped_bos);
    bo=make(1);last=bo;int before=restore_calls;
    wait_result=-EINTR;assert(release(&last)==-EINTR && last==bo && restore_calls==before && freed==2);
    wait_result=0;assert(release(&last)==-EBUSY && last==bo && restore_calls==before);
    wait_result=1;assert(!release(&last) && !last && freed==3);
    assert(release(NULL)==-EINVAL && release(&last)==-EINVAL);
    assert(!quiesce());
    puts("I915 mapping actual release/quarantine: last-user restore, failed restoration/owner retention, retry and GPU wait ordering PASS");
}
