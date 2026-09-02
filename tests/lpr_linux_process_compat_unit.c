#include <stdint.h>
#include <stdio.h>

#include <pacha/status.h>
#include "../userland/personality/linux/runtime/lpr_process/capability.h"
#include "../userland/personality/linux/runtime/lpr_process/compat.h"

enum {
    PR_SET_NO_NEW_PRIVS = 38u,
    PR_GET_NO_NEW_PRIVS = 39u,
    CLONE_FILES = 0x00000400ull,
    CLONE_NEWNS = 0x00020000ull,
    CLONE_NEWUSER = 0x10000000ull,
    CLONE_NEWNET = 0x40000000ull,
};

static int failures;

static void expect(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures += 1;
    }
}

int main(void)
{
    lpr_linux_capability_header_t cap_header = {
        .version = LPR_LINUX_CAPABILITY_VERSION_3,
        .pid = 0,
    };
    lpr_linux_capability_data_t caps[2] = {
        { .effective = 1, .permitted = 2, .inheritable = 3 },
        { .effective = 4, .permitted = 5, .inheritable = 6 },
    };
    expect(lpr_linux_capget((uint64_t)(uintptr_t)&cap_header,
                            (uint64_t)(uintptr_t)caps,
                            42) == 0,
           "capget accepts the v3 ABI used by bubblewrap");
    expect(caps[0].effective == 0 && caps[0].permitted == 0 &&
               caps[0].inheritable == 0 && caps[1].effective == 0 &&
               caps[1].permitted == 0 && caps[1].inheritable == 0,
           "CapabilityOS reports an empty Linux capability set");
    expect(lpr_linux_capset((uint64_t)(uintptr_t)&cap_header,
                            (uint64_t)(uintptr_t)caps,
                            42) == 0,
           "capset accepts retaining an empty capability set");
    caps[0].effective = 1;
    expect(lpr_linux_capset((uint64_t)(uintptr_t)&cap_header,
                            (uint64_t)(uintptr_t)caps,
                            42) == -LPR_LINUX_EPERM,
           "capset cannot manufacture Linux capabilities");
    cap_header.version = 0;
    expect(lpr_linux_capget((uint64_t)(uintptr_t)&cap_header, 0, 42) ==
               -LPR_LINUX_EINVAL &&
               cap_header.version == LPR_LINUX_CAPABILITY_VERSION_3,
           "capget negotiates an unsupported ABI version like Linux");

    expect(lpr_linux_prctl_compat(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) == 1,
           "CapabilityOS permanently enforces no-new-privileges");
    expect(lpr_linux_prctl_compat(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0,
           "PR_SET_NO_NEW_PRIVS accepts the Linux enable operation");
    expect(lpr_linux_prctl_compat(PR_SET_NO_NEW_PRIVS, 0, 0, 0, 0) ==
               -LPR_LINUX_EINVAL,
           "PR_SET_NO_NEW_PRIVS cannot be cleared");
    expect(lpr_linux_prctl_compat(PR_SET_NO_NEW_PRIVS, 1, 1, 0, 0) ==
               -LPR_LINUX_EINVAL,
           "PR_SET_NO_NEW_PRIVS rejects nonzero reserved arguments");
    expect(lpr_linux_prctl_compat(0xffffu, 0, 0, 0, 0) == -LPR_LINUX_EINVAL,
           "unknown prctl operation is not reported as success");

    expect(lpr_linux_clone_namespace_guard(17u) == 0,
           "ordinary SIGCHLD clone remains available");
    expect(lpr_linux_clone_namespace_guard(CLONE_NEWNS | 17u) ==
               -LPR_LINUX_EPERM,
           "mount-namespace clone is denied truthfully");
    expect(lpr_linux_clone_namespace_guard(CLONE_NEWUSER | CLONE_NEWNET | 17u) ==
               -LPR_LINUX_EPERM,
           "user/network namespace clone is denied truthfully");

    expect(lpr_linux_unshare(0) == 0,
           "unshare with no requested changes is a no-op");
    expect(lpr_linux_unshare(CLONE_NEWNS) == -LPR_LINUX_EPERM,
           "mount namespace unshare is denied truthfully");
    expect(lpr_linux_unshare(CLONE_FILES) == -LPR_LINUX_ENOSYS,
           "unimplemented file-table unshare is not faked");
    expect(lpr_linux_unshare(1ull << 63) == -LPR_LINUX_EINVAL,
           "unknown unshare flags are rejected");

    if (failures != 0) {
        return 1;
    }
    puts("lpr linux process compat unit: PASS");
    return 0;
}
