#define _GNU_SOURCE

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "filed/ipc_protocol.h"
#include "termd/boot_config.h"
#include "pacha/ipc.h"
#include "pachaos_capsule_launcher.h"

struct seed0_service {
    const char *name;
    unsigned flags;
    int control_fd;
    int peer_control_fd;
    int endpoint_fd;
};

static const uint64_t seed0_endpoint_rights =
    PACHA_FD_RIGHT_INSPECT |
    PACHA_FD_RIGHT_WAIT |
    PACHA_FD_RIGHT_POLL |
    PACHA_FD_RIGHT_CLOSE |
    PACHA_FD_RIGHT_SEND |
    PACHA_FD_RIGHT_RECV |
    PACHA_FD_RIGHT_CALL |
    PACHA_FD_RIGHT_TRANSFER;

static const uint64_t seed0_channel_rights =
    PACHA_FD_RIGHT_INSPECT |
    PACHA_FD_RIGHT_WAIT |
    PACHA_FD_RIGHT_POLL |
    PACHA_FD_RIGHT_SET_FLAGS |
    PACHA_FD_RIGHT_CLOSE |
    PACHA_FD_RIGHT_SEND |
    PACHA_FD_RIGHT_RECV |
    PACHA_FD_RIGHT_CALL |
    PACHA_FD_RIGHT_TRANSFER;

enum {
    SEED0_STORAGE_READY_MAGIC = 0x3159445252545330ull,
    SEED0_SERVICES_READY_MAGIC = 0x3159445256533053ull,
};

static void *seed0_filed_payload(void *page)
{
    return page == NULL ? NULL : (uint8_t *)page + PACHA_SERVICE_HEADER_BYTES;
}

static int seed0_filed_call(
    int filed_endpoint_fd,
    uint32_t op,
    uint64_t request_id,
    int transfer_fd)
{
    if (filed_endpoint_fd < 16 || request_id == 0) {
        return -22;
    }
    const uint64_t page_rights =
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE;
    const int page_fd = pacha_vmo_create(PACHA_SERVICE_PAGE_BYTES, page_rights, 0);
    if (page_fd < 16) {
        return page_fd;
    }
    void *page = pacha_mmap(
        page_fd,
        PACHA_SERVICE_PAGE_BYTES,
        PACHA_PROT_READ | PACHA_PROT_WRITE,
        PACHA_MMAP_SHARED,
        0);
    if (page == NULL) {
        (void)pacha_fd_close(page_fd);
        return -5;
    }
    memset(page, 0, PACHA_SERVICE_PAGE_BYTES);
    pacha_service_envelope_t *header = (pacha_service_envelope_t *)page;
    header->magic = PACHA_SERVICE_REQUEST_MAGIC;
    header->abi_version = PACHA_SERVICE_ABI_VERSION;
    header->service_id = FILED_SERVICE_ID;
    header->op = op;
    const int has_service_payload =
        op == FILED_OP_SERVICE_SET_NETD_SOCKET ||
        op == FILED_OP_SERVICE_SET_TERMD_TTY;
    header->flags = has_service_payload ? PACHA_SERVICE_FLAG_PAGE_PAYLOAD : 0;
    header->request_id = request_id;
    header->trace_id = request_id;
    header->payload_size = has_service_payload ? sizeof(filed_service_endpoint_request_t) : 0;
    header->fd_count = transfer_fd >= 16 ? 1u : 0u;
    if (has_service_payload) {
        filed_service_endpoint_request_t *payload =
            (filed_service_endpoint_request_t *)seed0_filed_payload(page);
        payload->endpoint_kind = op;
    }

    struct pacha_ipc_fd fds[2];
    memset(fds, 0, sizeof(fds));
    fds[0].fd = (uint64_t)(uint32_t)page_fd;
    fds[0].rights = page_rights;
    uint64_t fd_count = 1;
    if (transfer_fd >= 16) {
        fds[1].fd = (uint64_t)(uint32_t)transfer_fd;
        fds[1].rights =
            PACHA_FD_RIGHT_INSPECT |
            PACHA_FD_RIGHT_DUP |
            PACHA_FD_RIGHT_WAIT |
            PACHA_FD_RIGHT_POLL |
            PACHA_FD_RIGHT_CLOSE |
            PACHA_FD_RIGHT_SEND |
            PACHA_FD_RIGHT_RECV |
            PACHA_FD_RIGHT_SET_FLAGS |
            PACHA_FD_RIGHT_CALL |
            PACHA_FD_RIGHT_TRANSFER;
        fd_count = 2;
    }

    const struct pacha_ipc_msg request = {
        .word0 = PACHA_SERVICE_REQUEST_MAGIC,
        .word1 = 0,
        .word2 = 0,
        .word3 = request_id,
        .fds = fds,
        .fd_count = fd_count,
    };
    const int reply_fd = pacha_ipc_call(filed_endpoint_fd, &request);
    if (reply_fd < 16) {
        (void)pacha_munmap(page, PACHA_SERVICE_PAGE_BYTES);
        (void)pacha_fd_close(page_fd);
        return reply_fd;
    }

    struct pacha_ipc_msg reply;
    memset(&reply, 0, sizeof(reply));
    const int recv_status = pacha_ipc_recv_wait(reply_fd, &reply, PACHA_FD_WAIT_FOREVER);
    (void)pacha_fd_close(reply_fd);
    if (recv_status != 0) {
        (void)pacha_munmap(page, PACHA_SERVICE_PAGE_BYTES);
        (void)pacha_fd_close(page_fd);
        return recv_status;
    }
    const pacha_service_envelope_t *reply_header =
        (const pacha_service_envelope_t *)page;
    if (reply.word0 != PACHA_SERVICE_REPLY_MAGIC ||
        reply.word3 != request_id ||
        reply_header->magic != PACHA_SERVICE_REPLY_MAGIC ||
        reply_header->service_id != FILED_SERVICE_ID ||
        reply_header->op != op ||
        reply_header->request_id != request_id ||
        reply_header->status != 0)
    {
        const int status = reply_header->status != 0 ?
            (int)reply_header->status :
            (reply.word1 != 0 ? (int)(int64_t)reply.word1 : -5);
        (void)pacha_munmap(page, PACHA_SERVICE_PAGE_BYTES);
        (void)pacha_fd_close(page_fd);
        return status;
    }
    (void)pacha_munmap(page, PACHA_SERVICE_PAGE_BYTES);
    (void)pacha_fd_close(page_fd);
    return 0;
}

static int seed0_register_netd_socket_endpoint(int filed_endpoint_fd, int netd_socket_endpoint_fd)
{
    return seed0_filed_call(
        filed_endpoint_fd,
        FILED_OP_SERVICE_SET_NETD_SOCKET,
        0x53454544304e4554ull,
        netd_socket_endpoint_fd);
}

static int seed0_register_termd_tty_endpoint(int filed_endpoint_fd, int termd_tty_endpoint_fd)
{
    return seed0_filed_call(
        filed_endpoint_fd,
        FILED_OP_SERVICE_SET_TERMD_TTY,
        0x5345454430545459ull,
        termd_tty_endpoint_fd);
}

static int seed0_wait_filed_ready(int filed_endpoint_fd)
{
    return seed0_filed_call(
        filed_endpoint_fd,
        FILED_OP_HELLO,
        0x534545443046494cull,
        -1);
}

static int seed0_wait_termd_ready(int termd_ready_channel_fd)
{
    if (termd_ready_channel_fd < 16) {
        return -22;
    }

    struct pacha_ipc_msg reply;
    memset(&reply, 0, sizeof(reply));
    const int recv_status = pacha_ipc_recv_wait(termd_ready_channel_fd, &reply, PACHA_FD_WAIT_FOREVER);
    if (recv_status != 0) {
        return recv_status;
    }
    if (reply.word0 != TERMD_BOOT_READY_MAGIC ||
        (int64_t)reply.word1 != 0 ||
        reply.word2 == 0)
    {
        return reply.word1 != 0 ? (int)(int64_t)reply.word1 : -5;
    }
    return 0;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    printf("[seed0boot] start\n");

    enum { seed0_service_count = 1 };
    struct seed0_service *services = calloc(seed0_service_count, sizeof(*services));
    if (!services) {
        fprintf(stderr, "[seed0boot] calloc failed\n");
        return 1;
    }
    services[0].name = "seed0-root";
    services[0].flags = 0;
    services[0].control_fd = -1;
    services[0].peer_control_fd = -1;
    services[0].endpoint_fd = pacha_ipc_endpoint_create(seed0_endpoint_rights, 0);
    if (services[0].endpoint_fd < 0) {
        fprintf(stderr, "[seed0boot] endpoint create failed status=%d\n", services[0].endpoint_fd);
        return 10;
    }

    struct pacha_ipc_channel_pair control_pair = { .a = -1, .b = -1 };
    if (pacha_ipc_channel_create(&control_pair, seed0_channel_rights, 0) != 0 || control_pair.a < 16 || control_pair.b < 16) {
        fprintf(stderr, "[seed0boot] control channel create failed a=%d b=%d\n", control_pair.a, control_pair.b);
        return 11;
    }
    services[0].control_fd = control_pair.a;
    services[0].peer_control_fd = control_pair.b;

    struct pacha_fd_info endpoint_info = {0};
    struct pacha_fd_info control_info = {0};
    if (pacha_fd_get_info(services[0].endpoint_fd, &endpoint_info) != 0 ||
        pacha_fd_get_info(services[0].control_fd, &control_info) != 0 ||
        endpoint_info.kind != PACHA_FD_KIND_ENDPOINT ||
        control_info.kind != PACHA_FD_KIND_CHANNEL) {
        fprintf(stderr,
            "[seed0boot] ipc fd info failed endpoint_fd=%d kind=%llu control_fd=%d kind=%llu\n",
            services[0].endpoint_fd,
            (unsigned long long)endpoint_info.kind,
            services[0].control_fd,
            (unsigned long long)control_info.kind);
        return 12;
    }

    struct pacha_ipc_msg control_msg = {
        .word0 = 0x5345454430424f4full,
        .word1 = 1,
        .word2 = 0,
        .word3 = 0,
    };
    if (pacha_ipc_send(services[0].control_fd, &control_msg) != 0) {
        fprintf(stderr, "[seed0boot] control send failed\n");
        return 13;
    }
    struct pacha_ipc_msg received_msg = {0};
    if (pacha_ipc_recv(services[0].peer_control_fd, &received_msg) != 0 ||
        received_msg.word0 != control_msg.word0 ||
        received_msg.word1 != control_msg.word1) {
        fprintf(stderr,
            "[seed0boot] control recv failed word0=%llx word1=%llu\n",
            (unsigned long long)received_msg.word0,
            (unsigned long long)received_msg.word1);
        return 14;
    }

    struct pacha_ipc_channel_pair storage_ready_pair = { .a = -1, .b = -1 };
    struct pacha_ipc_channel_pair service_ready_pair = { .a = -1, .b = -1 };
    if (pacha_ipc_channel_create(&storage_ready_pair, seed0_channel_rights, PACHA_FD_FLAG_INHERIT) != 0 ||
        storage_ready_pair.a < 16 ||
        storage_ready_pair.b < 16) {
        fprintf(stderr,
            "[seed0boot] storage ready channel create failed a=%d b=%d\n",
            storage_ready_pair.a,
            storage_ready_pair.b);
        return 15;
    }
    if (pacha_ipc_channel_create(&service_ready_pair, seed0_channel_rights, PACHA_FD_FLAG_INHERIT) != 0 ||
        service_ready_pair.a < 16 ||
        service_ready_pair.b < 16) {
        fprintf(stderr,
            "[seed0boot] service ready channel create failed a=%d b=%d\n",
            service_ready_pair.a,
            service_ready_pair.b);
        return 15;
    }

    printf("[seed0boot] storage_boot starting\n");
    int capsule_status = seed0_launch_storage_boot_nvme(storage_ready_pair.b, service_ready_pair.b);
    if (capsule_status != 0) {
        fprintf(stderr, "[seed0boot] storage_boot launch failed status=%d\n", capsule_status);
        return 17;
    }
    (void)pacha_fd_close(storage_ready_pair.b);
    (void)pacha_fd_close(service_ready_pair.b);

    printf("[seed0boot] storage_boot started\n");

    struct pacha_ipc_fd ready_fds[1];
    memset(ready_fds, 0, sizeof(ready_fds));
    struct pacha_ipc_msg ready_msg = {
        .fds = ready_fds,
        .fd_capacity = 1,
    };
    int ready_status = pacha_ipc_recv_wait(storage_ready_pair.a, &ready_msg, PACHA_FD_WAIT_FOREVER);
    if (ready_status != 0 ||
        ready_msg.word0 != SEED0_STORAGE_READY_MAGIC ||
        ready_msg.word1 != 0 ||
        ready_msg.fd_count != 1 ||
        ready_fds[0].fd < 16) {
        fprintf(stderr,
            "[seed0boot] storage ready recv failed status=%d word0=0x%llx word1=%llu fds=%llu fd0=%llu\n",
            ready_status,
            (unsigned long long)ready_msg.word0,
            (unsigned long long)ready_msg.word1,
            (unsigned long long)ready_msg.fd_count,
            (unsigned long long)ready_fds[0].fd);
        return 18;
    }
    const int filed_endpoint_fd = (int)ready_fds[0].fd;
    (void)pacha_fd_close(storage_ready_pair.a);
    printf("[seed0boot] filed endpoint received fd=%d\n", filed_endpoint_fd);

    const int filed_ready_status = seed0_wait_filed_ready(filed_endpoint_fd);
    if (filed_ready_status != 0) {
        fprintf(stderr, "[seed0boot] filed ready wait failed status=%d\n", filed_ready_status);
        return 19;
    }
    printf("[seed0boot] filed ready\n");

    printf("[seed0boot] termd starting\n");
    int termd_tty_endpoint_fd = -1;
    struct pacha_ipc_channel_pair termd_ready_pair = { .a = -1, .b = -1 };
    if (pacha_ipc_channel_create(&termd_ready_pair, seed0_channel_rights, 0) != 0 ||
        termd_ready_pair.a < 16 ||
        termd_ready_pair.b < 16) {
        fprintf(stderr,
            "[seed0boot] termd ready channel create failed a=%d b=%d\n",
            termd_ready_pair.a,
            termd_ready_pair.b);
        return 20;
    }
    capsule_status = seed0_launch_termd(termd_ready_pair.b, &termd_tty_endpoint_fd);
    (void)pacha_fd_close(termd_ready_pair.b);
    if (capsule_status != 0) {
        (void)pacha_fd_close(termd_ready_pair.a);
        fprintf(stderr, "[seed0boot] termd launch failed status=%d\n", capsule_status);
        return 20;
    }
    printf("[seed0boot] termd started\n");
    if (termd_tty_endpoint_fd >= 16) {
        const int register_status = seed0_register_termd_tty_endpoint(filed_endpoint_fd, termd_tty_endpoint_fd);
        if (register_status != 0) {
            fprintf(stderr, "[seed0boot] termd tty endpoint register failed status=%d\n", register_status);
            return 21;
        }
        const int termd_ready_status = seed0_wait_termd_ready(termd_ready_pair.a);
        (void)pacha_fd_close(termd_ready_pair.a);
        if (termd_ready_status != 0) {
            fprintf(stderr, "[seed0boot] termd ready wait failed status=%d\n", termd_ready_status);
            return 21;
        }
        printf("[seed0boot] termd ready\n");
        (void)pacha_fd_close(termd_tty_endpoint_fd);
    }

    printf("[seed0boot] netd starting\n");
    int netd_socket_endpoint_fd = -1;
    capsule_status = seed0_launch_netd(filed_endpoint_fd, &netd_socket_endpoint_fd);
    if (capsule_status != 0) {
        fprintf(stderr, "[seed0boot] netd launch failed status=%d\n", capsule_status);
        return 22;
    }
    printf("[seed0boot] netd started\n");
    if (netd_socket_endpoint_fd >= 16) {
        const int register_status = seed0_register_netd_socket_endpoint(filed_endpoint_fd, netd_socket_endpoint_fd);
        if (register_status != 0) {
            fprintf(stderr, "[seed0boot] netd socket endpoint register failed status=%d\n", register_status);
            return 23;
        }
        (void)pacha_fd_close(netd_socket_endpoint_fd);
    }
    const struct pacha_ipc_msg service_ready_msg = {
        .word0 = SEED0_SERVICES_READY_MAGIC,
        .word1 = 0,
        .word2 = 0,
        .word3 = 0x5345525649434553ull,
    };
    const int service_ready_status = pacha_ipc_send(service_ready_pair.a, &service_ready_msg);
    (void)pacha_fd_close(service_ready_pair.a);
    if (service_ready_status != 0) {
        fprintf(stderr, "[seed0boot] services ready send failed status=%d\n", service_ready_status);
        return 24;
    }
    printf("[seed0boot] services ready signal sent\n");
    (void)pacha_fd_close(filed_endpoint_fd);
    printf("[seed0boot] ready\n");
    fflush(stdout);
    fflush(stderr);
    for (;;) __asm__ volatile("pause");
}
