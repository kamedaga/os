#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../userland/personality/linux/runtime/lpr_filed_internal.h"
#include "../userland/personality/linux/runtime/lpr_fd/device.h"

static struct pacha_fd_info capability;
static unsigned unmapped, closed;
int64_t lpr_pacha_syscall2(uint64_t nr, uint64_t address, uint64_t bytes)
{
    assert(nr == PACHAOS_SYSCALL_MUNMAP && address == 0x10000 &&
        bytes == LPR_DEVICE_DESCRIPTION_BYTES);
    unmapped++;
    return 0;
}
int64_t lpr_pacha_status_to_errno(int64_t status) { return status; }
int64_t lpr_close_native_fd_if_open(uint64_t fd)
{
    assert(fd == 24);
    closed++;
    return 0;
}
int lpr_native_fd_info(uint64_t fd, struct pacha_fd_info *out)
{
    if (fd != 24) return 0;
    *out = capability;
    return 1;
}
uint32_t lpr_control_status_flags_from_linux(uint64_t flags)
{
    return (flags & 2048 ? LPR_OFD_NONBLOCK : 0) | (flags & 1024 ? LPR_OFD_APPEND : 0);
}

int main(void)
{
    assert(lpr_device_kind_supported(1, 3));
    assert(lpr_device_kind_supported(1, 5));
    assert(!lpr_device_kind_supported(226, 128));
    assert(!lpr_device_kind_supported(1, 1));
    const uint64_t rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_DUP |
        PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE;
    capability = (struct pacha_fd_info){ .kind = PACHA_FD_KIND_VMO,
        .size = LPR_DEVICE_DESCRIPTION_BYTES, .rights = rights };
    assert(lpr_device_description_capability_valid(24));
    assert(!lpr_device_description_capability_valid(2));
    assert(!lpr_device_description_capability_valid(25));
    capability.size--;
    assert(!lpr_device_description_capability_valid(24));
    capability.size++;
    capability.kind = PACHA_FD_KIND_CHANNEL;
    assert(!lpr_device_description_capability_valid(24));
    capability.kind = PACHA_FD_KIND_VMO;
    capability.rights &= ~PACHA_FD_RIGHT_MAP_WRITE;
    assert(!lpr_device_description_capability_valid(24));
    capability.rights = rights | PACHA_FD_RIGHT_SET_FLAGS;
    assert(!lpr_device_description_capability_valid(24));
    uint32_t flags = UINT32_MAX;
    lpr_device_backend_t device = { .description_map = (uint64_t)(uintptr_t)&flags };
    assert(lpr_device_description_flags(&device) == (LPR_OFD_NONBLOCK | LPR_OFD_APPEND));
    lpr_device_description_set_flags(&device, LPR_OFD_APPEND | (1u << 31));
    assert(flags == LPR_OFD_APPEND);
    lpr_device_backend_t alias = device;
    lpr_device_description_set_flags(&alias, LPR_OFD_NONBLOCK);
    assert(lpr_device_description_flags(&device) == LPR_OFD_NONBLOCK);
    device.description_map = 0x10000;
    device.description_fd.raw = 24;
    assert(!lpr_device_description_unmap(&device));
    assert(unmapped == 1 && !closed && !device.description_map && device.description_fd.raw == 24);
    assert(!lpr_device_description_unmap(&device) && unmapped == 1);
    assert(!lpr_device_description_close(&device) && closed == 1 && device.description_fd.raw == -1);
    device.description_map = 0;
    device.flags = 2048;
    assert(lpr_device_description_flags(&device) == LPR_OFD_NONBLOCK);
    puts("device description: kind/capability validation, shared flags and untrusted bits: PASS");
}
