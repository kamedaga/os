#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../userland/personality/linux/runtime/lpr_pipe/io.c"
#include "../.artifacts/tests/lpr-poll-batch-slice.h"
static unsigned expected_count, polls, unpins;
static int fail_poll;
int64_t lpr_pacha_syscall2(uint64_t nr,uint64_t address,uint64_t count){
 assert(nr==PACHAOS_SYSCALL_FD_POLL&&count==expected_count);polls++;
 struct pacha_pollfd *p=(void *)(uintptr_t)address;
 for(unsigned i=0;i<count;i++){
  assert(p[i].fd==100+i&&p[i].revents==0&&p[i].events==PACHA_FD_EVENT_READABLE&&p[i].min_write_bytes==0);
  if(!fail_poll)p[i].revents=PACHA_FD_EVENT_READABLE;
 }
 return fail_poll?-4:(int64_t)count;
}
void lpr_fd_unpin(const lpr_fd_pin_t *pin){assert(pin->fd>=3&&pin->fd<3+expected_count);unpins++;}
static void one(unsigned count,int failure){
 struct lpr_pipe_poll_batch batch;
 // Poison every unused byte. Only explicitly initialized live rows are
 // admissible; completion must never poll or release a suffix row.
 memset(&batch,0xa5,sizeof batch);batch.count=0;
 lpr_linux_pollfd_t fds[256];memset(fds,0,sizeof fds);
 for(unsigned i=0;i<count;i++){
  batch.pins[i]=(lpr_fd_pin_t){.fd=i+3};batch.positions[i]=i;
  batch.leaves[i]=(struct pacha_pollfd){.fd=i+100,.events=PACHA_FD_EVENT_READABLE};batch.count++;
 }
 expected_count=count;fail_poll=failure;unsigned before=unpins,oldpolls=polls;
 assert(lpr_pipe_poll_batch_complete(&batch,fds)==count);
 assert(batch.count==0&&unpins-before==count&&polls-oldpolls==(count!=0));
 for(unsigned i=0;i<count;i++)assert(fds[i].revents==(failure?LPR_LINUX_POLLERR:LPR_LINUX_POLLIN));
 assert(lpr_pipe_poll_batch_complete(&batch,fds)==0&&unpins-before==count);
}
int main(void){one(0,0);one(1,0);one(256,0);one(1,1);one(256,1);puts("POLL_BATCH_PREFIX_PASS poisoned_suffix untouched exact_pin_release error_paths");return 0;}
