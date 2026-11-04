#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifndef PROC_IMPLEMENTATION
#define PROC_IMPLEMENTATION "../userland/personality/linux/runtime/lpr_vfs/proc.c"
#endif
#include PROC_IMPLEMENTATION
static char contents[2048];static size_t used;static int ramroot;
int64_t lpr_linux_statfs(uint64_t path,uint64_t out){lpr_linux_statfs_t *v=(void*)(uintptr_t)out;memset(v,0,sizeof(*v));v->f_type=ramroot||strcmp((char*)(uintptr_t)path,"/")?0x01021994:0xef53;v->f_fsid[0]=ramroot||!strcmp((char*)(uintptr_t)path,"/")?1:(uint32_t)strlen((char*)(uintptr_t)path)+2;return 0;}
size_t lpr_strlen(const char*s){return strlen(s);}
int64_t lpr_linux_write(uint64_t fd,uint64_t bytes,uint64_t n,unsigned*w){(void)w;assert(fd==17&&used+n<sizeof contents);memcpy(contents+used,(void*)(uintptr_t)bytes,n);used+=n;return n;}
int main(void){unsigned w=0;assert(!lpr_proc_write_mounts(17,&w));const char*mounts[]={"/tmp","/run","/dev/shm","/sys","/dev/dri"};for(unsigned i=0;i<5;i++){char needle[100];snprintf(needle,sizeof needle,"tmpfs %s tmpfs ",mounts[i]);assert(strstr(contents,needle));}used=0;memset(contents,0,sizeof contents);ramroot=1;assert(!lpr_proc_write_mounts(17,&w));assert(strstr(contents,"rootfs / tmpfs ")&&!strstr(contents,"tmpfs /tmp "));puts("PROC_MOUNTS_PASS disk submounts and RAM-root identity");}
