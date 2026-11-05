#include <assert.h>
#define main launcher_main
#define mmap mock_mmap
#include "../src/launch.c"
#undef mmap
#undef main
static unsigned char gate_memory[4096];
static unsigned closed;
void *mock_mmap(void *address,size_t n,int prot,int flags,int fd,off_t offset)
{
    assert(!address && n==4096 && prot==(PROT_READ|PROT_WRITE|PROT_EXEC));
    assert(flags==(MAP_PRIVATE|MAP_ANONYMOUS) && fd==-1 && !offset);
    return gate_memory;
}
int pacha_fd_get_info(pacha_fd_t fd,struct pacha_fd_info *info)
{
    assert(fd==241);
    info->rights=PACHA_FD_RIGHT_INSPECT|PACHA_FD_RIGHT_CLOSE|PACHA_FD_RIGHT_CALL|
        PACHA_FD_RIGHT_WAIT|PACHA_FD_RIGHT_POLL|PACHA_FD_RIGHT_TRANSFER;
    return 0;
}
int pacha_fd_close(pacha_fd_t fd) { assert(fd==500 || fd==501);++closed;return 0; }
int filedfs_call(pacha_fd_t endpoint,kb2_fs_request_t *r,const char *path,
    const char *second,const void *data,size_t bytes,void *out,size_t capacity,
    kb2_fs_response_t *reply,const pacha_fd_t *sources,size_t count,
    pacha_fd_t *received,size_t *received_count)
{
    assert(endpoint==240 && r->opcode==KB2_FILESYSTEM_OP_EXEC_PATH);
    assert(!strcmp(path,"/cmd/klogudp.elf") && !second && !out && !capacity);
    assert(count==1 && sources[0]==241 && *received_count==2);
    assert(r->flags==FILED_EXEC_INHERIT_FDS); /* no LPR, SELF, bootstrap */
    filed_exec_path_t exec;
    assert(!filed_exec_payload_decode(data,bytes,path,0,r->flags,&exec));
    assert(exec.argc==3 && !exec.envc && exec.inherit_fd_count==1);
    assert(exec.fd_grants[0].target==241 && !exec.inherit_handle_count);
    assert(!strcmp(filed_exec_string(&exec,exec.argv[1]),"192.168.3.8"));
    assert(!strcmp(filed_exec_string(&exec,exec.argv[2]),"5000"));
    filed_exec_payload_destroy(&exec);
    *reply=(kb2_fs_response_t){0};received[0]=500;received[1]=501;return 0;
}
int main(void)
{
    char *args[]={"klogudp-start","192.168.3.8","5000"};
    assert(!launcher_main(3,args));
    assert(closed==2 && gate_memory[0]==0x0f && gate_memory[1]==5 && gate_memory[2]==0xc3);
    puts("launcher model: native EXEC_PATH / argv / netd-only grant / detach passed");
}
