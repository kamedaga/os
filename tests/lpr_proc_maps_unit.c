#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <pacha/abi.h>
#define LPR_LINUX_EIO 5
#include "../userland/personality/linux/runtime/lpr_vfs/maps.h"
int main(void) {
 pacha_vm_mapping_info_t r={.start=0x1000,.size=0x3000,.prot=3,.flags=4,.offset=0x5000};
 char line[96];memset(line,0xa5,sizeof(line));uint64_t next;
 int64_t n=lpr_proc_maps_line(&r,0,line,&next);assert(n>0&&next==0x4000);
 line[n]=0;unsigned long start,end,off;char perms[5]={0};
 assert(sscanf(line,"%lx-%lx %4s %lx",&start,&end,perms,&off)==4);
 assert(start==0x1000&&end==0x4000&&off==0x5000&&!strcmp(perms,"rw-p"));
 r.flags=8;r.prot=5;n=lpr_proc_maps_line(&r,0x2000,line,&next);line[n]=0;
 assert(sscanf(line,"%lx-%lx %4s %lx",&start,&end,perms,&off)==4);
 assert(start==0x2000&&off==0x6000&&!strcmp(perms,"r-xs"));
 assert(lpr_proc_maps_line(&r,0x4000,line,&next)==-5);
 r.size=0;assert(lpr_proc_maps_line(&r,0,line,&next)==-5);
 r.start=UINT64_MAX-1;r.size=4;assert(lpr_proc_maps_line(&r,0,line,&next)==-5);
 r.start=0;r.size=4096;r.prot=8;assert(lpr_proc_maps_line(&r,0,line,&next)==-5);
 r.prot=0;n=lpr_proc_maps_line(&r,0,line,&next);line[n]=0;
 assert(sscanf(line,"%lx-%lx %4s %lx",&start,&end,perms,&off)==4&&start==0&&!strcmp(perms,"---s"));
 puts("proc maps: native ranges, permissions, merge progress and overflow PASS");
}
