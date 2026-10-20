/* Stage10-A: Linux-personality end-to-end costs, never context-switch-only.
 * HTTP barriers are outside timing. No rootfs installation is needed. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static uint64_t ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
static const char *host="10.0.2.2";
static int barrier(const char *kind,const char *phase,uint64_t elapsed,unsigned ops){
 int s=socket(AF_INET,SOCK_STREAM,0);struct sockaddr_in a={.sin_family=AF_INET,.sin_port=htons(18089)};inet_pton(AF_INET,host,&a.sin_addr);
 if(s<0||connect(s,(void*)&a,sizeof a))exit(90);
 char b[1024];int n=snprintf(b,sizeof b,"GET /%s?fs=micro&phase=%s&ns=%llu&ops=%u HTTP/1.0\r\nHost: host\r\n\r\n",kind,phase,(unsigned long long)elapsed,ops);
 for(int off=0;off<n;){int k=write(s,b+off,n-off);if(k<=0)exit(91);off+=k;}
 size_t used=0;while((n=read(s,b+used,sizeof b-1-used))>0){used+=n;if(used==sizeof b-1)exit(92);}close(s);b[used]=0;
 if(n<0||!strstr(b,"200 OK"))exit(93);return strstr(b,"RETRY\n")!=NULL;
}
static volatile unsigned word;
static int fut(unsigned *p,int op,unsigned v,const struct timespec *t){return syscall(SYS_futex,p,op,v,t,0,0);}
static int pair[2];static unsigned rounds;static int peer_error;
static void *peer(void *arg){int mode=(intptr_t)arg;
 for(unsigned i=0;i<rounds;i++){
  if(mode==0){while(__atomic_load_n(&word,__ATOMIC_ACQUIRE)!=1)fut((unsigned*)&word,FUTEX_WAIT,0,0);__atomic_store_n(&word,0,__ATOMIC_RELEASE);fut((unsigned*)&word,FUTEX_WAKE,1,0);}
  else {char c;if(read(pair[1],&c,1)!=1||write(pair[1],&c,1)!=1){peer_error=1;break;}}
 }return 0;
}
static void *empty_thread(void *unused){(void)unused;return 0;}
static int one(int mode,unsigned size){
 if(mode==0)return syscall(SYS_getpid)<0;
 if(mode==1)return sched_yield()!=0;
 if(mode==2){errno=0;return !(fut((unsigned*)&word,FUTEX_WAIT,1,0)==-1&&errno==EAGAIN);}
 if(mode==3)return fut((unsigned*)&word,FUTEX_WAKE,1,0)!=0;
 if(mode==4){struct timespec t={0,1000000};errno=0;return !(fut((unsigned*)&word,FUTEX_WAIT,0,&t)==-1&&errno==ETIMEDOUT);}
 if(mode==5){struct timespec t={0,1000000};return nanosleep(&t,0)!=0;}
 if(mode==6){int f=eventfd(0,0);if(f<0)return 1;int d=dup(f);int bad=d<0;if(d>=0)bad|=close(d)!=0;bad|=close(f)!=0;return bad;}
 if(mode==7){size_t bytes=size*4096;volatile char *p=mmap(0,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);if(p==MAP_FAILED)return 1;for(unsigned i=0;i<size;i++)p[i*4096]=1;return munmap((void*)p,bytes)!=0;}
 if(mode==8){size_t bytes=size*4096;char *p=mmap(0,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);if(p==MAP_FAILED)return 1;memset(p,1,bytes);pid_t c=fork();if(!c){for(unsigned i=0;i<size;i++)p[i*4096]=2;_exit(0);}int st=0,bad=c<0||waitpid(c,&st,0)!=c||st;for(unsigned i=0;i<size;i++)bad|=p[i*4096]!=1;bad|=munmap(p,bytes)!=0;return bad;}
 if(mode==9){pid_t c=fork();if(!c)_exit(0);int st=0;return c<0||waitpid(c,&st,0)!=c||st;}
 if(mode==10){pthread_t t;return pthread_create(&t,0,empty_thread,0)||pthread_join(t,0);}
 if(mode==11){size_t bytes=size*4096;void *p=mmap(0,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);if(p==MAP_FAILED)return 1;memset(p,1,bytes);int bad=mprotect(p,bytes,PROT_READ)!=0;bad|=mprotect(p,bytes,PROT_READ|PROT_WRITE)!=0;bad|=munmap(p,bytes)!=0;return bad;}
 if(mode==12){int p[2];if(socketpair(AF_UNIX,SOCK_STREAM,0,p))return 1;char b[64]={0};int bad=write(p[0],b,sizeof b)!=sizeof b||read(p[1],b,sizeof b)!=sizeof b;bad|=close(p[0])||close(p[1]);return bad;}
 if(mode==13){int p[2];if(socketpair(AF_UNIX,SOCK_DGRAM,0,p))return 1;int f=open("/tmp/s10-transfer",O_CREAT|O_TRUNC|O_RDWR,0600);if(f<0)return 1;char c=1,control[CMSG_SPACE(sizeof(int))];memset(control,0,sizeof control);struct iovec io={&c,1};struct msghdr m={.msg_iov=&io,.msg_iovlen=1,.msg_control=control,.msg_controllen=sizeof control};struct cmsghdr *h=CMSG_FIRSTHDR(&m);h->cmsg_level=SOL_SOCKET;h->cmsg_type=SCM_RIGHTS;h->cmsg_len=CMSG_LEN(sizeof(int));memcpy(CMSG_DATA(h),&f,sizeof f);int bad=sendmsg(p[0],&m,MSG_DONTWAIT)!=1; if(bad){perror("SCM_RIGHTS send");close(f);close(p[0]);close(p[1]);unlink("/tmp/s10-transfer");return 1;} memset(control,0,sizeof control);bad=recvmsg(p[1],&m,MSG_DONTWAIT)!=1;h=CMSG_FIRSTHDR(&m);if(!h||h->cmsg_type!=SCM_RIGHTS)bad=1;else{int got;memcpy(&got,CMSG_DATA(h),sizeof got);bad|=close(got)!=0;}close(f);close(p[0]);close(p[1]);unlink("/tmp/s10-transfer");return bad;}
 return 1;
}
static int cmp(const void *a,const void *b){uint64_t x=*(const uint64_t*)a,y=*(const uint64_t*)b;return (x>y)-(x<y);}
static int measure(const char *name,int mode,unsigned size,unsigned ops){
 for(int trial=0;trial<3;trial++)for(int retry=0;;retry++){
  if(retry>3)return 1;barrier("ready",name,0,0);uint64_t sample[256],start=ns();int bad=0,first=sched_getcpu(),migrations=0,previous=first;
  for(unsigned i=0;i<ops;i++){uint64_t t=ns();bad|=one(mode,size);sample[i]=ns()-t;int cpu=sched_getcpu();migrations+=cpu!=previous;previous=cpu;if(bad)break;}
  uint64_t elapsed=ns()-start;if(bad){fprintf(stderr,"MICRO_FAIL phase=%s errno=%d\n",name,errno);return 1;}
  if(barrier("done",name,elapsed,ops))continue;qsort(sample,ops,sizeof sample[0],cmp);
  printf("MICRO phase=%s trial=%d size=%u ops=%u ns=%llu p50=%llu p95=%llu p99=%llu cpu_first=%d cpu_last=%d migrations=%d\n",name,trial,size,ops,(unsigned long long)elapsed,(unsigned long long)sample[ops/2],(unsigned long long)sample[(ops-1)*95/100],(unsigned long long)sample[(ops-1)*99/100],first,previous,migrations);break;
 }return 0;
}
int main(int argc,char **argv){if(argc>1)host=argv[1];setvbuf(stdout,0,_IONBF,0);
 const char *names[]={"getpid_entry","yield","futex_mismatch","futex_wake_empty","futex_timeout","timer_sleep","fd_create_dup_close","fault_touch","cow_fork_touch","process_fork_exit","thread_create_exit","tlb_protect","unix_message_lifecycle","unix_fd_transfer"};
 for(int m=0;m<14;m++){if(m==7||m==8||m==11){for(unsigned n=1;n<=256;n*=16){char name[64];snprintf(name,sizeof name,"%s_%u",names[m],n);if(measure(name,m,n,m==8?8:32))return 1;}}else if(measure(names[m],m,1,m==4||m==5||m==9||m==10||m==13?16:128))return 1;}
 for(int mode=0;mode<2;mode++)for(int trial=0;trial<3;trial++){
  const char *name=mode?"unix_request_reply":"futex_wait_wake";rounds=128;word=0;peer_error=0;if(mode&&socketpair(AF_UNIX,SOCK_STREAM,0,pair))return 1;
  pthread_t t;if(pthread_create(&t,0,peer,(void*)(intptr_t)mode))return 1;barrier("ready",name,0,0);uint64_t start=ns();
  for(unsigned i=0;i<rounds;i++){if(!mode){__atomic_store_n(&word,1,__ATOMIC_RELEASE);fut((unsigned*)&word,FUTEX_WAKE,1,0);while(__atomic_load_n(&word,__ATOMIC_ACQUIRE)!=0)fut((unsigned*)&word,FUTEX_WAIT,1,0);}else{char c=1;if(write(pair[0],&c,1)!=1||read(pair[0],&c,1)!=1)return 1;}}
  uint64_t elapsed=ns()-start;if(pthread_join(t,0)||peer_error)return 1;if(mode){close(pair[0]);close(pair[1]);}if(barrier("done",name,elapsed,rounds)){trial--;continue;}
  printf("MICRO phase=%s trial=%d ops=%u ns=%llu\n",name,trial,rounds,(unsigned long long)elapsed);
 }
 for(int i=0;i<3;i++){barrier("ready","idle_1s",0,0);uint64_t t=ns();sleep(1);if(barrier("done","idle_1s",ns()-t,1))i--;}
 puts("MICRO_DONE");return 0;}
