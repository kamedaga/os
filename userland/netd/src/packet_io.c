#include "netd_internal.h"

#include "link.h"
#include "kobox2_nic.h"
#include "network_config.h"
#include "status_file.h"
#include "upper.h"

#include "pacha/ipc.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

enum {
    NETD_RX_QUEUE_CAP = 64,
    NETD_FRAME_MAX = 2048,
};

struct netd_rx_frame {
    uint64_t generation;
    void *dev;
    size_t len;
    unsigned char bytes[NETD_FRAME_MAX];
};

struct netd_packet_io {
    struct netd_rx_frame rx_queue[NETD_RX_QUEUE_CAP];
    unsigned rx_head;
    unsigned rx_tail;
    unsigned rx_count;
    uint64_t rx_frames;
    uint64_t tx_frames;
    uint64_t rx_drops;
    int trace;
    int dhcp_started;
    struct netd_dhcp_client dhcp;
};

static struct netd_packet_io g_packet_io;

static uint64_t monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

static int dhcp_send(void *context, const void *frame, size_t length)
{
    (void)context;
    return netd_link_send(frame, length);
}

static void dhcp_event(void *context, const char *event, int detail)
{
    const struct netd_boot_config *cfg = context;
    char phase[80];
    (void)snprintf(phase, sizeof(phase), "DHCP %s status=%d", event, detail);
    netd_status_file_note(cfg, phase);
}

static uint16_t read_be16(const unsigned char *bytes)
{
    return ((uint16_t)bytes[0] << 8) | bytes[1];
}

static void write_be16(unsigned char *dst, uint16_t value)
{
    dst[0] = (unsigned char)(value >> 8);
    dst[1] = (unsigned char)value;
}

static int netd_packet_rx_callback(void *ctx, uint64_t generation,
    const void *frame, size_t frame_len)
{
    struct netd_packet_io *io = ctx;
    if (io == NULL || frame == NULL || frame_len == 0) {
        return -22;
    }
    if (frame_len > NETD_FRAME_MAX || io->rx_count == NETD_RX_QUEUE_CAP) {
        io->rx_drops++;
        return -11;
    }

    struct netd_rx_frame *slot = &io->rx_queue[io->rx_tail];
    slot->generation = generation;
    slot->dev = NULL;
    slot->len = frame_len;
    memcpy(slot->bytes, frame, frame_len);
    io->rx_tail = (io->rx_tail + 1u) % NETD_RX_QUEUE_CAP;
    io->rx_count++;
    io->rx_frames++;

    if (io->trace && frame_len >= 14) {
        printf("[netd] rx frame dev=%p len=%zu ethertype=0x%04x\n",
            slot->dev,
            frame_len,
            read_be16(slot->bytes + 12));
    }
    return 0;
}

static void netd_packet_io_drain_rx(void)
{
    while (g_packet_io.rx_count != 0) {
        const struct netd_rx_frame *frame = &g_packet_io.rx_queue[g_packet_io.rx_head];
        const struct netd_link_info *link = netd_link_current();
        if (link != NULL && link->carrier &&
            link->generation == frame->generation) {
            const struct netd_upper_frame upper_frame = {
                .dev = frame->dev,
                .bytes = frame->bytes,
                .len = frame->len,
            };
            int dhcp_reply = g_packet_io.dhcp_started ?
                netd_dhcp_receive(&g_packet_io.dhcp, frame->bytes,
                    frame->len, monotonic_ms()) : 0;
            if (!dhcp_reply)
                (void)netd_upper_receive_frame(&upper_frame);
        } else {
            g_packet_io.rx_drops++;
        }
        g_packet_io.rx_head = (g_packet_io.rx_head + 1u) % NETD_RX_QUEUE_CAP;
        g_packet_io.rx_count--;
    }
}

static int netd_packet_io_tx_frame(const void *frame, size_t frame_len)
{
    int status = netd_link_send(frame, frame_len);
    if (status == 0) {
        g_packet_io.tx_frames++;
    }
    return status;
}

static size_t build_arp_probe(unsigned char *frame, size_t frame_capacity)
{
    const struct netd_network_config *network = netd_network_config_get();
    const size_t frame_len = 60;

    if (frame == NULL || network == NULL || frame_capacity < frame_len) {
        return 0;
    }
    memset(frame, 0, frame_capacity);
    memset(frame, 0xff, 6);
    memcpy(frame + 6, network->link_mac, sizeof(network->link_mac));
    write_be16(frame + 12, 0x0806);
    write_be16(frame + 14, 0x0001);
    write_be16(frame + 16, 0x0800);
    frame[18] = 6;
    frame[19] = 4;
    write_be16(frame + 20, 0x0001);
    memcpy(frame + 22, network->link_mac, sizeof(network->link_mac));
    memcpy(frame + 28, network->address, sizeof(network->address));
    memcpy(frame + 38, network->gateway, sizeof(network->gateway));
    return frame_len;
}

static int netd_packet_io_smoke(void)
{
    unsigned char frame[64];
    size_t frame_len = build_arp_probe(frame, sizeof(frame));
    if (frame_len == 0) {
        return 1;
    }

    int tx_status = netd_packet_io_tx_frame(frame, frame_len);
    for (unsigned i = 0; i < 64; i++) {
        netd_packet_io_pump_once();
    }
    printf("[netd] packet smoke tx=%d tx_frames=%llu rx_frames=%llu delivered=%llu drops=%llu\n",
        tx_status,
        (unsigned long long)g_packet_io.tx_frames,
        (unsigned long long)g_packet_io.rx_frames,
        (unsigned long long)netd_upper_delivered_frames(),
        (unsigned long long)g_packet_io.rx_drops);
    return tx_status == 0 ? 0 : 1;
}

int netd_packet_io_start(struct netd_runtime *runtime)
{
    if (runtime == NULL || runtime->cfg == NULL) {
        return 6;
    }

    memset(&g_packet_io, 0, sizeof(g_packet_io));
    g_packet_io.trace = (runtime->cfg->flags & NETD_BOOT_FLAG_TRACE) != 0;
    netd_link_init(netd_packet_rx_callback, &g_packet_io);
    int nic_status = netd_kobox2_nic_start(runtime);
    if (nic_status != 0) return nic_status;
    return 0;
}

int netd_packet_io_dhcp_start(struct netd_runtime *runtime)
{
    if (runtime == NULL || runtime->cfg == NULL) return -EINVAL;
    if (g_packet_io.dhcp_started) return 0;
    const struct netd_link_info *link = netd_link_current();
    if (link == NULL) return -ENODEV;
    uint32_t xid = 0;
    if (pacha_getrandom(&xid, sizeof(xid), 0) != sizeof(xid))
        return -EIO;
    g_packet_io.dhcp_started = 1;
    netd_dhcp_init(&g_packet_io.dhcp, link->mac, xid,
        dhcp_send, dhcp_event, (void *)runtime->cfg);
    netd_status_file_bind_dhcp(&g_packet_io.dhcp);
    return 0;
}

void netd_packet_io_dhcp_disable(void)
{
    if (g_packet_io.dhcp_started)
        netd_dhcp_disable(&g_packet_io.dhcp);
}

const struct netd_dhcp_client *netd_packet_io_dhcp_status(void)
{
    return g_packet_io.dhcp_started ? &g_packet_io.dhcp : NULL;
}

int netd_packet_io_smoke_after_ipv4(void)
{
    uint64_t start = netd_metrics_read_tsc();
    int status = netd_packet_io_smoke();
    netd_metrics_record("packet_smoke", start, netd_metrics_read_tsc());
    return status;
}

void netd_packet_io_pump_once(void)
{
    netd_link_poll();
    if (g_packet_io.dhcp_started) {
        const struct netd_link_info *link = netd_link_current();
        netd_dhcp_poll(&g_packet_io.dhcp, monotonic_ms(),
            link != NULL && link->carrier);
    }
    netd_packet_io_drain_rx();
    netd_upper_poll();
}
