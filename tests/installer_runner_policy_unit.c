#include <assert.h>
#include <stdio.h>
#include "../userland/installer/bootstrap.h"

int main(void) {
    for (unsigned flags = 0; flags < 32; flags++) {
        int allowed = flags == 1 || flags == 2 || flags == 3 || flags == 4 || flags == 8;
        assert(pacha_installer_runner_flags_valid(flags) == allowed);
        assert(pacha_installer_runner_read_only(flags) == (flags == 4 || flags == 8));
    }
    assert(!pacha_installer_runner_flags_valid(UINT32_MAX));
    puts("installer runner: install, import and preflight authority classes passed");
    return 0;
}
