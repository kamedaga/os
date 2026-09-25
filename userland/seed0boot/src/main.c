#define _GNU_SOURCE

#include "bootstrap_abi.h"
#include "bootfs_reader.h"
#include "filed/live_bootstrap.h"
#include "pacha/capsule.h"
#include "pacha/ipc.h"
#include "pacha/root_handoff.h"
#include "pacha/live_bootstrap.h"
#include "pacha/syscall.h"
#include "pachaos_capsule_launcher.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const uint64_t channel_rights = PACHA_FD_RIGHT_INSPECT |
    PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL |
    PACHA_FD_RIGHT_SET_FLAGS | PACHA_FD_RIGHT_CLOSE |
    PACHA_FD_RIGHT_SEND | PACHA_FD_RIGHT_RECV |
    PACHA_FD_RIGHT_CALL | PACHA_FD_RIGHT_TRANSFER;


static int is_nvme(const struct pacha_capsule_pci_function *device)
{
    return device->class_code == 0x010802;
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

static int send_root_batch(int channel_fd,
    const struct pacha_root_handoff *handoff, const int *selected)
{
    const int metadata_fd = create_handoff_vmo(handoff);
    if (metadata_fd < 16) return metadata_fd;
    struct pacha_ipc_fd fds[1 + PACHA_ROOT_HANDOFF_BATCH_DEVICES];
    memset(fds, 0, sizeof(fds));
    struct pacha_fd_info info;
    if (pacha_fd_get_info(metadata_fd, &info) != 0) {
        (void)pacha_fd_close(metadata_fd);
        return -13;
    }
    fds[0].fd = (uint64_t)(uint32_t)metadata_fd;
    fds[0].rights = info.rights;
    fds[0].transfer_flags = PACHA_IPC_TRANSFER_MOVE;
    for (uint64_t i = 0; i < handoff->device_count; i++) {
        if (pacha_fd_get_info(selected[i], &info) != 0) {
            (void)pacha_fd_close(metadata_fd);
            return -13;
        }
        fds[i + 1].fd = (uint64_t)(uint32_t)selected[i];
        fds[i + 1].rights = info.rights;
        fds[i + 1].transfer_flags = PACHA_IPC_TRANSFER_MOVE;
    }
    const struct pacha_ipc_msg msg = {
        .word0 = PACHA_ROOT_HANDOFF_MAGIC,
        .word1 = PACHA_ROOT_HANDOFF_VERSION,
        .word2 = handoff->device_count,
        .word3 = handoff->flags,
        .fds = fds,
        .fd_count = handoff->device_count + 1,
    };
    int status;
    do {
        status = pacha_ipc_send(channel_fd, &msg);
        if (status != PACHA_ERR_NOT_READY) break;
        struct pacha_pollfd wait = {
            .fd = channel_fd, .events = PACHA_FD_EVENT_WRITABLE,
        };
        const long ready = pacha_fd_wait_many(&wait, 1, PACHA_FD_WAIT_FOREVER);
        if (ready <= 0 || (wait.revents & PACHA_FD_EVENT_WRITABLE) == 0) {
            status = -5;
            break;
        }
    } while (1);
    if (status != 0) (void)pacha_fd_close(metadata_fd);
    return status;
}

static int send_root_handoff(int channel_fd, int live_mode)
{
    if (channel_fd < 16 || seed0_bootstrap_descriptor() == NULL) return -22;
    const long function_count = pacha_capsule_pci_function_count();
    if (function_count < 0) return (int)function_count;

    struct pacha_root_handoff handoff;
    memset(&handoff, 0, sizeof(handoff));
    handoff.magic = PACHA_ROOT_HANDOFF_MAGIC;
    handoff.version = PACHA_ROOT_HANDOFF_VERSION;
    int selected[PACHA_ROOT_HANDOFF_BATCH_DEVICES];
    int status = 0;
    for (uint64_t index = 0; index < (uint64_t)function_count; index++) {
        struct pacha_capsule_pci_function device;
        status = pacha_capsule_pci_function_at(index, &device);
        if (status != 0) goto fail;
        if (live_mode) {
            const int xhci = device.class_code == 0x0c0330;
            const int display = (device.class_code >> 16) == 0x03;
            const int ethernet = (device.class_code >> 8) == 0x0200;
            if (!xhci && !display && !ethernet) continue;
        } else if (is_nvme(&device)) continue;
        if (handoff.device_count == PACHA_ROOT_HANDOFF_BATCH_DEVICES) {
            status = send_root_batch(channel_fd, &handoff, selected);
            if (status != 0) goto fail;
            handoff.device_count = 0;
            memset(handoff.devices, 0, sizeof(handoff.devices));
        }
        const int fd = pacha_capsule_pci_claim(index);
        if (fd < 16) {
            status = fd;
            goto fail;
        }
        const uint64_t slot = handoff.device_count++;
        selected[slot] = fd;
        handoff.devices[slot] = (struct pacha_root_device_record) {
            .transfer_index = slot,
            .resource_id = device.resource_id,
            .vendor_id = device.vendor_id,
            .device_id = device.device_id,
            .subsystem_id = device.subsystem_id,
            .class_code = device.class_code,
            .pci_segment = 0,
            .pci_bus = (uint32_t)device.bus,
            .pci_device = (uint32_t)device.device,
            .pci_function = (uint32_t)device.function,
        };
    }
    handoff.flags = PACHA_ROOT_HANDOFF_FLAG_LAST;
    status = send_root_batch(channel_fd, &handoff, selected);
    if (status == 0) return 0;
fail:
    for (uint64_t i = 0; i < handoff.device_count; i++)
        (void)pacha_fd_close(selected[i]);
    return status;
}

static int wait_parked(void)
{
    struct pacha_ipc_channel_pair park = { .a = -1, .b = -1 };
    if (pacha_ipc_channel_create(&park, channel_rights, 0) != 0) return 14;
    struct pacha_ipc_msg ignored;
    memset(&ignored, 0, sizeof(ignored));
    return pacha_ipc_recv_wait(park.a, &ignored, PACHA_FD_WAIT_FOREVER);
}

static int launch_live_root(void)
{
    struct pacha_ipc_channel_pair ready = { .a = -1, .b = -1 };
    struct pacha_ipc_channel_pair unix_path = { .a = -1, .b = -1 };
    struct pacha_ipc_channel_pair root_handoff = { .a = -1, .b = -1 };
    struct pacha_ipc_channel_pair power = { .a = -1, .b = -1 };
    if (pacha_ipc_channel_create(&ready, channel_rights, 0) != 0 ||
        pacha_ipc_channel_create(&unix_path, channel_rights, 0) != 0 ||
        pacha_ipc_channel_create(&root_handoff, channel_rights, 0) != 0 ||
        pacha_ipc_channel_create(&power, channel_rights, 0) != 0)
        return 20;
    int endpoint_fd = -1;
    int status = seed0_launch_live_filed(ready.b, unix_path.a,
        &endpoint_fd);
    (void)pacha_fd_close(ready.b);
    (void)pacha_fd_close(unix_path.a);
    if (status != 0) {
        fprintf(stderr, "[seed0boot] live filed launch failed status=%d\n", status);
        (void)pacha_fd_close(ready.a);
        return 21;
    }
    struct pacha_ipc_msg message;
    memset(&message, 0, sizeof(message));
    status = pacha_ipc_recv_wait(ready.a, &message, PACHA_FD_WAIT_FOREVER);
    (void)pacha_fd_close(ready.a);
    if (status != 0 || message.word0 != FILED_LIVE_READY_MAGIC ||
        message.fd_count != 0) {
        fprintf(stderr, "[seed0boot] live root ready failed status=%d magic=0x%llx\n",
            status, (unsigned long long)message.word0);
        (void)pacha_fd_close(endpoint_fd);
        return 22;
    }
    printf("[seed0boot] RAM root ready; launching live services\n");
    fflush(stdout);
    status = seed0_launch_live_seed0root(endpoint_fd, unix_path.b,
        root_handoff.a, power.b);
    (void)pacha_fd_close(endpoint_fd);
    (void)pacha_fd_close(unix_path.b);
    (void)pacha_fd_close(root_handoff.a);
    (void)pacha_fd_close(power.b);
    if (status != 0) {
        fprintf(stderr, "[seed0boot] live services launch failed status=%d\n",
            status);
        (void)pacha_fd_close(root_handoff.b);
        return 23;
    }
    status = send_root_handoff(root_handoff.b, 1);
    (void)pacha_fd_close(root_handoff.b);
    if (status != 0) {
        fprintf(stderr, "[seed0boot] live device handoff failed status=%d\n",
            status);
        return 24;
    }
    for (;;) {
        struct pacha_ipc_fd reply_fd = {0};
        struct pacha_ipc_msg request;
        memset(&request, 0, sizeof(request));
        request.fds = &reply_fd;
        request.fd_capacity = 1;
        status = pacha_ipc_recv_wait(power.a, &request, PACHA_FD_WAIT_FOREVER);
        if (status != 0) {
            fprintf(stderr, "[seed0boot] power channel closed status=%d\n", status);
            return status;
        }
        if (request.word0 != PACHA_LIVE_POWER_REQUEST_MAGIC ||
            (request.word1 != PACHA_LIVE_POWER_OFF &&
             request.word1 != PACHA_LIVE_POWER_REBOOT) || request.fd_count != 1 ||
            reply_fd.fd < 16) {
            fprintf(stderr, "[seed0boot] invalid power request\n");
            if (request.fd_count == 1 && reply_fd.fd >= 16)
                (void)pacha_fd_close((int)reply_fd.fd);
            continue;
        }
        printf("[seed0boot] ACPI %s requested\n",
            request.word1 == PACHA_LIVE_POWER_OFF ? "poweroff" : "reboot");
        fflush(stdout);
        const long result = pacha_syscall1(PACHA_RUNTIME_SYSCALL_POWER_CONTROL,
            request.word1);
        fprintf(stderr, "[seed0boot] ACPI %s failed status=%ld\n",
            request.word1 == PACHA_LIVE_POWER_OFF ? "poweroff" : "reboot", result);
        const struct pacha_ipc_msg response = {
            .word0 = PACHA_LIVE_POWER_REQUEST_MAGIC,
            .word1 = (uint64_t)result,
        };
        (void)pacha_ipc_reply((int)reply_fd.fd, &response);
        (void)pacha_fd_close((int)reply_fd.fd);
    }
}

int main(void)
{
    printf("[seed0boot] start\n");
    const unsigned char *profile = NULL;
    uint32_t profile_size = 0;
    const int profile_status = seed0_bootfs_open_file(
        "/etc/pacha/boot-profile", &profile, &profile_size);
    if (profile_status == 0) {
        static const char wanted[] = "ram-tmpfs-v1\n";
        if (profile_size != sizeof(wanted) - 1u ||
            memcmp(profile, wanted, sizeof(wanted) - 1u) != 0) {
            fprintf(stderr, "[seed0boot] unsupported boot profile\n");
            return 19;
        }
        return launch_live_root();
    }
    if (profile_status != -5) return 18;
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
    status = send_root_handoff(root_handoff.a, 0);
    (void)pacha_fd_close(root_handoff.a);
    if (status != 0) {
        fprintf(stderr, "[seed0boot] root capability handoff failed status=%d\n", status);
        return 13;
    }
    printf("[seed0boot] root capability handoff complete; authority closed\n");
    fflush(stdout);
    fflush(stderr);

    return wait_parked();
}
