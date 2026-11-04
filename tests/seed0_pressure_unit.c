/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../userland/seed0boot/src/pressure_manager.c"

static uint64_t time_ns, free_bytes, dead_fd;
static int have_registration, have_ack, sends, closes, invalid_capability;
static uint64_t registration_generation;
static unsigned registration_offset;
static struct pacha_ipc_msg reply, sent;
long pacha_syscall1(uint64_t nr, uint64_t out) {
 assert(nr == PACHA_RUNTIME_SYSCALL_SYSTEM_INFO);
 ((pacha_system_info_t *)(uintptr_t)out)->free_memory_bytes=free_bytes;
 return 0;
}
long pacha_syscall2(uint64_t nr, uint64_t clock, uint64_t out) {
 assert(nr == PACHA_RUNTIME_SYSCALL_CLOCK_GETTIME && clock == 1);
 uint64_t *v=(void *)(uintptr_t)out;
 v[0]=time_ns/1000000000; v[1]=time_ns%1000000000;
 return 0;
}
int seed0_bootfs_open_file(const char *path, const unsigned char **data, uint32_t *size) {
 (void)path; (void)data; (void)size; return -5;
}
int pacha_fd_get_info(pacha_fd_t fd, struct pacha_fd_info *info) {
 *info=(struct pacha_fd_info){.kind=(fd==101 || fd==111 || fd==121) ? PACHA_FD_KIND_PROCESS : PACHA_FD_KIND_CHANNEL,
  .rights=PACHA_PRESSURE_RIGHTS};
 if (invalid_capability && fd==100) info->kind=PACHA_FD_KIND_VMO;
 return 0;
}
int pacha_fd_close(pacha_fd_t fd) { assert(fd>=16); ++closes; return 0; }
int pacha_ipc_recv(pacha_fd_t fd, struct pacha_ipc_msg *m) {
 if (fd==50 && have_registration) {
  assert(m->fd_capacity>=3); have_registration=0;
  m->word0=PACHA_PRESSURE_REGISTER; m->word1=registration_generation; m->fd_count=3;
  for (unsigned i=0;i<3;++i) m->fds[i].fd=100+registration_offset+i;
  return 0;
 }
 if (fd==100 && have_ack) { *m=reply; have_ack=0; return 0; }
 return PACHA_ERR_EMPTY;
}
int pacha_ipc_send(pacha_fd_t fd, const struct pacha_ipc_msg *m) {
 assert(fd==100); sent=*m; ++sends; return 0;
}
long pacha_fd_wait_many(struct pacha_pollfd *fds, uint64_t count, uint64_t timeout) {
 assert(count==3 && timeout==0);
 unsigned ready=0;
 for (unsigned i=0;i<count;++i) {
  if (fds[i].fd==dead_fd) fds[i].revents=PACHA_FD_EVENT_HANGUP;
  if (fds[i].fd==100 && have_ack) fds[i].revents|=PACHA_FD_EVENT_READABLE;
  ready+=!!fds[i].revents;
 }
 return ready ? (long)ready : PACHA_ERR_NOT_READY;
}
static void reg(uint64_t gen) { have_registration=1; registration_generation=gen; }
static void reset(void) {
 memset(owners,0,sizeof(owners)); memset(&policy,0,sizeof(policy));
 initialized=0; sequence=registration_sequence=cursor=registration_offset=0;
 time_ns=1000000000; free_bytes=32ul<<20; dead_fd=0;
 sends=closes=have_registration=have_ack=0;
}
int main(void) {
 struct pressure_policy p={.low=64,.high=128,.max_pages=1024};
 assert(pressure_poll_ms(&p,64)==250);
 assert(pressure_poll_ms(&p,63)==50);
 assert(pressure_poll_ms(&p,100)==50);
 assert(pressure_poll_ms(&p,128)==250);
 unsigned values[4];
 assert(config_numbers((const unsigned char *)"64 128 5000 1024\n",17,values));
 assert(!config_numbers((const unsigned char *)"1 2 3 4294967296",16,values));
 assert(!config_numbers((const unsigned char *)"1 2 3 4 extra",13,values));
 reset(); reg(1);
 assert(seed0_pressure_poll(50)==50 && sends==1 && !closes);
 assert(sent.word1==1024 && owners[0].pending.active);
 assert((sent.word0&0xffff)==PACHA_PRESSURE_REQUEST);
 time_ns+=100000000;
 seed0_pressure_poll(50); assert(!closes && sends==1); /* empty wait is -2 */
 time_ns+=3000000000; free_bytes+=4ul<<20;
 reply=(struct pacha_ipc_msg){.word0=pacha_pressure_tag(1,PACHA_PRESSURE_ACK),
  .word1=1024,.word2=32,.word3=512}; have_ack=1;
 seed0_pressure_poll(50); assert(!owners[0].pending.active && !closes && sends==1);
 time_ns=6000000000; seed0_pressure_poll(50); assert(sends==2);
 dead_fd=102; seed0_pressure_poll(50); assert(closes==3 && !owners[0].endpoint);
 dead_fd=0; reg(0); seed0_pressure_poll(50); assert(closes==6); /* invalid generation */
 reg(2); seed0_pressure_poll(50); assert(owners[0].generation==2);
 time_ns=11000000000; seed0_pressure_poll(50); assert(owners[0].pending.active);
 time_ns=16000000000; seed0_pressure_poll(50); assert(!owners[0].endpoint && closes==9);
 reg(3); seed0_pressure_poll(50); assert(owners[0].pending.active);
 reply.word0=pacha_pressure_tag(999,PACHA_PRESSURE_ACK); have_ack=1;
 seed0_pressure_poll(50); assert(closes==12 && !owners[0].endpoint);
 time_ns=21000000000; reg(4); seed0_pressure_poll(50);
 assert(owners[0].pending.active);
 seed0_pressure_shutdown(); assert(closes==15 && !owners[0].endpoint);
 /* Process exit wins over an apparently successful ack. */
 time_ns=26000000000; reg(5); seed0_pressure_poll(50);
 dead_fd=101; have_ack=1; seed0_pressure_poll(50); assert(closes==18);
 reset(); invalid_capability=1; reg(1); seed0_pressure_poll(50);
 assert(closes==3 && !registration_sequence && !sends);
 invalid_capability=0; reg(1); seed0_pressure_poll(50);
 assert(owners[0].generation==1 && sends==1);
 seed0_pressure_shutdown();
 reset(); free_bytes=256ul<<20;
 reg(1); seed0_pressure_poll(50);
 registration_offset=10; reg(1); seed0_pressure_poll(50);
 registration_offset=20; reg(1); seed0_pressure_poll(50);
 assert(owners[0].generation==1 && owners[1].generation==2 && owners[2].generation==3);
 dead_fd=112; seed0_pressure_poll(50);
 assert(owners[0].endpoint && !owners[1].endpoint && owners[2].endpoint && closes==3);
 seed0_pressure_shutdown(); assert(closes==9);
 puts("seed0 pressure: owner identities cooldown registration owner-death timeout ack shutdown PASS");
 return 0;
}
