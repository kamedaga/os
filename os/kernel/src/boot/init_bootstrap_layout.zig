const std = @import("std");
const abi_root = @import("kernel_abi_root");
const init_bootstrap_abi = abi_root.init_bootstrap_abi;
const process_abi = abi_root.process_abi;

pub const descriptor_page_va: u64 = process_abi.auxPageVa(7);
pub const source_base_page_index: u64 = 8;
pub const bootfs_image_va: u64 = 0x3C08_0000;
/// VA in init's address space where the framebuffer MMIO region is mapped.
/// Init installs a VM object here and passes it to boot_display during bootstrap.
pub const init_framebuffer_source_va: u64 = 0x3E00_0000;

pub const SourceSlot = enum(u64) {
    primary_panel_config = 0,
    primary_panel_state = 1,
    primary_panel_command = 2,
    pointer_shared = 3,
    window_service_config = 4,
    device_config0 = 5,
    device_config1 = 6,
    device_config2 = 7,
    device_config3 = 8,
    device_config4 = 9,
    device_config5 = 10,
};

pub fn sourceVa(slot: SourceSlot) u64 {
    return process_abi.auxPageVa(source_base_page_index + @intFromEnum(slot));
}

pub fn deviceConfigSourceVa(index: usize) u64 {
    if (index >= init_bootstrap_abi.max_device_descriptors) unreachable;
    const slot_value = @intFromEnum(SourceSlot.device_config0) + index;
    return sourceVa(@enumFromInt(slot_value));
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
            init_bootstrap_abi.spawn_page_flag_init_writable,
        .source_va = sourceVa(.primary_panel_state),
        .target_va = process_abi.auxPageVa(4),
        .spawn_flags = 0,
    },
    .{
        .kind = @intFromEnum(init_bootstrap_abi.SpawnPageKind.ui_command),
        .subject = @intFromEnum(init_bootstrap_abi.SpawnPageSubject.primary_panel),
        .flags = init_bootstrap_abi.spawn_page_flag_kernel_backed |
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

test "init bootstrap layout uses aux page space" {
    try std.testing.expectEqual(process_abi.auxPageVa(7), descriptor_page_va);
    try std.testing.expectEqual(process_abi.auxPageVa(source_base_page_index), sourceVa(.primary_panel_config));
    try std.testing.expectEqual(process_abi.auxPageVa(source_base_page_index + 4), sourceVa(.window_service_config));
    try std.testing.expectEqual(process_abi.auxPageVa(source_base_page_index + 5), deviceConfigSourceVa(0));
    try std.testing.expectEqual(process_abi.auxPageVa(source_base_page_index + 10), deviceConfigSourceVa(5));
}
