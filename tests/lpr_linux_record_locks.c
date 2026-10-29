#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { dprintf(2,"RECORD_LOCKS=FAIL line=%d errno=%d\n",__LINE__,errno);exit(1); } } while (0)
static int lock_range(int fd,int command,int type,off_t start,off_t length)
{
    struct flock lock={.l_type=type,.l_whence=SEEK_SET,.l_start=start,.l_len=length};
    return fcntl(fd,command,&lock);
}
static void byte_out(int fd) { CHECK(write(fd,"x",1)==1); }
static void byte_in(int fd) { char b;CHECK(read(fd,&b,1)==1); }
static void wait_ok(pid_t pid) { int status;CHECK(waitpid(pid,&status,0)==pid && WIFEXITED(status) && !WEXITSTATUS(status)); }
static void noop(int sig) { (void)sig; }
int main(int argc,char **argv)
{
    const char *path=argc>1 ? argv[1] : "/tmp/lpr-record-lock-test";
    int fd=open(path,O_CREAT|O_TRUNC|O_RDWR,0600);CHECK(fd>=0);
    CHECK(write(fd,"0123456789abcdef",16)==16);
    CHECK(!lock_range(fd,F_SETLK,F_WRLCK,0,4));
    int ready[2],go[2];CHECK(!pipe(ready) && !pipe(go));
    pid_t child=fork();CHECK(child>=0);
    if (!child) {
        CHECK(lock_range(fd,F_SETLK,F_WRLCK,0,4)==-1 && errno==EAGAIN);
        CHECK(!lock_range(fd,F_SETLK,F_WRLCK,8,-4));
        byte_out(ready[1]);byte_in(go[0]);
        CHECK(!lock_range(fd,F_SETLKW,F_WRLCK,0,4));
        byte_out(ready[1]);
        _exit(0); /* Process retirement must release the owned ranges. */
    }
    byte_in(ready[0]);
    CHECK(lock_range(fd,F_SETLK,F_WRLCK,4,4)==-1 && errno==EAGAIN);
    byte_out(go[1]);
    struct pollfd blocked={.fd=ready[0],.events=POLLIN};
    CHECK(poll(&blocked,1,30)==0);
    CHECK(!lock_range(fd,F_SETLK,F_UNLCK,0,4));byte_in(ready[0]);wait_ok(child);
    CHECK(!lock_range(fd,F_SETLK,F_WRLCK,0,8));
    int alias=dup(fd);CHECK(alias>=0);CHECK(!close(alias));
    child=fork();CHECK(child>=0);
    if (!child) { CHECK(!lock_range(fd,F_SETLK,F_WRLCK,0,8));_exit(0); }
    wait_ok(child);
    CHECK(!lock_range(fd,F_SETLK,F_WRLCK,0,8));
    int target=dup(fd),other=open("/dev/null",O_RDONLY);CHECK(target>=0 && other>=0);
    CHECK(dup2(other,target)==target); /* Replacing an alias is also a close. */
    child=fork();CHECK(child>=0);
    if (!child) { CHECK(!lock_range(fd,F_SETLK,F_WRLCK,0,8));_exit(0); }
    wait_ok(child);CHECK(!close(target) && !close(other));
    CHECK(!lock_range(fd,F_SETLK,F_WRLCK,0,4));
    child=fork();CHECK(child>=0);
    if (!child) {
        struct sigaction action={.sa_handler=noop};sigemptyset(&action.sa_mask);
        CHECK(!sigaction(SIGUSR1,&action,NULL));byte_out(ready[1]);
        CHECK(lock_range(fd,F_SETLKW,F_WRLCK,0,4)==-1 && errno==EINTR);
        _exit(0);
    }
    byte_in(ready[0]);usleep(20000);CHECK(!kill(child,SIGUSR1));wait_ok(child);
    CHECK(!lock_range(fd,F_SETLK,F_UNLCK,0,0));
    int ro=open(path,O_RDONLY);CHECK(ro>=0);
    CHECK(lock_range(ro,F_SETLK,F_WRLCK,0,1)==-1 && errno==EBADF);
    CHECK(!lock_range(ro,F_SETLK,F_RDLCK,0,1));CHECK(!close(ro));
    CHECK(!close(fd) && !unlink(path));
    puts("RECORD_LOCKS=PASS conflict disjoint negative-length fork wait exit dup-close dup2 signal read-only");
    return 0;
}
