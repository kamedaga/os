const std = @import("std");

pub const max_startup_program_descriptors: usize = 8;
pub const path_max_bytes: usize = 96;
pub const program_flag_present: u64 = 1 << 0;

pub const StartupProgramRole = enum(u64) {
    vfs = 1,
    keyboard_driver = 2,
    mouse_driver = 3,
    terminal_window = 4,
    taskbar = 5,
    mouse_button_demo = 6,
    compositor = 7,
    gpu_compositor = 8,
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
    };
}

pub fn roleFromKey(key: []const u8) ?StartupProgramRole {
    inline for (std.meta.fields(StartupProgramRole)) |field| {
        const role: StartupProgramRole = @enumFromInt(field.value);
        if (std.mem.eql(u8, key, roleKey(role))) return role;
    }
    return null;
}

test "descriptor size stays bounded" {
    try std.testing.expect(@sizeOf(StartupProgramDescriptor) <= 128);
}
