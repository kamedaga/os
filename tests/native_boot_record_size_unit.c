// SPDX-License-Identifier: MIT
#define _GNU_SOURCE
#define main inputd_boot_discarded_main
#include "../userland/inputd/src/main.c"
#undef main
#define main netd_boot_discarded_main
#include "../userland/netd/src/main.c"
#undef main
#include <assert.h>
#include <gpud/boot_config.h>
#include <usbd/boot_config.h>
#include <termd/boot_config.h>
#include <logd/boot_config.h>
#include <storage_boot/boot_config.h>
#include <pachaos_capsule/boot_config.h>
#include <pacha/root_handoff.h>
#include <pacha/live_bootstrap.h>
#include <unixd/ipc_protocol.h>
#include "../userland/installer/bootstrap.h"

int main(void)
{
    /* Test the real validators: a size is a measured record length, never an
     * ordinal accepted via a legacy alias or relaxed compatibility branch. */
    struct netd_boot_config net = {.magic = NETD_BOOT_CONFIG_MAGIC,
        .size = sizeof(net), .device_fd = 16, .filed_endpoint_fd = 17,
        .socket_endpoint_fd = 18};
    assert(validate_boot_config(&net));
    const uint64_t bad_sizes[] = {0, 9, sizeof(net) - 1, sizeof(net) + 1, UINT64_MAX};
    for (unsigned i = 0; i < sizeof(bad_sizes) / sizeof(bad_sizes[0]); ++i) {
        net.size = bad_sizes[i];
        assert(!validate_boot_config(&net));
    }
    struct inputd_boot_config input = {.magic = INPUTD_BOOT_CONFIG_MAGIC,
        .header_size = sizeof(input), .total_size = sizeof(input),
        .input_endpoint_fd = 16, .ready_channel_fd = 17, .source_endpoint_fd = 18,
        .device_record_size = sizeof(struct inputd_device_config),
        .devices_offset = sizeof(input)};
    assert(!inputd_boot_config_validate(&input));
    input.header_size--;
    assert(inputd_boot_config_validate(&input) == -22);
    input.header_size++;
    input.total_size++;
    assert(inputd_boot_config_validate(&input) == -22);
    input.total_size--;
    input.device_record_size--;
    assert(inputd_boot_config_validate(&input) == -22);
    input.device_record_size++;
    input.devices_offset++;
    assert(inputd_boot_config_validate(&input) == -22);
#define CHECK_SIZE(type) do { struct type record = {.size = sizeof(record)}; \
    assert(record.size == sizeof(record)); } while (0)
    CHECK_SIZE(gpud_boot_config); CHECK_SIZE(usbd_boot_config);
    CHECK_SIZE(termd_boot_config); CHECK_SIZE(logd_boot_config);
    CHECK_SIZE(storage_boot_config); CHECK_SIZE(pachaos_capsule_boot_config);
    CHECK_SIZE(pacha_root_handoff); CHECK_SIZE(pacha_live_root_bootstrap);
    CHECK_SIZE(unix_boot_config); CHECK_SIZE(unix_control);
    CHECK_SIZE(pacha_installer_bootstrap); CHECK_SIZE(pacha_installer_runner_plan);
    puts("native boot: actual netd/inputd validators reject size mismatches; fixed records name actual sizeof PASS");
}
