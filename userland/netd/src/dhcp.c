/* SPDX-License-Identifier: MIT */
#include "dhcp.h"

#include <errno.h>
#include <string.h>

enum {
    DHCP_BOOTP_BYTES = 236,
    DHCP_MESSAGE_BYTES = 300,
    DHCP_FRAME_BYTES = 14 + 20 + 8 + DHCP_MESSAGE_BYTES,
    DHCP_CLIENT_PORT = 68,
    DHCP_SERVER_PORT = 67,
    DHCP_DISCOVER = 1,
    DHCP_OFFER = 2,
    DHCP_REQUEST = 3,
    DHCP_ACK = 5,
    DHCP_NAK = 6,
};

struct dhcp_reply {
    uint8_t type;
    uint8_t address[4];
    uint8_t netmask[4];
    uint8_t gateway[4];
    uint8_t dns[4];
    uint8_t server[4];
    uint8_t source_mac[6];
    uint32_t lease_seconds;
    uint32_t renew_seconds;
    uint32_t rebind_seconds;
    unsigned options;
};

enum {
    DHCP_HAVE_TYPE = 1u << 0,
    DHCP_HAVE_MASK = 1u << 1,
    DHCP_HAVE_SERVER = 1u << 2,
    DHCP_HAVE_LEASE = 1u << 3,
    DHCP_HAVE_RENEW = 1u << 4,
    DHCP_HAVE_REBIND = 1u << 5,
};

static uint16_t read16(const uint8_t *bytes)
{
    return (uint16_t)((uint16_t)bytes[0] << 8 | bytes[1]);
}

static uint32_t read32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 |
        (uint32_t)bytes[2] << 8 | bytes[3];
}

static void write16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)(value >> 8);
    bytes[1] = (uint8_t)value;
}

static void write32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value >> 24);
    bytes[1] = (uint8_t)(value >> 16);
    bytes[2] = (uint8_t)(value >> 8);
    bytes[3] = (uint8_t)value;
}

static uint32_t checksum_add(uint32_t sum, const uint8_t *bytes, size_t count)
{
    while (count >= 2) {
        sum += read16(bytes);
        bytes += 2;
        count -= 2;
    }
    if (count != 0) sum += (uint32_t)bytes[0] << 8;
    return sum;
}

static uint16_t checksum_finish(uint32_t sum)
{
    while (sum >> 16) sum = (sum & 0xffffu) + (sum >> 16);
    return (uint16_t)~sum;
}

static int ipv4_valid(const uint8_t address[4])
{
    return address[0] != 0 && address[0] < 224 &&
        !(address[0] == 255 && address[1] == 255 &&
          address[2] == 255 && address[3] == 255);
}

static int mask_valid(const uint8_t mask[4])
{
    unsigned ones = 0;
    int zero_seen = 0;
    for (unsigned byte = 0; byte < 4; ++byte) {
        for (unsigned bit = 0; bit < 8; ++bit) {
            int one = (mask[byte] >> (7u - bit)) & 1u;
            if (!one) zero_seen = 1;
            else if (zero_seen) return 0;
            else ++ones;
        }
    }
    return ones > 0 && ones < 32;
}

static void event(struct netd_dhcp_client *client, const char *name, int detail)
{
    if (client->event != NULL)
        client->event(client->context, name, detail);
}

const char *netd_dhcp_state_name(enum netd_dhcp_state state)
{
    switch (state) {
    case NETD_DHCP_WAIT_LINK: return "wait-link";
    case NETD_DHCP_SELECTING: return "discover";
    case NETD_DHCP_REQUESTING: return "request";
    case NETD_DHCP_BOUND: return "bound";
    case NETD_DHCP_RENEWING: return "renew";
    case NETD_DHCP_REBINDING: return "rebind";
    case NETD_DHCP_EXPIRED: return "expired";
    case NETD_DHCP_DISABLED: return "disabled";
    }
    return "invalid";
}

void netd_dhcp_init(struct netd_dhcp_client *client, const uint8_t mac[6],
    uint32_t xid, netd_dhcp_send_fn send, netd_dhcp_event_fn note,
    void *context)
{
    if (client == NULL || mac == NULL) return;
    memset(client, 0, sizeof(*client));
    client->state = NETD_DHCP_WAIT_LINK;
    memcpy(client->mac, mac, sizeof(client->mac));
    client->xid = xid ? xid : 1;
    client->send = send;
    client->event = note;
    client->context = context;
    event(client, "waiting for Ethernet carrier", 0);
}

void netd_dhcp_disable(struct netd_dhcp_client *client)
{
    if (client == NULL || client->state == NETD_DHCP_DISABLED) return;
    client->state = NETD_DHCP_DISABLED;
    event(client, "disabled by configured IPv4 policy", 0);
}

static void option(uint8_t *message, size_t *cursor, uint8_t code,
    const void *value, uint8_t length)
{
    message[(*cursor)++] = code;
    message[(*cursor)++] = length;
    memcpy(message + *cursor, value, length);
    *cursor += length;
}

static int send_message(struct netd_dhcp_client *client)
{
    uint8_t frame[DHCP_FRAME_BYTES] = {0};
    uint8_t *ip = frame + 14;
    uint8_t *udp = ip + 20;
    uint8_t *bootp = udp + 8;
    const int discover = client->state == NETD_DHCP_SELECTING;
    const int renew = client->state == NETD_DHCP_RENEWING;
    const uint8_t broadcast[6] = {255, 255, 255, 255, 255, 255};
    const uint8_t all_hosts[4] = {255, 255, 255, 255};
    const uint8_t *destination_mac = renew ? client->server_mac : broadcast;
    const uint8_t *destination_ip = renew ? client->lease.server : all_hosts;
    const uint8_t *source_ip = renew || client->state == NETD_DHCP_REBINDING ?
        client->lease.address : NULL;
    const uint8_t request_options[] = {1, 3, 6, 51, 58, 59};
    const uint8_t message_type = discover ? DHCP_DISCOVER : DHCP_REQUEST;
    const uint8_t client_id[7] = {1, client->mac[0], client->mac[1],
        client->mac[2], client->mac[3], client->mac[4], client->mac[5]};
    const uint8_t max_message[2] = {2, 64}; /* RFC 2131 minimum: 576 bytes. */
    size_t cursor = 240;

    if (client->send == NULL) return -EINVAL;
    memcpy(frame, destination_mac, 6);
    memcpy(frame + 6, client->mac, 6);
    write16(frame + 12, 0x0800);
    ip[0] = 0x45;
    write16(ip + 2, 20 + 8 + DHCP_MESSAGE_BYTES);
    write16(ip + 4, (uint16_t)client->xid);
    ip[8] = 64;
    ip[9] = 17;
    if (source_ip != NULL) memcpy(ip + 12, source_ip, 4);
    memcpy(ip + 16, destination_ip, 4);
    write16(ip + 10, checksum_finish(checksum_add(0, ip, 20)));

    write16(udp, DHCP_CLIENT_PORT);
    write16(udp + 2, DHCP_SERVER_PORT);
    write16(udp + 4, 8 + DHCP_MESSAGE_BYTES);
    bootp[0] = 1;
    bootp[1] = 1;
    bootp[2] = 6;
    write32(bootp + 4, client->xid);
    if (!renew) write16(bootp + 10, 0x8000);
    if (source_ip != NULL) memcpy(bootp + 12, source_ip, 4);
    memcpy(bootp + 28, client->mac, 6);
    bootp[236] = 99;
    bootp[237] = 130;
    bootp[238] = 83;
    bootp[239] = 99;
    option(bootp, &cursor, 53, &message_type, 1);
    option(bootp, &cursor, 61, client_id, sizeof(client_id));
    option(bootp, &cursor, 55, request_options, sizeof(request_options));
    option(bootp, &cursor, 57, max_message, sizeof(max_message));
    if (client->state == NETD_DHCP_REQUESTING) {
        option(bootp, &cursor, 50, client->offer.address, 4);
        option(bootp, &cursor, 54, client->offer.server, 4);
    }
    bootp[cursor] = 255;

    /* A zero IPv4 UDP checksum is legal, but computing it also covers
     * stricter DHCP relays before the regular IP stack has an address. */
    uint32_t sum = checksum_add(0, ip + 12, 8);
    sum += 17 + 8 + DHCP_MESSAGE_BYTES;
    sum = checksum_add(sum, udp, 8 + DHCP_MESSAGE_BYTES);
    uint16_t udp_checksum = checksum_finish(sum);
    write16(udp + 6, udp_checksum ? udp_checksum : 0xffff);
    return client->send(client->context, frame, sizeof(frame));
}

static void start_discovery(struct netd_dhcp_client *client,
    uint64_t now_ms)
{
    client->state = NETD_DHCP_SELECTING;
    client->xid += 0x9e3779b9u;
    client->generation++;
    client->attempts = 0;
    memset(&client->offer, 0, sizeof(client->offer));
    client->next_send_ms = now_ms;
    event(client, "discover started", 0);
}

void netd_dhcp_poll(struct netd_dhcp_client *client, uint64_t now_ms,
    int carrier)
{
    if (client == NULL || client->state == NETD_DHCP_DISABLED) return;
    if (client->state == NETD_DHCP_BOUND ||
        client->state == NETD_DHCP_RENEWING ||
        client->state == NETD_DHCP_REBINDING) {
        if (now_ms >= client->expire_ms) {
            client->state = NETD_DHCP_EXPIRED;
            event(client, "lease expired", -ETIMEDOUT);
        } else if (now_ms >= client->rebind_ms &&
                   client->state != NETD_DHCP_REBINDING) {
            client->state = NETD_DHCP_REBINDING;
            client->attempts = 0;
            client->next_send_ms = now_ms;
            event(client, "lease rebind", 0);
        } else if (now_ms >= client->renew_ms &&
                   client->state == NETD_DHCP_BOUND) {
            client->state = NETD_DHCP_RENEWING;
            client->attempts = 0;
            client->next_send_ms = now_ms;
            event(client, "lease renew", 0);
        }
    }
    if (!carrier) {
        if (client->state == NETD_DHCP_SELECTING ||
            client->state == NETD_DHCP_REQUESTING) {
            client->state = NETD_DHCP_WAIT_LINK;
            event(client, "carrier lost during acquisition", -ENETDOWN);
        }
        return;
    }
    if (client->state == NETD_DHCP_WAIT_LINK ||
        client->state == NETD_DHCP_EXPIRED)
        start_discovery(client, now_ms);
    if (client->state == NETD_DHCP_BOUND || now_ms < client->next_send_ms)
        return;
    if (client->state == NETD_DHCP_REQUESTING && client->attempts >= 5) {
        start_discovery(client, now_ms);
    }
    int error = send_message(client);
    if (error != 0) client->last_error = error;
    client->attempts++;
    unsigned shift = client->attempts - 1;
    if (shift > 5) shift = 5;
    client->next_send_ms = now_ms + (UINT64_C(1000) << shift);
    event(client, client->state == NETD_DHCP_SELECTING ?
        "discover transmitted" : "request transmitted", error);
}

static int parse_reply(const struct netd_dhcp_client *client,
    const uint8_t *frame, size_t length, struct dhcp_reply *reply)
{
    if (length < 14 + 20 + 8 + 240 || read16(frame + 12) != 0x0800)
        return 0;
    const uint8_t *ip = frame + 14;
    size_t ip_bytes = (size_t)(ip[0] & 0x0f) * 4;
    if ((ip[0] >> 4) != 4 || ip_bytes < 20 || ip[9] != 17 ||
        read16(ip + 6) & 0x3fff || length < 14 + ip_bytes + 8 + 240)
        return 0;
    size_t ip_total = read16(ip + 2);
    if (ip_total < ip_bytes + 8 + 240 || ip_total > length - 14 ||
        checksum_finish(checksum_add(0, ip, ip_bytes)) != 0)
        return 0;
    const uint8_t *udp = ip + ip_bytes;
    size_t udp_bytes = read16(udp + 4);
    if (read16(udp) != DHCP_SERVER_PORT ||
        read16(udp + 2) != DHCP_CLIENT_PORT || udp_bytes < 8 + 240 ||
        udp_bytes > ip_total - ip_bytes)
        return 0;
    const uint8_t *bootp = udp + 8;
    if (bootp[0] != 2 || bootp[1] != 1 || bootp[2] != 6 ||
        read32(bootp + 4) != client->xid ||
        memcmp(bootp + 28, client->mac, 6) != 0 ||
        read32(bootp + DHCP_BOOTP_BYTES) != 0x63825363u)
        return 0;
    memset(reply, 0, sizeof(*reply));
    memcpy(reply->address, bootp + 16, 4);
    memcpy(reply->source_mac, frame + 6, 6);
    size_t option_bytes = udp_bytes - 8;
    for (size_t cursor = 240; cursor < option_bytes;) {
        uint8_t code = bootp[cursor++];
        if (code == 0) continue;
        if (code == 255) break;
        if (cursor == option_bytes) return -EINVAL;
        uint8_t size = bootp[cursor++];
        if (size > option_bytes - cursor) return -EINVAL;
        const uint8_t *value = bootp + cursor;
        switch (code) {
        case 1:
            if (size == 4) {
                memcpy(reply->netmask, value, 4);
                reply->options |= DHCP_HAVE_MASK;
            }
            break;
        case 3:
            if (size >= 4) memcpy(reply->gateway, value, 4);
            break;
        case 6:
            if (size >= 4) memcpy(reply->dns, value, 4);
            break;
        case 51:
            if (size == 4) {
                reply->lease_seconds = read32(value);
                reply->options |= DHCP_HAVE_LEASE;
            }
            break;
        case 53:
            if (size == 1) {
                reply->type = value[0];
                reply->options |= DHCP_HAVE_TYPE;
            }
            break;
        case 54:
            if (size == 4) {
                memcpy(reply->server, value, 4);
                reply->options |= DHCP_HAVE_SERVER;
            }
            break;
        case 58:
            if (size == 4) {
                reply->renew_seconds = read32(value);
                reply->options |= DHCP_HAVE_RENEW;
            }
            break;
        case 59:
            if (size == 4) {
                reply->rebind_seconds = read32(value);
                reply->options |= DHCP_HAVE_REBIND;
            }
            break;
        }
        cursor += size;
    }
    return 1;
}

static int lease_from_reply(const struct dhcp_reply *reply,
    struct netd_dhcp_lease *lease)
{
    if ((reply->options & (DHCP_HAVE_TYPE | DHCP_HAVE_MASK |
         DHCP_HAVE_SERVER | DHCP_HAVE_LEASE)) !=
        (DHCP_HAVE_TYPE | DHCP_HAVE_MASK | DHCP_HAVE_SERVER |
         DHCP_HAVE_LEASE) || !ipv4_valid(reply->address) ||
        !ipv4_valid(reply->server) || !mask_valid(reply->netmask) ||
        reply->lease_seconds < 4)
        return -EINVAL;
    memcpy(lease->address, reply->address, 4);
    memcpy(lease->netmask, reply->netmask, 4);
    memcpy(lease->gateway, reply->gateway, 4);
    memcpy(lease->dns, reply->dns, 4);
    memcpy(lease->server, reply->server, 4);
    lease->seconds = reply->lease_seconds;
    lease->renew_seconds = reply->options & DHCP_HAVE_RENEW ?
        reply->renew_seconds : lease->seconds / 2;
    lease->rebind_seconds = reply->options & DHCP_HAVE_REBIND ?
        reply->rebind_seconds : (uint32_t)((uint64_t)lease->seconds * 7 / 8);
    if (lease->renew_seconds == 0 ||
        lease->renew_seconds >= lease->rebind_seconds ||
        lease->rebind_seconds >= lease->seconds)
        return -EINVAL;
    return 0;
}

int netd_dhcp_receive(struct netd_dhcp_client *client, const void *frame,
    size_t length, uint64_t now_ms)
{
    if (client == NULL || frame == NULL ||
        client->state == NETD_DHCP_DISABLED) return 0;
    struct dhcp_reply reply;
    int parsed = parse_reply(client, frame, length, &reply);
    if (parsed == 0) return 0;
    if (parsed < 0) {
        client->rejected++;
        client->last_error = parsed;
        event(client, "malformed reply", parsed);
        return 1;
    }
    if (reply.type == DHCP_NAK &&
        (client->state == NETD_DHCP_REQUESTING ||
         client->state == NETD_DHCP_RENEWING ||
         client->state == NETD_DHCP_REBINDING)) {
        client->state = NETD_DHCP_EXPIRED;
        client->last_error = -EADDRNOTAVAIL;
        event(client, "server rejected lease", -EADDRNOTAVAIL);
        return 1;
    }
    if (client->state == NETD_DHCP_SELECTING && reply.type == DHCP_OFFER) {
        struct netd_dhcp_lease offered;
        if (lease_from_reply(&reply, &offered) != 0) goto rejected;
        client->offer = offered;
        memcpy(client->server_mac, reply.source_mac, 6);
        client->offers++;
        client->state = NETD_DHCP_REQUESTING;
        client->attempts = 0;
        client->next_send_ms = now_ms;
        event(client, "offer accepted", 0);
        return 1;
    }
    if ((client->state == NETD_DHCP_REQUESTING ||
         client->state == NETD_DHCP_RENEWING ||
         client->state == NETD_DHCP_REBINDING) && reply.type == DHCP_ACK) {
        struct netd_dhcp_lease accepted;
        if (lease_from_reply(&reply, &accepted) != 0) goto rejected;
        if (client->state == NETD_DHCP_REQUESTING &&
            (memcmp(accepted.server, client->offer.server, 4) != 0 ||
             memcmp(accepted.address, client->offer.address, 4) != 0))
            goto rejected;
        if (client->state != NETD_DHCP_REQUESTING &&
            memcmp(accepted.address, client->lease.address, 4) != 0) {
            client->last_error = -EADDRNOTAVAIL;
            event(client, "renewal changed IPv4 address", -EADDRNOTAVAIL);
            return 1;
        }
        client->lease = accepted;
        memcpy(client->server_mac, reply.source_mac, 6);
        client->renew_ms = now_ms + (uint64_t)accepted.renew_seconds * 1000;
        client->rebind_ms = now_ms + (uint64_t)accepted.rebind_seconds * 1000;
        client->expire_ms = now_ms + (uint64_t)accepted.seconds * 1000;
        client->state = NETD_DHCP_BOUND;
        client->attempts = 0;
        client->acks++;
        client->last_error = 0;
        event(client, "lease acknowledged", (int)accepted.seconds);
        return 1;
    }
    return 1;
rejected:
    client->rejected++;
    client->last_error = -EINVAL;
    event(client, "invalid lease offer or ACK", -EINVAL);
    return 1;
}
