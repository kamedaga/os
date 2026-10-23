/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <assert.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/syscall.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *path="/var/tmp/filecache-probe";
static void replace(unsigned char value, const char *name)
{
    int fd=open(name,O_CREAT|O_TRUNC|O_RDWR,0600);assert(fd>=0);
    unsigned char data[8192];memset(data,value,sizeof(data));
    assert(write(fd,data,sizeof(data))==sizeof(data));assert(!close(fd));
}
static unsigned char *image(int fd,int protection)
{
    unsigned char *p=mmap(NULL,8192,protection,MAP_PRIVATE,fd,0);
    assert(p!=MAP_FAILED);return p;
}
static void new_value(unsigned char value)
{
    int fd=open(path,O_RDONLY);assert(fd>=0);
    unsigned char *p=image(fd,PROT_READ);assert(p[0]==value);
    assert(!close(fd));assert(!munmap(p,8192));
}
static void dso(const char *name)
{
    int gate[2],done[2];assert(!pipe(gate) && !pipe(done));
    pid_t child=fork();assert(child>=0);
    if (!child) {
        close(gate[1]);close(done[0]);char byte;
        assert(read(gate[0],&byte,1)==1);
        void *lib=dlopen(name,RTLD_NOW|RTLD_LOCAL);assert(lib);
        int (*bump)(void)=dlsym(lib,"snapshot_bump");
        long (*pid)(void)=dlsym(lib,"snapshot_pid");assert(bump && pid);
        assert(bump()==8 && bump()==9 && pid()==getpid());
        assert(write(done[1],"x",1)==1);_exit(0);
    }
    close(gate[0]);close(done[1]);assert(write(gate[1],"x",1)==1);
    void *lib=dlopen(name,RTLD_NOW|RTLD_LOCAL);assert(lib);
    int (*bump)(void)=dlsym(lib,"snapshot_bump");
    long (*pid)(void)=dlsym(lib,"snapshot_pid");assert(bump && pid);
    char byte;assert(read(done[0],&byte,1)==1);
    assert(bump()==8 && pid()==getpid());
    int status;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    assert(!dlclose(lib));close(gate[1]);close(done[0]);
    puts("FILECACHE_GUEST dso two-process relocation and zpoline: PASS");
}
int main(int argc,char **argv)
{
    setvbuf(stdout,NULL,_IONBF,0);
    if (argc>1) path=argv[1];
    struct statfs filesystem;assert(!statfs("/var/tmp",&filesystem));
    assert(filesystem.f_type==0xef53);
    replace('A',path);
    struct statx metadata;
    const unsigned masks[]={0x7fffffffU,0x40000000U,0x400007ffU};
    int statfd=open(path,O_RDONLY);assert(statfd>=0);
    for (unsigned i=0;i<sizeof(masks)/sizeof(masks[0]);++i) {
        assert(!syscall(SYS_statx,AT_FDCWD,path,0,masks[i],&metadata));
        assert(!(metadata.stx_mask&0x40000000U) && metadata.stx_size==8192);
        assert(!syscall(SYS_statx,statfd,"",AT_EMPTY_PATH,masks[i],&metadata));
        assert(!(metadata.stx_mask&0x40000000U) && metadata.stx_size==8192);
        printf("FILECACHE_GUEST statx mask=0x%08x path and AT_EMPTY_PATH: PASS\n",masks[i]);
    }
    assert(!close(statfd));
    errno=0;assert(syscall(SYS_statx,AT_FDCWD,path,0,0x80000000U,&metadata)==-1 && errno==EINVAL);
    puts("FILECACHE_GUEST Linux statx remains Linux layout, internal cookie bit ignored: PASS");
    int rd=open(path,O_RDONLY);assert(rd>=0);
    unsigned char *a=image(rd,PROT_READ|PROT_WRITE),*b=image(rd,PROT_READ);
    a[0]='a';assert(b[0]=='A');
    assert(!mprotect(b,8192,PROT_READ|PROT_WRITE));b[1]='b';
    unsigned char *c=image(rd,PROT_READ);assert(c[0]=='A' && c[1]=='A');
    assert(!close(rd));assert(c[4096]=='A');
    puts("FILECACHE_GUEST private write and mprotect COW isolation: PASS");
    int wr=open(path,O_WRONLY);assert(wr>=0);
    assert(pwrite(wr,"B",1,0)==1 && !close(wr));new_value('B');
    assert(c[0]=='A' && a[0]=='a' && b[0]=='A');
    wr=open(path,O_WRONLY);assert(wr>=0);
    assert(!ftruncate(wr,4096) && !ftruncate(wr,8192) && !close(wr));
    rd=open(path,O_RDONLY);assert(rd>=0);unsigned char *t=image(rd,PROT_READ);
    assert(t[0]=='B' && !t[4096] && c[4096]=='A');
    assert(!close(rd) && !munmap(t,8192));
    puts("FILECACHE_GUEST write/truncate new content and old snapshots: PASS");
    char replacement[256];snprintf(replacement,sizeof(replacement),"%s.new",path);
    replace('C',replacement);assert(!rename(replacement,path));new_value('C');
    assert(c[0]=='A' && c[4096]=='A');
    puts("FILECACHE_GUEST rename replacement identity: PASS");
    wr=open(path,O_RDWR);assert(wr>=0);
    unsigned char *shared=mmap(NULL,8192,PROT_READ|PROT_WRITE,MAP_SHARED,wr,0);
    assert(shared!=MAP_FAILED && !close(wr));
    shared[0]='D';new_value('D');
    rd=open(path,O_RDONLY);assert(rd>=0);t=image(rd,PROT_READ);assert(t[0]=='D');
    shared[0]='E';new_value('E');assert(t[0]=='D');
    assert(!munmap(shared,8192));new_value('E');
    assert(!close(rd) && !munmap(t,8192));
    rd=open(path,O_RDONLY);assert(rd>=0);t=image(rd,PROT_READ);
    assert(!unlink(path));unsigned char *deleted=image(rd,PROT_READ);
    assert(deleted[0]=='E' && t[0]=='E');
    assert(!close(rd) && !munmap(t,8192) && !munmap(deleted,8192));
    puts("FILECACHE_GUEST shared writable mmap repeat stores, close-FD lifetime and unlink: PASS");
    assert(!munmap(a,8192) && !munmap(b,8192) && !munmap(c,8192));
    const int64_t sizes[]={INT64_C(2147483647),INT64_C(2147483648),
        INT64_C(3221225472),INT64_C(4294971392)};
    for (unsigned i=0;i<sizeof(sizes)/sizeof(sizes[0]);++i) {
        wr=open(path,O_CREAT|O_TRUNC|O_RDWR,0600);assert(wr>=0);
        assert(!ftruncate(wr,sizes[i]));
        shared=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,wr,0);
        assert(shared!=MAP_FAILED);shared[0]=(unsigned char)i;
        assert(!munmap(shared,4096) && !close(wr) && !unlink(path));
    }
    puts("FILECACHE_GUEST shared sparse file 2GiB-1/2GiB/3GiB/4GiB+4KiB: PASS");
    if (argc>2) dso(argv[2]);
    puts("FILECACHE_GUEST ALL PASS");
    return 0;
}
