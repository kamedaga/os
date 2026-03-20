const std = @import("std");
const init_bootstrap_abi = @import("../abi/init_bootstrap_abi.zig");
const process_abi = @import("../abi/process_abi.zig");

pub const descriptor_page_va: u64 = process_abi.auxPageVa(7);
pub const source_base_page_index: u64 = 8;
pub const bootfs_image_va: u64 = 0x3C08_0000;

pub const SourceSlot = enum(u64) {
    primary_panel_config = 0,
    primary_panel_state = 1,
    primary_panel_command = 2,
    pointer_shared = 3,
    pointer_input_config = 4,
    keyboard_input_config = 5,
    window_service_config = 6,
};

pub fn sourceVa(slot: SourceSlot) u64 {
    return process_abi.auxPageVa(source_base_page_index + @intFromEnum(slot));
}

pub const builtin_spawn_pages = [_]init_bootstrap_abi.SpawnPageDescriptor{
    .{
        .kind = @intFromEnum(init_bootstrap_abi.SpawnPageKind.ui_config),
        .subject = @intFromEnum(init_bootstrap_abi.SpawnPageSubject.primary_panel),
        .flags = init_bootstrap_abi.spawn_page_flag_kernel_backed | init_bootstrap_abi.spawn_page_flag_init_writable,
        .source_va = sourceVa(.primary_panel_config),
        .target_va = process_abi.standard_config_target_va,
        .spawn_flags = process_abi.spawn_flag_bootstrap_page_writable,
    },
    .{
        .kind = @intFromEnum(init_bootstrap_abi.SpawnPageKind.ui_state),
        .subject = @intFromEnum(init_bootstrap_abi.SpawnPageSubject.primary_panel),
        .flags = init_bootstrap_abi.spawn_page_flag_kernel_backed |
            init_bootstrap_abi.spawn_page_flag_mirror_to_boot_display |
            init_bootstrap_abi.spawn_page_flag_mirror_writable |
            init_bootstrap_abi.spawn_page_flag_init_writable,
        .source_va = sourceVa(.primary_panel_state),
        .target_va = process_abi.auxPageVa(4),
        .spawn_flags = 0,
    },
    .{
        .kind = @intFromEnum(init_bootstrap_abi.SpawnPageKind.ui_command),
        .subject = @intFromEnum(init_bootstrap_abi.SpawnPageSubject.primary_panel),
        .flags = init_bootstrap_abi.spawn_page_flag_kernel_backed |
            init_bootstrap_abi.spawn_page_flag_mirror_to_boot_display |
            init_bootstrap_abi.spawn_page_flag_mirror_writable |
            init_bootstrap_abi.spawn_page_flag_init_writable,
        .source_va = sourceVa(.primary_panel_command),
        .target_va = process_abi.auxPageVa(6),
        .spawn_flags = process_abi.spawn_flag_bootstrap_page_writable,
    },
    .{
        .kind = @intFromEnum(init_bootstrap_abi.SpawnPageKind.input_shared),
        .subject = @intFromEnum(init_bootstrap_abi.SpawnPageSubject.pointer),
        .flags = 0,
        .source_va = sourceVa(.pointer_shared),
        .target_va = process_abi.auxPageVa(3),
        .spawn_flags = 0,
    },
    .{
        .kind = @intFromEnum(init_bootstrap_abi.SpawnPageKind.input_shared),
        .subject = @intFromEnum(init_bootstrap_abi.SpawnPageSubject.keyboard),
        .flags = 0,
        .source_va = process_abi.auxPageVa(6),
        .target_va = process_abi.auxPageVa(6),
        .spawn_flags = 0,
    },
    .{
        .kind = @intFromEnum(init_bootstrap_abi.SpawnPageKind.service_config),
        .subject = @intFromEnum(init_bootstrap_abi.SpawnPageSubject.window_service),
        .flags = init_bootstrap_abi.spawn_page_flag_kernel_backed | init_bootstrap_abi.spawn_page_flag_init_writable,
        .source_va = sourceVa(.window_service_config),
        .target_va = process_abi.auxPageVa(5),
        .spawn_flags = process_abi.spawn_flag_bootstrap_page_writable,
    },
};

pub const builtin_input_devices = [_]init_bootstrap_abi.InputDeviceDescriptor{
    .{
        .kind = @intFromEnum(init_bootstrap_abi.InputDeviceKind.pointer),
        .flags = 0,
        .config_source_va = sourceVa(.pointer_input_config),
        .config_target_va = process_abi.standard_config_target_va,
        .config_spawn_flags = process_abi.spawn_flag_bootstrap_page_writable,
        .common_page_paddr = 0,
        .notify_page_paddr = 0,
        .isr_page_paddr = 0,
        .device_page_paddr = 0,
        .common_page_offset = 0,
        .notify_page_offset = 0,
        .isr_page_offset = 0,
        .device_page_offset = 0,
        .notify_off_multiplier = 0,
        .init_queue_submit_token = 0,
        .init_queue_notify_token = 0,
    },
    .{
        .kind = @intFromEnum(init_bootstrap_abi.InputDeviceKind.keyboard),
        .flags = 0,
        .config_source_va = sourceVa(.keyboard_input_config),
        .config_target_va = process_abi.standard_config_target_va,
        .config_spawn_flags = process_abi.spawn_flag_bootstrap_page_writable,
        .common_page_paddr = 0,
        .notify_page_paddr = 0,
        .isr_page_paddr = 0,
        .device_page_paddr = 0,
        .common_page_offset = 0,
        .notify_page_offset = 0,
        .isr_page_offset = 0,
        .device_page_offset = 0,
        .notify_off_multiplier = 0,
        .init_queue_submit_token = 0,
        .init_queue_notify_token = 0,
    },
};

test "init bootstrap layout uses aux page space" {
    try std.testing.expectEqual(process_abi.auxPageVa(7), descriptor_page_va);
    try std.testing.expectEqual(process_abi.auxPageVa(source_base_page_index), sourceVa(.primary_panel_config));
    try std.testing.expectEqual(process_abi.auxPageVa(source_base_page_index + 5), sourceVa(.keyboard_input_config));
    try std.testing.expectEqual(process_abi.auxPageVa(source_base_page_index + 6), sourceVa(.window_service_config));
}
