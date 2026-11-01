#include <stdint.h>
int ph_notifications_save(uint64_t *mask){*mask=0;return 0;}
int ph_notifications_restore(uint64_t mask){(void)mask;return 0;}
#define main original_dma_unit_main
#include "kobox2_dma_unit.c"
#undef main
int main(void){
 char text[8192];size_t n;
 assert(ph_dma_latency_request("NET_PERF_TRACE_START",20,text,sizeof(text),&n)==0);
 int result=original_dma_unit_main();
 assert(ph_dma_latency_request("NET_PERF_TRACE_STOP",19,text,sizeof(text),&n)==0);
 assert(dl.total[0].n && dl.total[1].n && dl.total[2].n);
 assert(ph_dma_latency_request("NET_PERF_TRACE_DUMP",19,text,sizeof(text),&n)==1 && n<sizeof(text));
 printf("PASS instrumented DMA ownership suite: DERIVE=%llu CLOSE=%llu LOCK=%llu\n",(unsigned long long)dl.total[0].n,(unsigned long long)dl.total[1].n,(unsigned long long)dl.total[2].n);
 return result;
}
