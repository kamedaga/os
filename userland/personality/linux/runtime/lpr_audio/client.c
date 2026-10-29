#include "../lpr_filed_internal.h"
#include "../lpr_fd/allocate.h"
#include "../../../../audiod/include/audiod/protocol.h"

int64_t lpr_audio_install(pacha_fd_t native, uint64_t flags)
{
    struct pacha_fd_info info = {0};
    const uint64_t needed = AUDIOD_CLIENT_RIGHTS;
    if (native < 16 || !lpr_native_fd_info((uint64_t)native, &info) ||
        info.kind != PACHA_FD_KIND_CHANNEL || (info.rights & needed) != needed)
        return -LPR_LINUX_EIO;
    const lpr_audio_backend_t audio = {.active = 1, .flags = (uint32_t)flags,
        .native.raw = native};
    return lpr_fd_alloc_state(LPR_FD_OPS_AUDIO, flags, 0, &audio, sizeof(audio));
}
static int64_t call(lpr_audio_backend_t *audio, uint64_t operation,
    uint64_t argument, void *buffer, uint64_t bytes)
{
    void *page = 0;
    pacha_fd_t page_fd = -1;
    struct pacha_ipc_fd grant = {0};
    const uint64_t correlation = lpr_next_request_id(&lpr_termd_request_id);
    struct pacha_ipc_msg request = {.word0 = AUDIOD_MAGIC, .word1 = operation,
        .word2 = argument, .word3 = correlation};
    if (bytes) {
        const uint64_t rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE |
            PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE;
        int64_t created = lpr_pacha_syscall3(PACHAOS_SYSCALL_VMO_CREATE, AUDIO_DATA_BYTES, rights, 0);
        if (created < 16) return lpr_pacha_status_to_errno(created);
        page_fd = (pacha_fd_t)created;
        int64_t mapped = lpr_pacha_syscall6(PACHAOS_SYSCALL_MMAP, (uint64_t)page_fd, 0,
            AUDIO_DATA_BYTES, PACHAOS_PROT_READ | PACHAOS_PROT_WRITE, PACHAOS_MMAP_SHARED, 0);
        if (mapped < 4096) {
            (void)lpr_close_native_fd_if_open((uint64_t)page_fd);
            return -LPR_LINUX_ENOMEM;
        }
        page = (void *)(uintptr_t)mapped;
        lpr_memcpy(page, buffer, (size_t)bytes);
        grant = (struct pacha_ipc_fd){.fd = (uint64_t)page_fd,
            .rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE |
                PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE};
        request.fds = &grant; request.fd_count = 1;
    }
    struct pacha_ipc_msg reply = {0};
    int64_t status = lpr_native_ipc_call_wait(audio->native.raw, &request, &reply, NULL);
    if (status) status = lpr_pacha_status_to_errno(status);
    else if (reply.word0 != AUDIOD_MAGIC || reply.word3 != correlation) status = -LPR_LINUX_EIO;
    else {
        status = (int64_t)reply.word1;
        if (!status) {
            status = (int64_t)reply.word2;
            if (page && operation == AUDIO_IOCTL) lpr_memcpy(buffer, page, (size_t)bytes);
        }
    }
    if (page) {
        (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP, (uintptr_t)page, AUDIO_DATA_BYTES);
        (void)lpr_close_native_fd_if_open((uint64_t)page_fd);
    }
    return status;
}
int64_t lpr_audio_write(uint64_t fd, uint64_t buffer, uint64_t count)
{
    lpr_audio_backend_t *audio = lpr_audio_backend(fd);
    if (!audio) return -LPR_LINUX_EBADF;
    if (!count) return 0;
    if (!buffer || buffer > UINT64_MAX - count) return -LPR_LINUX_EFAULT;
    if (count > AUDIO_DATA_BYTES) count = AUDIO_DATA_BYTES;
    int64_t result = call(audio, AUDIO_WRITE, count, (void *)(uintptr_t)buffer, count);
    return result > (int64_t)count ? -LPR_LINUX_EIO : result;
}
int64_t lpr_audio_ioctl(uint64_t fd, uint64_t command, uint64_t argument)
{
    lpr_audio_backend_t *audio = lpr_audio_backend(fd);
    if (!audio) return -LPR_LINUX_EBADF;
    if (command > UINT32_MAX) return -LPR_LINUX_ENOTTY;
    const size_t size = (command >> 16) & 0x3fff;
    if (size > 64) return -LPR_LINUX_ENOTTY;
    if (size && !argument) return -LPR_LINUX_EFAULT;
    uint8_t data[64] = {0};
    if (size && (command & (1ull << 30))) lpr_memcpy(data, (const void *)(uintptr_t)argument, size);
    int64_t result = call(audio, AUDIO_IOCTL, command, data, size);
    if (result >= 0 && size && (command & (2ull << 30)))
        lpr_memcpy((void *)(uintptr_t)argument, data, size);
    return result;
}
int64_t lpr_audio_poll_events(uint64_t fd, uint32_t events)
{
    lpr_audio_backend_t *audio = lpr_audio_backend(fd);
    if (!audio) return -LPR_LINUX_EBADF;
    /* Consume the old edge before the driver's durable readiness check. A
     * callback during or after that check leaves the next wait wakeable. */
    const int64_t consumed = lpr_pacha_syscall3(PACHA_IPC_SYSCALL_NOTIFY_WAIT,
        (uint64_t)audio->native.raw, 0, 0);
    if (consumed < 0 && lpr_pacha_status_to_errno(consumed) != -LPR_LINUX_EAGAIN)
        return 0x0010u | 0x0008u;
    const int64_t result = call(audio, AUDIO_POLL, 0, 0, 0);
    return result < 0 ? result : result & (events | 0x0018u);
}

int64_t lpr_audio_status_flags(uint64_t fd, uint64_t flags)
{
    lpr_audio_backend_t *audio = lpr_audio_backend(fd);
    return audio ? call(audio, AUDIO_FLAGS, flags, 0, 0) : -LPR_LINUX_EBADF;
}
