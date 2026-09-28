/* SPDX-License-Identifier: MIT */
#include "../userland/gpud/inspect_policy.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const char valid[] =
    "package_root=/srv/kobox2/amdgpu\n"
    "firmware_sha256=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n"
    "manifest_sha256=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\n"
    "sandbox_sha256=cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\n";

static void expect_rejected(const char *bytes, size_t size) {
    struct gpud_boot_config config = {.package_root = "/srv/kobox2/amdgpu"};
    struct gpud_boot_config original = config;

    memset(config.firmware_sha256, 0x11, 32);
    memset(config.manifest_sha256, 0x22, 32);
    memset(config.sandbox_sha256, 0x33, 32);
    original = config;
    assert(gpud_inspect_policy_parse(bytes, size, &config) == -EINVAL);
    assert(!memcmp(&config, &original, sizeof(config)));
}

int main(void) {
    struct gpud_boot_config config = {.package_root = "/srv/kobox2/amdgpu"};
    char changed[sizeof(valid)];

    assert(gpud_inspect_policy_parse(valid, sizeof(valid) - 1, &config) == 0);
    for (size_t i = 0; i < 32; ++i) {
        assert(config.firmware_sha256[i] == 0xaa);
        assert(config.manifest_sha256[i] == 0xbb);
        assert(config.sandbox_sha256[i] == 0xcc);
    }
    expect_rejected(valid, sizeof(valid));
    expect_rejected(valid, sizeof(valid) - 2);
    expect_rejected(valid, 385);
    memcpy(changed, valid, sizeof(valid));
    changed[sizeof("package_root=") - 1] = '/';
    changed[sizeof("package_root=/") - 1] = 'x';
    expect_rejected(changed, sizeof(valid) - 1);
    memcpy(changed, valid, sizeof(valid));
    changed[sizeof("package_root=/srv/kobox2/amdgpu\nfirmware_sha256=") - 1] = 'G';
    expect_rejected(changed, sizeof(valid) - 1);
    memcpy(changed, valid, sizeof(valid));
    changed[sizeof(valid) - 2] = '\0';
    expect_rejected(changed, sizeof(valid) - 1);
    puts("gpud RAM policy parser: PASS");
    return 0;
}
