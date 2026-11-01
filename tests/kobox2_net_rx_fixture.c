/* SPDX-License-Identifier: MIT */
/* Actual owned port, real Linux layouts; only host/work scheduling is modeled. */
#include "../kobox2/linux-sandbox/kobox/boot/net_port.c"

static struct work_struct *pending_work;
struct workqueue_struct *system_wq;
static unsigned hints, released, copy_error, refuse_features;
bool queue_work_on(int cpu, struct workqueue_struct *queue, struct work_struct *work)
{ (void)cpu; (void)queue; pending_work = work; return true; }
bool cancel_work_sync(struct work_struct *work)
{ bool pending = pending_work == work; if (pending) pending_work = NULL; return pending; }
void sk_skb_reason_drop(struct sock *sk, struct sk_buff *skb, enum skb_drop_reason reason) { (void)sk; (void)skb; (void)reason; released++; }
int skb_copy_bits(const struct sk_buff *skb, int offset, void *to, int length)
{ if (copy_error) return -EIO; memcpy(to, skb->data + offset, length); return 0; }
void netdev_update_features(struct net_device *dev)
{ if (!refuse_features) dev->features = dev->wanted_features; }
static int hint(void *context)
{ if (context != &hints) return -EINVAL; hints++; return 0; }
static void work_run(void)
{ struct work_struct *w = pending_work; pending_work = NULL; if (w) w->func(w); }
#define REQUIRE(x) do { if (!(x)) return __LINE__; } while (0)

int kobox_net_rx_fixture(void)
{
 static struct kobox_linux_net_port port;
 static struct net_device dev;
 static struct kobox_linux_net_frame queue[KOBOX_NET_RX_CAPACITY], output[KOBOX_NET_RX_CAPACITY];
 static unsigned char data[4096];
 struct sk_buff skb;
 struct kobox_linux_net_rx_stats stats;
 size_t count;
 memset(&port, 0, sizeof(port)); memset(&dev, 0, sizeof(dev)); memset(&skb, 0, sizeof(skb));
 memset(data, 0x5a, sizeof(data));
 port.device = &dev; port.rx = queue; spin_lock_init(&port.rx_lock); INIT_WORK(&port.rx_work, rx_wake_work);
 port.packets.func = receive_packet;
 skb.head = data; skb.data = data + ETH_HLEN; skb.end = 2048; skb.mac_header = 0; skb.len = 100;
 memset(skb_shinfo(&skb), 0, sizeof(*skb_shinfo(&skb)));
 REQUIRE(!kobox_linux_net_port_bind_wake(&port, hint, &hints));
 if (KOBOX_NET_RX_CONTROL) {
  for (unsigned i=0;i<129;i++) REQUIRE(!receive_packet(&skb,&dev,&port.packets,&dev));
  REQUIRE(port.count==128 && port.stats.drop_full==1 && !pending_work);
  REQUIRE(!kobox_linux_net_port_read(&port,output,128,&count) && count==128);
  skb_shinfo(&skb)->gso_size=50;
  REQUIRE(!receive_packet(&skb,&dev,&port.packets,&dev));
  REQUIRE(port.count==1 && port.stats.gso_seen==1 && !port.stats.drop_gso);
  skb.len=3000; REQUIRE(!receive_packet(&skb,&dev,&port.packets,&dev));
  REQUIRE(port.stats.gso_seen==2 && port.stats.drop_oversize==1);
  dev.features=dev.wanted_features=NETIF_F_GRO|NETIF_F_SG;
  REQUIRE(!require_standalone_frames(&port,&dev));
  REQUIRE(dev.features==(NETIF_F_GRO|NETIF_F_SG) && port.stats.measurement_control==1 && !port.features_changed);
  REQUIRE(!kobox_linux_net_port_bind_wake(&port,NULL,NULL));
  return 0;
 }
 skb.pkt_type = PACKET_OUTGOING; REQUIRE(!receive_packet(&skb, &dev, &port.packets, &dev)); REQUIRE(!port.count && !port.drops); skb.pkt_type = PACKET_HOST;
 for (unsigned i=0; i<3; i++) REQUIRE(!receive_packet(&skb, &dev, &port.packets, &dev));
 REQUIRE(port.count == 3 && !hints && pending_work);
 work_run(); REQUIRE(hints == 1 && port.stats.wake_edges == 1);
 REQUIRE(!kobox_linux_net_port_read(&port, output, 1, &count) && count == 1 && port.count == 2);
 REQUIRE(!pending_work); /* Nonempty queue requires consumer continuation, not another edge. */
 REQUIRE(!kobox_linux_net_port_read(&port, output, 128, &count) && count == 2 && !port.count);
 REQUIRE(!receive_packet(&skb, &dev, &port.packets, &dev)); work_run(); REQUIRE(hints == 2);
 REQUIRE(!kobox_linux_net_port_read(&port, output, 128, &count) && count == 1);
 for (unsigned i=0; i<129; i++) REQUIRE(!receive_packet(&skb, &dev, &port.packets, &dev));
 REQUIRE(port.count == 128 && port.stats.drop_full == 1 && port.stats.high_water == 128);
 REQUIRE(!kobox_linux_net_port_read(&port, output, 64, &count) && count == 64 && port.count == 64);
 REQUIRE(!kobox_linux_net_port_read(&port, output, 64, &count) && count == 64 && !port.count);
 REQUIRE(output[0].length == 114 && output[0].bytes[0] == 0x5a);
 work_run();
 copy_error = 1; REQUIRE(!receive_packet(&skb, &dev, &port.packets, &dev)); copy_error = 0;
 REQUIRE(port.stats.drop_copy == 1);
 skb.len = 3000; REQUIRE(!receive_packet(&skb, &dev, &port.packets, &dev)); REQUIRE(port.stats.drop_oversize == 1);
 skb.len = 100; skb_shinfo(&skb)->gso_size = 50;
 REQUIRE(!receive_packet(&skb, &dev, &port.packets, &dev)); REQUIRE(port.stats.drop_gso == 1);
 skb_shinfo(&skb)->gso_size = 0;
 REQUIRE(!receive_packet(&skb, &dev, &port.packets, &dev)); REQUIRE(pending_work);
 unsigned before = hints;
 REQUIRE(!kobox_linux_net_port_bind_wake(&port, NULL, NULL)); work_run(); REQUIRE(hints == before);
 REQUIRE(!kobox_linux_net_port_bind_wake(&port, hint, &hints)); REQUIRE(pending_work); work_run(); REQUIRE(hints == before+1);
 REQUIRE(!kobox_linux_net_port_rx_stats(&port, &stats) && stats.queued == 1 && stats.high_water == 128);
 REQUIRE(!kobox_linux_net_port_bind_wake(&port, NULL, NULL));
 REQUIRE(kobox_linux_net_port_bind_wake(&port, NULL, &hints) == -EINVAL);
 dev.features = dev.wanted_features = NETIF_F_GRO | NETIF_F_GRO_HW | NETIF_F_LRO | NETIF_F_SG;
 REQUIRE(!require_standalone_frames(&port, &dev));
 REQUIRE(dev.features == NETIF_F_SG && port.saved_wanted != dev.features);
 REQUIRE(port.stats.features_before & NETIF_F_GRO);
 refuse_features = 1; dev.features |= NETIF_F_GRO;
 REQUIRE(require_standalone_frames(&port, &dev) == -EOPNOTSUPP);
 REQUIRE(released == 1+3+1+129+4);
 return 0;
}
