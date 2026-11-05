/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "pacha/ipc.h"
#include "netd/ipc_protocol.h"

/* The launcher grants only this client endpoint. No filesystem capabilities. */
#define NET_ENDPOINT 241
static uint64_t sequence, attachment, socket_handle, tx, failures;
static netd_io_t *page;
static pacha_fd_t notification_fd=-1;
static netd_sockaddr_in_t destination;

static int exchange(uint64_t op, struct pacha_ipc_fd *fds, unsigned count,
                    uint64_t *result)
{
    struct pacha_ipc_msg q = {.word0=PACHA_SERVICE_REQUEST_MAGIC,
        .word1=op, .word2=attachment, .word3=++sequence,
        .fds=fds, .fd_count=count}, r={0};
    int sent=0;
    /* Never reuse a shared request page after an ambiguous committed timeout.
     * netd may still be reading it. A stuck netd therefore stops this probe. */
    if (pacha_ipc_call(NET_ENDPOINT,&q,&r,PACHA_FD_WAIT_FOREVER,&sent) ||
        r.word0!=PACHA_SERVICE_REPLY_MAGIC || r.word3!=q.word3) return -1;
    if (result) *result=r.word2;
    return (int)(int64_t)r.word1;
}
static int send_bytes(const void *bytes, size_t length)
{
    /* Drain coalesced readiness messages even though UDP is sent without poll. */
    for (unsigned i=0;i<8 && notification_fd>=16;++i) {
        struct pacha_ipc_msg ready={0};
        if (pacha_ipc_recv(notification_fd,&ready)) break;
    }
    page->handle=socket_handle; page->length=length;
    page->flags=0; page->addr=destination;
    memcpy(page->data,bytes,length);
    uint64_t sent=0;
    ++tx;
    if (exchange(NETD_OP_SEND,NULL,0,&sent) || sent!=length) {
        ++failures; return -1;
    }
    return 0;
}
static int parse_destination(const char *ip,const char *port)
{
    uint8_t octets[4];
    for (unsigned i=0;i<4;++i) {
        unsigned n=0,digits=0;
        while (*ip>='0' && *ip<='9') {
            n=n*10+(unsigned)(*ip++-'0'); if (++digits>3 || n>255) return -1;
        }
        if (!digits || (i<3 ? *ip++!='.' : *ip!='\0')) return -1;
        octets[i]=(uint8_t)n;
    }
    unsigned n=0,digits=0;
    while (*port>='0' && *port<='9') {
        n=n*10+(unsigned)(*port++-'0'); if (++digits>5 || n>65535) return -1;
    }
    if (!digits || *port || !n) return -1;
    memcpy(&destination.addr_be,octets,4);
    destination.port_be=(uint16_t)((n>>8)|(n<<8));
    return 0;
}
int main(int argc,char **argv)
{
    int from_start=argc==4 && !strcmp(argv[3],"--from-start");
    if ((argc!=3 && !from_start) || parse_destination(argv[1],argv[2])) return 2;
    const uint64_t rights=PACHA_FD_RIGHT_INSPECT|PACHA_FD_RIGHT_CLOSE|
        PACHA_FD_RIGHT_TRANSFER|PACHA_FD_RIGHT_MAP_READ|PACHA_FD_RIGHT_MAP_WRITE;
    pacha_fd_t vmo=pacha_vmo_create(NETD_PAGE_BYTES,rights,0);
    if (vmo<16) return 3;
    page=pacha_mmap(vmo,NETD_PAGE_BYTES,PACHA_PROT_READ|PACHA_PROT_WRITE,PACHA_MMAP_SHARED,0);
    if (!page) return 3;
    const uint64_t channel=PACHA_FD_RIGHT_INSPECT|PACHA_FD_RIGHT_CLOSE|
        PACHA_FD_RIGHT_TRANSFER|PACHA_FD_RIGHT_SEND|PACHA_FD_RIGHT_RECV|
        PACHA_FD_RIGHT_WAIT|PACHA_FD_RIGHT_POLL;
    struct pacha_ipc_channel_pair lease,notify;
    if (pacha_ipc_channel_create(&lease,channel,0) ||
        pacha_ipc_channel_create(&notify,channel,0)) return 3;
    struct pacha_ipc_fd attach[2]={{.fd=vmo,.rights=rights & ~PACHA_FD_RIGHT_TRANSFER},
        {.fd=lease.b,.rights=channel & ~PACHA_FD_RIGHT_TRANSFER}};
    if (exchange(NETD_OP_PAGE_ATTACH,attach,2,&attachment) || !attachment) return 3;
    pacha_fd_close(lease.b); pacha_fd_close(vmo);
    *(netd_socket_t *)page=(netd_socket_t){.domain=NETD_AF_INET,
        .type=NETD_SOCK_DGRAM,.protocol=NETD_IPPROTO_UDP};
    struct pacha_ipc_fd notification={.fd=notify.b,
        .rights=channel & ~PACHA_FD_RIGHT_TRANSFER};
    if (exchange(NETD_OP_SOCKET,&notification,1,&socket_handle) || !socket_handle) return 3;
    pacha_fd_close(notify.b);
    notification_fd=notify.a;
    uint64_t cursor=from_start ? 0 : UINT64_MAX,dropped=0,tick=0;
    for (;;) {
        uint64_t ns=0;
        if (pacha_clock_read(1,&ns)) return 4;
        char line[256];
        int n=snprintf(line,sizeof(line),"[klogudp alive seq=%llu kernel_ms=%llu tx=%llu send_errors=%llu cursor=%llu]\n",
            (unsigned long long)tick++,(unsigned long long)(ns/1000000),
            (unsigned long long)tx,(unsigned long long)failures,(unsigned long long)cursor);
        send_bytes(line,(size_t)n);
        /* Bounded drain preserves heartbeat even under continuous logging. */
        for (unsigned i=0;i<64;++i) {
            struct pacha_kernel_log_read r={.cursor=cursor,.limit=PACHA_KERNEL_LOG_READ_MAX_BYTES};
            long status=pacha_syscall2(PACHA_KERNEL_LOG_SYSCALL_READ,(uintptr_t)&r,sizeof(r));
            if (status || r.written>r.limit) {
                n=snprintf(line,sizeof(line),"[klogudp ring_error=%ld]\n",status);
                send_bytes(line,(size_t)n); break;
            }
            if (r.overrun || r.dropped!=dropped) {
                n=snprintf(line,sizeof(line),"[klogudp overrun=%llu dropped=%llu oldest=%llu next=%llu]\n",
                    (unsigned long long)r.overrun,(unsigned long long)r.dropped,
                    (unsigned long long)r.oldest_cursor,(unsigned long long)r.next_cursor);
                send_bytes(line,(size_t)n);
            }
            dropped=r.dropped;
            if (r.written && send_bytes(r.bytes,(size_t)r.written)) break;
            cursor=r.next_cursor;
            if (r.written<r.limit) break;
        }
        struct timespec delay={.tv_sec=0,.tv_nsec=250000000};
        if (pacha_syscall2(PACHA_RUNTIME_SYSCALL_NANOSLEEP,(uintptr_t)&delay,0)) return 4;
    }
}
