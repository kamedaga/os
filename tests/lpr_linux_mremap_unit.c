#include <stdint.h>
#include <stdio.h>

#include <pacha/abi.h>
#include <pacha/status.h>
#include "../userland/personality/linux/runtime/lpr_filed_internal.h"
#include "../userland/personality/linux/runtime/lpr_memory.h"

enum {
    MREMAP_MAYMOVE = 1u,
    MREMAP_FIXED = 2u,
    MREMAP_DONTUNMAP = 4u,
};

lpr_state_t lpr_state;

static uint64_t syscall_nr;
static uint64_t syscall_args[5];
static int64_t syscall_result;
static unsigned int syscall_count;
static unsigned int remap_notifications;
static uint64_t remap_args[4];
static int failures;

void lpr_drm_mapping_remapped(uint64_t old_address, uint64_t old_size,
    uint64_t new_address, uint64_t new_size)
{
    remap_notifications++;
    remap_args[0] = old_address;
    remap_args[1] = old_size;
    remap_args[2] = new_address;
    remap_args[3] = new_size;
}

int64_t lpr_pacha_syscall2(uint64_t nr, uint64_t a0, uint64_t a1)
{
    (void)nr;
    (void)a0;
    (void)a1;
    return 0;
}

int64_t lpr_pacha_syscall5(
    uint64_t nr,
    uint64_t a0,
    uint64_t a1,
    uint64_t a2,
    uint64_t a3,
    uint64_t a4)
{
    syscall_nr = nr;
    syscall_args[0] = a0;
    syscall_args[1] = a1;
    syscall_args[2] = a2;
    syscall_args[3] = a3;
    syscall_args[4] = a4;
    syscall_count++;
    return syscall_result;
}

int64_t lpr_drm_native_mremap(uint64_t address, uint64_t length,
    uint64_t new_length, uint64_t flags, uint64_t target)
{
    int64_t result = lpr_pacha_syscall5(PACHA_VM_SYSCALL_MREMAP,
        address, length, new_length, flags, target);
    if (result >= 4096)
        lpr_drm_mapping_remapped(address, length, (uint64_t)result, new_length);
    return result;
}

int64_t lpr_pacha_syscall6(
    uint64_t nr,
    uint64_t a0,
    uint64_t a1,
    uint64_t a2,
    uint64_t a3,
    uint64_t a4,
    uint64_t a5)
{
    (void)nr;
    (void)a0;
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    return 0;
}

static void expect(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

int main(void)
{
    syscall_result = 0x20000000;
    expect(lpr_linux_mremap(
               0x10000000, 135168, 266240,
               MREMAP_MAYMOVE, 0xdeadbeef) == syscall_result,
           "MAYMOVE returns the CapabilityOS mapping address");
    expect(remap_notifications == 1 && remap_args[0] == 0x10000000 &&
               remap_args[1] == 135168 && remap_args[2] == 0x20000000 &&
               remap_args[3] == 266240,
           "successful remap updates tracked DRM mappings");
    expect(syscall_count == 1 && syscall_nr == PACHA_VM_SYSCALL_MREMAP,
           "mremap uses the existing VM syscall");
    expect(syscall_args[0] == 0x10000000 &&
               syscall_args[1] == 135168 &&
               syscall_args[2] == 266240 &&
               syscall_args[3] == MREMAP_MAYMOVE &&
               syscall_args[4] == 0,
           "non-FIXED mremap discards the unspecified fifth argument");

    syscall_result = 0x30000000;
    expect(lpr_linux_mremap(
               0x10000000, 4096, 8192,
               MREMAP_MAYMOVE | MREMAP_FIXED,
               0x30000000) == syscall_result,
           "FIXED mremap returns the requested mapping address");
    expect(syscall_count == 2 && syscall_args[3] == 3 &&
               syscall_args[4] == 0x30000000,
           "FIXED forwards its target and Linux-compatible flags");

    expect(lpr_linux_mremap(
               0x10000000, 4096, 4096,
               MREMAP_FIXED, 0x30000000) == -LPR_LINUX_EINVAL,
           "FIXED without MAYMOVE is rejected");
    expect(lpr_linux_mremap(
               0x10000000, 4096, 4096,
               MREMAP_MAYMOVE | MREMAP_DONTUNMAP, 0) ==
               -LPR_LINUX_EINVAL,
           "unsupported DONTUNMAP is not silently ignored");
    expect(syscall_count == 2,
           "invalid flags do not reach the CapabilityOS VM syscall");

    syscall_result = 3;
    expect(lpr_linux_mremap(
               0x10000000, 4096, 8192,
               MREMAP_MAYMOVE, 0) == -LPR_LINUX_ENOMEM,
           "CapabilityOS allocation failure maps to Linux ENOMEM");
    expect(remap_notifications == 2,
           "failed remap does not notify mapping trackers");

    if (failures != 0) return 1;
    puts("lpr linux mremap unit: PASS");
    return 0;
}
