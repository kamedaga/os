#define _GNU_SOURCE
#include <assert.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "../userland/gpud/discovery.c"
#include "../userland/gpud/discovery_filed.c"

static unsigned char wire[FILED_PAGE_BYTES];
static uint64_t identity = UINT64_C(0x50434900002d0000);
static int failure_at, calls, live, maps, closed_handles, fail_config;
static char paths[32][160];
static int nodes;
int pacha_capsule_query(int fd, struct pacha_capsule_info *out)
{ assert(fd == 42); *out = (struct pacha_capsule_info){.kind=PACHA_CAPSULE_KIND_DEVICE, .device=identity}; return 0; }
int pacha_capsule_pci_config_read(int fd, uint16_t offset, unsigned width, uint32_t *out)
{
    assert(fd == 42);
    if (fail_config) return -EACCES;
    switch (offset) {
    case 0: assert(width == 4); *out = 0x12345678; break;
    case 8: assert(width == 1); *out = 0x91; break;
    case 14: assert(width == 1); *out = 0; break;
    case 44: assert(width == 4); *out = 0x23456789; break;
    default: assert(0);
    }
    return 0;
}
int pacha_vmo_create(uint64_t size, uint64_t rights, uint32_t flags)
{ assert(size == sizeof(wire) && rights && !flags); ++maps; return 50; }
void *pacha_mmap(int fd,uint64_t size,uint64_t prot,uint64_t flags,uint64_t offset)
{ assert(fd==50 && size==sizeof(wire) && prot && flags && !offset); return wire; }
int pacha_munmap(void *p,uint64_t size)
{ assert(p==wire && size==sizeof(wire)); --maps; return 0; }
int pacha_fd_close(int fd) { assert(fd==50 || fd==51); return 0; }
int pacha_ipc_call(int fd, const struct pacha_ipc_msg *req)
{ assert(fd==43 && req->fd_count==1 && req->fds[0].fd==50); return 51; }
int pacha_ipc_recv_wait(int fd, struct pacha_ipc_msg *reply, uint64_t timeout)
{
    assert(fd==51 && timeout==PACHA_FD_WAIT_FOREVER);
    pacha_service_envelope_t *h=(void *)wire;
    void *p=wire+PACHA_SERVICE_HEADER_BYTES;
    h->status=0;
    reply->word0=PACHA_SERVICE_REPLY_MAGIC;
    h->magic=PACHA_SERVICE_REPLY_MAGIC;
    if (++calls == failure_at) { h->status=-EIO; return 0; }
    const char *path=NULL;
    switch(h->op) {
    case FILED_OP_VFS_MKDIR: path=((filed_mkdir_t *)p)->name; break;
    case FILED_OP_VFS_MKNOD: path=((filed_mknod_t *)p)->name;
        assert(((filed_mknod_t *)p)->dev==0xe280); ++nodes; break;
    case FILED_OP_VFS_SYMLINK: path=((filed_symlink_t *)p)->name;
        assert(((filed_symlink_t *)p)->target_length==strlen(((filed_symlink_t *)p)->target)); break;
    case FILED_OP_VFS_OPENAT: path=((filed_path_request_t *)p)->path; h->result=77; break;
    case FILED_OP_VFS_PWRITE: assert(((filed_io_t *)p)->handle==77);
        h->result=((filed_io_t *)p)->length; return 0;
    case FILED_OP_VFS_CLOSE: assert(((filed_handle_request_t *)p)->handle==77); ++closed_handles; return 0;
    case FILED_OP_VFS_UNLINK: case FILED_OP_VFS_RMDIR:
        assert(live>0 && !strcmp(paths[live-1], ((filed_unlink_t *)p)->name)); --live; return 0;
    default: assert(0);
    }
    /* Shared directories belong to somebody else. */
    if (h->op==FILED_OP_VFS_MKDIR && live==0 && strcmp(path,"/sys/dev/char/226:128"))
        h->status=-EEXIST;
    else { assert(live<32); strcpy(paths[live++],path); }
    return 0;
}
int main(int argc, char **argv)
{
    struct gpud_discovery d;
    assert(!gpud_discovery_prepare(&d,42,0));
    assert(d.count==22);
    assert(!strcmp(d.entries[13].value,"PCI_SLOT_NAME=0000:2d:00.0\nPCI_ID=5678:1234\n"));
    assert(!strcmp(d.entries[14].value,"0x5678\n"));
    assert(!gpud_discovery_publish(&d,43) && nodes==1 && live==14 && maps==0);
    int total=calls;
    assert(gpud_discovery_publish(&d,43)==-EINVAL);
    assert(!gpud_discovery_remove(&d,43) && !live && !maps && !d.owned);
    for (int i=1;i<=total;++i) {
        assert(!gpud_discovery_prepare(&d,42,0)); calls=0; failure_at=i;
        assert(gpud_discovery_publish(&d,43)==-EIO);
        assert(!live && !maps && !d.owned);
    }
    failure_at=0; fail_config=1;
    assert(gpud_discovery_prepare(&d,42,0)==-EACCES && !d.count);
    fail_config=0; identity=1;
    assert(gpud_discovery_prepare(&d,42,0)==-ENODEV);
    identity=UINT64_C(0x5043490000010301);
    assert(!gpud_discovery_prepare(&d,42,1) && d.count==28);
    assert(!strcmp(d.entries[27].path,"/dev/dri/card0") && d.entries[27].dev==0xe200);
    puts("gpud discovery: capability metadata, publication, reverse cleanup and RPC failures PASS");
    if (argc == 2) {
        identity=UINT64_C(0x50434900002d0000);
        assert(!gpud_discovery_prepare(&d,42,0));
        char path[1024];
        snprintf(path,sizeof(path),"%s/dev",argv[1]);
        assert(!mkdir(path,0755) || errno==EEXIST);
        for(size_t i=0;i<d.count;++i) {
            const struct gpud_discovery_entry *e=&d.entries[i];
            snprintf(path,sizeof(path),"%s%s",argv[1],e->path);
            if(e->kind==GPUD_DISC_DIR) assert(!mkdir(path,0755));
            else if(e->kind==GPUD_DISC_LINK) assert(!symlink(e->value,path));
            else { FILE *f=fopen(path,"w"); assert(f); fputs(e->value,f); assert(!fclose(f)); }
        }
    }
}
