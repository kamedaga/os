pub const syscall_create_ipc_buffer_from_page: u64 = 0x5E;
pub const syscall_grant_ipc_buffer_on_endpoint: u64 = 0x5F;
pub const syscall_share_ipc_buffer_on_endpoint: u64 = 0x60;
pub const syscall_accept_ipc_buffer_transfer: u64 = 0x61;
pub const syscall_map_ipc_buffer_anywhere: u64 = 0x62;

pub const token_tag: u64 = 0xA000_0000_0000_0000;
pub const token_payload_mask: u64 = 0x0FFF_FFFF_FFFF_FFFF;

pub const Rights = packed struct(u32) {
    read: bool = false,
    write: bool = false,
    map: bool = false,
    grant: bool = false,
    _reserved: u28 = 0,
};

pub const Role = enum(u64) {
    generic = 0,
    request = 1,
    response = 2,
    bulk = 3,
};

pub fn rightsToBits(rights: Rights) u64 {
    return @as(u64, @as(u32, @bitCast(rights)));
}

pub fn rightsFromBits(bits: u64) Rights {
    return @bitCast(@as(u32, @truncate(bits)));
}

pub fn encodeIpcBufferToken(cap_id: u64) u64 {
    return token_tag | (cap_id & token_payload_mask);
}

pub fn decodeIpcBufferToken(token: u64) ?u64 {
    if ((token & ~token_payload_mask) != token_tag) return null;
    const cap_id = token & token_payload_mask;
    if (cap_id == 0) return null;
    return cap_id;
}

test "ipc buffer token round-trip" {
    const std = @import("std");
    const encoded = encodeIpcBufferToken(0x1234);
    try std.testing.expectEqual(@as(?u64, 0x1234), decodeIpcBufferToken(encoded));
    try std.testing.expectEqual(@as(?u64, null), decodeIpcBufferToken(0x1234));
}
