/* SPDX-License-Identifier: MIT */
#include "network_config.h"

#include <stdio.h>
#include <string.h>

static struct netd_network_config active;
static int configured;

static int valid_mac(const uint8_t mac[6])
{
    uint8_t any = 0;
    for (unsigned i = 0; i < 6; ++i) any |= mac[i];
    return any != 0 && (mac[0] & 1u) == 0;
}

static int valid_mask(const uint8_t mask[4])
{
    unsigned bits = 0;
    int ended = 0;
    for (unsigned i = 0; i < 4; ++i) {
        for (unsigned bit = 0; bit < 8; ++bit) {
            int set = (mask[i] >> (7u - bit)) & 1u;
            if (!set) ended = 1;
            else if (ended) return 0;
            else ++bits;
        }
    }
    return bits > 0 && bits < 32;
}

static void format_ipv4(char out[16], const uint8_t address[4])
{
    (void)snprintf(out, 16, "%u.%u.%u.%u",
        address[0], address[1], address[2], address[3]);
}

static int parse_ipv4(const char *text, size_t length, uint8_t out[4])
{
    size_t cursor = 0;
    for (unsigned octet = 0; octet < 4; ++octet) {
        unsigned value = 0, digits = 0;
        while (cursor < length && text[cursor] >= '0' && text[cursor] <= '9') {
            value = value * 10 + (unsigned)(text[cursor++] - '0');
            if (++digits > 3 || value > 255) return -22;
        }
        if (!digits) return -22;
        out[octet] = (uint8_t)value;
        if (octet < 3) {
            if (cursor >= length || text[cursor++] != '.') return -22;
        }
    }
    return cursor == length ? 0 : -22;
}

int netd_network_config_parse_text(const char *text,
    struct netd_ipv4_boot_config *out)
{
    static const char *const keys[] = {"address", "netmask", "gateway", "dns"};
    if (!text || !out) return -22;
    memset(out, 0, sizeof(*out));
    uint8_t *const fields[] = {out->address, out->netmask, out->gateway, out->dns};
    unsigned seen = 0;
    for (const char *line = text; *line;) {
        const char *end = strchr(line, '\n');
        if (!end) end = line + strlen(line);
        size_t length = (size_t)(end - line);
        if (length && line[length - 1] == '\r') --length;
        if (length && line[0] != '#') {
            const char *equal = memchr(line, '=', length);
            if (!equal) return -22;
            size_t key_length = (size_t)(equal - line);
            unsigned key = 0;
            for (; key < 4; ++key)
                if (strlen(keys[key]) == key_length &&
                    !memcmp(keys[key], line, key_length)) break;
            if (key == 4 || (seen & (1u << key)) ||
                parse_ipv4(equal + 1, length - key_length - 1, fields[key])) return -22;
            seen |= 1u << key;
        }
        line = *end ? end + 1 : end;
    }
    return seen == 0x0f && valid_mask(out->netmask) &&
        (out->address[0] || out->address[1] ||
         out->address[2] || out->address[3]) ? 0 : -22;
}

int netd_network_config_init(const struct netd_ipv4_boot_config *boot)
{
    if (!boot || !valid_mask(boot->netmask) ||
        (!boot->address[0] && !boot->address[1] && !boot->address[2] && !boot->address[3]))
        return -22;
    /* The NIC may have attached before IPv4 policy exists. Preserve its MAC
     * when a policy is supplied later through the RAM root. */
    uint8_t link_mac[sizeof(active.link_mac)];
    memcpy(link_mac, active.link_mac, sizeof(link_mac));
    memset(&active, 0, sizeof(active));
    memcpy(active.link_mac, link_mac, sizeof(link_mac));
    memcpy(active.address, boot->address, 4);
    memcpy(active.netmask, boot->netmask, 4);
    memcpy(active.gateway, boot->gateway, 4);
    memcpy(active.dns, boot->dns, 4);
    for (unsigned i = 0; i < 4; ++i)
        active.broadcast[i] = active.address[i] | (uint8_t)~active.netmask[i];
    format_ipv4(active.address_text, active.address);
    format_ipv4(active.mask_text, active.netmask);
    format_ipv4(active.broadcast_text, active.broadcast);
    format_ipv4(active.gateway_text, active.gateway);
    format_ipv4(active.dns_text, active.dns);
    configured = 1;
    return 0;
}

int netd_network_config_set_link_mac(const uint8_t mac[6])
{
    if (!mac || !valid_mac(mac)) return -22;
    memcpy(active.link_mac, mac, 6);
    return 0;
}

const struct netd_network_config *netd_network_config_get(void)
{
    return configured ? &active : NULL;
}
