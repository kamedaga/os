#include <assert.h>
#include <stdio.h>
#define PH_ARENA_LAZY 1
#define PH_SANDBOX_FS 1
#include "../image.c"
static long native_status;
long pacha_syscall3(uint64_t nr,uint64_t address,uint64_t bytes,uint64_t advice) {
 assert(nr==PACHA_VM_SYSCALL_MADVISE && address==0x102000 && bytes==3*4096 && advice==PACHA_VM_MADV_POPULATE_WRITE);
 return native_status;
}
long pacha_syscall4(uint64_t nr,uint64_t fd,uint64_t ptr,uint64_t count,uint64_t flags) {
 assert(nr==PACHA_FD_SYSCALL_VMO_DISCARD && fd==60 && count==1 && !flags);
 const struct pacha_vmo_discard_range *r=(void *)(uintptr_t)ptr;
 assert(r->first_page==2 && r->page_count==3);return native_status;
}
int main(void) {
 struct ph_image image={.ram_fd=60,.ram_size=16*4096,.direct=(void *)0x100000};
 struct kobox_linux_memory_range range={2,3};
 for(unsigned i=0;i<=PACHA_SYSCALL_ERR_MAP;i++) {
  native_status=i;
  assert(memory_populate(&image,2,3)==-(int)i);
  assert(memory_discard(&image,&range,1)==-(int)i);
 }
 assert(memory_populate(&image,16,1)==PACHA_ERR_INVALID);
 assert(memory_populate(&image,15,2)==PACHA_ERR_INVALID);
 puts("resident adapter: actual MADV_POPULATE_WRITE/discard bridge, raw ALLOC/MAP/INVALID/NOT_READY status and bounds PASS");
}
