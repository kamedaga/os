#define main old_partial_io_main
#include "netd_libuinet_reconfiguration.c"
#undef main
static void initialize_partial_fixture(void)
{
    struct netd_boot_config config = {0};
    memcpy(config.ipv4.address, old_ip, 4);
    memset(config.ipv4.netmask, 255, 3);
    memcpy(config.ipv4.gateway, peer_ip, 4);
    CHECK(netd_network_config_init(&config.ipv4) == 0);
    netd_link_init(receive, NULL);
    struct netd_link_info info = {.generation=1,
        .interface_id=NETD_LINK_ID_PRIMARY,.mtu=1500,.carrier=1};
    memcpy(info.mac,local_mac,6);
    struct netd_link_ops ops={.send=capture};
    CHECK(netd_link_attach(&info,&ops,NULL)==0);
    struct netd_runtime runtime={.cfg=&config};
    CHECK(netd_libuinet_start(&runtime)==0);
    pump(5);
}

int main(void)
{
 initialize_partial_fixture();char reply[256];
 CHECK(!netd_libuinet_perf_buffers(0,reply,sizeof reply));puts(reply);
 CHECK(strstr(reply,"tcp_sendspace=32768\n") && strstr(reply,"tcp_recvspace=65536\n"));
 CHECK(!netd_libuinet_perf_buffers(1,reply,sizeof reply));
 uint64_t h=0;CHECK(!netd_libuinet_socket_open(2,1,6,16,&h));
 struct netd_libuinet_api_socket *s=netd_libuinet_api_socket_find(h);CHECK(s);
 int value=0;unsigned n=sizeof(value);CHECK(!uinet_sogetsockopt(s->socket,UINET_SOL_SOCKET,UINET_SO_SNDBUF,&value,&n));CHECK(value==524288);
 n=sizeof(value);CHECK(!uinet_sogetsockopt(s->socket,UINET_SOL_SOCKET,UINET_SO_RCVBUF,&value,&n));CHECK(value==524288);
 CHECK(!netd_libuinet_perf_buffers(0,reply,sizeof reply));
 /* Existing sockets keep their allocation; restore affects future sockets. */
 n=sizeof(value);CHECK(!uinet_sogetsockopt(s->socket,UINET_SOL_SOCKET,UINET_SO_RCVBUF,&value,&n));CHECK(value==524288);
 CHECK(!netd_libuinet_socket_open(2,1,6,16,&h));s=netd_libuinet_api_socket_find(h);CHECK(s);
 n=sizeof(value);CHECK(!uinet_sogetsockopt(s->socket,UINET_SOL_SOCKET,UINET_SO_SNDBUF,&value,&n));CHECK(value==32768);
 n=sizeof(value);CHECK(!uinet_sogetsockopt(s->socket,UINET_SOL_SOCKET,UINET_SO_RCVBUF,&value,&n));CHECK(value==65536);
 puts("PASS actual libuinet defaults 32K/64K -> 512K/512K -> restored; existing sockets unchanged");return 0;
}
