const std = @import("std");

pub const syscall_capsule_first: u64 = 62;
pub const syscall_capsule_query: u64 = 62;
pub const syscall_capsule_derive_mmio: u64 = 63;
pub const syscall_capsule_derive_dma_buffer: u64 = 64;
pub const syscall_capsule_derive_dma_mapping: u64 = 65;
pub const syscall_capsule_derive_dma_mapping_pages: u64 = 66;
pub const syscall_capsule_derive_dma_mapping_from_buffer: u64 = 67;
pub const syscall_capsule_derive_irq: u64 = 68;
pub const syscall_capsule_pci_config_read: u64 = 69;
pub const syscall_capsule_pci_config_write: u64 = 70;
pub const syscall_capsule_pci_bar_info: u64 = 71;
pub const syscall_capsule_irq_poll: u64 = 72;
pub const syscall_capsule_dma_pool_create: u64 = 73;
pub const syscall_capsule_last: u64 = syscall_capsule_dma_pool_create;
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
pub const dma_iova_kernel_choose: u64 = std.math.maxInt(u64);
pub const dma_buffer_known_flags_mask: u64 = 0;
pub const dma_mapping_known_flags_mask: u64 = 0;
pub const dma_mapping_pages_max_pages: usize = 512;
pub const dma_pool_known_flags_mask: u64 = 0;
pub const dma_pool_max_pages: usize = 16384;
pub const irq_known_flags_mask: u64 = 0;

pub const DmaMappingPagesLayout = struct {
    first_page_va: u64,
    first_page_offset: u64,
    end_va_exclusive: u64,
    page_count: usize,
    output_size_bytes: u64,
};

pub fn dmaMappingPagesLayout(user_va: u64, size: u64) ?DmaMappingPagesLayout {
    if (user_va == 0 or size == 0) return null;
    const end_va, const end_overflow = @addWithOverflow(user_va, size);
    if (end_overflow != 0) return null;
    const first_page_va = user_va & ~@as(u64, 4095);
    const first_page_offset = user_va - first_page_va;
    const span, const span_overflow = @addWithOverflow(first_page_offset, size);
    if (span_overflow != 0) return null;
    const rounded_span, const rounded_overflow = @addWithOverflow(span, 4095);
    if (rounded_overflow != 0) return null;
    const page_count_u64 = rounded_span / 4096;
    if (page_count_u64 == 0 or page_count_u64 > dma_mapping_pages_max_pages) return null;
    return .{
        .first_page_va = first_page_va,
        .first_page_offset = first_page_offset,
        .end_va_exclusive = end_va,
        .page_count = @intCast(page_count_u64),
        .output_size_bytes = page_count_u64 * @sizeOf(u64),
    };
}

pub fn rangesOverlap(first_start: u64, first_size: u64, second_start: u64, second_size: u64) ?bool {
    if (first_size == 0 or second_size == 0) return null;
    const first_end, const first_overflow = @addWithOverflow(first_start, first_size);
    const second_end, const second_overflow = @addWithOverflow(second_start, second_size);
    if (first_overflow != 0 or second_overflow != 0) return null;
    return first_start < second_end and second_start < first_end;
}

pub fn dmaMappingPagesOutputOverlapsInput(
    layout: DmaMappingPagesLayout,
    output_va: u64,
) ?bool {
    const input_page_span, const span_overflow = @mulWithOverflow(
        @as(u64, @intCast(layout.page_count)),
        @as(u64, 4096),
    );
    if (span_overflow != 0) return null;
    return rangesOverlap(
        layout.first_page_va,
        input_page_span,
        output_va,
        layout.output_size_bytes,
    );
}

pub const pci_bar_count: u32 = 6;
pub const pci_config_space_size: u32 = 256;

pub const irq_poll_word_count: usize = 1;
pub const irq_poll_count_index: usize = 0;

comptime {
    std.debug.assert(syscall_capsule_query == syscall_capsule_first + 0);
    std.debug.assert(syscall_capsule_derive_mmio == syscall_capsule_first + 1);
    std.debug.assert(syscall_capsule_derive_dma_buffer == syscall_capsule_first + 2);
    std.debug.assert(syscall_capsule_derive_dma_mapping == syscall_capsule_first + 3);
    std.debug.assert(syscall_capsule_derive_dma_mapping_pages == syscall_capsule_first + 4);
    std.debug.assert(syscall_capsule_derive_dma_mapping_from_buffer == syscall_capsule_first + 5);
    std.debug.assert(syscall_capsule_derive_irq == syscall_capsule_first + 6);
    std.debug.assert(syscall_capsule_pci_config_read == syscall_capsule_first + 7);
    std.debug.assert(syscall_capsule_pci_config_write == syscall_capsule_first + 8);
    std.debug.assert(syscall_capsule_pci_bar_info == syscall_capsule_first + 9);
    std.debug.assert(syscall_capsule_irq_poll == syscall_capsule_first + 10);
    std.debug.assert(syscall_capsule_dma_pool_create == syscall_capsule_first + 11);
}

test "capsule rights mask strips reserved bits" {
    const rights = rightsFromBits(std.math.maxInt(u64));
    try std.testing.expectEqual(known_rights_mask, rightsToBits(rights));
}

test "capsule syscall range is stable and contiguous" {
    try std.testing.expect(isCapsuleSyscall(syscall_capsule_query));
    try std.testing.expect(isCapsuleSyscall(syscall_capsule_dma_pool_create));
    try std.testing.expect(!isCapsuleSyscall(syscall_capsule_first - 1));
    try std.testing.expect(!isCapsuleSyscall(syscall_capsule_last + 1));
    try std.testing.expectEqual(@as(usize, 12), syscall_capsule_count);
}

test "DMA mapping page layout checks boundaries and overflow" {
    const aligned = dmaMappingPagesLayout(0x4000, 4096).?;
    try std.testing.expectEqual(@as(usize, 1), aligned.page_count);
    try std.testing.expectEqual(@as(u64, 0), aligned.first_page_offset);
    try std.testing.expectEqual(@as(u64, 8), aligned.output_size_bytes);

    const unaligned = dmaMappingPagesLayout(0x4fff, 4096).?;
    try std.testing.expectEqual(@as(usize, 2), unaligned.page_count);
    try std.testing.expectEqual(@as(u64, 4095), unaligned.first_page_offset);

    const max_size = @as(u64, dma_mapping_pages_max_pages) * 4096 - 4095;
    try std.testing.expectEqual(
        dma_mapping_pages_max_pages,
        dmaMappingPagesLayout(0x4fff, max_size).?.page_count,
    );
    try std.testing.expect(dmaMappingPagesLayout(0x4fff, max_size + 1) == null);
    try std.testing.expect(dmaMappingPagesLayout(0x4000, 0) == null);
    try std.testing.expect(dmaMappingPagesLayout(std.math.maxInt(u64) - 1, 2) == null);
}

test "DMA mapping page output must not overlap the DMA range" {
    const layout = dmaMappingPagesLayout(0x4000, 8192).?;
    try std.testing.expectEqual(false, dmaMappingPagesOutputOverlapsInput(layout, 0x8000).?);
    try std.testing.expectEqual(true, dmaMappingPagesOutputOverlapsInput(layout, 0x5ff8).?);
    try std.testing.expect(dmaMappingPagesOutputOverlapsInput(layout, std.math.maxInt(u64)) == null);

    const same_page = dmaMappingPagesLayout(0x4ff0, 8).?;
    const output_va: u64 = 0x4100;
    try std.testing.expectEqual(
        false,
        rangesOverlap(0x4ff0, 8, output_va, same_page.output_size_bytes).?,
    );
    try std.testing.expectEqual(
        true,
        dmaMappingPagesOutputOverlapsInput(same_page, output_va).?,
    );
}
