#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#define PH_OK(x) assert((x)==0)
static uintptr_t thread=1;
static void *ph_current_task(void){return (void*)thread;}
static void *ph_current_fiber(void){return (void*)(thread+100);}
static int ph_notifications_save(uint64_t *mask){*mask=0;return 0;}
static int ph_notifications_restore(uint64_t mask){(void)mask;return 0;}
#define PH_DMA_LATENCY_IMPLEMENTATION
#include "dma_latency.h"
#define dl_begin(op,p,s,l) dl_begin((op),(p),(s),(l),1)
static int request(const char *op,char *out,size_t cap,size_t *n){return ph_dma_latency_request(op,strlen(op),out,cap,n);}
int main(void){
 char out[8192];size_t n;assert(dl_tsc()>0);assert(dl_begin(0,1,0,0)==-1);
 assert(request("NET_PERF_TRACE_START",out,sizeof(out),&n)==0);
 int a=dl_begin(0,1,0,100);dl_started(a,100);thread=2;int b=dl_begin(1,4,1,500);dl_started(b,200);
 thread=1;dl_finished(a,1100);dl_end(a,1000,0,0);thread=2;dl_finished(b,5200);dl_end(b,5000,-1,0);
 assert(dl.total[0].n==1 && dl.total[1].failures==1);
 assert(dl.row[0].key.overlap==2 && dl.row[1].key.overlap==1);
 assert(dl.row[0].key.live==2 && dl.row[1].key.live==3);
 assert(request("NET_PERF_TRACE_STOP",out,sizeof(out),&n)==0);
 assert(request("NET_PERF_TRACE_DUMP",out,1,&n)==-ENOSPC && dl.cursor==0);
 assert(request("NET_PERF_TRACE_DUMP",out,sizeof(out),&n)==1 && n<sizeof(out));
 out[n]=0;assert(strstr(out,"done=1") && strstr(out,"overlap=2"));
 assert(request("NET_PERF_TRACE_START",out,sizeof(out),&n)==0);
 thread=1;a=dl_begin(0,1,0,0);thread=2;b=dl_begin(1,1,0,0);
 dl_started(b,200);dl_finished(b,300);dl_end(b,100,0,0);
 thread=1;dl_started(a,400);dl_finished(a,500);dl_end(a,100,0,0);
 assert(dl.row[0].key.overlap==0 && dl.row[1].key.overlap==0);
 assert(request("NET_PERF_TRACE_START",out,sizeof(out),&n)==0);
 a=dl_begin(0,2,0,0);assert(request("NET_PERF_TRACE_STOP",out,sizeof(out),&n)==0);
 assert(request("NET_PERF_TRACE_DUMP",out,sizeof(out),&n)==-EAGAIN);
 assert(request("NET_PERF_TRACE_START",out,sizeof(out),&n)==-EBUSY);dl_end(a,0,-2,0);
 assert(request("NET_PERF_TRACE_DUMP",out,sizeof(out),&n)==1);
 assert(request("NET_PERF_TRACE_START",out,sizeof(out),&n)==0);
 for(unsigned i=0;i<70;i++){thread=i+1;a=dl_begin(0,16,0,100);dl_end(a,1ULL<<25,0,0);}
 assert(dl.rows==64 && dl.overflow==6 && dl.total[0].n==70);
 assert(request("NET_PERF_TRACE_STOP",out,sizeof(out),&n)==0);
 unsigned pages=0;do {assert(request("NET_PERF_TRACE_DUMP",out,sizeof(out),&n)==1);assert(n<sizeof(out));pages++;}while(dl.cursor<dl.rows);
 assert(pages==22);
 assert(dl_add(UINT64_MAX,1)==UINT64_MAX && dl_bin(UINT64_MAX)==31);
 assert(request("NET_PERF_TRACE_START",out,sizeof(out),&n)==0);
 int active[32];for(int i=0;i<32;i++)active[i]=dl_begin(0,1,0,0);
 assert(dl_begin(0,1,0,0)==-1 && dl.active_overflow==1);
 for(int i=0;i<32;i++){dl_end(active[i],42,0,0);}
 assert(!dl.active);
 /* Max-width saturation still fits the six-row page bound. */
 for(unsigned i=0;i<3;i++)memset(&dl.total[i],255,sizeof(dl.total[i]));
 for(unsigned i=0;i<3;i++)memset(&dl.row[i],255,sizeof(dl.row[i]));
 dl.rows=3;dl.cursor=0;assert(request("NET_PERF_TRACE_STOP",out,sizeof(out),&n)==0);
 assert(request("NET_PERF_TRACE_DUMP",out,sizeof(out),&n)==1 && n<8192);
 printf("PASS latency: overlap both ends, failure, pages/live bins, stop drain, pagination, overflow, saturation; worst dump %zu bytes\n",n);
}
