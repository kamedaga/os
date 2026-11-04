#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include "../runtime/lpr_input/client.c"
static struct inputd_event_ring ring;
static lpr_input_backend_t backend;
static uint64_t producer_tail,native_calls;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed=PTHREAD_COND_INITIALIZER;
static int interrupt_wait, publish_on_drain;
static void publish(int n);
lpr_input_backend_t *lpr_input_backend(uint64_t fd){return fd==24?&backend:0;}
int64_t lpr_pacha_syscall6(uint64_t n,uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f){(void)n;(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;native_calls++;return -1;}
int64_t lpr_pacha_syscall2(uint64_t n,uint64_t a,uint64_t b){(void)n;(void)a;(void)b;native_calls++;return -1;}
int64_t lpr_ipc_message(uint64_t n,uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,int*f){(void)n;(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;native_calls++;if(publish_on_drain){publish_on_drain=0;publish(777);}return 2;}
void lpr_wait_graph_init(lpr_wait_graph_t*g){memset(g,0,sizeof(*g));}
int64_t lpr_wait_graph_add_fd(lpr_wait_graph_t*g,uint64_t fd,uint32_t events){(void)events;int r=lpr_input_prepare_wait(fd);if(r<0)return r;g->immediate_ready=r;return 0;}
int64_t lpr_wait_deadline_init(lpr_wait_deadline_t*d,int64_t ms){(void)d;(void)ms;return 0;}
int64_t lpr_wait_graph_block(lpr_wait_graph_t*g,const lpr_wait_deadline_t*d,unsigned*w){(void)d;(void)w;if(g->immediate_ready)return 0;pthread_mutex_lock(&lock);while(!inputd_ring_ready(&ring)&&!interrupt_wait)pthread_cond_wait(&changed,&lock);int r=interrupt_wait?-4:0;pthread_mutex_unlock(&lock);return r;}
static void reset(void){memset(&ring,0,sizeof ring);producer_tail=0;native_calls=0;backend=(lpr_input_backend_t){.flags=LPR_LINUX_O_NONBLOCK,.ring_address=(uintptr_t)&ring,.wait_fd.raw=16};interrupt_wait=0;}
static void publish(int n){inputd_input_event_t e[3]={{.type=2,.code=0,.value=n},{.type=2,.code=1,.value=-n},{.type=0,.code=0}};pthread_mutex_lock(&lock);inputd_ring_publish(&ring,&producer_tail,e,3);pthread_cond_broadcast(&changed);pthread_mutex_unlock(&lock);}
static void *blocking(void*p){inputd_input_event_t e[3];long r=lpr_input_read_events(24,(uintptr_t)e,sizeof e,0);*(long*)p=r;return 0;}
static uint64_t ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1000000000ull+t.tv_nsec;}
static int compare(const void*a,const void*b){uint64_t x=*(uint64_t*)a,y=*(uint64_t*)b;return(x>y)-(x<y);}
static void bench(void){enum{N=30000};uint64_t *read_ns=malloc(N*8),*again_ns=malloc(N*8);inputd_input_event_t e[3];reset();for(int i=0;i<N;i++){publish(i);uint64_t s=ns();assert(lpr_input_read_events(24,(uintptr_t)e,72,0)==72);read_ns[i]=ns()-s;s=ns();assert(lpr_input_read_events(24,(uintptr_t)e,72,0)==-11);again_ns[i]=ns()-s;}qsort(read_ns,N,8,compare);qsort(again_ns,N,8,compare);printf("{\"scope\":\"host actual LPR C read function, mocked native boundary, no syscall entry\",\"read_p50_ns\":%llu,\"read_p99_ns\":%llu,\"eagain_p50_ns\":%llu,\"native_calls\":%llu}\n",(unsigned long long)read_ns[N/2],(unsigned long long)read_ns[N*99/100],(unsigned long long)again_ns[N/2],(unsigned long long)native_calls);assert(native_calls==0);free(read_ns);free(again_ns);}
struct concurrent{struct inputd_event_ring*r;unsigned char *seen;};
static void *reader(void*v){struct concurrent*c=v;inputd_input_event_t e[17];for(;;){int n=inputd_ring_read(c->r,e,17);if(n==-19)break;if(n==-11){sched_yield();continue;}assert(n>0);for(int i=0;i<n;i++){assert(e[i].type==2);assert(e[i].value>=0&&e[i].value<100000);assert(__atomic_fetch_add(&c->seen[e[i].value],1,__ATOMIC_RELAXED)==0);}}return 0;}
static void concurrent(void){struct inputd_event_ring*r=calloc(1,sizeof(*r));unsigned char*seen=calloc(100000,1);struct concurrent c={r,seen};pthread_t a,b;pthread_create(&a,0,reader,&c);pthread_create(&b,0,reader,&c);uint64_t t=0;for(int i=0;i<100000;i++){while(t-inputd_ring_head(r)>INPUTD_SHARED_EVENTS-2)sched_yield();inputd_input_event_t e={.type=2,.value=i};inputd_ring_publish(r,&t,&e,1);}while(inputd_ring_head(r)!=t)sched_yield();__atomic_store_n(&r->error,-19,__ATOMIC_RELEASE);pthread_join(a,0);pthread_join(b,0);for(int i=0;i<100000;i++)assert(seen[i]==1);free(seen);free(r);}
int main(void){reset();inputd_input_event_t e[INPUTD_SHARED_EVENTS];assert(lpr_input_read_events(24,(uintptr_t)e,72,0)==-11);assert(lpr_input_read_events(24,(uintptr_t)e,1,0)==-22);assert(lpr_input_read_events(24,(uintptr_t)e,0,0)==-11);publish(7);assert(lpr_input_read_events(24,(uintptr_t)e,0,0)==0);assert(lpr_input_poll_events(24,1)==1);assert(lpr_input_read_events(24,(uintptr_t)e,25,0)==24);assert(e[0].value==7);assert(lpr_input_poll_events(24,1)==1);assert(lpr_input_read_events(24,(uintptr_t)e,1000,0)==48);assert(!lpr_input_poll_events(24,1));assert(!native_calls);publish(8);publish(9);assert(lpr_input_read_events(24,(uintptr_t)e,sizeof e,0)==144);assert(e[0].value==8&&e[3].value==9);
reset();for(int i=0;i<800;i++)publish(i);assert(ring.overflows>0);int n=inputd_ring_read(&ring,e,INPUTD_SHARED_EVENTS);assert(n>0&&e[0].type==0&&e[0].code==3);assert(e[n-1].type==0&&e[n-1].code==0);
reset();publish_on_drain=1;assert(lpr_input_prepare_wait(24)==1);assert(inputd_ring_read(&ring,e,3)==3&&e[0].value==777);assert(lpr_input_prepare_wait(24)==0&&ring.armed==1);publish(778);assert(ring.armed==0);assert(lpr_input_prepare_wait_sequence(24,4u,0,0)==0);
reset();backend.flags=0;long result=0;pthread_t t;pthread_create(&t,0,blocking,&result);usleep(10000);publish(10);pthread_join(t,0);assert(result==72);pthread_create(&t,0,blocking,&result);usleep(10000);pthread_mutex_lock(&lock);interrupt_wait=1;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&lock);pthread_join(t,0);assert(result==-4);__atomic_store_n(&ring.error,-19,__ATOMIC_RELEASE);assert(lpr_input_read_events(24,(uintptr_t)e,72,0)==-19);assert(lpr_input_poll_events(24,1)==0x18);
struct inputd_event_ring *r=mmap(0,INPUTD_RING_BYTES,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);assert(r!=MAP_FAILED);uint64_t tail=0;inputd_input_event_t one={.type=2,.value=42};inputd_ring_publish(r,&tail,&one,1);pid_t child=fork();assert(child>=0);if(!child){inputd_input_event_t out;_exit(inputd_ring_read(r,&out,1)==1&&out.value==42?0:1);}int status;waitpid(child,&status,0);assert(WIFEXITED(status)&&!WEXITSTATUS(status));assert(inputd_ring_read(r,e,1)==-11);munmap(r,INPUTD_RING_BYTES);
concurrent();bench();puts("PASS actual LPR read: batching, partial sizes, nonblock/zero, blocking/EINTR model, local poll, overflow, removal, fork-shared cursor, concurrent readers and wrap");return 0;}
