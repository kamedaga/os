/* Native-kernel contention through the existing Linux personality. No OSS
 * changes. Every phase has a host acknowledgement outside the timed region. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static const char *self;
static int family, iterations, gate[2];
static int shared_pair[2];
static uint64_t ns(void) { struct timespec t; if(clock_gettime(CLOCK_MONOTONIC,&t)) abort(); return (uint64_t)t.tv_sec*1000000000+t.tv_nsec; }
static int transfer(int fd, void *p, size_t n, int writing) {
    while(n) { ssize_t k=writing?write(fd,p,n):read(fd,p,n); if(k<0&&errno==EINTR)continue; if(k<=0)return -1; p=(char*)p+k;n-=k; } return 0;
}
static int checkpoint(const char *kind,int id,uint64_t elapsed,int ops,int errors) {
    int s=socket(AF_INET,SOCK_STREAM,0); struct sockaddr_in a={.sin_family=AF_INET,.sin_port=htons(18087)};
    inet_pton(AF_INET,"10.0.2.2",&a.sin_addr);
    if(s<0||connect(s,(void*)&a,sizeof a)) {perror("checkpoint connect");exit(90);}
    char b[512]; int n=snprintf(b,sizeof b,"GET /%s?id=%d&ns=%llu&ops=%d&errors=%d HTTP/1.0\r\nHost: host\r\n\r\n",kind,id,(unsigned long long)elapsed,ops,errors);
    if(transfer(s,b,n,1))exit(91);
    size_t used=0;
    while((n=read(s,b+used,sizeof(b)-1-used))>0){used+=(size_t)n;if(used==sizeof(b)-1)exit(94);}
    if(n<0||!used)exit(92);b[used]=0;close(s);
    if(!strstr(b,"200 OK"))exit(93);
    char *body=strstr(b,"\r\n\r\n");if(!body)exit(95);
    return !strcmp(body+4,"RETRY\n");
}
static int operations(int shared) {
    int pair[2]={-1,-1}, errors=0;
    if(family==2) {
        if(shared) {pair[0]=shared_pair[0];pair[1]=shared_pair[1];}
        else if(socketpair(AF_UNIX,SOCK_STREAM,0,pair))return 1;
    }
    for(int i=0;i<iterations;i++) {
        if(family==0) {
            size_t bytes=(i%8==0?256:16)*4096;
            volatile unsigned char *p=mmap(0,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
            if(p==MAP_FAILED){errors++;break;}
            for(size_t j=0;j<bytes;j+=4096)p[j]=(unsigned char)(j/4096+1);
            for(size_t j=0;j<bytes;j+=4096)if(p[j]!=(unsigned char)(j/4096+1))errors++;
            if(munmap((void*)p,bytes))errors++;
        } else if(family==1) {
            pid_t p=fork(); if(p==0){execl(self,self,"child",(char*)0);_exit(71);}
            int status=0;if(p<0||waitpid(p,&status,0)!=p||!WIFEXITED(status)||WEXITSTATUS(status))errors++;
        } else if(family==2) {
            char data[64];memset(data,0x5a,sizeof data);
            if(transfer(pair[0],data,sizeof data,1)||transfer(pair[1],data,sizeof data,0))errors++;
            for(unsigned j=0;j<sizeof data;j++)if(data[j]!=0x5a)errors++;
        } else {
            int fd=eventfd(0,EFD_CLOEXEC), d=fd<0?-1:dup(fd);
            if(fd<0||d<0)errors++;
            if(d>=0&&close(d))errors++;
            if(fd>=0&&close(fd))errors++;
        }
        if(errors)break;
    }
    if(family==2&&!shared){close(pair[0]);close(pair[1]);}
    return errors;
}
static void *thread(void *arg) { char c;if(transfer(gate[0],&c,1,0))return (void*)1;return (void*)(uintptr_t)operations((int)(uintptr_t)arg); }
int main(int argc,char **argv) {
    if(argc==2&&!strcmp(argv[1],"child"))return 0;
    self=argv[0];setvbuf(stdout,0,_IONBF,0);puts("S7_WORKLOAD_BEGIN");
    int id=0;
    for(family=0;family<4;family++) for(int different=0;different<2;different++) for(int n=1;n<=4;n*=2) for(int trial=0;trial<3;trial++,id++) {
        iterations=family==1?8:family==0?128:1024;
        int saved_iterations=iterations;iterations=4;if(operations(0))return 8;iterations=saved_iterations;
        printf("S7_PHASE_READY id=%d family=%d separate=%d workers=%d trial=%d iterations=%d\n",id,family,different,n,trial,iterations);
        if(pipe(gate))return 2;
        if(family==2&&!different&&socketpair(AF_UNIX,SOCK_STREAM,0,shared_pair))return 3;
        pthread_t tids[4];pid_t pids[4];
        for(int i=0;i<n;i++) {
            if(different){pids[i]=fork();if(pids[i]<0)return 4;if(!pids[i]){_exit((int)(uintptr_t)thread(0));}}
            else if(pthread_create(&tids[i],0,thread,(void*)(uintptr_t)(family==2)))return 5;
        }
        checkpoint("ready",id,0,0,0);
        uint64_t start=ns();char go[4]={0};if(transfer(gate[1],go,n,1))return 6;
        int errors=0;
        for(int i=0;i<n;i++) {
            if(different){int status=0;if(waitpid(pids[i],&status,0)!=pids[i]||!WIFEXITED(status)||WEXITSTATUS(status))errors++;}
            else {void *v;if(pthread_join(tids[i],&v)||v)errors++;}
        }
        uint64_t elapsed=ns()-start;
        printf("S7_PHASE_DONE id=%d ns=%llu ops=%d errors=%d\n",id,(unsigned long long)elapsed,n*iterations,errors);
        int retry=checkpoint("done",id,elapsed,n*iterations,errors);
        close(gate[0]);close(gate[1]);if(family==2&&!different){close(shared_pair[0]);close(shared_pair[1]);}
        if(errors)return 7;
        if(retry){trial--;id--;}
    }
    puts("S7_WORKLOAD_DONE");return 0;
}
