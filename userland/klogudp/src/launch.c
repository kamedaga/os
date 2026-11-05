/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pacha/ipc.h"
#include "filed/payload.h"
#include "filed_client/client.h"

void *klogudp_gate;
int main(int argc,char **argv)
{
    if (argc!=3 && !(argc==4 && !strcmp(argv[3],"--from-start"))) {
        fprintf(stderr,"usage: klogudp-start IPv4 PORT [--from-start]\n"); return 2;
    }
    /* LPR rewrites syscall instructions in Linux ELF executable segments.
     * This explicit diagnostic bridge creates a three-byte native gate AFTER
     * the ELF instruction-rewrite pass. Never run it on host Linux:
     * native syscall numbers overlap Linux numbers. The native worker itself
     * has ordinary unpatched native syscalls and needs no gate or LPR. */
    klogudp_gate=mmap(NULL,4096,PROT_READ|PROT_WRITE|PROT_EXEC,
        MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if (klogudp_gate==MAP_FAILED) { perror("native gate mmap"); return 3; }
    volatile unsigned char *gate=klogudp_gate;
    gate[0]=0x0f; gate[1]=0x05; gate[2]=0xc3;
    const char *path="/cmd/klogudp.elf";
    char strings[256]={0};
    filed_exec_string_ref_t refs[4];
    const char *args[4]={path,argv[1],argv[2],argc==4 ? argv[3] : NULL};
    size_t used=0;
    for (int i=0;i<argc;++i) {
        size_t n=strlen(args[i])+1;
        if (n>sizeof(strings)-used) return 2;
        refs[i]=(filed_exec_string_ref_t){used,n};
        memcpy(strings+used,args[i],n); used+=n;
    }
    struct pacha_fd_info info;
    int status=pacha_fd_get_info(241,&info);
    if (status) { fprintf(stderr,"netd capability unavailable: %d\n",status); return 3; }
    const uint64_t client=PACHA_FD_RIGHT_INSPECT|PACHA_FD_RIGHT_CLOSE|
        PACHA_FD_RIGHT_CALL|PACHA_FD_RIGHT_WAIT|PACHA_FD_RIGHT_POLL;
    if ((info.rights & (client|PACHA_FD_RIGHT_TRANSFER))!=(client|PACHA_FD_RIGHT_TRANSFER)) return 3;
    filed_exec_fd_grant_t grant={.target=241,.rights=client};
    filed_exec_path_t exec={.flags=FILED_EXEC_INHERIT_FDS,
        .inherit_fd_count=1,.fd_grants=&grant,.argc=(uint64_t)argc,
        .argv=refs,.strings=strings,.string_bytes=used};
    uint8_t *payload=NULL; size_t payload_size=0;
    status=filed_exec_payload_encode(&exec,&payload,&payload_size);
    pacha_fd_t source=241,received[2]={-1,-1}; size_t count=2;
    kb2_fs_request_t r={.opcode=KB2_FILESYSTEM_OP_EXEC_PATH,.flags=exec.flags};
    kb2_fs_response_t reply={0};
    if (!status) status=filedfs_call(240,&r,path,NULL,payload,payload_size,
        NULL,0,&reply,&source,1,received,&count);
    if (!status) status=(int)reply.result;
    free(payload);
    /* Closing observation capabilities does not kill a native process. No
     * supervisor registration, Linux parent/child or inherited SSH pipe. */
    for (size_t i=0;i<count;++i) if (received[i]>=16) pacha_fd_close(received[i]);
    if (status || count!=2) {
        fprintf(stderr,"native spawn failed: status=%d capabilities=%zu\n",status,count); return 3;
    }
    puts("klogudp native worker spawned; verify receiver heartbeat");
    return 0;
}
