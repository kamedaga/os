const std = @import("std");

pub const max_startup_program_descriptors: usize = 12;
pub const path_max_bytes: usize = 96;
pub const program_flag_present: u64 = 1 << 0;

pub const StartupAction = enum(u8) {
    vfs = 1,
    input_driver = 3,
    block_driver = 4,
    persistent_fs_server = 5,
    block_client = 6,
    window_client = 7,
    deferred_compositor = 8,
    gpu_driver = 9,
    window_service = 10,
    process = 11,
    process_builder = 12,
};

pub const StartupExecSource = enum(u8) {
    startup_path = 1,
    bootfs = 2,
};

pub const StartupInputSelector = enum(u8) {
    keyboard = 1,
    pointer = 2,
};

pub const StartupBlockSelector = enum(u8) {
    virtio_blk = 1,
};

pub const StartupWindowConfig = enum(u8) {
    terminal = 1,
    taskbar = 2,
    mouse_demo = 3,
};

pub const StartupCompositorVariant = enum(u8) {
    classic = 1,
    gpu = 2,
};

pub const ensure_flag_boot_display: u64 = 1 << 0;

pub const require_flag_keyboard_shared: u64 = 1 << 0;
pub const require_flag_pointer_shared: u64 = 1 << 1;
pub const require_flag_block_service: u64 = 1 << 2;
pub const require_flag_persistent_fs_service: u64 = 1 << 4;
pub const require_flag_compositor_armed: u64 = 1 << 5;

pub const StartupProgramRole = enum(u64) {
    vfs = 1,
    keyboard_driver = 2,
    mouse_driver = 3,
    terminal_window = 4,
    taskbar = 5,
    mouse_button_demo = 6,
    compositor = 7,
    gpu_compositor = 8,
    block_driver = 9,
    block_demo = 10,
    persistent_fs = 11,
    gpu_driver = 12,
};

pub const StartupProgramDescriptor = extern struct {
    role: u64,
    flags: u64,
    path_len: u64,
    path_bytes: [path_max_bytes]u8,
};

pub fn pathSlice(desc: *const StartupProgramDescriptor) []const u8 {
    const len: usize = @intCast(if (desc.path_len > path_max_bytes) path_max_bytes else desc.path_len);
    return desc.path_bytes[0..len];
}

pub fn roleLabel(role: StartupProgramRole) []const u8 {
    return switch (role) {
        .vfs => "vfs",
        .keyboard_driver => "keyboard driver",
        .mouse_driver => "mouse driver",
        .terminal_window => "terminal window",
        .taskbar => "taskbar",
        .mouse_button_demo => "mouse button demo",
        .compositor => "compositor",
        .gpu_compositor => "gpu compositor",
        .block_driver => "block driver",
        .block_demo => "block demo",
        .persistent_fs => "persistent fs",
        .gpu_driver => "gpu driver",
    };
}

pub fn roleKey(role: StartupProgramRole) []const u8 {
    return switch (role) {
        .vfs => "vfs",
        .keyboard_driver => "keyboard_driver",
        .mouse_driver => "mouse_driver",
        .terminal_window => "terminal_window",
        .taskbar => "taskbar",
        .mouse_button_demo => "mouse_button_demo",
        .compositor => "compositor",
        .gpu_compositor => "gpu_compositor",
        .block_driver => "block_driver",
        .block_demo => "block_demo",
        .persistent_fs => "persistent_fs",
        .gpu_driver => "gpu_driver",
    };
}

pub fn roleFromKey(key: []const u8) ?StartupProgramRole {
    inline for (std.meta.fields(StartupProgramRole)) |field| {
        const role: StartupProgramRole = @enumFromInt(field.value);
        if (std.mem.eql(u8, key, roleKey(role))) return role;
    }
    return null;
}

pub fn actionFromKey(key: []const u8) ?StartupAction {
    if (std.mem.eql(u8, key, "vfs")) return .vfs;
    if (std.mem.eql(u8, key, "input_driver")) return .input_driver;
    if (std.mem.eql(u8, key, "block_driver")) return .block_driver;
    if (std.mem.eql(u8, key, "persistent_fs")) return .persistent_fs_server;
    if (std.mem.eql(u8, key, "gpu_driver")) return .gpu_driver;
    if (std.mem.eql(u8, key, "window_service")) return .window_service;
    if (std.mem.eql(u8, key, "process") or std.mem.eql(u8, key, "program")) return .process;
    if (std.mem.eql(u8, key, "process_builder")) return .process_builder;
    if (std.mem.eql(u8, key, "block_client") or std.mem.eql(u8, key, "block_demo")) return .block_client;
    if (std.mem.eql(u8, key, "window_client")) return .window_client;
    if (std.mem.eql(u8, key, "deferred_compositor")) return .deferred_compositor;
    return null;
}

pub fn execSourceFromKey(key: []const u8) ?StartupExecSource {
    if (std.mem.eql(u8, key, "startup_path") or
        std.mem.eql(u8, key, "startup_fs") or
        std.mem.eql(u8, key, "rootfs") or
        std.mem.eql(u8, key, "default") or
        std.mem.eql(u8, key, "fs"))
    {
        return .startup_path;
    }
    if (std.mem.eql(u8, key, "bootfs")) return .bootfs;
    return null;
}

pub fn inputSelectorFromKey(key: []const u8) ?StartupInputSelector {
    if (std.mem.eql(u8, key, "keyboard")) return .keyboard;
    if (std.mem.eql(u8, key, "pointer") or std.mem.eql(u8, key, "mouse")) return .pointer;
    return null;
}

pub fn blockSelectorFromKey(key: []const u8) ?StartupBlockSelector {
    if (std.mem.eql(u8, key, "virtio_blk")) return .virtio_blk;
    return null;
}

pub fn windowConfigFromKey(key: []const u8) ?StartupWindowConfig {
    if (std.mem.eql(u8, key, "terminal")) return .terminal;
    if (std.mem.eql(u8, key, "taskbar")) return .taskbar;
    if (std.mem.eql(u8, key, "mouse_demo")) return .mouse_demo;
    return null;
}

pub fn compositorVariantFromKey(key: []const u8) ?StartupCompositorVariant {
    if (std.mem.eql(u8, key, "classic")) return .classic;
    if (std.mem.eql(u8, key, "gpu")) return .gpu;
    return null;
}

pub fn ensureBitFromKey(key: []const u8) ?u64 {
    if (std.mem.eql(u8, key, "boot_display")) return ensure_flag_boot_display;
    return null;
}

pub fn requirementBitFromKey(key: []const u8) ?u64 {
    if (std.mem.eql(u8, key, "keyboard_shared")) return require_flag_keyboard_shared;
    if (std.mem.eql(u8, key, "pointer_shared")) return require_flag_pointer_shared;
    if (std.mem.eql(u8, key, "block_service")) return require_flag_block_service;
    if (std.mem.eql(u8, key, "persistent_fs_service")) return require_flag_persistent_fs_service;
    if (std.mem.eql(u8, key, "compositor_armed")) return require_flag_compositor_armed;
    return null;
}

test "descriptor size stays bounded" {
    try std.testing.expect(@sizeOf(StartupProgramDescriptor) <= 128);
}

test "policy token helpers parse expected keys" {
    try std.testing.expect(execSourceFromKey("bootfs") == .bootfs);
    try std.testing.expect(inputSelectorFromKey("mouse") == .pointer);
    try std.testing.expect(blockSelectorFromKey("virtio_blk") == .virtio_blk);
    try std.testing.expect(windowConfigFromKey("taskbar") == .taskbar);
    try std.testing.expect(compositorVariantFromKey("gpu") == .gpu);
    try std.testing.expectEqual(ensure_flag_boot_display, ensureBitFromKey("boot_display").?);
    try std.testing.expectEqual(require_flag_compositor_armed, requirementBitFromKey("compositor_armed").?);
}
