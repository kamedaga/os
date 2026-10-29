#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(test) do { if (!(test)) { \
    printf("MLOCK_PROBE=FAIL line=%d errno=%d\n",__LINE__,errno);return 1; \
} } while (0)

int main(void)
{
    size_t page=(size_t)sysconf(_SC_PAGESIZE);
    unsigned char *p=mmap(NULL,3*page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CHECK(p!=MAP_FAILED);
    p[0]=19;
    CHECK(!mlock(p,page));
    for (unsigned i=0;i<20;++i) {
        CHECK(!mprotect(p,page,PROT_NONE));
        CHECK(!mprotect(p,page,PROT_READ|PROT_WRITE));
        CHECK(__atomic_fetch_add(p,1,__ATOMIC_SEQ_CST)==19+i);
    }
    CHECK(!munlock(p,page) && p[0]==39);
    struct rlimit limit;
    CHECK(!getrlimit(RLIMIT_MEMLOCK,&limit));limit.rlim_cur=page;
    CHECK(!setrlimit(RLIMIT_MEMLOCK,&limit));
    CHECK(!mlock(p,page) && !mlock(p,page));
    errno=0;CHECK(mlock(p+page,page)==-1 && errno==ENOMEM);
    pid_t child=fork();CHECK(child>=0);
    if (!child) _exit(mlock(p+page,page)!=0);
    int status=0;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    CHECK(!munmap(p,page));
    CHECK(!mlock(p+page,page));
    CHECK(mmap(p+page,page,PROT_READ|PROT_WRITE,
        MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)==p+page);
    CHECK(!mlock(p+2*page,page));
    CHECK(!munmap(p+page,2*page));
    puts("MLOCK_PROBE=PASS guard-toggle=20 limit overlap fork unmap fixed-replace");
    return 0;
}
