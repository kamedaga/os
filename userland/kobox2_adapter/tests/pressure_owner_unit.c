/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include "../../gpud/pressure_owner.h"
static int next_fd, closed[2048], bind_error, export_error, missing, binds, exports;
int pacha_fd_get_info(pacha_fd_t fd, struct pacha_fd_info *info) {
 assert(fd==PACHA_PRESSURE_EXPORT_FD);
 if (missing) return -9;
 *info=(struct pacha_fd_info){.kind=PACHA_FD_KIND_CHANNEL};return 0;
}
int pacha_fd_close(pacha_fd_t fd) { assert(fd>=16 && fd<2048);++closed[fd];return 0; }
int pacha_ipc_channel_create(struct pacha_ipc_channel_pair *out,uint64_t rights,uint32_t flags) {
 assert(rights==(PACHA_PRESSURE_RIGHTS|PACHA_FD_RIGHT_TRANSFER) && !flags);
 out->a=next_fd++;out->b=next_fd++;return 0;
}
int ph_ipc_send(struct ph_ipc *ipc,struct ph_ipc_packet *bind) {
 assert(ipc->generation==7 && bind->generation==7 && bind->operation==PACHA_PRESSURE_BIND);
 assert(bind->fd_count==1 && bind->fds[0].rights==PACHA_PRESSURE_RIGHTS);
 ++binds;return bind_error;
}
int pacha_ipc_send(pacha_fd_t fd,const struct pacha_ipc_msg *m) {
 assert(fd==PACHA_PRESSURE_EXPORT_FD && m->word0==PACHA_PRESSURE_REGISTER && m->word1==7);
 assert(!m->word2 && !m->word3 && m->fd_count==3);
 assert(m->fds[0].rights==PACHA_PRESSURE_RIGHTS && m->fds[1].fd==77);
 assert(!(m->fds[2].rights&(PACHA_FD_RIGHT_SEND|PACHA_FD_RIGHT_RECV)));
 ++exports;return export_error;
}
int main(void) {
 struct ph_ipc ipc={.generation=7,.fd=70,.admitted=1};pacha_fd_t life=-1;
 next_fd=100;missing=1;
 assert(!gpud_export_pressure(&ipc,77,&life) && life==-1 && !binds);
 missing=0;
 assert(!gpud_export_pressure(&ipc,77,&life) && life==102 && binds==1 && exports==1);
 assert(closed[100] && closed[101] && !closed[102] && closed[103] && !closed[77]);
 bind_error=-5;
 assert(gpud_export_pressure(&ipc,77,&life)==-5 && life==-1 && closed[102]);
 for(int fd=104;fd<108;++fd)assert(closed[fd]);
 assert(exports==1);
 bind_error=0;export_error=-11;
 assert(gpud_export_pressure(&ipc,77,&life)==-11 && life==-1);
 for(int fd=108;fd<112;++fd)assert(closed[fd]);
 puts("GPU/Wi-Fi owner: bind, private registration triple, liveness retention and failure cleanup PASS");
}
