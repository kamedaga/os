#include "netd_internal.h"

#include "linux_subsystem/net/net_device.h"

#include <stdio.h>
#include <string.h>

enum {
    NETD_RX_QUEUE_CAP = 32,
    NETD_FRAME_MAX = 2048,
};

struct netd_rx_frame {
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
    uint64_t upper_delivered;
    int trace;
};

static struct netd_packet_io g_packet_io;

static uint16_t read_be16(const unsigned char *bytes)
{
    return ((uint16_t)bytes[0] << 8) | bytes[1];
}

static void write_be16(unsigned char *dst, uint16_t value)
{
    dst[0] = (unsigned char)(value >> 8);
    dst[1] = (unsigned char)value;
}

static void netd_packet_rx_callback(void *ctx, void *dev, const void *frame, size_t frame_len)
{
    struct netd_packet_io *io = ctx;
    if (io == NULL || frame == NULL || frame_len == 0) {
        return;
    }
    if (frame_len > NETD_FRAME_MAX || io->rx_count == NETD_RX_QUEUE_CAP) {
        io->rx_drops++;
        return;
    }

    struct netd_rx_frame *slot = &io->rx_queue[io->rx_tail];
    slot->dev = dev;
    slot->len = frame_len;
    memcpy(slot->bytes, frame, frame_len);
    io->rx_tail = (io->rx_tail + 1u) % NETD_RX_QUEUE_CAP;
    io->rx_count++;
    io->rx_frames++;

    if (io->trace && frame_len >= 14) {
        printf("[netd] rx frame dev=%p len=%zu ethertype=0x%04x\n",
            dev,
            frame_len,
            read_be16(slot->bytes + 12));
    }
}

static void netd_packet_deliver_upper(const struct netd_rx_frame *frame)
{
    (void)frame;
    g_packet_io.upper_delivered++;
}

static void netd_packet_io_drain_rx(void)
{
    while (g_packet_io.rx_count != 0) {
        const struct netd_rx_frame *frame = &g_packet_io.rx_queue[g_packet_io.rx_head];
        netd_packet_deliver_upper(frame);
        g_packet_io.rx_head = (g_packet_io.rx_head + 1u) % NETD_RX_QUEUE_CAP;
        g_packet_io.rx_count--;
    }
}

static int netd_packet_io_tx_frame(const void *frame, size_t frame_len)
{
    int status = kb_net_device_tx_frame(frame, frame_len);
    if (status == 0) {
        g_packet_io.tx_frames++;
    }
    return status;
}

static size_t build_arp_probe(unsigned char *frame, size_t frame_capacity)
{
    static const unsigned char source_mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
    static const unsigned char source_ip[4] = {10, 0, 2, 15};
    static const unsigned char target_ip[4] = {10, 0, 2, 2};
    const size_t frame_len = 60;

    if (frame == NULL || frame_capacity < frame_len) {
        return 0;
    }
    memset(frame, 0, frame_capacity);
    memset(frame, 0xff, 6);
    memcpy(frame + 6, source_mac, sizeof(source_mac));
    write_be16(frame + 12, 0x0806);
    write_be16(frame + 14, 0x0001);
    write_be16(frame + 16, 0x0800);
    frame[18] = 6;
    frame[19] = 4;
    write_be16(frame + 20, 0x0001);
    memcpy(frame + 22, source_mac, sizeof(source_mac));
    memcpy(frame + 28, source_ip, sizeof(source_ip));
    memcpy(frame + 38, target_ip, sizeof(target_ip));
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
        (unsigned long long)g_packet_io.upper_delivered,
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
    kb_net_device_set_rx_frame_callback(netd_packet_rx_callback, &g_packet_io);

    if ((runtime->cfg->flags & NETD_BOOT_FLAG_SMOKE) != 0) {
        uint64_t stage_start_cycles = netd_metrics_read_tsc();
        int smoke_status = netd_packet_io_smoke();
        netd_metrics_record("packet_smoke", stage_start_cycles, netd_metrics_read_tsc());
        if (smoke_status != 0) {
            return smoke_status;
        }
    }
    return 0;
}

void netd_packet_io_pump_once(void)
{
    kb_net_device_poll();
    netd_packet_io_drain_rx();
}
