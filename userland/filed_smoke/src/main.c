/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "filed_smoke/bootstrap.h"
#include "filed_client/client.h"
#include "pacha/bootstrap.h"
#include "pacha/ipc.h"
#include "pacha/syscall.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int endpoint;
static unsigned checks;
/* The native loader/stdio boundary is under test, so progress must use the
 * same raw LOG syscall as pacha_trace, without allocating or calling stdio. */
static void raw_marker(const char *text,size_t length)
{
    (void)pacha_syscall2(1,(uint64_t)(uintptr_t)text,length);
}
#define RAW_MARK(text) raw_marker(text,sizeof(text)-1)
static void raw_status(const char *text,size_t length,long status)
{
    volatile char line[128];
    size_t used=0;
    for(size_t i=0;i<length && used<sizeof(line)-24;++i) line[used++]=text[i];
    line[used++]='=';
    uint64_t value=(uint64_t)status;
    if(status<0) { line[used++]='-';value=0-value; }
    char digits[20];size_t count=0;
    do { digits[count++]=(char)('0'+value%10);value/=10; } while(value);
    while(count) line[used++]=digits[--count];
    line[used++]='\n';
    raw_marker((const char *)line,used);
}
#define RAW_STATUS(text,status) raw_status(text,sizeof(text)-1,(long)(status))
static int call(kb2_fs_request_t request,const char *path,const char *second,
                const void *input,size_t input_size,void *output,size_t capacity,
                kb2_fs_response_t *response)
{
    int status=filedfs_call(endpoint,&request,path,second,input,input_size,
        output,capacity,response,NULL,0,NULL,NULL);
    return status ? status : response->result<0 ? (int)response->result : 0;
}
#define CHECK(condition) do { ++checks; if (!(condition)) { \
    RAW_STATUS("[filed-smoke/raw] failure line",__LINE__); \
    RAW_STATUS("[filed-smoke/raw] failure check",checks); \
    fprintf(stderr,"[filed-smoke] failure line=%u check=%u\n",__LINE__,checks); \
    return -1; } } while (0)
static uint64_t rd64(const uint8_t *p)
{
    uint64_t v=0;
    for(unsigned i=0;i<8;++i) v|=(uint64_t)p[i]<<(i*8);
    return v;
}
static int close_handle(uint64_t handle)
{
    kb2_fs_response_t response;
    return call((kb2_fs_request_t){.opcode=KB2_FILESYSTEM_OP_CLOSE,.handle=handle},
        NULL,NULL,NULL,0,NULL,0,&response);
}
static int smoke(void)
{
    kb2_fs_response_t response;
    RAW_MARK("[filed-smoke/raw] HELLO begin\n");
    int status=call((kb2_fs_request_t){.opcode=KB2_FILESYSTEM_OP_HELLO},
        NULL,NULL,NULL,0,NULL,0,&response);
    RAW_STATUS("[filed-smoke/raw] HELLO status",status);
    CHECK(!status);
    RAW_MARK("[filed-smoke/raw] OPEN begin\n");
    status=call((kb2_fs_request_t){.opcode=KB2_FILESYSTEM_OP_OPENAT2},
        "/sbin/filed.elf",NULL,NULL,0,NULL,0,&response);
    RAW_STATUS("[filed-smoke/raw] OPEN status",status);
    CHECK(!status);
    uint64_t image=response.handle;
    uint8_t magic[4];
    CHECK(!call((kb2_fs_request_t){.opcode=KB2_FILESYSTEM_OP_PREAD,.handle=image,
        .length=sizeof(magic)},NULL,NULL,NULL,0,magic,sizeof(magic),&response));
    CHECK(response.data_length==4 && !memcmp(magic,"\177ELF",4));
    CHECK(!close_handle(image));
    /* This fixture owns one scratch directory. O_EXCL rejects stale evidence
     * instead of silently reusing a prior run's filesystem state. */
    CHECK(!call((kb2_fs_request_t){.opcode=KB2_FILESYSTEM_OP_MKDIRAT,.mode=0700},
        "/tmp/filed-new-abi-smoke",NULL,NULL,0,NULL,0,&response));
    CHECK(!call((kb2_fs_request_t){.opcode=KB2_FILESYSTEM_OP_OPENAT2,
        .flags=O_PATH|O_DIRECTORY},"/tmp/filed-new-abi-smoke",NULL,NULL,0,NULL,0,&response));
    uint64_t directory=response.handle;
    CHECK(!call((kb2_fs_request_t){.opcode=KB2_FILESYSTEM_OP_OPENAT2,
        .handle=directory,.flags=O_CREAT|O_EXCL|O_RDWR,.mode=0600},
        "a",NULL,NULL,0,NULL,0,&response));
    uint64_t file=response.handle;
    size_t length=20000;
    RAW_MARK("[filed-smoke/raw] malloc begin\n");
    uint8_t *data=malloc(length),*readback=malloc(length);
    RAW_STATUS("[filed-smoke/raw] malloc success",data && readback);
    CHECK(data && readback);
    for(size_t i=0;i<length;++i) data[i]=(uint8_t)(i*17+3);
    size_t written=0;
    while(written<length) {
        CHECK(!call((kb2_fs_request_t){.opcode=KB2_FILESYSTEM_OP_PWRITE,
            .handle=file,.offset=(int64_t)written,.length=length-written},NULL,NULL,
            data+written,length-written,NULL,0,&response));
        CHECK(response.result>0 && (uint64_t)response.result<=length-written);
        written+=(size_t)response.result;
    }
    CHECK(!call((kb2_fs_request_t){.opcode=KB2_FILESYSTEM_OP_PREAD,
        .handle=file,.length=length},NULL,NULL,NULL,0,readback,length,&response));
    CHECK(response.data_length==length && !memcmp(data,readback,length));
    uint8_t stat[KB2_FILESYSTEM_STATX_SIZE];
    CHECK(!call((kb2_fs_request_t){.opcode=KB2_FILESYSTEM_OP_STATX,
        .handle=file,.flags=AT_EMPTY_PATH,.mask=0x7ff},NULL,NULL,NULL,0,
        stat,sizeof(stat),&response));
    CHECK(response.data_length==sizeof(stat) &&
        rd64(stat+KB2_FILESYSTEM_STATX_SIZE_OFFSET)==length);
    CHECK(!call((kb2_fs_request_t){.opcode=KB2_FILESYSTEM_OP_DUP,.handle=file},
        NULL,NULL,NULL,0,NULL,0,&response));
    uint64_t duplicate=response.handle;
    CHECK(!call((kb2_fs_request_t){.opcode=KB2_FILESYSTEM_OP_READ,.handle=file,.length=7},
        NULL,NULL,NULL,0,readback,length,&response));
    CHECK(response.data_length==7 && !memcmp(data,readback,7));
    CHECK(!call((kb2_fs_request_t){.opcode=KB2_FILESYSTEM_OP_READ,.handle=duplicate,.length=9},
        NULL,NULL,NULL,0,readback,length,&response));
    CHECK(response.data_length==9 && !memcmp(data+7,readback,9));
    CHECK(!call((kb2_fs_request_t){.opcode=KB2_FILESYSTEM_OP_FCNTL,.handle=file,
        .flags=F_SETFL,.length=O_APPEND|O_NONBLOCK},NULL,NULL,NULL,0,NULL,0,&response));
    CHECK(!call((kb2_fs_request_t){.opcode=KB2_FILESYSTEM_OP_FCNTL,.handle=duplicate,
        .flags=F_GETFL},NULL,NULL,NULL,0,NULL,0,&response));
    CHECK((response.result&(O_APPEND|O_NONBLOCK))==(O_APPEND|O_NONBLOCK));
    CHECK(!call((kb2_fs_request_t){.opcode=KB2_FILESYSTEM_OP_FTRUNCATE,.handle=file,
        .offset=(int64_t)(length+4096)},NULL,NULL,NULL,0,NULL,0,&response));
    memset(readback,1,4096);
    CHECK(!call((kb2_fs_request_t){.opcode=KB2_FILESYSTEM_OP_PREAD,.handle=file,
        .offset=(int64_t)length,.length=4096},NULL,NULL,NULL,0,readback,length,&response));
    CHECK(response.data_length==4096);
    for(size_t i=0;i<4096;++i) CHECK(readback[i]==0);
    CHECK(!call((kb2_fs_request_t){.opcode=KB2_FILESYSTEM_OP_FSYNC,.handle=file},
        NULL,NULL,NULL,0,NULL,0,&response));
    CHECK(!call((kb2_fs_request_t){.opcode=KB2_FILESYSTEM_OP_LINKAT,.handle=directory,
        .second_handle=directory},"a","b",NULL,0,NULL,0,&response));
    CHECK(!call((kb2_fs_request_t){.opcode=KB2_FILESYSTEM_OP_RENAMEAT2,.handle=directory,
        .second_handle=directory},"b","c",NULL,0,NULL,0,&response));
    CHECK(!call((kb2_fs_request_t){.opcode=KB2_FILESYSTEM_OP_SYMLINKAT,
        .handle=directory},"a","link",NULL,0,NULL,0,&response));
    CHECK(!call((kb2_fs_request_t){.opcode=KB2_FILESYSTEM_OP_READLINKAT,
        .handle=directory,.length=length},"link",NULL,NULL,0,readback,length,&response));
    CHECK(response.data_length==1 && readback[0]=='a');
    char too_long[257];memset(too_long,'x',256);too_long[256]=0;
    CHECK(call((kb2_fs_request_t){.opcode=KB2_FILESYSTEM_OP_OPENAT2,.handle=directory},
        too_long,NULL,NULL,0,NULL,0,&response)==-ENAMETOOLONG);
    CHECK(!close_handle(duplicate));
    CHECK(!close_handle(file));
    CHECK(close_handle(file)==-EBADF);
    const char *names[]={"a","c","link"};
    for(size_t i=0;i<sizeof(names)/sizeof(names[0]);++i)
        CHECK(!call((kb2_fs_request_t){.opcode=KB2_FILESYSTEM_OP_UNLINKAT,
            .handle=directory},names[i],NULL,NULL,0,NULL,0,&response));
    CHECK(!close_handle(directory));
    CHECK(!call((kb2_fs_request_t){.opcode=KB2_FILESYSTEM_OP_UNLINKAT,.flags=AT_REMOVEDIR},
        "/tmp/filed-new-abi-smoke",NULL,NULL,0,NULL,0,&response));
    free(data);free(readback);
    RAW_STATUS("[filed-smoke/raw] completed checks",checks);
    static const char write_probe[]="[filed-smoke/fd-write] raw write probe\n";
    RAW_MARK("[filed-smoke/raw] FD_WRITE begin\n");
    long write_status=pacha_fd_write(1,write_probe,sizeof(write_probe)-1);
    RAW_STATUS("[filed-smoke/raw] FD_WRITE status",write_status);
    RAW_MARK("[filed-smoke/raw] printf begin\n");
    printf("[filed-smoke] ready path=/sbin/filed.elf bytes=%zu write=ok lifecycle=ok checks=%u ABI=storage-vq\n",
        length,checks);
    RAW_MARK("[filed-smoke/raw] printf returned\n");
    RAW_MARK("[filed-smoke/raw] fflush begin\n");
    int flush_status=fflush(stdout);
    RAW_STATUS("[filed-smoke/raw] fflush status",flush_status);
    return 0;
}
int main(int argc,char **argv,char **envp)
{
    RAW_MARK("[filed-smoke/raw] main entry\n");
    int found=0;
    for(char **p=envp;p && *p;++p) if(!strcmp(*p,"PACHA_FILED_EXEC_SMOKE=1")) found=1;
    if(argc<2 || !argv || !argv[0] || strcmp(argv[0],"/sbin/filed_smoke.elf") ||
        strcmp(argv[1],"--execve-smoke") || !found) {
        RAW_MARK("[filed-smoke/raw] argv/env invalid\n");return 2;
    }
    RAW_MARK("[filed-smoke/raw] bootstrap begin\n");
    int fd=pacha_bootstrap_fd_from_argv(argv);
    RAW_STATUS("[filed-smoke/raw] bootstrap fd",fd);
    filed_smoke_bootstrap_t bootstrap={0};
    long read_status=fd<16 ? -EBADF : pacha_fd_read(fd,&bootstrap,sizeof(bootstrap));
    RAW_STATUS("[filed-smoke/raw] bootstrap read",read_status);
    if(fd<16 || read_status!=(long)sizeof(bootstrap) ||
        bootstrap.magic!=FILED_SMOKE_BOOTSTRAP_MAGIC ||
        bootstrap.public_endpoint_fd<16 || bootstrap.public_endpoint_fd>INT_MAX) {
        RAW_MARK("[filed-smoke/raw] bootstrap invalid\n");return 1;
    }
    endpoint=(int)bootstrap.public_endpoint_fd;
    RAW_STATUS("[filed-smoke/raw] bootstrap endpoint",endpoint);
    int status=smoke()!=0;
    RAW_STATUS("[filed-smoke/raw] main return status",status);
    return status;
}
