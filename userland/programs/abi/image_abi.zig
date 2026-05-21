const std = @import("std");

pub const syscall_grant_vm_object: u64 = 0x1F;
pub const syscall_map_vm_object: u64 = 0x28;
pub const syscall_create_vm_object_from_current_pages: u64 = 0x3F;

pub const vm_object_token_tag: u64 = 1 << 62;

pub const VmObjectRights = packed struct(u32) {
    read: bool = false,
    write: bool = false,
    map: bool = false,
    grant: bool = false,
    _reserved: u28 = 0,
};

pub fn vmObjectRightsToBits(rights: VmObjectRights) u64 {
    return @as(u64, @as(u32, @bitCast(rights)));
}

pub fn vmObjectRightsFromBits(bits: u64) VmObjectRights {
    return @bitCast(@as(u32, @truncate(bits)));
}

pub fn encodeVmObjectToken(cap_id: u64) u64 {
    std.debug.assert(cap_id != 0);
    std.debug.assert((cap_id & vm_object_token_tag) == 0);
    return vm_object_token_tag | cap_id;
}

pub fn decodeVmObjectToken(token: u64) ?u64 {
    if ((token & vm_object_token_tag) == 0) return null;
    const cap_id = token & ~vm_object_token_tag;
    if (cap_id == 0) return null;
    return cap_id;
}

test "vm object token round-trips" {
    const encoded = encodeVmObjectToken(0x1234);
    try std.testing.expectEqual(@as(?u64, 0x1234), decodeVmObjectToken(encoded));
    try std.testing.expectEqual(@as(?u64, null), decodeVmObjectToken(0x1234));
}
