const std = @import("std");

pub const syscall_capsule_first: u64 = 51;
pub const syscall_capsule_query: u64 = 51;
pub const syscall_capsule_derive_mmio: u64 = 52;
pub const syscall_capsule_derive_dma_buffer: u64 = 53;
pub const syscall_capsule_derive_dma_mapping: u64 = 54;
pub const syscall_capsule_derive_dma_mapping_from_buffer: u64 = 55;
pub const syscall_capsule_derive_irq: u64 = 56;
pub const syscall_capsule_pci_config_read: u64 = 57;
pub const syscall_capsule_pci_config_write: u64 = 58;
pub const syscall_capsule_pci_bar_info: u64 = 59;
pub const syscall_capsule_irq_poll: u64 = 60;
pub const syscall_capsule_last: u64 = syscall_capsule_irq_poll;
pub const syscall_capsule_count: usize = @intCast(syscall_capsule_last - syscall_capsule_first + 1);

pub fn isCapsuleSyscall(nr: u64) bool {
    return nr >= syscall_capsule_first and nr <= syscall_capsule_last;
}

pub const CapsuleKind = enum(u8) {
    device = 2,
    mmio = 3,
    dma_buffer = 4,
    dma_mapping = 5,
    irq = 6,
};

pub const Rights = packed struct(u64) {
    query: bool = false,
    config_read: bool = false,
    config_write: bool = false,
    bar_info: bool = false,
    bar_map: bool = false,
    dma_alloc: bool = false,
    dma_map_user: bool = false,
    irq_bind: bool = false,
    bus_master: bool = false,
    reset: bool = false,
    power: bool = false,
    hotplug_observe: bool = false,
    grant: bool = false,
    _reserved: u51 = 0,
};

pub const known_rights_mask: u64 = (1 << 13) - 1;
pub const rights_grant_bit: u64 = 1 << 12;

pub const DmaDirection = enum(u2) {
    to_device = 1,
    from_device = 2,
    bidirectional = 3,
};

pub const IrqKind = enum(u2) {
    auto = 0,
    intx = 1,
    msi = 2,
    msix = 3,
};

pub fn rightsFromBits(bits: u64) Rights {
    return @bitCast(bits & known_rights_mask);
}

pub fn rightsToBits(rights: Rights) u64 {
    return @as(u64, @bitCast(rights)) & known_rights_mask;
}

pub const snapshot_word_count: usize = 11;
pub const snapshot_fd_index: usize = 0;
pub const snapshot_kind_index: usize = 1;
pub const snapshot_rights_index: usize = 2;
pub const snapshot_owner_index: usize = 3;
pub const snapshot_device_index: usize = 4;
pub const snapshot_object_id_index: usize = 5;
pub const snapshot_user_va_index: usize = 6;
pub const snapshot_iova_index: usize = 7;
pub const snapshot_size_index: usize = 8;
pub const snapshot_index_index: usize = 9;
pub const snapshot_flags_index: usize = 10;

pub const bar_info_word_count: usize = 4;
pub const bar_info_start_index: usize = 0;
pub const bar_info_end_index: usize = 1;
pub const bar_info_size_index: usize = 2;
pub const bar_info_flags_index: usize = 3;

pub const bar_flag_io: u64 = 1 << 0;
pub const bar_flag_mem: u64 = 1 << 1;
pub const bar_flag_prefetchable: u64 = 1 << 2;
pub const bar_flag_64bit: u64 = 1 << 3;
pub const known_bar_flags_mask: u64 =
    bar_flag_io |
    bar_flag_mem |
    bar_flag_prefetchable |
    bar_flag_64bit;

pub const mmio_map_flag_replace_existing: u64 = 1 << 0;
pub const mmio_map_known_flags_mask: u64 = mmio_map_flag_replace_existing;
pub const dma_iova_kernel_choose: u64 = 0;
pub const dma_buffer_known_flags_mask: u64 = 0;
pub const dma_mapping_known_flags_mask: u64 = 0;
pub const irq_known_flags_mask: u64 = 0;

pub const pci_bar_count: u32 = 6;
pub const pci_config_space_size: u32 = 256;

pub const irq_poll_word_count: usize = 1;
pub const irq_poll_count_index: usize = 0;

comptime {
    std.debug.assert(syscall_capsule_query == syscall_capsule_first + 0);
    std.debug.assert(syscall_capsule_derive_mmio == syscall_capsule_first + 1);
    std.debug.assert(syscall_capsule_derive_dma_buffer == syscall_capsule_first + 2);
    std.debug.assert(syscall_capsule_derive_dma_mapping == syscall_capsule_first + 3);
    std.debug.assert(syscall_capsule_derive_dma_mapping_from_buffer == syscall_capsule_first + 4);
    std.debug.assert(syscall_capsule_derive_irq == syscall_capsule_first + 5);
    std.debug.assert(syscall_capsule_pci_config_read == syscall_capsule_first + 6);
    std.debug.assert(syscall_capsule_pci_config_write == syscall_capsule_first + 7);
    std.debug.assert(syscall_capsule_pci_bar_info == syscall_capsule_first + 8);
    std.debug.assert(syscall_capsule_irq_poll == syscall_capsule_first + 9);
}

test "capsule rights mask strips reserved bits" {
    const rights = rightsFromBits(std.math.maxInt(u64));
    try std.testing.expectEqual(known_rights_mask, rightsToBits(rights));
}

test "capsule syscall range is stable and contiguous" {
    try std.testing.expect(isCapsuleSyscall(syscall_capsule_query));
    try std.testing.expect(isCapsuleSyscall(syscall_capsule_irq_poll));
    try std.testing.expect(!isCapsuleSyscall(syscall_capsule_first - 1));
    try std.testing.expect(!isCapsuleSyscall(syscall_capsule_last + 1));
    try std.testing.expectEqual(@as(usize, 10), syscall_capsule_count);
}
