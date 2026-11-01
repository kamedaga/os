#define kobox_net_rx_fixture previous_rx_fixture
#include "kobox2_net_rx_fixture.c"
#undef kobox_net_rx_fixture
int kobox_net_rx_fixture(void)
{
 int result=previous_rx_fixture();if(result)return result;
 static struct kobox_linux_net_port port;
 static struct net_device dev;
 static struct kobox_linux_net_frame queue[128],out[128];
 static unsigned char data[4096];struct sk_buff skb;char reply[8192];size_t used,count;
 memset(&port,0,sizeof port);memset(&dev,0,sizeof dev);memset(&skb,0,sizeof skb);memset(data,0x5a,sizeof data);
 port.device=&dev;port.rx=queue;spin_lock_init(&port.rx_lock);INIT_WORK(&port.rx_work,rx_wake_work);
 skb.head=data;skb.data=data+ETH_HLEN;skb.end=2048;skb.mac_header=0;skb.len=100;
 memset(skb_shinfo(&skb),0,sizeof(*skb_shinfo(&skb)));pending_work=NULL;
 REQUIRE(kobox_linux_net_port_perf_request(&port,"NET_PERF_TRACE_START",20,reply,2,&used)==-EINVAL);
 REQUIRE(!port.trace_enabled);
 REQUIRE(kobox_linux_net_port_perf_request(&port,"NET_PERF_TRACE_START",20,reply,sizeof reply,&used)==1 && used==3);
 REQUIRE(kobox_linux_net_port_perf_request(&port,"NET_PERF_TRACE_DUMP",19,reply,sizeof reply,&used)==-EBUSY);
 unsigned i;for(i=0;i<256;i++){data[20]=i;if(!(kobox_net_perf_key(data,114)&63))break;}REQUIRE(i<256);
 kobox_net_perf_irq_mark();
 REQUIRE(!receive_packet(&skb,&dev,&port.packets,&dev));REQUIRE(port.trace_count==1);
 REQUIRE(!kobox_linux_net_port_read(&port,out,128,&count)&&count==1);
 REQUIRE(port.trace[0].key==kobox_net_perf_key(data,114));
 REQUIRE(port.trace[0].irq_hint<=port.trace[0].tap && port.trace[0].tap<=port.trace[0].queued && port.trace[0].queued<=port.trace[0].dequeued);
 for(i=0;i<100;i++){REQUIRE(!receive_packet(&skb,&dev,&port.packets,&dev));REQUIRE(!kobox_linux_net_port_read(&port,out,128,&count));}
 REQUIRE(port.trace_count==32);
 REQUIRE(kobox_linux_net_port_perf_request(&port,"NET_PERF_TRACE_STOP",19,reply,sizeof reply,&used)==1);
 REQUIRE(kobox_linux_net_port_perf_request(&port,"NET_PERF_TRACE_DUMP",19,reply,sizeof reply,&used)==1 && used>0 && used<sizeof reply);
 REQUIRE(kobox_linux_net_port_perf_request(&port,"NET_PERF_TRACE_DUMP",19,reply,4,&used)==-ENOSPC);
 REQUIRE(kobox_linux_net_port_perf_request(&port,"NET_PERF_TRACE_START",20,reply,sizeof reply,&used)==1);
 REQUIRE(port.trace_count==0);return 0;
}
