pub const syscall_install_vm_object: u64 = 0x1E;
pub const syscall_grant_vm_object: u64 = 0x1F;
pub const syscall_install_exec_image: u64 = 0x20;
pub const syscall_grant_exec_image: u64 = 0x21;
pub const syscall_map_vm_object: u64 = 0x28;
pub const syscall_slice_vm_object: u64 = 0x29;

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

pub fn execImageRightsToBits(rights: ExecImageRights) u64 {
    return @as(u64, @as(u32, @bitCast(rights)));
}

pub fn decodeVmObjectToken(token: u64) ?u64 {
    if ((token & exec_image_token_tag) == exec_image_token_tag) return null;
    if ((token & vm_object_token_tag) == 0) return null;
    const cap_id = token & ~vm_object_token_tag;
    if (cap_id == 0) return null;
    return cap_id;
}

pub fn decodeExecImageToken(token: u64) ?u64 {
    if ((token & exec_image_token_tag) != exec_image_token_tag) return null;
    const cap_id = token & ~exec_image_token_tag;
    if (cap_id == 0) return null;
    return cap_id;
}
