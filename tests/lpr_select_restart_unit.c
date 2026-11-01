#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#define LPR_LINUX_EINVAL 22
#define LPR_LINUX_EFAULT 14
#define LPR_LINUX_POLLIN 1
#define LPR_LINUX_POLLOUT 4
#define LPR_LINUX_POLLERR 8
#define LPR_WAIT_RESTART_SYSCALL -4096
typedef struct {int unused;} lpr_wait_deadline_t;
typedef struct {unsigned count;} lpr_wait_graph_t;
static unsigned scans,blocks,mode;
static uint64_t lpr_fdset_word_count(uint64_t n){return (n+63)/64;}
static int lpr_fdset_test(const uint64_t *p,uint64_t fd){return (p[fd/64]>>(fd%64))&1;}
static int lpr_user_range_plausible(uint64_t p,uint64_t n){return !n||p>4096;}
#define lpr_memcpy memcpy
#define lpr_memset memset
static int64_t lpr_wait_deadline_init(lpr_wait_deadline_t *d,int64_t t){(void)d;(void)t;return 0;}
static int64_t lpr_wait_deadline_expired(lpr_wait_deadline_t *d,int *expired){(void)d;*expired=0;return 0;}
static void lpr_wait_graph_init(lpr_wait_graph_t *g){g->count=0;}
static int64_t lpr_wait_graph_add_fd(lpr_wait_graph_t *g,uint64_t fd,uint32_t events){assert(fd==3||fd==5||fd==7);assert(events);g->count++;return 0;}
static int64_t lpr_wait_graph_block(lpr_wait_graph_t *g,lpr_wait_deadline_t *d,unsigned *observed){(void)d;assert(g->count==3);++blocks;*observed=1;return mode? -4:LPR_WAIT_RESTART_SYSCALL;}
static int64_t lpr_linux_select_scan(uint64_t n,const uint64_t *r,const uint64_t *w,const uint64_t *e,uint64_t *ro,uint64_t *wo,uint64_t *eo,unsigned *observed){(void)n;(void)observed;assert(r[0]==8&&w[0]==32&&e[0]==128);++scans;ro[0]=wo[0]=eo[0]=0;return 0;}
#include "select-wait.inc"
int main(void){for(mode=0;mode<2;++mode){uint64_t r=8,w=32,e=128;unsigned hint=0;int64_t z=lpr_linux_select_wait(8,(uintptr_t)&r,(uintptr_t)&w,(uintptr_t)&e,-1,&hint);assert(z==(mode?-4:LPR_WAIT_RESTART_SYSCALL));assert(r==8&&w==32&&e==128&&hint==1);}assert(scans==2&&blocks==2);puts("SELECT_INPUT_RESTORE PASS restart and EINTR");}
