/* Minimal allocator/workqueue model for executing the actual hosted worker. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#define EINVAL 22
#define EOPNOTSUPP 95
#define EBUSY 16
#define EAGAIN 11
#include <stdarg.h>
#include <string.h>
typedef uint64_t u64;
#define __printf(a,b)
#define PAGE_SHIFT 12
#define NSEC_PER_MSEC 1000000UL
#define min_t(t,a,b) ((t)(a)<(t)(b)?(t)(a):(t)(b))
#define DEFINE_MUTEX(n) int n
#define mutex_lock(n) ((void)(n))
#define mutex_unlock(n) ((void)(n))
struct list_head {struct list_head *next,*prev;};
#define LIST_HEAD(n) struct list_head n={&n,&n}
#define list_empty(h) ((h)->next==(h))
#define list_first_entry(h,t,m) ((t *)((char *)(h)->next - offsetof(t,m)))
static void list_add_tail(struct list_head *p,struct list_head *h) {p->next=h;p->prev=h->prev;h->prev->next=p;h->prev=p;}
static void list_del_init(struct list_head *p) {p->prev->next=p->next;p->next->prev=p->prev;p->prev=p->next=p;}
struct page {struct list_head lru; bool reserved,free,resident;};
static struct page model_pages[2048];
struct zone {long managed,free;};
static struct zone model_zone;
static long model_total;
#define for_each_populated_zone(z) for(z=&model_zone;z;z=NULL)
#define NR_FREE_PAGES 0
#define zone_page_state(z,x) ((z)->free)
#define low_wmark_pages(z) 16L
#define GFP_KERNEL 0
#define GFP_NOWAIT 0
#define __GFP_IO 1
#define __GFP_FS 2
#define __GFP_NORETRY 4
#define __GFP_NOWARN 8
#define pfn_to_page(p) (&model_pages[p])
#define page_to_pfn(p) ((uint64_t)((p)-model_pages))
#define PageReserved(p) ((p)->reserved)
static void adjust_managed_page_count(struct page *p,long n) {(void)p;model_zone.managed+=n;model_total+=n;}
static void __free_page(struct page *p) {assert(!p->free && p->resident);p->free=true;model_zone.free++;}
static void free_reserved_page(struct page *p) {assert(p->reserved && p->resident);p->reserved=false;__free_page(p);adjust_managed_page_count(p,1);}
static struct page *alloc_page(unsigned flags) {(void)flags;for(unsigned i=0;i<2048;i++)if(model_pages[i].free){model_pages[i].free=false;model_zone.free--;return &model_pages[i];}return NULL;}
static void setup_per_zone_wmarks(void) {}
static unsigned long jiffies;
#define msecs_to_jiffies(n) (n)
#define time_before(a,b) ((long)((a)-(b))<0)
static u64 ktime_get_ns(void) {return jiffies*NSEC_PER_MSEC;}
#define cond_resched() ((void)0)
struct work_struct {int unused;};
struct delayed_work {void (*fn)(struct work_struct *);unsigned delay;};
#define INIT_DELAYED_WORK(w,f) ((w)->fn=(f))
#define system_unbound_wq NULL
static void queue_delayed_work(void *q,struct delayed_work *w,unsigned d){(void)q;w->delay=d;}
#define cancel_delayed_work_sync(w) ((void)(w))
typedef struct {long v;} atomic_t;
typedef atomic_t atomic64_t;
#define ATOMIC_INIT(n) {n}
#define ATOMIC64_INIT(n) {n}
#define atomic64_read(p) ((p)->v)
#define atomic64_add(n,p) ((p)->v+=(n))
#define atomic_set(p,n) ((p)->v=(n))
#define atomic_set_release atomic_set
#define atomic_read_acquire(p) ((p)->v)
static long atomic_cmpxchg(atomic_t *p,long old,long n){long v=p->v;if(v==old)p->v=n;return v;}
#define numa_node_id() 0
#define node_zonelist(a,b) NULL
static unsigned long try_to_free_pages(void *a,int b,int c,void *d){(void)a;(void)b;(void)c;(void)d;return 0;}
#define in_atomic() false
#define irqs_disabled() false
#define might_sleep() ((void)0)
#define panic(...) abort()
void kobox_linux_boot_diagnostic(const char *format,...) {(void)format;}

#define READ_ONCE(x) (x)
#define EXPORT_SYMBOL_GPL(x)

#define WRITE_ONCE(x,v) ((x)=(v))
