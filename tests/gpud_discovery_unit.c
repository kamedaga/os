#define _GNU_SOURCE
#include <assert.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "../userland/gpud/discovery.c"
#include "../userland/gpud/discovery_filed.c"

#include "../userland/libfiled_client/src/client.c"
static unsigned char *wire;
static size_t wire_bytes, request_bytes;
static kb2_fs_request_t decoded;
static kb2_fs_response_t last_reply;
static uint64_t identity = UINT64_C(0x50434900002d0000);
static int failure_at, calls, live, maps, closed_handles, fail_config;
static char paths[32][160];
static int nodes;
int pacha_capsule_query(pacha_fd_t fd, struct pacha_capsule_info *out)
{ assert(fd == 42); *out = (struct pacha_capsule_info){.kind=PACHA_CAPSULE_KIND_DEVICE, .device=identity}; return 0; }
int pacha_capsule_pci_config_read(pacha_fd_t fd, uint16_t offset, unsigned width, uint32_t *out)
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
pacha_fd_t pacha_vmo_create(uint64_t size, uint64_t rights, uint32_t flags)
{ assert(!wire && size >= 4096 && !(size & 4095) && rights && !flags);
  wire = calloc(1, size); assert(wire); wire_bytes = size; ++maps; return 50; }
void *pacha_mmap(pacha_fd_t fd,uint64_t size,uint64_t prot,uint64_t flags,uint64_t offset)
{ assert(fd==50 && size==wire_bytes && prot && flags && !offset); return wire; }
int pacha_munmap(void *p,uint64_t size)
{ assert(p==wire && size==wire_bytes); --maps; return 0; }
int pacha_fd_close(pacha_fd_t fd)
{ assert(fd==50 || fd==51); if(fd==50){free(wire); wire=NULL;} return 0; }
int pacha_fd_get_info(pacha_fd_t fd, struct pacha_fd_info *info)
{ (void)fd; (void)info; assert(0); return -EIO; }
static void operate(void)
{
    kb2_fs_request_t *r = &decoded;
    const char *path = r->path.length ? (char *)wire + r->path.offset : NULL;
    switch(r->opcode) {
    case KB2_FILESYSTEM_OP_MKDIRAT: assert(r->mode==0755); break;
    case KB2_FILESYSTEM_OP_MKNODAT:
        assert(r->length==0xe280 || r->length==0xe200); ++nodes; break;
    case KB2_FILESYSTEM_OP_SYMLINKAT:
        assert(path && r->second_path.length);
        path=(char *)wire+r->second_path.offset; break;
    case KB2_FILESYSTEM_OP_OPENAT2:
        assert(r->flags==(O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC));
        last_reply.handle=77; break;
    case KB2_FILESYSTEM_OP_PWRITE: assert(r->handle==77 && r->data.length==r->length);
        last_reply.result=(int64_t)r->length; return;
    case KB2_FILESYSTEM_OP_CLOSE: assert(r->handle==77); ++closed_handles; return;
    case KB2_FILESYSTEM_OP_UNLINKAT:
        assert(live>0 && !strcmp(paths[live-1],path)); --live; return;
    default: assert(0);
    }
    /* Shared directories belong to somebody else. */
    if(r->opcode==KB2_FILESYSTEM_OP_MKDIRAT && live==0 && strcmp(path,"/sys/dev/char/226:128"))
        last_reply.result=-EEXIST;
    else { assert(live<32); strcpy(paths[live++],path); }
}
int pacha_ipc_call(pacha_fd_t fd, const struct pacha_ipc_msg *req, struct pacha_ipc_msg *reply, uint64_t timeout, int *sent)
{
    assert(fd==43 && req->fd_count==1 && req->fds[0].fd==50);
    request_bytes = req->word1;
    assert(!kb2_fs_request_decode(wire, request_bytes, request_bytes, &decoded));
    assert(!kb2_fs_request_validate_paths(wire, request_bytes, &decoded));
    assert(req->word0 == decoded.opcode && req->word3 == decoded.sequence);
    if (sent) *sent = 1;
    assert(timeout == PACHA_FD_WAIT_FOREVER);
    last_reply=(kb2_fs_response_t){.opcode=decoded.opcode,
        .sequence=decoded.sequence,.generation=decoded.generation};
    if(++calls==failure_at) last_reply.result=-EIO;
    else operate();
    assert(!kb2_fs_response_encode(wire,wire_bytes,&last_reply));
    reply->word0=last_reply.opcode;
    reply->word1=(uint64_t)last_reply.result;
    reply->word2=KB2_FILESYSTEM_RESPONSE_SIZE;
    reply->word3=last_reply.sequence;
    return 0;
}
int main(int argc, char **argv)
{
    struct gpud_discovery d;
    assert(!gpud_discovery_prepare(&d,42,0));
    assert(d.count==24 && d.ancestor_count==9);
    assert(!strcmp(d.entries[3].path,"/sys/dev/block") && d.entries[3].kind==GPUD_DISC_DIR);
    assert(!strcmp(d.entries[14].value,"PCI_SLOT_NAME=0000:2d:00.0\nPCI_ID=5678:1234\n"));
    assert(!strcmp(d.entries[15].value,"0x5678\n"));
    assert(!strcmp(d.entries[21].path,"/sys/dev/char/226:128/uevent"));
    assert(!strcmp(d.entries[21].value,"MAJOR=226\nMINOR=128\nDEVNAME=dri/renderD128\n"));
    assert(!gpud_discovery_publish(&d,43) && nodes==1 && live==15 && maps==0);
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
    assert(!gpud_discovery_prepare(&d,42,1) && d.count==31);
    assert(!strcmp(d.entries[27].path,"/sys/dev/char/226:0/uevent"));
    assert(!strcmp(d.entries[27].value,"MAJOR=226\nMINOR=0\nDEVNAME=dri/card0\n"));
    assert(!strcmp(d.entries[30].path,"/dev/dri/card0") && d.entries[30].dev==0xe200);
    int before_nodes=nodes;
    calls=0;
    assert(!gpud_discovery_publish(&d,43) && nodes==before_nodes+2 && live==22);
    total=calls;
    assert(!gpud_discovery_remove(&d,43) && !live && !d.owned);
    for (int i=1;i<=total;++i) {
        assert(!gpud_discovery_prepare(&d,42,1)); calls=0; failure_at=i;
        assert(gpud_discovery_publish(&d,43)==-EIO);
        assert(!live && !maps && !d.owned);
    }
    failure_at=0;
    puts("gpud discovery: capability metadata, publication, reverse cleanup and RPC failures PASS");
    if (argc >= 2) {
        identity=UINT64_C(0x50434900002d0000);
        assert(!gpud_discovery_prepare(&d,42,argc == 3));
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
