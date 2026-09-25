/* SPDX-License-Identifier: MIT */
#include "../userland/netd/src/dhcp.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct capture {
    uint8_t frame[400];
    size_t length;
    unsigned sends;
    unsigned events;
};

static void be16(uint8_t *p, uint16_t n)
{
    p[0] = (uint8_t)(n >> 8);
    p[1] = (uint8_t)n;
}

static void be32(uint8_t *p, uint32_t n)
{
    p[0] = (uint8_t)(n >> 24);
    p[1] = (uint8_t)(n >> 16);
    p[2] = (uint8_t)(n >> 8);
    p[3] = (uint8_t)n;
}

static uint32_t sum_words(uint32_t sum, const uint8_t *p, size_t length)
{
    while (length >= 2) {
        sum += (uint16_t)((uint16_t)p[0] << 8 | p[1]);
        p += 2;
        length -= 2;
    }
    if (length) sum += (uint16_t)p[0] << 8;
    while (sum >> 16) sum = (sum & 65535) + (sum >> 16);
    return sum;
}

static int capture_send(void *context, const void *frame, size_t length)
{
    struct capture *capture = context;
    assert(length <= sizeof(capture->frame));
    memcpy(capture->frame, frame, length);
    capture->length = length;
    capture->sends++;
    return 0;
}

static void capture_event(void *context, const char *name, int detail)
{
    struct capture *capture = context;
    assert(name != NULL);
    (void)detail;
    capture->events++;
}

static size_t make_reply(uint8_t frame[400],
    const struct netd_dhcp_client *client, uint8_t kind,
    const uint8_t address[4], const uint8_t mask[4],
    const uint8_t server[4])
{
    memset(frame, 0, 400);
    uint8_t *ip = frame + 14;
    uint8_t *udp = ip + 20;
    uint8_t *bootp = udp + 8;
    memset(frame, 255, 6);
    const uint8_t sender_mac[6] = {2, 3, 4, 5, 6, 7};
    memcpy(frame + 6, sender_mac, 6);
    be16(frame + 12, 0x0800);
    ip[0] = 0x45;
    ip[8] = 64;
    ip[9] = 17;
    memcpy(ip + 12, server, 4);
    memset(ip + 16, 255, 4);
    be16(udp, 67);
    be16(udp + 2, 68);
    bootp[0] = 2;
    bootp[1] = 1;
    bootp[2] = 6;
    be32(bootp + 4, client->xid);
    memcpy(bootp + 16, address, 4);
    memcpy(bootp + 28, client->mac, 6);
    be32(bootp + 236, 0x63825363);
    size_t pos = 240;
    bootp[pos++] = 53; bootp[pos++] = 1; bootp[pos++] = kind;
    bootp[pos++] = 1; bootp[pos++] = 4;
    memcpy(bootp + pos, mask, 4); pos += 4;
    bootp[pos++] = 54; bootp[pos++] = 4;
    memcpy(bootp + pos, server, 4); pos += 4;
    bootp[pos++] = 51; bootp[pos++] = 4;
    be32(bootp + pos, 60); pos += 4;
    bootp[pos++] = 3; bootp[pos++] = 4;
    memcpy(bootp + pos, server, 4); pos += 4;
    bootp[pos++] = 6; bootp[pos++] = 4;
    memcpy(bootp + pos, server, 4); pos += 4;
    bootp[pos++] = 255;
    be16(udp + 4, (uint16_t)(8 + pos));
    be16(ip + 2, (uint16_t)(20 + 8 + pos));
    be16(ip + 10, (uint16_t)~sum_words(0, ip, 20));
    return 14 + 20 + 8 + pos;
}

int main(void)
{
    static const uint8_t mac[6] = {2, 0, 0, 1, 2, 3};
    static const uint8_t address[4] = {192, 0, 2, 77};
    static const uint8_t mask[4] = {255, 255, 255, 0};
    static const uint8_t server[4] = {192, 0, 2, 1};
    uint8_t reply[400];
    struct capture capture = {0};
    struct netd_dhcp_client client;
    netd_dhcp_init(&client, mac, 0x12345678,
        capture_send, capture_event, &capture);
    netd_dhcp_poll(&client, 1000, 0);
    assert(capture.sends == 0 && client.state == NETD_DHCP_WAIT_LINK);
    netd_dhcp_poll(&client, 2000, 1);
    assert(capture.sends == 1 && client.state == NETD_DHCP_SELECTING);
    assert(capture.length >= 14 + 20 + 8 + 300);
    assert(sum_words(0, capture.frame + 14, 20) == 65535);

    size_t length = make_reply(reply, &client, 2, address, mask, server);
    assert(netd_dhcp_receive(&client, reply, length, 2100) == 1);
    assert(client.state == NETD_DHCP_REQUESTING && client.offers == 1);
    netd_dhcp_poll(&client, 2100, 1);
    assert(capture.sends == 2);
    length = make_reply(reply, &client, 5, address, mask, server);
    assert(netd_dhcp_receive(&client, reply, length, 2200) == 1);
    assert(client.state == NETD_DHCP_BOUND && client.acks == 1);
    assert(memcmp(client.lease.address, address, 4) == 0);
    netd_dhcp_poll(&client, 32200, 1);
    assert(client.state == NETD_DHCP_RENEWING && capture.sends == 3);
    assert(netd_dhcp_receive(&client, reply, length, 32300) == 1);
    assert(client.state == NETD_DHCP_BOUND && client.acks == 2);
    netd_dhcp_poll(&client, 92301, 1);
    assert(client.state == NETD_DHCP_SELECTING);
    assert(capture.events > 5);
    puts("netd DHCP unit: ok");
    return 0;
}
