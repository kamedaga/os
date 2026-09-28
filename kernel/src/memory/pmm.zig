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
        const image_base = h.kernel_image_base_paddr.*;
        const image_bytes = h.kernel_image_size_bytes.*;
        // Without Limine's physical image extent there is no safe physical
        // fallback: a linked code address is virtual (or a compiler sentinel).
        if (image_base == 0 or image_bytes == 0) {
            h.write("pmm: missing physical kernel image extent\n");
            return null;
        }
        const image_start = kernel_vm.pageAlignDown(image_base);
        const image_limit = std.math.add(u64, image_base, @as(u64, @intCast(image_bytes))) catch {
            h.write("pmm: physical kernel image extent overflows\n");
            return null;
        };
        const image_rounded_limit = std.math.add(u64, image_limit, 4095) catch {
            h.write("pmm: physical kernel image extent overflows\n");
            return null;
        };
        const image_end = image_rounded_limit & ~@as(u64, 4095);
        if (image_end <= image_start) return null;
        free_list.lock_word = 0;
        free_list.len = 0;
        free_list.range_len = 0;
        const kernel_static_start = kernel_vm.pageAlignDown(h.kernel_static_start_addr);
        const kernel_static_end = kernel_vm.pageAlignUp(h.kernel_static_end_addr);
        // Limine's executable physical base is authoritative. A Zig function
        // pointer is not a physical-address fallback (and may be represented
        // as 1), so taking its minimum with the image base can accidentally
        // reserve all usable RAM below the kernel image.
        const kernel_reserved_start = image_start;
        const kernel_reserved_end = image_end;

        var trace_buf: [128]u8 = undefined;
        const trace = std.fmt.bufPrint(&trace_buf, "pmm: kernel physical reservation=0x{x}..0x{x}\n", .{ kernel_reserved_start, kernel_reserved_end }) catch "";
        h.write(trace);

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
        if (hooks) |h| {
            const low = self.free_list.availableBelow(1 << 32);
            var trace_buf: [112]u8 = undefined;
            const trace = std.fmt.bufPrint(&trace_buf, "pmm: usable={} MiB free_below4g={} pages\n", .{ self.total_usable_bytes / (1024 * 1024), low.total }) catch "";
            h.write(trace);
        }
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

test "MMIO overlay PMM reserves the known physical kernel image without losing lower RAM" {
    const allocator = std.testing.allocator;
    const free_list = try allocator.create(kernel.FreePageList);
    defer allocator.destroy(free_list);
    free_list.* = .{};
    var image_base: u64 = 0x6efbf000;
    var image_bytes: usize = 0xaf32000;
    const previous_hooks = hooks;
    defer hooks = previous_hooks;
    init(.{
        .write = struct {
            fn write(_: []const u8) void {}
        }.write,
        .main_addr = 1,
        .kernel_cr3_addr = 0xffff_ffff_8014_d248,
        .kernel_image_base_paddr = &image_base,
        .kernel_image_size_bytes = &image_bytes,
        .kernel_static_start_addr = 0xffff_ffff_800e_5000,
        .kernel_static_end_addr = 0xffff_ffff_8af3_2000,
        .reserved_low_mem_end = 64 * 1024 * 1024,
    });
    image_base = 0;
    try std.testing.expect(MemoryCollector.init(free_list, 0, 0) == null);
    image_base = 0x6efbf000;
    image_bytes = 0;
    try std.testing.expect(MemoryCollector.init(free_list, 0, 0) == null);
    image_base = std.math.maxInt(u64) - 4095;
    image_bytes = 8192;
    try std.testing.expect(MemoryCollector.init(free_list, 0, 0) == null);
    image_base = 0x6efbf000;
    image_bytes = 0xaf32000;
    var collector = MemoryCollector.init(free_list, 0, 0) orelse return error.InvalidState;
    try std.testing.expectEqual(@as(u64, 0x6efbf000), collector.reserved[1].start);
    try std.testing.expectEqual(@as(u64, 0x79ef1000), collector.reserved[1].end);
    try collector.addRegion(.{ .base = 0x4000000, .length = 8192, .kind = .usable });
    try collector.addRegion(.{ .base = 0x6efbf000, .length = 8192, .kind = .usable });
    try std.testing.expectEqual(@as(usize, 2), free_list.availableBelow(1 << 32).total);
}
