#include <assert.h>
#include <stdio.h>
#include "../kobox2/linux-sandbox/kobox/boot/wifi_service.h"
int main(void)
{
 assert(!strcmp(kobox_wifi_perf_ps_request("NET_PERF_PS_OFF",15),"SET ps 0"));
 assert(!strcmp(kobox_wifi_perf_ps_request("NET_PERF_PS_ON",14),"SET ps 1"));
 assert(!kobox_wifi_perf_ps_request(NULL,15));
 assert(!kobox_wifi_perf_ps_request("NET_PERF_PS_OFF\n",16));
 assert(!kobox_wifi_perf_ps_request("NET_PERF_PS_OFF_MORE",20));
 char out[100];size_t n=0;const char *cmd=kobox_wifi_perf_ps_request("NET_PERF_PS_OFF",15);
 assert(!kobox_wifi_qualify_request("wlan0",cmd,strlen(cmd),out,sizeof out,&n));
 assert(n==strlen("IFNAME=wlan0 SET ps 0")&&!strcmp(out,"IFNAME=wlan0 SET ps 0"));
 assert(kobox_wifi_qualify_request("wlan0","SET ps 0\nSCAN",13,out,sizeof out,&n)==-EINVAL);
 puts("PASS PS commands use exact matching and upstream interface-qualified SET ps; no injection");return 0;
}
