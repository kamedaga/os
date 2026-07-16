const std = @import("std");
const kernel = @import("../kernel.zig");
const kernel_vm = @import("kernel_vm.zig");

pub const MemoryStats = struct {
    detected_regions: usize,
    total_usable_bytes: u64,
};

const ReservedRange = struct {
    start: u64,
    end: u64,
};

pub const MemoryRegionKind = enum {
    usable,
    reserved,
    bootloader_reclaimable,
    executable_and_modules,
    framebuffer,
    acpi_reclaimable,
    acpi_nvs,
    bad_memory,
    unknown,
};

pub const MemoryRegion = struct {
    base: u64,
    length: u64,
    kind: MemoryRegionKind,
};

pub const Hooks = struct {
    write: *const fn ([]const u8) void,
    main_addr: usize,
    kernel_cr3_addr: usize,
    kernel_image_base_paddr: *const u64,
    kernel_image_size_bytes: *const usize,
    kernel_static_start_addr: usize,
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

pub const MemoryCollector = struct {
    free_list: *kernel.FreePageList,
    reserved: [4]ReservedRange,
    detected_regions: usize,
    total_usable_bytes: u64,

    pub fn init(
        free_list: *kernel.FreePageList,
        user_spaces_start: u64,
        user_spaces_end: u64,
    ) ?MemoryCollector {
        const h = hooks orelse return null;
        free_list.lock_word = 0;
        free_list.len = 0;
        free_list.range_len = 0;

        const fallback_kernel_start = kernel_vm.pageAlignDown(@min(h.main_addr, h.kernel_cr3_addr));
        const image_start = if (h.kernel_image_base_paddr.* != 0)
            kernel_vm.pageAlignDown(h.kernel_image_base_paddr.*)
        else
            fallback_kernel_start;
        const image_end = if (h.kernel_image_base_paddr.* != 0 and h.kernel_image_size_bytes.* != 0)
            kernel_vm.pageAlignUp(h.kernel_image_base_paddr.* + h.kernel_image_size_bytes.*)
        else
            fallback_kernel_start;
        const kernel_static_start = kernel_vm.pageAlignDown(h.kernel_static_start_addr);
        const kernel_static_end = kernel_vm.pageAlignUp(h.kernel_static_end_addr);
        const kernel_reserved_start = @min(image_start, fallback_kernel_start);
        const kernel_reserved_end = image_end;

        return .{
            .free_list = free_list,
            .reserved = .{
                .{ .start = 0, .end = h.reserved_low_mem_end },
                .{ .start = kernel_reserved_start, .end = kernel_reserved_end },
                .{ .start = kernel_static_start, .end = kernel_static_end },
                .{ .start = user_spaces_start, .end = user_spaces_end },
            },
            .detected_regions = 0,
            .total_usable_bytes = 0,
        };
    }

    pub fn addRegion(self: *MemoryCollector, region: MemoryRegion) !void {
        if (region.kind != .usable) return;
        const region_id = self.detected_regions;
        var offset: u64 = 0;
        while (offset + 4096 <= region.length) : (offset += 4096) {
            const paddr = region.base + offset;
            if (isReserved(paddr, self.reserved[0..])) continue;
            try self.free_list.appendPage(region_id, paddr);
        }
        self.detected_regions += 1;
        self.total_usable_bytes += region.length;
    }

    pub fn finish(self: MemoryCollector) MemoryStats {
        return .{
            .detected_regions = self.detected_regions,
            .total_usable_bytes = self.total_usable_bytes,
        };
    }
};

pub fn collectMemoryStatsAndFreePagesFromRegions(
    free_list: *kernel.FreePageList,
    regions: []const MemoryRegion,
    user_spaces_start: u64,
    user_spaces_end: u64,
) ?MemoryStats {
    var collector = MemoryCollector.init(free_list, user_spaces_start, user_spaces_end) orelse return null;
    for (regions) |region| {
        collector.addRegion(region) catch return null;
    }
    return collector.finish();
}
