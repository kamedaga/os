#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type"
#define main previous_host_main
#include "kobox2_net_rx_unit.c"
#undef main
#pragma GCC diagnostic pop
#include <stdarg.h>
int scnprintf(char *s,size_t n,const char *format,...)
{va_list ap;va_start(ap,format);int z=vsnprintf(s,n,format,ap);va_end(ap);return z<0?z:(size_t)z>=n?(n?(int)n-1:0):z;}
int main(void){int z=kobox_net_rx_fixture();if(z)fprintf(stderr,"PERF fixture line=%d\n",z);assert(!z);puts("PASS bounded core ring: IRQ hint, RX key/TSC order, drain, capacity, stop-before-dump, reset");}
