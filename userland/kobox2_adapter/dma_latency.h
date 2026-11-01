/* SPDX-License-Identifier: MIT */
#ifndef PH_DMA_LATENCY_H
#define PH_DMA_LATENCY_H
/* Private diagnostic build; no native or Linux resource ABI is extended. */
int ph_dma_latency_request(const void *, size_t, void *, size_t, size_t *);
#ifdef PH_DMA_LATENCY_IMPLEMENTATION
#define DL_ROWS 64
#define DL_ACTIVE 32
#define DL_BINS 32
struct dl_key { uint64_t thread, fiber, device; unsigned op, pages, scatter, live, overlap; };
struct dl_stat { struct dl_key key; uint64_t n, failures, sum, min, max,
 pages_min,pages_max,live_min,live_max,migrated,hist[DL_BINS]; };
struct dl_call { struct dl_key key; uint64_t pages,live,start,end; unsigned used; };
static struct {
 atomic_uint lock; unsigned enabled,active,rows,cursor; uint64_t epoch,overflow,active_overflow;
 struct dl_call calls[DL_ACTIVE]; struct dl_stat total[3],row[DL_ROWS];
} dl;
static uint64_t dl_tsc(void) {
 unsigned a,d;__asm__ volatile("lfence;rdtsc;lfence":"=a"(a),"=d"(d)::"memory");
 return ((uint64_t)d<<32)|a;
}
/* Never hold this lock across a syscall. Mask notification-driven Linux entry
 * before acquiring it, so the same native thread cannot reenter its collector.
 * Its wait/update time lies outside the timed native call. */
static uint64_t dl_lock(void) {
 uint64_t mask;PH_OK(ph_notifications_save(&mask));
 while(atomic_exchange_explicit(&dl.lock,1,memory_order_acquire)) __asm__ volatile("pause");
 return mask;
}
static void dl_unlock(uint64_t mask) {
 atomic_store_explicit(&dl.lock,0,memory_order_release);PH_OK(ph_notifications_restore(mask));
}
static uint64_t dl_add(uint64_t a,uint64_t b) { return UINT64_MAX-a<b?UINT64_MAX:a+b; }
static unsigned dl_pages(uint64_t p) { return p==1?1:p==2?2:p==4?4:p==16?16:0; }
static unsigned dl_live(uint64_t n) { return n==0?0:n<100?1:n<500?2:3; }
static unsigned dl_bin(uint64_t ticks) { unsigned b=0;while(ticks>1 && b<DL_BINS-1){ticks>>=1;b++;}return b; }
static int dl_begin(unsigned op,uint64_t pages,unsigned scatter,uint64_t live,uint64_t device) {
 if(!__atomic_load_n(&dl.enabled,__ATOMIC_ACQUIRE))return -1;
 uint64_t mask=dl_lock();int slot=-1;
 if(!dl.enabled){dl_unlock(mask);return -1;}
 for(unsigned i=0;i<DL_ACTIVE;i++)if(!dl.calls[i].used){slot=(int)i;break;}
 if(slot<0){dl.active_overflow=dl_add(dl.active_overflow,1);dl_unlock(mask);return -1;}
 struct dl_call *c=&dl.calls[slot];memset(c,0,sizeof(*c));c->used=1;
 c->key=(struct dl_key){.thread=(uintptr_t)ph_current_task(),.fiber=(uintptr_t)ph_current_fiber(),
  .device=device,.op=op,.pages=dl_pages(pages),.scatter=scatter,.live=dl_live(live)};
 c->pages=pages;c->live=live;
 /* Actual TSC envelopes are published immediately around the native call,
  * after releasing this metadata lock and restoring notifications. */
 dl.active++;dl_unlock(mask);return slot;
}
static void dl_started(int slot,uint64_t stamp) {
 if(slot>=0)__atomic_store_n(&dl.calls[slot].start,stamp,__ATOMIC_RELEASE);
}
static void dl_finished(int slot,uint64_t stamp) {
 if(slot>=0)__atomic_store_n(&dl.calls[slot].end,stamp,__ATOMIC_RELEASE);
}
static void dl_update(struct dl_stat *s,const struct dl_call *c,uint64_t ticks,long result) {
 if(!s->n){s->min=s->max=ticks;s->pages_min=s->pages_max=c->pages;s->live_min=s->live_max=c->live;}
 s->n=dl_add(s->n,1);s->sum=dl_add(s->sum,ticks);s->hist[dl_bin(ticks)]=dl_add(s->hist[dl_bin(ticks)],1);
 if(ticks<s->min)s->min=ticks;
 if(ticks>s->max)s->max=ticks;
 if(c->pages<s->pages_min)s->pages_min=c->pages;
 if(c->pages>s->pages_max)s->pages_max=c->pages;
 if(c->live<s->live_min)s->live_min=c->live;
 if(c->live>s->live_max)s->live_max=c->live;
 if(result<0)s->failures=dl_add(s->failures,1);
 if(c->key.thread!=(uintptr_t)ph_current_task())s->migrated=dl_add(s->migrated,1);
}
static int dl_key_equal(const struct dl_key *a,const struct dl_key *b) {
 return a->thread==b->thread && a->fiber==b->fiber && a->device==b->device && a->op==b->op && a->pages==b->pages &&
 a->scatter==b->scatter && a->live==b->live && a->overlap==b->overlap;
}
static void dl_end(int slot,uint64_t ticks,long result,uint64_t live) {
 if(slot<0)return;
 uint64_t mask=dl_lock();struct dl_call *c=&dl.calls[slot];
 if(c->key.op<2)for(unsigned j=0;j<DL_ACTIVE;j++) {
  struct dl_call *a=&dl.calls[j];
  if(j==(unsigned)slot || !a->used || a->key.op>=2 || a->key.thread==c->key.thread)continue;
  uint64_t begin=__atomic_load_n(&a->start,__ATOMIC_ACQUIRE);
  uint64_t end=__atomic_load_n(&a->end,__ATOMIC_ACQUIRE);
  if(begin && c->start && begin<c->end && (!end || c->start<end)) {
   c->key.overlap|=1u<<a->key.op;a->key.overlap|=1u<<c->key.op;
  }
 }
 if(c->key.op==2){c->live=live;c->key.live=dl_live(live);}
 dl_update(&dl.total[c->key.op],c,ticks,result);
 unsigned i;for(i=0;i<dl.rows;i++)if(dl_key_equal(&dl.row[i].key,&c->key))break;
 if(i==dl.rows && dl.rows<DL_ROWS){dl.rows++;dl.row[i].key=c->key;}
 if(i<DL_ROWS)dl_update(&dl.row[i],c,ticks,result);else dl.overflow=dl_add(dl.overflow,1);
 c->used=0;dl.active--;dl_unlock(mask);
}
struct dl_text { char *p;size_t n,cap;unsigned bad; };
static void dl_s(struct dl_text *b,const char *s) { while(*s){if(b->n==b->cap){b->bad=1;return;}b->p[b->n++]=*s++;} }
static void dl_u(struct dl_text *b,uint64_t n) { char d[20];unsigned i=0;do{d[i++]=(char)('0'+n%10);n/=10;}while(n);while(i){char s[2]={d[--i],0};dl_s(b,s);} }
static void dl_field(struct dl_text *b,const char *name,uint64_t n) { dl_s(b,name);dl_u(b,n); }
static void dl_line(struct dl_text *b,const struct dl_stat *s,unsigned id,unsigned total) {
 dl_s(b,"dma ");dl_field(b,"total=",total);dl_field(b," id=",id);dl_field(b," op=",s->key.op);
 dl_field(b," pages_bucket=",s->key.pages);dl_field(b," scatter=",s->key.scatter);dl_field(b," live_bucket=",s->key.live);
 dl_field(b," thread=",s->key.thread);dl_field(b," fiber=",s->key.fiber);dl_field(b," device=",s->key.device);dl_field(b," overlap=",s->key.overlap);
 dl_field(b," n=",s->n);dl_field(b," fail=",s->failures);dl_field(b," sum=",s->sum);dl_field(b," min=",s->min);dl_field(b," max=",s->max);
 dl_field(b," pages_min=",s->pages_min);dl_field(b," pages_max=",s->pages_max);dl_field(b," live_min=",s->live_min);dl_field(b," live_max=",s->live_max);
 dl_field(b," migrated=",s->migrated);dl_s(b," hist=");for(unsigned i=0;i<DL_BINS;i++){if(i)dl_s(b,",");dl_u(b,s->hist[i]);}dl_s(b,"\n");
}
int ph_dma_latency_request(const void *request,size_t length,void *out,size_t capacity,size_t *received) {
 const char *r=request;unsigned start=length==20 && !memcmp(r,"NET_PERF_TRACE_START",20);
 unsigned stop=length==19 && !memcmp(r,"NET_PERF_TRACE_STOP",19);
 unsigned dump=length==19 && !memcmp(r,"NET_PERF_TRACE_DUMP",19);
 if(!start && !stop && !dump)return 0;
 uint64_t mask=dl_lock();
 if(start){
  if(dl.active){dl_unlock(mask);return -EBUSY;}
  memset(dl.total,0,sizeof(dl.total));memset(dl.row,0,sizeof(dl.row));dl.rows=dl.cursor=0;
  dl.overflow=dl.active_overflow=0;dl.epoch++;for(unsigned i=0;i<3;i++)dl.total[i].key.op=i;
  __atomic_store_n(&dl.enabled,1,__ATOMIC_RELEASE);dl_unlock(mask);return 0;
 }
 if(stop){__atomic_store_n(&dl.enabled,0,__ATOMIC_RELEASE);dl_unlock(mask);return 0;}
 if(dl.enabled || dl.active){dl_unlock(mask);return -EAGAIN;}
 struct dl_text b={.p=out,.cap=capacity};unsigned end=dl.cursor+3;
 if(end>dl.rows)end=dl.rows;
 dl_s(&b,"dma-lat ");dl_field(&b,"epoch=",dl.epoch);dl_field(&b," rows=",dl.rows);dl_field(&b," cursor=",dl.cursor);
 dl_field(&b," next=",end);dl_field(&b," done=",end==dl.rows);dl_field(&b," overflow=",dl.overflow);dl_field(&b," active_overflow=",dl.active_overflow);dl_s(&b,"\n");
 for(unsigned i=0;i<3;i++)dl_line(&b,&dl.total[i],i,1);
 for(unsigned i=dl.cursor;i<end;i++)dl_line(&b,&dl.row[i],i,0);
 if(b.bad){dl_unlock(mask);return -ENOSPC;}
 dl.cursor=end;*received=b.n;dl_unlock(mask);return 1;
}
#endif
#endif
