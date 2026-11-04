/* SPDX-License-Identifier: MIT */
#include "pressure_manager.h"
#include "pressure_policy.h"
#include "bootfs_reader.h"
#include <pacha/ipc.h>
#include <pacha/syscall.h>
#include <stdio.h>
#include <string.h>

#define OWNERS 4
struct owner {
 pacha_fd_t endpoint, process, life;
 uint64_t generation, free_before;
 struct pressure_pending pending;
};
static struct owner owners[OWNERS];
static struct pressure_policy policy;
/* Owners have independent generation spaces. Assign manager-local IDs only
 * after validating private registration capabilities; do not compare unrelated
 * owner generations against a global watermark. */
static uint64_t registration_sequence, sequence;
static unsigned cursor;
static int initialized;
static uint64_t clock_ns(void) {
 uint64_t time[2] = {0};
 if (pacha_syscall2(PACHA_RUNTIME_SYSCALL_CLOCK_GETTIME, 1, (uintptr_t)time)) return 0;
 return time[0] * UINT64_C(1000000000) + time[1];
}
static void retire(struct owner *o, const char *reason) {
 printf("[pressure] retire generation=%llu reason=%s pending=%d\n",
  (unsigned long long)o->generation, reason, o->pending.active);
 if (o->endpoint >= 16) (void)pacha_fd_close(o->endpoint);
 if (o->process >= 16) (void)pacha_fd_close(o->process);
 if (o->life >= 16) (void)pacha_fd_close(o->life);
 memset(o, 0, sizeof(*o));
}
static int capability(pacha_fd_t fd, uint64_t kind, uint64_t rights) {
 struct pacha_fd_info info = {0};
 return fd >= 16 && !pacha_fd_get_info(fd, &info) && info.kind == kind &&
  (info.rights & rights) == rights;
}
static void registration(pacha_fd_t fd) {
 struct pacha_ipc_fd fds[PACHA_IPC_MAX_TRANSFER_FDS] = {0};
 struct pacha_ipc_msg m = {.fds = fds, .fd_capacity = PACHA_IPC_MAX_TRANSFER_FDS};
 if (pacha_ipc_recv(fd, &m)) return;
 unsigned slot;
 for (slot = 0; slot < OWNERS && owners[slot].endpoint; ++slot) {}
 uint64_t watch = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_POLL | PACHA_FD_RIGHT_WAIT;
 if (m.word0 == PACHA_PRESSURE_REGISTER && m.word1 != 0 &&
     !m.word2 && !m.word3 && m.fd_count == 3 && slot < OWNERS &&
     capability(fds[0].fd, PACHA_FD_KIND_CHANNEL, PACHA_PRESSURE_RIGHTS) &&
     capability(fds[1].fd, PACHA_FD_KIND_PROCESS, watch) &&
     capability(fds[2].fd, PACHA_FD_KIND_CHANNEL, watch)) {
  owners[slot] = (struct owner){.endpoint = fds[0].fd, .process = fds[1].fd,
   .life = fds[2].fd, .generation = ++registration_sequence};
  printf("[pressure] registered generation=%llu owner_generation=%llu\n",
   (unsigned long long)owners[slot].generation, (unsigned long long)m.word1);
  return;
 }
 for (uint64_t i = 0; i < m.fd_count; ++i) if (fds[i].fd >= 16) (void)pacha_fd_close(fds[i].fd);
 fprintf(stderr, "[pressure] rejected registration\n");
}
static int config_numbers(const unsigned char *data, unsigned size, unsigned values[4]) {
 unsigned pos=0;
 for (unsigned i=0;i<4;++i) {
  while (pos<size && (data[pos]==' ' || data[pos]=='\n')) ++pos;
  if (pos==size || data[pos]<'0' || data[pos]>'9') return 0;
  unsigned n=0;
  while (pos<size && data[pos]>='0' && data[pos]<='9') {
   unsigned digit=data[pos++]-'0';
   if (n>(UINT32_MAX-digit)/10) return 0;
   n=n*10+digit;
  }
  values[i]=n;
 }
 while (pos<size && (data[pos]==' ' || data[pos]=='\n')) ++pos;
 return pos==size;
}
static void configure(void) {
 policy = (struct pressure_policy){.low = 64ul<<20, .high = 128ul<<20,
  .cooldown_ns = UINT64_C(5000000000), .max_pages = 1024};
 const unsigned char *data; uint32_t size;
 if (!seed0_bootfs_open_file("/etc/pacha/memory-pressure", &data, &size) && size < 128) {
  unsigned values[4]={0};
  int valid=config_numbers(data, size, values);
  unsigned low=values[0], high=values[1], cooldown=values[2], pages=values[3];
  if (valid &&
      low && high > low && cooldown >= 3000 && cooldown <= 60000 &&
      pages && pages <= PACHA_PRESSURE_MAX_PAGES) {
   policy.low = (uint64_t)low<<20; policy.high = (uint64_t)high<<20;
   policy.cooldown_ns = (uint64_t)cooldown * 1000000; policy.max_pages = pages;
  } else fprintf(stderr, "[pressure] invalid config; using defaults\n");
 }
 initialized = 1;
}
unsigned seed0_pressure_poll(pacha_fd_t registration_fd) {
 if (!initialized) configure();
 registration(registration_fd);
 pacha_system_info_t info = {0};
 if (pacha_syscall1(PACHA_RUNTIME_SYSCALL_SYSTEM_INFO, (uintptr_t)&info)) return 250;
 uint64_t now = clock_ns();
 unsigned interval = pressure_poll_ms(&policy, info.free_memory_bytes);
 int outstanding = 0;
 for (unsigned i = 0; i < OWNERS; ++i) {
  struct owner *o = &owners[i];
  if (!o->endpoint) continue;
  struct pacha_pollfd events[3] = {
   {.fd=o->process, .events=PACHA_FD_EVENT_READABLE|PACHA_FD_EVENT_HANGUP|PACHA_FD_EVENT_ERROR},
   {.fd=o->life, .events=PACHA_FD_EVENT_HANGUP|PACHA_FD_EVENT_ERROR},
   {.fd=o->endpoint, .events=PACHA_FD_EVENT_READABLE|PACHA_FD_EVENT_HANGUP|PACHA_FD_EVENT_ERROR}};
  long ready = pacha_fd_wait_many(events, 3, 0);
  if ((ready < 0 && ready != PACHA_ERR_NOT_READY) || events[0].revents || events[1].revents ||
      (events[2].revents & (PACHA_FD_EVENT_HANGUP|PACHA_FD_EVENT_ERROR))) {
   retire(o, "owner-death"); continue;
  }
  if (o->pending.active && now >= o->pending.deadline) { retire(o, "timeout"); continue; }
  if (events[2].revents & PACHA_FD_EVENT_READABLE) {
   struct pacha_ipc_msg ack = {0};
   if (pacha_ipc_recv(o->endpoint, &ack) || ack.fd_count ||
       !pressure_ack_matches(&o->pending, ack.word0, ack.word1, now)) {
    retire(o, "invalid-ack"); continue;
   }
   printf("[pressure] ack generation=%llu requested_pages=%llu linux_freed_pages=%llu reported_ok_pages=%llu pmm_before=%llu pmm_after=%llu pmm_increased=%u\n",
    (unsigned long long)o->generation, (unsigned long long)ack.word1, (unsigned long long)ack.word2,
    (unsigned long long)ack.word3, (unsigned long long)o->free_before,
    (unsigned long long)info.free_memory_bytes, info.free_memory_bytes > o->free_before);
   /* PMM free is the effectiveness observation, not reported_ok_pages.
    * Concurrent native allocations/frees can also contribute to this delta. */
   o->pending.active = 0;
  }
  outstanding |= o->pending.active;
 }
 uint64_t pages = pressure_target(&policy, info.free_memory_bytes, now);
 if (pages && !outstanding && sequence < (UINT64_MAX >> 16)) {
  for (unsigned n = 0; n < OWNERS; ++n) {
   struct owner *o = &owners[cursor++ % OWNERS];
   if (!o->endpoint) continue;
   struct pressure_pending p = {.sequence=++sequence, .requested=pages,
    .deadline=now+UINT64_C(5000000000), .active=1};
   struct pacha_ipc_msg request = {.word0=pacha_pressure_tag(p.sequence, PACHA_PRESSURE_REQUEST),
    .word1=pages, .word2=now+UINT64_C(1000000000)};
   if (pacha_ipc_send(o->endpoint, &request)) retire(o, "send-failed");
   else {
    o->pending = p; o->free_before = info.free_memory_bytes;
    policy.next_request_ns = now + policy.cooldown_ns;
    printf("[pressure] request generation=%llu pages=%llu pmm_free=%llu\n",
     (unsigned long long)o->generation, (unsigned long long)pages,
     (unsigned long long)info.free_memory_bytes);
   }
   break;
  }
 }
 fflush(stdout);
 return interval;
}

void seed0_pressure_shutdown(void) {
 for (unsigned i=0;i<OWNERS;++i) if (owners[i].endpoint) retire(&owners[i], "shutdown");
}
