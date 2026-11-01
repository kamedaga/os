#define NETD_PATH_PROFILE 1
#include "../userland/netd/src/kobox2_frame_client.c"
#include <assert.h>
int main(void)
{
 char text[8192];assert(!netd_path_control("start",NULL,0));
 assert(netd_path_control("dump",text,sizeof text)==-EBUSY);
 for(unsigned i=0;i<200;i++)netd_path_record(1,64,100,200,i);
 assert(path_count==64);assert(!netd_path_control("stop",NULL,0));
 int n=netd_path_control("dump",text,sizeof text);assert(n>0&&n<(int)sizeof text);
 assert(strstr(text,"stage=1 key=40 begin=100 end=200"));
 assert(netd_path_control("dump",text,4)==-ENOSPC);
 assert(!netd_path_control("start",NULL,0));assert(!path_count);
 netd_path_record(1,65,0,1,0);assert(!path_count);
 assert(!netd_path_control("stop",NULL,0));assert(!netd_path_control("dump",text,sizeof text));
 puts("PASS netd path ring bounded/filter/reset/stop-before-dump and output capacity");return 0;
}
