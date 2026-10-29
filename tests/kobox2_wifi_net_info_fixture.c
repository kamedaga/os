/* SPDX-License-Identifier: MIT */
/* Exercise the actual port implementation with a real net_device layout. */
#include "../kobox2/linux-sandbox/kobox/boot/net_port.c"

static struct kobox_linux_net_port test_port;
static struct net_device test_device;
static const unsigned char test_mac[6] = {2, 1, 2, 3, 4, 5};
#if KOBOX_BOOT_WIFI
static unsigned int poll_count, poll_action;
int kobox_linux_wifi_service_poll_events(struct kobox_linux_wifi_service *service)
{
	if (service != (void *)&test_port)
		return -EINVAL;
	poll_count++;
	if (poll_action == 1)
		kobox_linux_net_port_wifi_association(&test_port);
	if (poll_action == 2)
		kobox_linux_net_port_wifi_authorize(&test_port, true);
	if (poll_action == 3) {
		kobox_linux_net_port_wifi_authorize(&test_port, false);
		return -EIO;
	}
	poll_action = 0;
	return 0;
}
#endif

#define REQUIRE(value) do { if (!(value)) return __LINE__; } while (0)
int kobox_wifi_net_info_fixture(void)
{
	struct kobox_linux_net_info info;

	memset(&test_port, 0, sizeof(test_port));
	memset(&test_device, 0, sizeof(test_device));
	spin_lock_init(&test_port.rx_lock);
	test_device.dev_addr = test_mac;
	test_device.mtu = 1500;
	test_port.device = &test_device;
	/* Ethernet has no association counter, including in a WiFi-capable core. */
	REQUIRE(!kobox_linux_net_port_info(&test_port, &info));
	REQUIRE(info.association_sequence == 0 && info.carrier == 1 && info.mtu == 1500);
	REQUIRE(!memcmp(info.mac, test_mac, 6));
#if KOBOX_BOOT_WIFI
	test_port.wireless = true;
	test_port.wifi = (void *)&test_port;
	REQUIRE(!kobox_linux_net_port_info(&test_port, &info));
	REQUIRE(poll_count == 1 && !info.carrier && !info.association_sequence);
	kobox_linux_net_port_wifi_authorize(&test_port, true);
	test_port.count = 3;
	test_port.head = 7;
	poll_action = 1;
	REQUIRE(!kobox_linux_net_port_info(&test_port, &info));
	REQUIRE(info.association_sequence == 2 && !info.carrier && info.rx_drops == 3);
	REQUIRE(!test_port.count && !test_port.head);
	poll_action = 2;
	REQUIRE(!kobox_linux_net_port_info(&test_port, &info));
	REQUIRE(info.association_sequence == 3 && info.carrier);
	poll_action = 1; /* Another CONNECTED while already authorized. */
	REQUIRE(!kobox_linux_net_port_info(&test_port, &info));
	REQUIRE(info.association_sequence == 4 && !info.carrier);
	REQUIRE(!kobox_linux_net_port_info(&test_port, &info));
	REQUIRE(info.association_sequence == 4 && !info.carrier);
	poll_action = 2;
	REQUIRE(!kobox_linux_net_port_info(&test_port, &info));
	REQUIRE(info.association_sequence == 5 && info.carrier);
	set_bit(__LINK_STATE_NOCARRIER, &test_device.state);
	REQUIRE(!kobox_linux_net_port_info(&test_port, &info));
	REQUIRE(info.association_sequence == 5 && !info.carrier);
	clear_bit(__LINK_STATE_NOCARRIER, &test_device.state);
	poll_action = 3;
	REQUIRE(kobox_linux_net_port_info(&test_port, &info) == -EIO);
	REQUIRE(!test_port.authorized);
#else
	REQUIRE(kobox_linux_net_wifi_request(&test_port, "STATUS", 6, &info, sizeof(info), NULL) == -EOPNOTSUPP);
	REQUIRE(kobox_linux_net_wifi_configure(&test_port, "x", 1) == -EOPNOTSUPP);
	REQUIRE(kobox_linux_net_wifi_events(&test_port, NULL) == -EOPNOTSUPP);
#endif
	return 0;
}
