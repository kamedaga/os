/* Exercise the real backend accessor against its existing host boundary. */
#define main reconfiguration_fixture_main
#include "netd_libuinet_reconfiguration.c"
#undef main
int main(void)
{
    struct netd_interface_snapshot snapshot;
    CHECK(netd_libuinet_interface_snapshot(&snapshot)==0);
    struct netd_boot_config config={0};
    memcpy(config.ipv4.address,old_ip,4);
    memset(config.ipv4.netmask,255,3);
    memcpy(config.ipv4.gateway,peer_ip,4);
    CHECK(netd_network_config_init(&config.ipv4)==0);
    netd_link_init(receive,NULL);
    struct netd_link_info info={.generation=1,.interface_id=1,.mtu=1500,.carrier=1};
    memcpy(info.mac,local_mac,6);
    struct netd_link_ops ops={.send=capture};
    CHECK(netd_link_attach(&info,&ops,NULL)==0);
    CHECK(netd_libuinet_interface_snapshot(&snapshot)==1 && !snapshot.has_address);
    struct netd_runtime runtime={.cfg=&config};
    CHECK(netd_libuinet_start(&runtime)==0);
    CHECK(netd_libuinet_interface_snapshot(&snapshot)==1);
    CHECK(snapshot.admin_up && snapshot.has_address && snapshot.carrier);
    CHECK(snapshot.mtu==1500 && !memcmp(snapshot.mac,local_mac,6));
    CHECK(!memcmp(snapshot.address,old_ip,4));
    /* Pending policy is not an applied address. */
    memcpy(config.ipv4.address,new_ip,4);
    memset(config.ipv4.gateway,0,4);
    CHECK(netd_network_config_init(&config.ipv4)==0);
    CHECK(netd_libuinet_interface_snapshot(&snapshot)==1);
    CHECK(!memcmp(snapshot.address,old_ip,4));
    netd_libuinet_ipv4_suspend();
    CHECK(netd_libuinet_interface_snapshot(&snapshot)==1 && !snapshot.admin_up && !snapshot.has_address);
    CHECK(netd_libuinet_reconfigure(&runtime)==0);
    CHECK(netd_libuinet_interface_snapshot(&snapshot)==1 && snapshot.admin_up && snapshot.has_address);
    CHECK(!memcmp(snapshot.address,new_ip,4));
    CHECK(netd_link_set_carrier(1,0)==0);
    CHECK(netd_libuinet_interface_snapshot(&snapshot)==1 && !snapshot.carrier && snapshot.admin_up);
    CHECK(netd_link_detach(1)==0);
    CHECK(netd_libuinet_interface_snapshot(&snapshot)==0);
    puts("netlink applied snapshot: PASS");
    return 0;
}
