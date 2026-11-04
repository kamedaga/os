/* Execute production supply, accounting, pressure and balloon transitions. */
#include <sys/wait.h>
#include <sys/resource.h>
#include <unistd.h>
#include <signal.h>
#include "resident_linux_mock.h"
#include "../../../kobox2/linux-sandbox/kobox/memory/reporting.c"
static uint64_t fail_pfn=2048;
static int native_error=KOBOX_RESIDENT_ALLOC,discard_error;
static unsigned calls,largest,notifications;
static int populate(void *ctx,uint64_t first,uint64_t count) {
 (void)ctx;++calls;if(count>largest)largest=count;
 for(uint64_t i=first;i<first+count;i++) {
  if(i>=fail_pfn)return native_error;
  model_pages[i].resident=true;
 }
 return 0;
}
static int discard_pages(void *ctx,const struct kobox_linux_memory_range *r,size_t count) {
 (void)ctx;assert(count==1);assert(!model_pages[r->first_page].free);
 if(!discard_error)model_pages[r->first_page].resident=false;
 return discard_error;
}
static int wake(void *ctx){(void)ctx;++notifications;return 0;}
static void rejects_unadmitted(unsigned pfn) {
 pid_t child=fork();assert(child>=0);
 if(!child) {
  struct rlimit limit={0,0};setrlimit(RLIMIT_CORE,&limit);
  kobox_linux_resident_assert_page(&model_pages[pfn]);
  _exit(0);
 }
 int status;assert(waitpid(child,&status,0)==child);
 assert(WIFSIGNALED(status) && WTERMSIG(status)==SIGABRT);
}
int main(void) {
 assert(kobox_resident_initial_bytes(2ULL<<30,32<<20,64<<20)==(192ULL<<20));
 assert(kobox_resident_initial_bytes(160ULL<<20,32<<20,64<<20)==(160ULL<<20));
 assert(kobox_resident_initial_bytes(2ULL<<30,(128ULL<<20)+1,0)==(194ULL<<20));
 assert(!kobox_resident_initial_bytes(4097,0,0));
 assert(!kobox_resident_initial_bytes(1ULL<<30,2ULL<<30,0));
 assert(!kobox_resident_initial_bytes(1ULL<<30,0,UINT64_MAX));
 struct kobox_linux_memory_host_operations ops={.populate=populate,.discard=discard_pages};
 struct kobox_linux_memory_layout m={.operations=&ops,.initial_resident_size=64<<12,.ram_size=1025<<12};
 for(unsigned i=0;i<2048;i++)model_pages[i].reserved=true;
 for(unsigned i=0;i<64;i++){model_pages[i].resident=true;free_reserved_page(&model_pages[i]);}
 assert(!kobox_linux_reporting_start(&m));
 supply_run(NULL);assert(supply_cursor==320 && model_total==320 && largest==256);
 assert(admitted_end==320);
 kobox_linux_resident_assert_page(&model_pages[319]);
 rejects_unadmitted(320); /* Reserved, unpopulated tail. */
 model_pages[320].reserved=false;
 rejects_unadmitted(320); /* Missing reserved bit cannot bypass admission. */
 model_pages[320].reserved=true;
 /* Exhaust resources after a partial native batch. Only proven prefix publishes. */
 model_zone.free=0;fail_pfn=320;
 supply_run(NULL);assert(supply_cursor==320 && model_total==320 && model_pages[320].reserved);
 jiffies+=5000;fail_pfn=333;
 supply_run(NULL);assert(supply_cursor==333 && model_total==333);
 assert(model_pages[333].reserved && !model_pages[333].free);
 unsigned old=calls;supply_run(NULL);assert(calls==old); /* cooldown */
 jiffies+=5000;fail_pfn=2048;
 for(unsigned n=0;n<3;n++){model_zone.free=0;supply_run(NULL);}
 assert(supply_cursor==1025 && model_total==1025); /* final short batch */
 /* Restore accurate free counter after the simulated low watermark. */
 model_zone.free=0;for(unsigned i=0;i<1025;i++)model_zone.free+=model_pages[i].free;
 long total=model_total;
 assert(!kobox_linux_pressure_start(wake,NULL));
 assert(!kobox_linux_pressure_submit(8,100*NSEC_PER_MSEC));pressure_run(NULL);
 assert(pressure_result.reported_ok_pages==8 && notifications==1);
 assert(model_total==total-8 && !list_empty(&balloon));
 for(struct list_head *p=balloon.next;p!=&balloon;p=p->next){struct page *pg=(void *)((char *)p - offsetof(struct page,lru));assert(!pg->resident && !pg->free);}
 jiffies+=5000;supply_warm=true;fail_pfn=0;
 supply_run(NULL);assert(model_total==total-8 && !list_empty(&balloon));
 jiffies+=5000;fail_pfn=3;supply_warm=true;
 supply_run(NULL);assert(model_total==total-5 && !list_empty(&balloon));
 jiffies+=5000;fail_pfn=2048;supply_warm=true;
 supply_run(NULL);assert(model_total==total && list_empty(&balloon));
 /* Busy shared page stays held, and only a later populate can release it. */
 discard_error=-2;assert(balloon_inflate(1)==0 && model_total==total-1);
 discard_error=0;jiffies+=5000;supply_warm=true;supply_run(NULL);
 assert(model_total==total && list_empty(&balloon));
 discard_error=-3;assert(balloon_inflate(8)==0 && model_total==total-1);
 discard_error=0;jiffies+=5000;supply_warm=true;supply_run(NULL);
 assert(model_total==total && list_empty(&balloon));
 /* Spare remains resident even if native requests more. Driver-held DMA
  * pages do not appear in the mock free set and cannot enter balloon. */
 long actual_free=model_zone.free;
 model_zone.free=low_wmark_pages(&model_zone)+KOBOX_RESIDENT_BATCH-1;
 unsigned before_discard=calls;
 assert(!balloon_inflate(1024) && list_empty(&balloon));
 assert(calls==before_discard && model_total==total);
 model_zone.free=actual_free;
 /* Fatal MAP/INVALID are never interpreted as resource exhaustion. */
 uint64_t cursor=1025;fail_pfn=0;native_error=-4;
 assert(kobox_resident_supply(&m,&cursor,1026,resident_populate,resident_publish)==-4);
 assert(cursor==1025 && model_pages[1025].reserved);
 assert(!kobox_linux_resident_test_start() && resident_probe_work.delay==20000);
 long free_before_probe=model_zone.free;
 resident_probe_run(NULL);assert(!list_empty(&resident_probe_pages) && model_total==total);
 resident_probe_failed=true;resident_probe_run(NULL);
 assert(list_empty(&resident_probe_pages) && model_zone.free==free_before_probe);
 puts("resident: reserved/publication, batch tail, partial ALLOC, cooldown, pressure balloon, failed/successful deflate, busy/fatal PASS");
}
