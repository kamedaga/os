#include <assert.h>
#include <stdio.h>
#include <sys/socket.h>
#include <string.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/if_link.h>
#include <linux/if_addr.h>
#include "../userland/netd/src/netlink_socket.c"

static struct netd_interface_snapshot actual = {.mtu=1500,
    .mac={2,3,4,5,6,7},.carrier=1,.admin_up=1,.has_address=1,
    .address={192,168,4,8},.netmask={255,255,255,0},.broadcast={192,168,4,255}};
static int attached = 1;
int netd_libuinet_interface_snapshot(struct netd_interface_snapshot *out)
{ *out = actual; return attached; }
int pacha_fd_close(pacha_fd_t fd) { (void)fd; return 0; }
int pacha_ipc_send(pacha_fd_t fd, const struct pacha_ipc_msg *msg)
{ (void)fd; (void)msg; return 0; }
int pacha_service_wait_add(struct pacha_service_wait_set *s,pacha_fd_t fd,uint32_t e)
{ (void)s; (void)fd; (void)e; return 0; }
uint64_t pacha_service_wait_revents(const struct pacha_service_wait_set *s,pacha_fd_t fd)
{ (void)s; (void)fd; return 0; }
static netd_io_t io;
static size_t received;
static void request(uint64_t handle, unsigned type, unsigned family)
{
    memset(&io,0,sizeof(io));io.handle=handle;io.length=17;
    struct nlmsghdr h={17,type,NLM_F_REQUEST|NLM_F_DUMP,1234,999};
    memcpy(io.data,&h,16);io.data[16]=family;
}
static struct nlmsghdr receive(uint64_t handle)
{
    memset(&io,0,sizeof(io));io.handle=handle;
    assert(netd_netlink_socket_recv(&io,sizeof(io.data),&received)==0);
    struct nlmsghdr h;memcpy(&h,io.data,16);
    assert(h.nlmsg_seq==1234 && h.nlmsg_flags==NLM_F_MULTI);
    unsigned remaining=(unsigned)received, records=0;
    for (struct nlmsghdr *record=(struct nlmsghdr *)io.data;
         NLMSG_OK(record,remaining); record=NLMSG_NEXT(record,remaining)) {
        assert(record->nlmsg_seq==h.nlmsg_seq && record->nlmsg_pid==h.nlmsg_pid);
        assert(record->nlmsg_flags==NLM_F_MULTI);
        if (++records==1 && record->nlmsg_type!=NLMSG_DONE) continue;
        assert(record->nlmsg_type==NLMSG_DONE && record->nlmsg_len==NLMSG_LENGTH(4));
        int32_t status;memcpy(&status,NLMSG_DATA(record),4);assert(status==0);
    }
    assert(remaining==0 && records==(h.nlmsg_type==NLMSG_DONE?1:2));
    return h;
}
static void attribute(unsigned type,const void *expected,size_t size,unsigned fixed)
{
    size_t at=16+fixed;
    uint32_t record_length;memcpy(&record_length,io.data,4);
    while(at<record_length) {
        struct rtattr a;memcpy(&a,io.data+at,4);
        assert(a.rta_len>=4 && at+a.rta_len<=record_length);
        if(a.rta_type==type) {
            assert(a.rta_len==size+4 && !memcmp(io.data+at+4,expected,size));return;
        }
        at+=RTA_ALIGN(a.rta_len);
    }
    assert(!"missing attribute");
}
int main(void)
{
    uint64_t a,b,u;size_t sent;
    assert(netd_netlink_socket_open(NETD_SOCK_RAW,0,16,&a)==0);
    assert(netd_netlink_socket_open(NETD_SOCK_DGRAM,0,17,&b)==0);
    assert(netd_netlink_socket_open(NETD_SOCK_DGRAM,15,18,&u)==0);
    uint32_t revents; int32_t error;
    assert(netd_netlink_socket_poll(a,NETD_POLLOUT,&revents,&error)==0 && revents==NETD_POLLOUT);
    netd_netlink_bind_t ba={a,42,1},bb={b,42,0},bu={u,42,1};
    assert(netd_netlink_socket_bind(&ba)==0);
    assert(netd_netlink_socket_bind(&bb)==-98);bb.pid=0;
    assert(netd_netlink_socket_bind(&bb)==0 && bb.pid && bb.pid!=42);
    assert(netd_netlink_socket_bind(&bu)==0);
    request(a,RTM_GETLINK,0);assert(netd_netlink_socket_send(&io,&sent)==0 && sent==17);
    struct nlmsghdr h=receive(a);assert(h.nlmsg_type==RTM_NEWLINK && h.nlmsg_pid==42);
    struct ifinfomsg link;memcpy(&link,io.data+16,sizeof(link));
    assert(link.ifi_index==1 && link.ifi_type==1 && (link.ifi_flags&1));
    attribute(IFLA_IFNAME,"net0",5,16);attribute(IFLA_ADDRESS,actual.mac,6,16);
    attribute(IFLA_MTU,&actual.mtu,4,16);attribute(IFLA_CARRIER,&actual.carrier,1,16);
    assert(find_socket(a)->queue_len==0);
    io.handle=a;io.flags=MSG_PEEK|MSG_TRUNC;
    request(a,RTM_GETLINK,0);assert(netd_netlink_socket_send(&io,&sent)==0);
    io.flags=MSG_PEEK|MSG_TRUNC;
    assert(netd_netlink_socket_recv(&io,sizeof(io.data),&received)==0 && io.flags==0);
    receive(a);
    request(a,RTM_GETADDR,0);assert(netd_netlink_socket_send(&io,&sent)==0);
    assert(receive(a).nlmsg_type==RTM_NEWADDR);
    struct ifaddrmsg addr;memcpy(&addr,io.data+16,sizeof(addr));
    assert(addr.ifa_index==1 && addr.ifa_prefixlen==24 && addr.ifa_family==2);
    attribute(IFA_LOCAL,actual.address,4,8);attribute(IFA_ADDRESS,actual.address,4,8);
    attribute(IFA_BROADCAST,actual.broadcast,4,8);attribute(IFA_LABEL,"net0",5,8);
    assert(find_socket(a)->queue_len==0);
    /* glibc sizeof({nlmsghdr, rtgenmsg}) includes three padding bytes. */
    request(a,RTM_GETLINK,0);io.length=20;
    ((struct nlmsghdr *)io.data)->nlmsg_len=20;
    assert(netd_netlink_socket_send(&io,&sent)==0);
    assert(receive(a).nlmsg_type==RTM_NEWLINK);
    request(a,RTM_GETADDR,10);assert(netd_netlink_socket_send(&io,&sent)==0);
    assert(receive(a).nlmsg_type==NLMSG_DONE);
    actual.has_address=0;request(a,RTM_GETADDR,0);
    assert(netd_netlink_socket_send(&io,&sent)==0);assert(receive(a).nlmsg_type==NLMSG_DONE);
    attached=0;request(a,RTM_GETLINK,0);assert(netd_netlink_socket_send(&io,&sent)==0);
    assert(receive(a).nlmsg_type==NLMSG_DONE);attached=1;
    request(a,RTM_GETLINK,0);assert(netd_netlink_socket_send(&io,&sent)==0);
    size_t full=find_socket(a)->queue[find_socket(a)->queue_head].length;
    memset(&io,0xa5,sizeof(io));io.handle=a;io.flags=MSG_PEEK|MSG_TRUNC;
    assert(netd_netlink_socket_recv(&io,1,&received)==0 && received==full);
    assert(io.flags==MSG_TRUNC && io.data[1]==0xa5 && find_socket(a)->queue_len==1);
    io.flags=MSG_PEEK;assert(netd_netlink_socket_recv(&io,0,&received)==0 && received==0);
    io.flags=0;assert(netd_netlink_socket_recv(&io,1,&received)==0 && received==1);
    assert(io.flags==MSG_TRUNC && find_socket(a)->queue_len==0);
    request(a,RTM_GETLINK,0);io.length=16;
    assert(netd_netlink_socket_send(&io,&sent)==-22 && find_socket(a)->queue_len==0);
    request(a,RTM_GETLINK,0);io.length=34;memcpy(io.data+20,io.data,14);
    assert(netd_netlink_socket_send(&io,&sent)==-22 && find_socket(a)->queue_len==0);
    request(a,RTM_GETLINK,0);io.length=40;
    ((struct nlmsghdr *)io.data)->nlmsg_len=20;
    memcpy(io.data+20,io.data,20);
    assert(netd_netlink_socket_send(&io,&sent)==-95 && sent==0);
    assert(find_socket(a)->queue_len==0);
    request(a,RTM_NEWLINK,0);assert(netd_netlink_socket_send(&io,&sent)==-95);
    for(unsigned i=0;i<15;i++) {request(a,RTM_GETLINK,0);assert(netd_netlink_socket_send(&io,&sent)==0);}
    assert(find_socket(a)->queue_len==15);
    assert(netd_netlink_socket_poll(a,NETD_POLLOUT,&revents,&error)==0 && revents==NETD_POLLOUT);
    request(a,RTM_GETADDR,0);actual.has_address=1;
    assert(netd_netlink_socket_send(&io,&sent)==0 && find_socket(a)->queue_len==16);
    netd_netlink_socket_state_t saved=*find_socket(a);
    request(a,RTM_GETLINK,0);assert(netd_netlink_socket_send(&io,&sent)==-11 && sent==0);
    assert(!memcmp(&saved,find_socket(a),sizeof(saved)));
    request(a,RTM_GETADDR,0);assert(netd_netlink_socket_send(&io,&sent)==-11 && sent==0);
    assert(!memcmp(&saved,find_socket(a),sizeof(saved)));
    assert(netd_netlink_socket_poll(a,NETD_POLLOUT,&revents,&error)==0 && revents==0);
    for(unsigned i=0;i<15;i++) assert(receive(a).nlmsg_type==RTM_NEWLINK);
    assert(receive(a).nlmsg_type==RTM_NEWADDR && find_socket(a)->queue_len==0);
    /* GETLINK also admits its complete dump into the final free slot. */
    for(unsigned i=0;i<15;i++) {request(a,RTM_GETLINK,0);assert(netd_netlink_socket_send(&io,&sent)==0);}
    request(a,RTM_GETLINK,0);assert(netd_netlink_socket_send(&io,&sent)==0);
    for(unsigned i=0;i<16;i++) assert(receive(a).nlmsg_type==RTM_NEWLINK);
    assert(netd_netlink_publish_device(NETD_UEVENT_DRM_CARD0)==0);
    assert(find_socket(u)->queue_len==1 && find_socket(b)->queue_len==0);
    memset(&io,0,sizeof(io));io.handle=u;
    assert(netd_netlink_socket_recv(&io,sizeof(io.data),&received)==0);
    assert(!strcmp((char *)io.data,"change@/devices/pci0000:00/0000:00:03.0/virtio1/drm/card0"));
    assert(netd_netlink_socket_close(a)==0);assert(netd_netlink_socket_close(b)==0);
    uint64_t auto_handle;assert(netd_netlink_socket_open(NETD_SOCK_RAW,0,19,&auto_handle)==0);
    request(auto_handle,RTM_GETLINK,0);assert(netd_netlink_socket_send(&io,&sent)==0);
    assert(find_socket(auto_handle)->bound && receive(auto_handle).nlmsg_pid!=0);
    assert(netd_netlink_socket_close(auto_handle)==0);assert(netd_netlink_socket_close(u)==0);
    puts("netlink route: PASS");
}
