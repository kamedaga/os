#!/usr/bin/env python3
"""Execute the production dispatch loop with a deterministic IPC fixture."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[3]
source=(root/'userland/netd/src/socket_service.c').read_text()
start=source.index('int netd_socket_service_poll(void)');end=source.index('\n}',start)+2
fixture=r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#define PACHA_IPC_MAX_TRANSFER_FDS 4
#define PACHA_ERR_ALLOC -12
struct pacha_ipc_fd { int fd; };
struct pacha_ipc_msg { struct pacha_ipc_fd *fds; unsigned fd_capacity; };
static int g_netd_socket_endpoint_fd=16, pending, dispatched, reports, failure;
static unsigned long g_netd_socket_requests;
static int pacha_ipc_recv(int fd,struct pacha_ipc_msg *r) {
 assert(fd==16 && r->fds && r->fd_capacity==4);
 if(failure)return failure;
 if(!pending)return -11;
 --pending;return 0;
}
static void netd_report_recv_table_full(void){++reports;}
static int netd_socket_dispatch_request(struct pacha_ipc_msg *r,struct pacha_ipc_fd *f){
 assert(r->fds==f);++dispatched;return -5;
}
'''
fixture+=source[start:end]
fixture+=r'''
int main(void){
 g_netd_socket_endpoint_fd=-1;assert(!netd_socket_service_poll());
 g_netd_socket_endpoint_fd=16;assert(!netd_socket_service_poll());
 /* RX/timer must regain control between every two queued socket requests. */
 pending=33;int rx_pumps=0;
 while(pending){int before=dispatched;assert(netd_socket_service_poll()==1);
  assert(dispatched==before+1);++rx_pumps;}
 assert(rx_pumps==33 && g_netd_socket_requests==33);
 assert(!netd_socket_service_poll());
 /* Receive failure must not dispatch stale input; allocation report retained. */
 pending=1;failure=PACHA_ERR_ALLOC;assert(!netd_socket_service_poll());
 assert(pending==1 && dispatched==33 && reports==1);
 failure=-5;assert(!netd_socket_service_poll());assert(reports==1);
 failure=0;assert(netd_socket_service_poll()==1 && pending==0);
 assert(dispatched==34 && g_netd_socket_requests==34);
}
'''
out=root/'.artifacts/net-perf/run-6';out.mkdir(parents=True,exist_ok=True)
with tempfile.TemporaryDirectory(dir=out) as d:
 p=Path(d);(p/'test.c').write_text(fixture)
 subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=undefined,address',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS socket fairness: bounded dispatch, queued progress, empty/invalid endpoint, receive failures')
