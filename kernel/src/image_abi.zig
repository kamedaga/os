const std = @import("std");

pub const syscall_install_vm_object: u64 = 0x1E;
pub const syscall_grant_vm_object: u64 = 0x1F;
pub const syscall_install_exec_image: u64 = 0x20;
pub const syscall_grant_exec_image: u64 = 0x21;

pub const vm_object_token_tag: u64 = 1 << 62;
pub const exec_image_token_tag: u64 = (1 << 62) | (1 << 61);

pub const VmObjectRights = packed struct(u32) {
    read: bool = false,
    map: bool = false,
    grant: bool = false,
    _reserved: u29 = 0,
};

pub const ExecImageRights = packed struct(u32) {
    exec: bool = false,
    grant: bool = false,
    _reserved: u30 = 0,
};

pub fn vmObjectRightsToBits(rights: VmObjectRights) u64 {
    return @as(u64, @as(u32, @bitCast(rights)));
}

pub fn vmObjectRightsFromBits(bits: u64) VmObjectRights {
    return @bitCast(@as(u32, @truncate(bits)));
}

pub fn execImageRightsToBits(rights: ExecImageRights) u64 {
    return @as(u64, @as(u32, @bitCast(rights)));
}

pub fn execImageRightsFromBits(bits: u64) ExecImageRights {
    return @bitCast(@as(u32, @truncate(bits)));
}

pub fn encodeVmObjectToken(cap_id: u64) u64 {
    std.debug.assert(cap_id != 0);
    std.debug.assert((cap_id & vm_object_token_tag) == 0);
    std.debug.assert((cap_id & exec_image_token_tag) != exec_image_token_tag);
    return vm_object_token_tag | cap_id;
}

pub fn decodeVmObjectToken(token: u64) ?u64 {
    if ((token & exec_image_token_tag) == exec_image_token_tag) return null;
    if ((token & vm_object_token_tag) == 0) return null;
    const cap_id = token & ~vm_object_token_tag;
    if (cap_id == 0) return null;
    return cap_id;
}

pub fn encodeExecImageToken(cap_id: u64) u64 {
    std.debug.assert(cap_id != 0);
    std.debug.assert((cap_id & exec_image_token_tag) == 0);
    return exec_image_token_tag | cap_id;
}

pub fn decodeExecImageToken(token: u64) ?u64 {
    if ((token & exec_image_token_tag) != exec_image_token_tag) return null;
    const cap_id = token & ~exec_image_token_tag;
    if (cap_id == 0) return null;
    return cap_id;
}

test "vm object token round-trips" {
    const encoded = encodeVmObjectToken(0x1234);
    try std.testing.expectEqual(@as(?u64, 0x1234), decodeVmObjectToken(encoded));
    try std.testing.expectEqual(@as(?u64, null), decodeVmObjectToken(0x1234));
}

test "exec image token round-trips" {
    const encoded = encodeExecImageToken(0x5678);
    try std.testing.expectEqual(@as(?u64, 0x5678), decodeExecImageToken(encoded));
    try std.testing.expectEqual(@as(?u64, null), decodeExecImageToken(0x5678));
}
