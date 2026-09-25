/* SPDX-License-Identifier: MIT */
#include "../userland/netd/src/link.h"
#include "../userland/netd/src/network_config.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static unsigned received;
static unsigned sent;
static unsigned polled;

static int receive_one(void *context, uint64_t generation,
    const void *frame, size_t size)
{
    (void)context;
    assert(generation == 1 || generation == 2);
    (void)frame;
    (void)size;
    if (received) return -EAGAIN;
    ++received;
    return 0;
}

static int send_one(void *context, const void *frame, size_t size)
{
    (void)context;
    (void)frame;
    (void)size;
    if (sent) return -EAGAIN;
    ++sent;
    return 0;
}

static void poll_one(void *context)
{
    (void)context;
    ++polled;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    FILE *fixture = fopen(argv[1], "rb");
    assert(fixture != NULL);
    char text[512];
    size_t length = fread(text, 1, sizeof(text) - 1, fixture);
    assert(ferror(fixture) == 0 && feof(fixture));
    assert(fclose(fixture) == 0);
    text[length] = '\0';
    struct netd_ipv4_boot_config parsed;
    assert(netd_network_config_parse_text(text, &parsed) == 0);
    assert(netd_network_config_parse_text(
        "# generated from DHCP on this live boot\n"
        "address=10.0.2.15\nnetmask=255.255.255.0\n"
        "gateway=10.0.2.2\ndns=10.0.2.3\n", &parsed) == 0);
    assert(netd_network_config_parse_text(
        "address=not-an-ip\n", &parsed) == -EINVAL);
    assert(netd_network_config_get() == NULL);

    struct netd_link_info info = {.generation = 1,
        .interface_id = NETD_LINK_ID_PRIMARY, .mtu = 1500, .carrier = 1,
        .mac = {0x52, 0x54, 0, 0x12, 0x34, 0x56}};
    struct netd_link_ops ops = {.send = send_one, .poll = poll_one};
    unsigned char frame[64] = {0};
    netd_link_init(receive_one, NULL);
    assert(netd_link_attach(&info, &ops, NULL) == 0);
    assert(netd_network_config_get() == NULL);
    assert(netd_network_config_parse_text(text, &parsed) == 0);
    assert(netd_network_config_init(&parsed) == 0);
    assert(memcmp(netd_network_config_get()->link_mac, info.mac, sizeof(info.mac)) == 0);
    parsed.netmask[0] = 0x55;
    assert(netd_network_config_init(&parsed) == -EINVAL);
    assert(netd_link_receive(1, frame, sizeof(frame)) == 0);
    assert(netd_link_receive(1, frame, sizeof(frame)) == -EAGAIN);
    assert(netd_link_send(frame, sizeof(frame)) == 0);
    assert(netd_link_send(frame, sizeof(frame)) == -EAGAIN);
    netd_link_poll();
    assert(polled == 1);
    assert(netd_link_set_carrier(1, 0) == 0);
    assert(netd_link_send(frame, sizeof(frame)) == -ENETDOWN);
    assert(netd_link_receive(1, frame, sizeof(frame)) == -ENETDOWN);
    assert(netd_link_set_carrier(2, 1) == -ESTALE);
    sent = 0;
    received = 0;
    assert(netd_link_set_carrier(1, 1) == 0);
    assert(netd_link_send(frame, sizeof(frame)) == 0);
    assert(netd_link_receive(1, frame, sizeof(frame)) == 0);
    assert(netd_link_detach(1) == 0);
    assert(netd_link_receive(1, frame, sizeof(frame)) == -ESTALE);
    assert(netd_link_attach(&info, &ops, NULL) == -EINVAL);
    info.generation = 2;
    assert(netd_link_attach(&info, &ops, NULL) == 0);
    assert(netd_link_receive(1, frame, sizeof(frame)) == -ESTALE);
    assert(netd_link_detach(2) == 0);
    return 0;
}
