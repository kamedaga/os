#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/memfd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <unistd.h>
static void error_is(int fd,int mode,off_t off,off_t len,int e){errno=0;assert(fallocate(fd,mode,off,len)==-1&&errno==e);}
int main(void){
 char path[]="/tmp/fallocate-oracle-XXXXXX";int fd=mkstemp(path);assert(fd>=0);struct stat s;
 assert(write(fd,"seed",4)==4);assert(!fallocate(fd,0,0,4096));assert(!fstat(fd,&s)&&s.st_size==4096&&s.st_blocks>=8);
 assert(!fallocate(fd,FALLOC_FL_KEEP_SIZE,8192,4096));assert(!fstat(fd,&s)&&s.st_size==4096&&s.st_blocks>=16);
 char buf[4];assert(pread(fd,buf,4,0)==4&&buf[0]=='s');
 error_is(fd,0,-1,4096,EINVAL);error_is(fd,0,0,0,EINVAL);error_is(fd,-1,0,4096,EOPNOTSUPP);
 assert(!fallocate(fd,FALLOC_FL_PUNCH_HOLE|FALLOC_FL_KEEP_SIZE,0,4096));assert(pread(fd,buf,4,0)==4&&!buf[0]&&!buf[1]);
 int ro=open(path,O_RDONLY),p[2],sock=socket(AF_UNIX,SOCK_STREAM,0),dir=open("/tmp",O_RDONLY|O_DIRECTORY);assert(ro>=0&&sock>=0&&dir>=0&&!pipe(p));
 error_is(ro,0,0,4096,EBADF);error_is(p[0],0,0,4096,EBADF);error_is(p[1],0,0,4096,ESPIPE);error_is(sock,0,0,4096,ENODEV);error_is(dir,0,0,4096,EBADF);error_is(-1,0,0,4096,EBADF);
 int sealed=syscall(SYS_memfd_create,"fallocate-seals",MFD_ALLOW_SEALING);assert(sealed>=0);assert(!ftruncate(sealed,4096));assert(!fcntl(sealed,F_ADD_SEALS,F_SEAL_GROW));error_is(sealed,0,4096,4096,EPERM);assert(!fcntl(sealed,F_ADD_SEALS,F_SEAL_WRITE));error_is(sealed,FALLOC_FL_PUNCH_HOLE|FALLOC_FL_KEEP_SIZE,0,4096,EPERM);close(sealed);
 const char *quota=getenv("FALLOCATE_QUOTA_DIR");if(!quota)quota="/dev/shm";struct statvfs space;int enospc=0;if(!statvfs(quota,&space)){uint64_t available=(uint64_t)space.f_bavail*space.f_frsize;if(available>4096&&available<=128*1024*1024){char tmp[1024];assert(snprintf(tmp,sizeof tmp,"%s/fallocate-enospc-XXXXXX",quota)>0);int full=mkstemp(tmp);assert(full>=0);error_is(full,0,0,available+4096,ENOSPC);close(full);unlink(tmp);enospc=1;}}
 close(fd);close(ro);close(p[0]);close(p[1]);close(sock);close(dir);unlink(path);printf("FALLOCATE_LINUX_ORACLE_PASS reserve/KEEP_SIZE/blocks/content/hole/range/mode/types/seals ENOSPC=%s\n",enospc?"PASS":"SKIP(no bounded tmpfs)");
}
