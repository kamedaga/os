/* SPDX-License-Identifier: MIT */
#include "link.h"
#include "network_config.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static struct {
    struct netd_link_info info;
    struct netd_link_ops ops;
    void *backend;
    netd_link_receive_fn receive;
    void *receiver;
    uint64_t last_generation;
    int attached;
} link;

void netd_link_init(netd_link_receive_fn receive, void *context)
{
    memset(&link, 0, sizeof(link));
    link.receive = receive;
    link.receiver = context;
}

int netd_link_attach(const struct netd_link_info *info,
    const struct netd_link_ops *ops, void *context)
{
    if (!info || !ops || !ops->send || !link.receive || link.attached ||
        !info->generation || info->generation <= link.last_generation ||
        info->interface_id != NETD_LINK_ID_PRIMARY ||
        info->mtu < 68 || info->mtu > NETD_LINK_FRAME_MAX - 18 ||
        info->carrier > 1 || netd_network_config_set_link_mac(info->mac) != 0)
        return -EINVAL;
    link.info = *info;
    link.ops = *ops;
    link.backend = context;
    link.last_generation = info->generation;
    link.attached = 1;
    return 0;
}

int netd_link_set_carrier(uint64_t generation, int carrier)
{
    if (!link.attached || generation != link.info.generation ||
        (carrier != 0 && carrier != 1))
        return -ESTALE;
    if (link.info.carrier != (uint8_t)carrier) {
        printf("[netd] link generation=%llu carrier=%d\n",
            (unsigned long long)generation, carrier);
        fflush(stdout);
    }
    link.info.carrier = (uint8_t)carrier;
    return 0;
}

int netd_link_detach(uint64_t generation)
{
    if (!link.attached || generation != link.info.generation)
        return -ESTALE;
    memset(&link.info, 0, sizeof(link.info));
    memset(&link.ops, 0, sizeof(link.ops));
    link.backend = NULL;
    link.attached = 0;
    return 0;
}

int netd_link_receive(uint64_t generation, const void *frame, size_t length)
{
    if (!link.attached || generation != link.info.generation)
        return -ESTALE;
    if (!link.info.carrier) return -ENETDOWN;
    if (!frame || length < 14 || length > NETD_LINK_FRAME_MAX ||
        length > (size_t)link.info.mtu + 18)
        return -EMSGSIZE;
    return link.receive(link.receiver, generation, frame, length);
}

int netd_link_send(const void *frame, size_t length)
{
    if (!link.attached || !link.info.carrier) return -ENETDOWN;
    if (!frame || length < 14 || length > NETD_LINK_FRAME_MAX ||
        length > (size_t)link.info.mtu + 18)
        return -EMSGSIZE;
    return link.ops.send(link.backend, frame, length);
}

void netd_link_poll(void)
{
    if (link.attached && link.ops.poll)
        link.ops.poll(link.backend);
}

const struct netd_link_info *netd_link_current(void)
{
    return link.attached ? &link.info : NULL;
}
