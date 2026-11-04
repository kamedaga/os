#define _GNU_SOURCE
#include "../userland/personality/linux/runtime/lpr_filed_internal.h"
#include "../userland/personality/linux/runtime/lpr_wait.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
lpr_state_t lpr_state;
static unsigned pins,calls,sleeps,local;
static int reuse_fd=-1, unlock_fd=-1;
static volatile sig_atomic_t interrupted;
static lpr_filed_backend_t held;
void lpr_fd_arrays_init(void) {}
int lpr_fd_table_pin(lpr_fd_table_t *t,lpr_linux_fd_t fd,lpr_fd_pin_t *pin){
 assert(t==&lpr_control_fd_table&&!pins);int h=dup(fd);if(h<0)return -1;
 held=(lpr_filed_backend_t){.active=1,.handle=(uint64_t)h};*pin=(lpr_fd_pin_t){.fd=fd,.ops_id=local?LPR_FD_OPS_EVENT:LPR_FD_OPS_FILED,.state=&held};pins++;return 0;
}
void lpr_fd_unpin(const lpr_fd_pin_t *p){assert(pins==1&&p->state==&held);assert(!close(held.handle));pins--;}
int64_t lpr_filed_endpoint_ready(void){return 0;}
int64_t lpr_fs_call(kb2_fs_request_t*r,const char*a,const char*b,const void*c,uint64_t d,void*e,uint64_t f,kb2_fs_response_t*g,pacha_fd_t h,pacha_fd_t*i){
 (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;(void)i;assert(pins==1&&r->opcode==KB2_FILESYSTEM_OP_FLOCK&&(r->flags&LOCK_NB));calls++;
 return flock((int)r->handle,(int)r->flags)?-errno:0;
}
int64_t lpr_pacha_nanosleep(const struct pachaos_timespec *delay){assert(pins==1&&delay->tv_nsec==1000000);sleeps++;if(reuse_fd>=0){int fd=reuse_fd;reuse_fd=-1;assert(!close(fd));assert(open("/dev/null",O_RDONLY)==fd);assert(!flock(unlock_fd,LOCK_UN));}if(interrupted)return -LPR_LINUX_EAGAIN;struct timespec t={0,1000000};return nanosleep(&t,0)?-LPR_LINUX_EAGAIN:0;}
static void signal_seen(int n){(void)n;interrupted=1;}
static void wait_ok(pid_t p){int st;assert(waitpid(p,&st,0)==p&&WIFEXITED(st)&&WEXITSTATUS(st)==0);}
int main(void){
 char path[]="/tmp/lpr-flock-XXXXXX";int a=mkstemp(path);assert(a>=0);int b=open(path,O_RDONLY);assert(b>=0);unsigned w=0;
 assert(!lpr_linux_flock(a,LOCK_EX,&w));assert(lpr_linux_flock(b,LOCK_EX|LOCK_NB,&w)==-EWOULDBLOCK);
 int alias=dup(a);assert(alias>=0);assert(!lpr_linux_flock(alias,LOCK_SH,&w));assert(!lpr_linux_flock(b,LOCK_SH|LOCK_NB,&w));assert(lpr_linux_flock(a,LOCK_EX|LOCK_NB,&w)==-EAGAIN);assert(!lpr_linux_flock(b,LOCK_UN,&w));assert(!lpr_linux_flock(a,LOCK_EX,&w));
 assert(!close(a));assert(lpr_linux_flock(b,LOCK_EX|LOCK_NB,&w)==-EAGAIN);assert(!close(alias));assert(!lpr_linux_flock(b,LOCK_EX|LOCK_NB,&w));
 int ready[2],acquired[2];assert(!pipe(ready)&&!pipe(acquired));
 pid_t child=fork();assert(child>=0);
 if(!child){close(b);int fd=open(path,O_RDONLY);assert(fd>=0);assert(write(ready[1],"r",1)==1);assert(!lpr_linux_flock(fd,LOCK_EX,&w)&&sleeps>0);assert(write(acquired[1],"a",1)==1);assert(!lpr_linux_flock(fd,LOCK_UN,&w));close(fd);_exit(0);}
 char byte;assert(read(ready[0],&byte,1)==1);struct pollfd p={acquired[0],POLLIN,0};assert(poll(&p,1,30)==0);assert(!lpr_linux_flock(b,LOCK_UN,&w));assert(read(acquired[0],&byte,1)==1);wait_ok(child);
 assert(!lpr_linux_flock(b,LOCK_EX,&w));child=fork();assert(child>=0);
 if(!child){close(b);int fd=open(path,O_RDONLY);assert(fd>=0);struct sigaction sa={.sa_handler=signal_seen};sigemptyset(&sa.sa_mask);assert(!sigaction(SIGUSR1,&sa,0));assert(write(ready[1],"r",1)==1);assert(lpr_linux_flock(fd,LOCK_EX,&w)==LPR_WAIT_RESTART_SYSCALL&&w==1&&!pins);close(fd);_exit(0);}
 assert(read(ready[0],&byte,1)==1);usleep(20000);assert(!kill(child,SIGUSR1));wait_ok(child);
 /* close/reuse the public fd while sleeping: retry must use the pinned OFD. */
 int contender=open(path,O_RDONLY);assert(contender>=0);reuse_fd=contender;unlock_fd=b;assert(!lpr_linux_flock(contender,LOCK_EX,&w));int separate=open(path,O_RDONLY);assert(separate>=0);assert(!flock(separate,LOCK_EX|LOCK_NB)); /* unpin released final OFD, replacement fd is untouched */close(separate);close(contender);
 assert(lpr_linux_flock(b,3,&w)==-EINVAL);assert(lpr_linux_flock(UINT64_MAX,LOCK_EX,&w)==-EBADF);local=1;assert(lpr_linux_flock(b,LOCK_EX,&w)==-EOPNOTSUPP);local=0;assert(!lpr_linux_flock(b,LOCK_UN,&w));close(b);unlink(path);for(unsigned i=0;i<2;i++){close(ready[i]);close(acquired[i]);}assert(!pins&&calls);puts("LPR_FLOCK_PASS OFD exclusion/NB/SH/conversion/dup/last-close/blocking/signal-unwind/close-reuse/invalid/local unsupported");
}
