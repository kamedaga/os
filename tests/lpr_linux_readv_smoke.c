#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr,"READV_FAIL line=%d errno=%d\n",__LINE__,errno); return 1; } } while (0)
enum { RECORDS=256, RECORD_BYTES=64 };
static unsigned char records[RECORDS][RECORD_BYTES];
static unsigned seen[RECORDS];
static int worker_error;
static void *reader(void *arg)
{
    int fd = *(int *)arg;
    for (;;) {
        unsigned char buf[RECORD_BYTES];
        struct iovec vec[] = {{buf,31},{buf+31,33}};
        ssize_t n = readv(fd,vec,2);
        if (!n) return NULL;
        if (n != RECORD_BYTES || memcmp(buf,records[buf[0]],RECORD_BYTES)) {
            __atomic_store_n(&worker_error,1,__ATOMIC_RELAXED); return NULL;
        }
        __atomic_fetch_add(&seen[buf[0]],1,__ATOMIC_RELAXED);
    }
}
int main(void)
{
    char path[] = "/tmp/readv-stream-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    for (unsigned i=0;i<RECORDS;++i)
        for (unsigned j=0;j<RECORD_BYTES;++j) records[i][j]=(unsigned char)i;
    CHECK(write(fd,records,sizeof(records)) == sizeof(records));
    CHECK(lseek(fd,0,SEEK_SET) == 0);
    int alias = dup(fd);
    CHECK(alias >= 0);
    unsigned char a[1024], b[19];
    struct iovec vec[] = {{a,sizeof(a)},{NULL,0},{b,sizeof(b)}};
    CHECK(readv(alias,vec,3) == 1043);
    CHECK(lseek(fd,0,SEEK_CUR) == 1043);
    CHECK(!memcmp(a,records,1024) && !memcmp(b,(unsigned char *)records+1024,19));
    CHECK(pwrite(fd,"xy",2,1100) == 2 && lseek(alias,0,SEEK_CUR) == 1043);
    CHECK(lseek(fd,sizeof(records)-1029,SEEK_SET) == sizeof(records)-1029);
    memset(b,0x7f,sizeof(b));
    CHECK(readv(alias,vec,3) == 1029 && b[5] == 0x7f);
    CHECK(readv(fd,vec,3) == 0);
    CHECK(lseek(fd,0,SEEK_SET) == 0);
    vec[2].iov_base = NULL;
    CHECK(readv(fd,vec,3) == 1024 && lseek(alias,0,SEEK_CUR) == 1024);
    vec[2].iov_base = b;
    CHECK(lseek(fd,0,SEEK_SET) == 0);
    vec[0].iov_base = NULL;
    CHECK(readv(fd,vec,3) == -1 && errno == EFAULT);
    CHECK(lseek(fd,0,SEEK_CUR) == 0);
    vec[0].iov_base = a;
    int write_only = open(path,O_WRONLY);
    CHECK(write_only >= 0);
    CHECK(readv(write_only,vec,3) == -1 && errno == EBADF);
    CHECK(close(write_only) == 0);
    CHECK(fcntl(fd,F_SETFL,O_APPEND) == 0);
    CHECK(readv(alias,vec,3) == 1043 && lseek(fd,0,SEEK_CUR) == 1043);
    CHECK(fcntl(fd,F_SETFL,0) == 0);
    CHECK(pwrite(fd,records,sizeof(records),0) == sizeof(records));
    CHECK(lseek(fd,0,SEEK_SET) == 0);
    pthread_t threads[2];
    CHECK(!pthread_create(&threads[0],NULL,reader,&fd));
    CHECK(!pthread_create(&threads[1],NULL,reader,&alias));
    CHECK(!pthread_join(threads[0],NULL) && !pthread_join(threads[1],NULL));
    CHECK(!worker_error);
    for (unsigned i=0;i<RECORDS;++i) CHECK(seen[i] == 1);
    CHECK(lseek(fd,0,SEEK_CUR) == sizeof(records));
    CHECK(close(alias) == 0 && close(fd) == 0 && unlink(path) == 0);
    puts("READV_SHARED_STREAM_PASS");
    return 0;
}
