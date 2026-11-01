/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include "../userland/gpud/package.c"
static struct pacha_capsule_info info;
static int query_error;
static unsigned char blob[4096];
pacha_fd_t pacha_vmo_create(uint64_t size, uint64_t rights, uint32_t flags) {
 assert(size==sizeof(blob) && (rights&PACHA_FD_RIGHT_MAP_WRITE) && !flags); return 100;
}
void *pacha_mmap(pacha_fd_t fd,uint64_t size,uint64_t prot,uint64_t flags,uint64_t offset) {
 assert(fd==100 && size==sizeof(blob) && prot==(PACHA_PROT_READ|PACHA_PROT_WRITE));
 assert(flags==PACHA_MMAP_SHARED && !offset); return blob;
}
int pacha_munmap(void *view,uint64_t size) { assert(view==blob && size==sizeof(blob)); return 0; }
int pacha_fd_close(pacha_fd_t fd) { assert(fd==100); return 0; }
int pacha_capsule_query(pacha_fd_t fd, struct pacha_capsule_info *out) {
 assert(fd==224); *out=info; return query_error;
}
int main(void) {
 struct gpud_package p={.device_fd=224,.device={.native_device=42,.slot_id=1,.object_id=1},
  .device_rights=PH_DEVICE_FUNCTION_RIGHTS,.manifest_size=1,.artifact_count=1};
 p.identity.manifest_digest[0]=1;
 struct gpud_package ordinary=p;
 assert(!gpud_package_bind(&ordinary,7));
 assert(ordinary.items[3].capability.rights==PH_DEVICE_FUNCTION_RIGHTS);
 info=(struct pacha_capsule_info){.kind=PACHA_CAPSULE_KIND_DEVICE,.device=42,.rights=PH_DEVICE_VGA_FUNCTION_RIGHTS|PACHA_FD_RIGHT_TRANSFER};
 const struct pacha_capsule_info good=info;
 for(unsigned scenario=0;scenario<6;++scenario) {
  info=good;query_error=0;
  switch(scenario) {
   case 0:info.rights &= ~PACHA_FD_RIGHT_CPU_READ;break;
   case 1:info.rights &= ~PACHA_FD_RIGHT_CPU_WRITE;break;
   case 2:info.rights &= ~PACHA_FD_RIGHT_TRANSFER;break;
   case 3:info.device=43;break;
   case 4:info.flags=PACHA_CAPSULE_DMA_QUARANTINED;break;
   case 5:query_error=-5;break;
  }
  assert(gpud_package_enable_port_io(&p)<0 && p.device_rights==PH_DEVICE_FUNCTION_RIGHTS);
 }
 info=good;query_error=0;
 assert(!gpud_package_enable_port_io(&p) && p.device_rights==PH_DEVICE_VGA_FUNCTION_RIGHTS);
 assert(!gpud_package_bind(&p,7));
 assert(p.items[3].capability.fd==224 && p.items[3].capability.rights==PH_DEVICE_VGA_FUNCTION_RIGHTS);
 assert(gpud_package_enable_port_io(&p)<0);
 puts("gpud CPU port delegation: missing/foreign/quarantined authority rejected; exact grant PASS");
}
