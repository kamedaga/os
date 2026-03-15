const std = @import("std");

pub const syscall_install_cap: u64 = 0x18;
pub const syscall_grant_cap: u64 = 0x19;
pub const syscall_move_cap: u64 = 0x1A;
pub const syscall_send_cap: u64 = 0x1B;
pub const syscall_recv_cap: u64 = 0x1C;

pub const syscall_ok: u64 = 0;
pub const syscall_err_invalid: u64 = 1;
pub const syscall_err_not_ready: u64 = 2;
pub const syscall_err_alloc: u64 = 4;
pub const syscall_err_map: u64 = 5;
pub const syscall_err_move: u64 = 6;
pub const syscall_err_drop_present: u64 = 7;
pub const syscall_err_send: u64 = 8;
pub const syscall_err_endpoint: u64 = 9;
pub const syscall_err_revoke: u64 = 10;
pub const syscall_err_grant: u64 = 11;
pub const syscall_err_empty: u64 = 13;

pub const cap_token_tag: u64 = 1 << 63;

pub const ObjectKind = enum(u8) {
    none = 0,
    mount = 1,
    vnode_dir = 2,
    vnode_file = 3,
    open_file = 4,
    exec = 5,
};

pub const Rights = packed struct(u32) {
    lookup: bool = false,
    read: bool = false,
    write: bool = false,
    readdir: bool = false,
    stat: bool = false,
    create: bool = false,
    unlink: bool = false,
    rename: bool = false,
    exec: bool = false,
    mount: bool = false,
    grant: bool = false,
    admin: bool = false,
    _reserved: u20 = 0,
};

pub fn rightsToBits(rights: Rights) u64 {
    return @as(u64, @as(u32, @bitCast(rights)));
}

pub fn rightsFromBits(bits: u64) Rights {
    return @bitCast(@as(u32, @truncate(bits)));
}

pub fn encodeCapToken(cap_id: u64) u64 {
    std.debug.assert(cap_id != 0);
    std.debug.assert((cap_id & cap_token_tag) == 0);
    return cap_token_tag | cap_id;
}

pub fn decodeCapToken(token: u64) ?u64 {
    if ((token & cap_token_tag) == 0) return null;
    const cap_id = token & ~cap_token_tag;
    if (cap_id == 0) return null;
    return cap_id;
}

pub fn isCapToken(value: u64) bool {
    return decodeCapToken(value) != null;
}

test "filesystem cap token round-trips" {
    const encoded = encodeCapToken(0x1234);
    try std.testing.expect(isCapToken(encoded));
    try std.testing.expectEqual(@as(?u64, 0x1234), decodeCapToken(encoded));
    try std.testing.expectEqual(@as(?u64, null), decodeCapToken(0x1234));
}

test "filesystem rights bits round-trip" {
    const rights: Rights = .{
        .lookup = true,
        .read = true,
        .readdir = true,
        .grant = true,
    };
    try std.testing.expectEqual(rights, rightsFromBits(rightsToBits(rights)));
}
