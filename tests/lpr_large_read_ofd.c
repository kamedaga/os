#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

/* Run unchanged on Linux and on the LPR guest. Every large READ must observe
 * one contiguous OFD cursor interval even across fork and dup. */
enum { WORDS=524288, CHUNK=16384 };
static unsigned char *seen;
static int shared_fd;
static void *reader(void *unused)
{
    (void)unused;
    uint64_t *buf=malloc(CHUNK*sizeof(*buf)); assert(buf);
    int fd=dup(shared_fd); assert(fd>=0);
    for (;;) {
        ssize_t n=read(fd,buf,CHUNK*sizeof(*buf));
        assert(n>=0 && !(n%8));
        if (!n) break;
        for (size_t k=0;k<(size_t)n/8;++k) {
            assert(buf[k]<WORDS);
            if (k) assert(buf[k]==buf[k-1]+1);
            assert(!__atomic_exchange_n(&seen[buf[k]],1,__ATOMIC_SEQ_CST));
        }
    }
    assert(!close(fd)); free(buf); return NULL;
}
int main(void)
{
    alarm(60);
    char name[]="/tmp/lpr-large-ofd-XXXXXX";
    shared_fd=mkstemp(name); assert(shared_fd>=0); assert(!unlink(name));
    uint64_t data[4096];
    for (size_t base=0;base<WORDS;base+=4096) {
        for (size_t i=0;i<4096;++i)data[i]=base+i;
        assert(write(shared_fd,data,sizeof(data))==(ssize_t)sizeof(data));
    }
    seen=mmap(NULL,WORDS,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
    assert(seen!=MAP_FAILED && lseek(shared_fd,0,SEEK_SET)==0);
    fflush(NULL); pid_t ch=fork(); assert(ch>=0);
    pthread_t thread; assert(!pthread_create(&thread,NULL,reader,NULL));
    reader(NULL); assert(!pthread_join(thread,NULL));
    if (!ch) _exit(0);
    int status; assert(waitpid(ch,&status,0)==ch && WIFEXITED(status) && !WEXITSTATUS(status));
    for (size_t i=0;i<WORDS;++i)assert(seen[i]);
    assert(lseek(shared_fd,0,SEEK_CUR)==WORDS*8);
    uint64_t *buf=malloc(CHUNK*8); assert(buf);
    assert(!read(shared_fd,buf,CHUNK*8));
    assert(!ftruncate(shared_fd,19)); assert(!lseek(shared_fd,0,SEEK_SET));
    assert(read(shared_fd,buf,CHUNK*8)==19);
    assert(!read(shared_fd,buf,CHUNK*8));
    assert(pread(shared_fd,buf,CHUNK*8,8)==11);
    assert(lseek(shared_fd,0,SEEK_CUR)==19);
    assert(pread(shared_fd,buf,CHUNK*8,-1)==-1 && errno==EINVAL);
    assert(lseek(shared_fd,0,SEEK_CUR)==19);
    assert(!close(shared_fd)); free(buf); assert(!munmap(seen,WORDS));
    puts("LPR large READ fork/dup/thread OFD intervals, short read, EOF, pread: PASS");
    return 0;
}
