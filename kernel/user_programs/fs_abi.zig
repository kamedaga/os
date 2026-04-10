const std = @import("std");

pub const syscall_install_cap: u64 = 0x18;
pub const syscall_grant_cap: u64 = 0x19;
pub const syscall_move_cap: u64 = 0x1A;
pub const syscall_send_cap: u64 = 0x1B;
pub const syscall_recv_cap: u64 = 0x1C;

pub const cap_token_tag: u64 = 1 << 63;

pub const ObjectKind = enum(u8) {
    none = 0,
    mount = 1,
    vnode_dir = 2,
    vnode_file = 3,
    open_file = 4,
    exec = 5,
    block_device = 6,
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
