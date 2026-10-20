/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr,"[native-fs-smoke] FAIL line=%u errno=%d\n",__LINE__,errno); \
    return 1; } } while (0)

static int child(int argc,char **argv,char **envp)
{
    CHECK(argc==300 && !strcmp(argv[1],"--child"));
    for(int i=5;i<argc;++i)
        CHECK(!strcmp(argv[i],"argument-crossing-the-former-fixed-string-payload-budget"));
    size_t environment=0;
    for(char **p=envp;p && *p;++p) ++environment;
    CHECK(environment==80 && getenv("PACHA_NATIVE_FS_CHILD") &&
        !strcmp(getenv("PACHA_NATIVE_FS_CHILD"),"1"));
    int inherited=atoi(argv[2]),shared=atoi(argv[3]),cloexec=atoi(argv[4]);
    char bytes[4]={0};struct stat info;
    CHECK(read(inherited,bytes,3)==3 && !memcmp(bytes,"cde",3));
    CHECK(read(shared,bytes,3)==3 && !memcmp(bytes,"fgh",3));
    CHECK(lseek(inherited,0,SEEK_CUR)==8 && !fstat(inherited,&info) && info.st_size==10);
    CHECK(fstat(cloexec,&info)<0 && errno==EBADF);
    CHECK(!close(inherited) && !close(shared));
    puts("[native-fs-smoke] child complete public-libc=ok inherited-offset=ok cloexec=ok argv=300 env=80");
    CHECK(!fflush(stdout));
    return 0;
}

int main(int argc,char **argv,char **envp)
{
    if(argc>=2 && !strcmp(argv[1],"--child")) return child(argc,argv,envp);
    CHECK(argc>=1 && !strcmp(argv[0],"/sbin/native_fs_smoke.elf"));
    const char *directory="/tmp/native-fs-cutover-smoke";
    CHECK(!mkdir(directory,0700));
    int dir=open(directory,O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    CHECK(dir>=0);
    int file=openat(dir,"a",O_CREAT|O_EXCL|O_RDWR|O_CLOEXEC,0600);
    CHECK(file>=0);
    const size_t length=20000;
    unsigned char *data=malloc(length),*back=malloc(length);
    CHECK(data && back);
    for(size_t i=0;i<length;++i) data[i]=(unsigned char)(i*17+3);
    size_t done=0;
    while(done<length) {
        ssize_t count=write(file,data+done,length-done);
        CHECK(count>0 && (size_t)count<=length-done);done+=(size_t)count;
    }
    CHECK(pread(file,back,length,0)==(ssize_t)length && !memcmp(data,back,length));
    struct stat info;
    CHECK(!fstat(file,&info) && info.st_size==(off_t)length);
    void *snapshot=mmap(NULL,length,PROT_READ,MAP_PRIVATE,file,0);
    CHECK(snapshot!=MAP_FAILED && !memcmp(snapshot,data,length));
    CHECK(!munmap(snapshot,length));
    /* Shared page-cache export is unsupported;
     * successful private snapshots must not masquerade as MAP_SHARED. */
    void *unsupported=mmap(NULL,length,PROT_READ,MAP_SHARED,file,0);
    CHECK(unsupported==MAP_FAILED && errno==EOPNOTSUPP);
    CHECK(lseek(file,0,SEEK_SET)==0);
    int duplicate=dup(file);CHECK(duplicate>=0);
    CHECK(read(file,back,7)==7 && !memcmp(back,data,7));
    CHECK(read(duplicate,back,9)==9 && !memcmp(back,data+7,9));
    CHECK(lseek(file,0,SEEK_CUR)==16);
    CHECK(!fcntl(file,F_SETFL,O_APPEND|O_NONBLOCK));
    CHECK((fcntl(duplicate,F_GETFL)&(O_APPEND|O_NONBLOCK))==(O_APPEND|O_NONBLOCK));
    CHECK(!ftruncate(file,(off_t)length+4096));
    unsigned char zero[4096]={0};
    CHECK(pread(file,back,sizeof(zero),(off_t)length)==(ssize_t)sizeof(zero) &&
        !memcmp(back,zero,sizeof(zero)));
    CHECK(!fsync(file));
    CHECK(!syncfs(file));
    sync(); /* Public sync has a void result; raw ABI/GPL gates check its result. */
    int descriptors[300];
    for(size_t i=0;i<300;++i) { descriptors[i]=dup(file);CHECK(descriptors[i]>=0); }
    for(size_t i=0;i<300;++i) CHECK(!close(descriptors[i]));
    CHECK(!linkat(dir,"a",dir,"b",0));
    CHECK(!renameat(dir,"b",dir,"c"));
    CHECK(!symlinkat("a",dir,"link"));
    char link[8]={0};CHECK(readlinkat(dir,"link",link,sizeof(link))==1 && link[0]=='a');
    CHECK(!close(duplicate) && !close(file));
    CHECK(!unlinkat(dir,"a",0) && !unlinkat(dir,"c",0) && !unlinkat(dir,"link",0));
    int inherited=openat(dir,"exec",O_CREAT|O_EXCL|O_RDWR,0600);
    CHECK(inherited>=0 && write(inherited,"abcdefghij",10)==10 &&
        lseek(inherited,2,SEEK_SET)==2);
    int shared=dup(inherited);CHECK(shared>=0);
    int cloexec=fcntl(inherited,F_DUPFD_CLOEXEC,16);CHECK(cloexec>=0);
    /* The child must use inherited file descriptions after unlink, not reopen
     * a pathname and accidentally hide broken exec handle transfer. */
    CHECK(!unlinkat(dir,"exec",0) && !close(dir) && !rmdir(directory));
    free(data);free(back);
    puts("[native-fs-smoke] public-libc bytes=20000 read=ok write=ok dup=ok descriptors=300 lifecycle=ok");
    CHECK(!fflush(stdout));
    char fd_text[3][32];
    snprintf(fd_text[0],sizeof(fd_text[0]),"%d",inherited);
    snprintf(fd_text[1],sizeof(fd_text[1]),"%d",shared);
    snprintf(fd_text[2],sizeof(fd_text[2]),"%d",cloexec);
    char **child_argv=calloc(301,sizeof(*child_argv));
    char **child_env=calloc(81,sizeof(*child_env));
    CHECK(child_argv && child_env);
    child_argv[0]=argv[0];child_argv[1]="--child";
    child_argv[2]=fd_text[0];child_argv[3]=fd_text[1];child_argv[4]=fd_text[2];
    for(size_t i=5;i<300;++i)
        child_argv[i]="argument-crossing-the-former-fixed-string-payload-budget";
    child_env[0]="PACHA_NATIVE_FS_CHILD=1";
    for(size_t i=1;i<80;++i) child_env[i]="NATIVE_FS_LARGE_ENV=payload-crossing-old-inline-budget";
    /* Pacha native exec currently spawns then exits the parent. The runner
     * separately requires the child's complete marker, not just parent exit. */
    execve(child_argv[0],child_argv,child_env);
    fprintf(stderr,"[native-fs-smoke] FAIL execve returned errno=%d\n",errno);
    return 1;
}
