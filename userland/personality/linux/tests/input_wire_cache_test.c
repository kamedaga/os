#define _GNU_SOURCE
#include "../runtime/lpr_input/wire_cache.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
static unsigned creates,destroys;static int failure;
static void lock(volatile uint32_t*w){while(__atomic_exchange_n(w,1,__ATOMIC_ACQUIRE))syscall(SYS_futex,w,FUTEX_WAIT_PRIVATE,1,0,0,0);}
static void unlock(volatile uint32_t*w){__atomic_store_n(w,0,__ATOMIC_RELEASE);syscall(SYS_futex,w,FUTEX_WAKE_PRIVATE,1,0,0,0);}
static int create(void**p){if(failure)return -24;int raw=memfd_create("input-wire-test",MFD_CLOEXEC);assert(raw>=0);int fd=fcntl(raw,F_DUPFD_CLOEXEC,16);close(raw);assert(fd>=16&&!ftruncate(fd,4096));*p=mmap(0,4096,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);assert(*p!=MAP_FAILED);creates++;return fd;}
static void destroy(int fd,void*p){if(p)assert(!munmap(p,4096));if(fd>=16)assert(!close(fd));destroys++;}
static const struct lpr_input_wire_io io={lock,unlock,create,destroy};static struct lpr_input_wire_cache cache={.fd=-1};
static void*worker(void*a){(void)a;for(unsigned i=0;i<2000;i++){void*p;assert(lpr_input_wire_cache_acquire(&cache,&io,&p)>=16);(*(uint64_t*)p)++;lpr_input_wire_cache_release(&cache,&io);}return 0;}
int main(void){void*p=(void*)1;failure=1;assert(lpr_input_wire_cache_acquire(&cache,&io,&p)==-24&&!p&&!cache.lock);failure=0;assert(lpr_input_wire_cache_acquire(&cache,&io,&p)>=16);*(uint64_t*)p=0;lpr_input_wire_cache_release(&cache,&io);pthread_t t[4];for(int i=0;i<4;i++)assert(!pthread_create(&t[i],0,worker,0));for(int i=0;i<4;i++)assert(!pthread_join(t[i],0));assert(creates==1&&*(uint64_t*)cache.page==8000);
 lock(&cache.lock);int inherited=cache.fd;*(uint64_t*)cache.page=19;pid_t child=fork();assert(child>=0);if(!child){lpr_input_wire_cache_child(&cache,&io);assert(!cache.page&&cache.fd==-1&&!cache.lock);errno=0;assert(fcntl(inherited,F_GETFD)==-1&&errno==EBADF);assert(lpr_input_wire_cache_acquire(&cache,&io,&p)>=16);*(uint64_t*)p=42;assert(fcntl(cache.fd,F_GETFD)&FD_CLOEXEC);lpr_input_wire_cache_release(&cache,&io);lpr_input_wire_cache_child(&cache,&io);_exit(0);}unlock(&cache.lock);int status;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));assert(*(uint64_t*)cache.page==19);assert(creates==1);lpr_input_wire_cache_child(&cache,&io);assert(destroys==1);puts("PASS: allocation failure, concurrent lease integrity/reuse, fork cleanup/isolation, CLOEXEC");}
