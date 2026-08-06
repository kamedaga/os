#define _GNU_SOURCE

#include "bootstrap_abi.h"
#include "pacha/ipc.h"
#include "pacha/root_handoff.h"
#include "pachaos_capsule_launcher.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    SEED0_QEMU_NVME_VENDOR_ID = 0x1b36,
    SEED0_QEMU_NVME_DEVICE_ID = 0x0010,
};

static const uint64_t channel_rights = PACHA_FD_RIGHT_INSPECT |
    PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL |
    PACHA_FD_RIGHT_SET_FLAGS | PACHA_FD_RIGHT_CLOSE |
    PACHA_FD_RIGHT_SEND | PACHA_FD_RIGHT_RECV |
    PACHA_FD_RIGHT_CALL | PACHA_FD_RIGHT_TRANSFER;

static int is_nvme(const struct seed0_device_descriptor *device)
{
    return device->vendor_id == SEED0_QEMU_NVME_VENDOR_ID &&
        device->device_id == SEED0_QEMU_NVME_DEVICE_ID;
}

static int set_inherit(int fd, int enabled)
{
    return (int)pacha_fd_fcntl(fd, PACHA_FD_FCNTL_SET_FLAGS,
        enabled ? PACHA_FD_FLAG_INHERIT : 0,
        PACHA_FD_FLAG_INHERIT);
}

static int create_handoff_vmo(const struct pacha_root_handoff *handoff)
{
    const uint64_t rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE;
    const int fd = pacha_vmo_create(4096, rights, 0);
    if (fd < 16) return fd;
    void *page = pacha_mmap(fd, 4096, PACHA_PROT_READ | PACHA_PROT_WRITE,
        PACHA_MMAP_SHARED, 0);
    if (page == NULL) {
        (void)pacha_fd_close(fd);
        return -5;
    }
    memset(page, 0, 4096);
    memcpy(page, handoff, sizeof(*handoff));
    (void)pacha_munmap(page, 4096);
    return fd;
}

static int send_root_handoff(int channel_fd)
{
    const struct seed0_init_descriptor_page *desc = seed0_bootstrap_descriptor();
    if (channel_fd < 16 || desc == NULL) return -22;

    struct pacha_root_handoff handoff;
    memset(&handoff, 0, sizeof(handoff));
    handoff.magic = PACHA_ROOT_HANDOFF_MAGIC;
    handoff.version = PACHA_ROOT_HANDOFF_VERSION;

    const struct seed0_device_descriptor *selected[PACHA_ROOT_HANDOFF_MAX_DEVICES];
    memset(selected, 0, sizeof(selected));
    uint64_t source_count = desc->device_count;
    if (source_count > SEED0_INIT_MAX_DEVICE_DESCRIPTORS)
        source_count = SEED0_INIT_MAX_DEVICE_DESCRIPTORS;
    for (uint64_t i = 0; i < source_count; i++) {
        const struct seed0_device_descriptor *device = &desc->devices[i];
        if ((device->flags & SEED0_INIT_DEVICE_FLAG_PRESENT) == 0 ||
            !seed0_fd_is_dynamic(device->init_device_fd) || is_nvme(device))
            continue;
        if (handoff.device_count >= PACHA_ROOT_HANDOFF_MAX_DEVICES) return -7;
        const uint64_t slot = handoff.device_count++;
        selected[slot] = device;
        handoff.devices[slot] = (struct pacha_root_device_record) {
            .transfer_index = slot,
            .resource_id = device->resource_id,
            .vendor_id = device->vendor_id,
            .device_id = device->device_id,
            .subsystem_id = device->subsystem_id,
            .pci_segment = 0,
            .pci_bus = (uint32_t)device->pci_bus,
            .pci_device = (uint32_t)device->pci_device,
            .pci_function = (uint32_t)device->pci_function,
        };
    }

    const int metadata_fd = create_handoff_vmo(&handoff);
    if (metadata_fd < 16) return metadata_fd;
    struct pacha_ipc_fd fds[1 + PACHA_ROOT_HANDOFF_MAX_DEVICES];
    memset(fds, 0, sizeof(fds));
    struct pacha_fd_info metadata_info;
    if (pacha_fd_get_info(metadata_fd, &metadata_info) != 0) {
        (void)pacha_fd_close(metadata_fd);
        return -13;
    }
    fds[0].fd = (uint64_t)(uint32_t)metadata_fd;
    fds[0].rights = metadata_info.rights;
    fds[0].transfer_flags = PACHA_IPC_TRANSFER_MOVE;
    for (uint64_t i = 0; i < handoff.device_count; i++) {
        const int fd = (int)selected[i]->init_device_fd;
        struct pacha_fd_info info;
        if (pacha_fd_get_info(fd, &info) != 0) {
            (void)pacha_fd_close(metadata_fd);
            return -13;
        }
        fds[i + 1].fd = (uint64_t)(uint32_t)fd;
        fds[i + 1].rights = info.rights;
        fds[i + 1].transfer_flags = PACHA_IPC_TRANSFER_MOVE;
    }
    const struct pacha_ipc_msg msg = {
        .word0 = PACHA_ROOT_HANDOFF_MAGIC,
        .word1 = PACHA_ROOT_HANDOFF_VERSION,
        .word2 = handoff.device_count,
        .fds = fds,
        .fd_count = handoff.device_count + 1,
    };
    const int status = pacha_ipc_send(channel_fd, &msg);
    if (status != 0) (void)pacha_fd_close(metadata_fd);
    return status;
}

int main(void)
{
    printf("[seed0boot] start\n");
    struct pacha_ipc_channel_pair root_ready = { .a = -1, .b = -1 };
    struct pacha_ipc_channel_pair root_handoff = { .a = -1, .b = -1 };
    if (pacha_ipc_channel_create(&root_ready, channel_rights,
            PACHA_FD_FLAG_INHERIT) != 0 ||
        pacha_ipc_channel_create(&root_handoff, channel_rights,
            PACHA_FD_FLAG_INHERIT) != 0)
        return 10;
    if (set_inherit(root_ready.a, 0) != 0 ||
        set_inherit(root_handoff.a, 0) != 0)
        return 10;

    int status = seed0_launch_storage_boot_nvme(root_ready.b, root_handoff.b);
    if (status != 0) {
        fprintf(stderr, "[seed0boot] storage_boot launch failed status=%d\n", status);
        return 11;
    }
    (void)pacha_fd_close(root_ready.b);
    (void)pacha_fd_close(root_handoff.b);

    struct pacha_ipc_msg ready;
    memset(&ready, 0, sizeof(ready));
    status = pacha_ipc_recv_wait(root_ready.a, &ready, PACHA_FD_WAIT_FOREVER);
    (void)pacha_fd_close(root_ready.a);
    if (status != 0 || ready.word0 != PACHA_ROOT_READY_MAGIC ||
        ready.word1 != 0 || ready.fd_count != 0) {
        fprintf(stderr, "[seed0boot] root ready failed status=%d magic=0x%llx fds=%llu\n",
            status, (unsigned long long)ready.word0,
            (unsigned long long)ready.fd_count);
        return 12;
    }
    status = send_root_handoff(root_handoff.a);
    (void)pacha_fd_close(root_handoff.a);
    if (status != 0) {
        fprintf(stderr, "[seed0boot] root capability handoff failed status=%d\n", status);
        return 13;
    }
    printf("[seed0boot] root capability handoff complete; authority closed\n");
    fflush(stdout);
    fflush(stderr);

    struct pacha_ipc_channel_pair park = { .a = -1, .b = -1 };
    if (pacha_ipc_channel_create(&park, channel_rights, 0) != 0) return 14;
    struct pacha_ipc_msg ignored;
    memset(&ignored, 0, sizeof(ignored));
    return pacha_ipc_recv_wait(park.a, &ignored, PACHA_FD_WAIT_FOREVER);
}
