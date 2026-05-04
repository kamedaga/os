const std = @import("std");
const kernel = @import("../kernel.zig");
const uefi = std.os.uefi;
const kernel_vm = @import("kernel_vm.zig");

pub const MemoryStats = struct {
    detected_regions: usize,
    total_usable_bytes: u64,
};

const ReservedRange = struct {
    start: u64,
    end: u64,
};

pub const Hooks = struct {
    write: *const fn ([]const u8) void,
    main_addr: usize,
    kernel_cr3_addr: usize,
    kernel_image_base_paddr: *const u64,
    kernel_image_size_bytes: *const usize,
    kernel_static_end_addr: usize,
    reserved_low_mem_end: u64,
};

var hooks: ?Hooks = null;

pub fn init(new_hooks: Hooks) void {
    hooks = new_hooks;
}

fn isReserved(paddr: u64, reserved: []const ReservedRange) bool {
    for (reserved) |r| {
        if (paddr >= r.start and paddr < r.end) return true;
    }
    return false;
}

pub fn collectMemoryStatsAndFreePages(
    bs: *uefi.tables.BootServices,
    free_list: *kernel.FreePageList,
    mmap_buffer: []align(@alignOf(uefi.tables.MemoryDescriptor)) u8,
    user_spaces_start: u64,
    user_spaces_end: u64,
) ?MemoryStats {
    const h = hooks orelse return null;
    h.write("[stage] collectMemoryStats begin\n");
    const mmap = bs.getMemoryMap(mmap_buffer) catch |err| {
        h.write("getMemoryMap failed: ");
        h.write(@errorName(err));
        h.write("\n");
        return null;
    };
    h.write("[stage] collectMemoryStats got map\n");

    free_list.* = .{};
    var detected_regions: usize = 0;
    var total_usable_bytes: u64 = 0;
    const fallback_kernel_start = kernel_vm.pageAlignDown(@min(h.main_addr, h.kernel_cr3_addr));
    const image_start = if (h.kernel_image_base_paddr.* != 0)
        kernel_vm.pageAlignDown(h.kernel_image_base_paddr.*)
    else
        fallback_kernel_start;
    const image_end = if (h.kernel_image_base_paddr.* != 0 and h.kernel_image_size_bytes.* != 0)
        kernel_vm.pageAlignUp(h.kernel_image_base_paddr.* + h.kernel_image_size_bytes.*)
    else
        fallback_kernel_start;
    const kernel_static_end = kernel_vm.pageAlignUp(h.kernel_static_end_addr);
    const static_reserved_start = if (kernel_static_end > h.reserved_low_mem_end)
        h.reserved_low_mem_end
    else
        kernel_static_end;
    const kernel_reserved_start = @min(@min(image_start, fallback_kernel_start), static_reserved_start);
    const kernel_reserved_end = @max(image_end, kernel_static_end);
    const reserved = [_]ReservedRange{
        .{ .start = 0, .end = h.reserved_low_mem_end },
        .{ .start = kernel_reserved_start, .end = kernel_reserved_end },
        .{ .start = user_spaces_start, .end = user_spaces_end },
    };

    var it = mmap.iterator();
    while (it.next()) |desc| {
        if (desc.type == .conventional_memory) {
            const region_id = detected_regions;
            var i: u64 = 0;
            while (i < desc.number_of_pages) : (i += 1) {
                const paddr = desc.physical_start + (i * 4096);
                if (isReserved(paddr, reserved[0..])) continue;
                free_list.appendPage(region_id, paddr) catch return null;
            }
            detected_regions += 1;
            total_usable_bytes += desc.number_of_pages * 4096;
        }
    }

    return .{
        .detected_regions = detected_regions,
        .total_usable_bytes = total_usable_bytes,
    };
}
