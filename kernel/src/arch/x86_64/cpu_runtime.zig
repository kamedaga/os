//! BSP-owned lifetime storage shared by every user root through PML4[511].
//! Allocation failure aborts boot: partially acquired pages remain owned here,
//! never published to an AP and never returned underneath an installed PTE.
const std = @import("std");
const kernel = @import("../../kernel.zig");
const platform = @import("platform.zig");
const physical = @import("physical_layout.zig");
const image = @import("../../boot/image_range.zig");
const boot_map = @import("boot_direct_map.zig");
const dm = @import("direct_map.zig");
const address_mask: u64 = 0x000f_ffff_ffff_f000;
// Runtime grows downward and boot scratch upward in shared PML4[511]. Their
// boundary is reserved at PDP granularity; capacity comes from the discovered
// topology and available address space, not a compile-time CPU count.
var next: u64 = 0;
var end: u64 = 0;
var owner: ?*kernel.FreePageList = null;
pub export var runtime_cpu_capacity: usize = 1;
var roots: [512]u64 = @splat(0);
var sealed = false;
const DirectSplit = struct { next: ?*DirectSplit, va: u64, size: dm.LeafSize, physical: u64 };
var direct_splits: ?*DirectSplit = null;
pub fn directSplitOwns(va: u64, size: dm.LeafSize, pa: u64) bool {
    var current = direct_splits;
    while (current) |split| : (current = split.next) {
        if (split.va == va and split.size == size and split.physical == pa) return true;
    }
    return false;
}

pub fn init(list: *kernel.FreePageList) !void {
    if (owner != null) return error.AlreadyInitialized;
    owner = list;
    end = image.virtual_base & ~@as(u64, (1 << 30) - 1);
    next = end;
}
pub fn capacity() usize {
    return runtime_cpu_capacity;
}
// TSC_AUX is 32-bit and x2APIC reserves the all-ones destination for broadcast.
// Actual allocation remains bounded by discovered topology, RAM and virtual space.
pub fn setCapacity(count: usize) !void {
    if (count == 0 or count > std.math.maxInt(u32) or sealed) return error.InvalidCpuCount;
    runtime_cpu_capacity = count;
}
pub fn seal() void {
    sealed = true;
}
pub fn reserve(bytes: usize, alignment: usize) !u64 {
    if (sealed or owner == null) return error.InvalidState;
    if (alignment == 0 or !std.math.isPowerOfTwo(alignment)) return error.InvalidAlignment;
    const unaligned = try std.math.sub(u64, next, bytes);
    const start = std.mem.alignBackward(u64, unaligned, alignment);
    try boot_map.reserveRuntimeBoundary(start);
    next = start;
    return start;
}
fn page() !u64 {
    const pa = try owner.?.popContiguousBelow(1, physical.allocation_limit);
    const words = physical.identitySlice(u64, .fromInt(pa), 512) orelse return error.InvalidPhysicalPage;
    @memset(words, 0);
    return pa;
}
fn child(link: *u64) !*[512]u64 {
    if (link.* == 0) link.* = (try page()) | 3;
    if (link.* & 128 != 0) return error.MappingConflict;
    return physical.tablePointer(.fromInt(link.* & address_mask)) orelse error.InvalidPhysicalPage;
}
pub fn leaf(va: u64) !*u64 {
    if (sealed or va < next or va >= end) return error.InvalidState;
    const pdp = platform.runtimePdp();
    const index = (va >> 30) & 511;
    if (pdp[index] != 0 and roots[index] == 0) return error.MappingConflict;
    const pd = try child(&pdp[index]);
    roots[index] = pdp[index] & ~@as(u64, 32);
    const pt = try child(&pd[(va >> 21) & 511]);
    return &pt[(va >> 12) & 511];
}
pub fn map(va: u64, pa: u64) !void {
    const pte = try leaf(va);
    if (pte.* != 0) return error.MappingConflict;
    pte.* = pa | 3 | platform.page_nx;
}
pub fn alloc(comptime T: type, count: usize) ![]T {
    if (@import("builtin").is_test) {
        const result = try std.heap.page_allocator.alloc(T, count);
        @memset(std.mem.sliceAsBytes(result), 0);
        return result;
    }
    if (count == 0) return &.{};
    const bytes = try std.math.mul(usize, @sizeOf(T), count);
    const rounded = (try std.math.add(usize, bytes, 4095)) & ~@as(usize, 4095);
    const va = try reserve(rounded, @max(4096, @alignOf(T)));
    var off: usize = 0;
    while (off < rounded) : (off += 4096) try map(va + off, try page());
    const ptr: [*]T = @ptrFromInt(va);
    @memset(std.mem.sliceAsBytes(ptr[0..count]), 0);
    return ptr[0..count];
}
pub fn rootMatches(index: usize, entry: u64) bool {
    return roots[index] != 0 and roots[index] == (entry & ~@as(u64, 32));
}
// Generated entry text is sealed before AP startup. Split direct-map large
// leaves before withdrawing its writable alias; no writable alias survives
// publication of executable bytes, including the allocator's original alias.
pub fn makeExecutable(bytes: []u8) !void {
    const start = @intFromPtr(bytes.ptr);
    var off: usize = 0;
    while (off < bytes.len) : (off += 4096) {
        const pte = try leaf(start + off);
        const pa = pte.* & address_mask;
        var table = platform.runtimeRoot();
        const va = dm.base + pa;
        for ([_]u6{ 39, 30, 21 }) |shift| {
            const link = &table[(va >> shift) & 511];
            if (link.* & 128 != 0) {
                const large = link.*;
                // This boot-only path precedes RAM cache policy publication;
                // direct-map leaves must still be WB/PAT0. In particular a
                // large-page PAT bit must not become a physical address bit.
                if (large & (8 | 16 | 4096) != 0) return error.UnexpectedDirectMapCache;
                const child_pa = try page();
                const words = physical.tablePointer(.fromInt(child_pa)).?;
                const step: u64 = @as(u64, 1) << (shift - 9);
                const flags = (large & ~address_mask) & ~@as(u64, 128);
                for (words, 0..) |*word, i| word.* = (large & address_mask) + i * step | flags | (if (shift == 30) @as(u64, 128) else 0);
                const record = &(try alloc(DirectSplit, 1))[0];
                record.* = .{ .next = direct_splits, .va = va & ~((@as(u64, 1) << shift) - 1), .size = @enumFromInt(shift), .physical = child_pa };
                direct_splits = record;
                link.* = child_pa | (platform.page_nx | 3);
            }
            table = physical.tablePointer(.fromInt(link.* & address_mask)) orelse return error.InvalidPhysicalPage;
        }
        table[(va >> 12) & 511] = 0;
        platform.invlpg(va);
        pte.* = pa | 1;
        platform.invlpg(start + off);
    }
    // A serializing CR3 load completes publication before any LSTAR uses text.
    platform.writeCr3ForMaintenance(platform.readCr3());
    // Exercise the protection boundary before publishing LSTAR, using the
    // existing BSP fault probe rather than trusting PTE bits alone.
    const probe = @import("boot_hhdm.zig");
    off = 0;
    while (off < bytes.len) : (off += 4096) {
        const pte = try leaf(start + off);
        if (!probe.permissionFault(start + off, false) or
            !probe.unmappedReadFault(dm.base + (pte.* & address_mask))) return error.ExecutableAliasProtection;
    }
}

// Translation does not grant ownership: callers keep the pointed-to runtime
// object (or temporary window lease) alive. The island is not a linear physical
// extent; adjacent virtual pages may have unrelated PMM backing. Walk only its
// owned supervisor parents, including after boot allocation has been sealed.
fn physicalWith(comptime Reader: type, va: u64, low: u64, high: u64, owned_roots: *const [512]u64) ?u64 {
    if (low == 0 or va < low or va >= high or !Reader.rootLinked()) return null;
    const root_index = (va >> 30) & 511;
    const root = @atomicLoad(u64, &Reader.pdp()[root_index], .acquire);
    if (owned_roots[root_index] == 0 or root & ~@as(u64, 32) != owned_roots[root_index]) return null;
    if (root & ~(address_mask | @as(u64, 32)) != 3) return null;
    const pd = Reader.table(root & address_mask) orelse return null;
    const directory = @atomicLoad(u64, &pd[(va >> 21) & 511], .acquire);
    if (directory & ~(address_mask | @as(u64, 32)) != 3) return null;
    const pt = Reader.table(directory & address_mask) orelse return null;
    const entry = @atomicLoad(u64, &pt[(va >> 12) & 511], .acquire);
    // Supervisor leaf, including RX generated text. Do not infer backing from
    // a reservation: guard pages and unpopulated windows remain unmapped.
    if (entry & 1 == 0 or entry & 4 != 0 or entry & address_mask == 0) return null;
    return (entry & address_mask) | (va & 4095);
}

const NativeReader = struct {
    pub fn rootLinked() bool {
        const linked = physical.kernelAddressToPhysical(.fromInt(@intFromPtr(platform.runtimePdp())), .{
            .virtual_base = .fromInt(image.virtual_base),
            .physical_base = .fromInt(image.base_paddr),
            .size_bytes = image.size_bytes,
        }) orelse return false;
        return (@atomicLoad(u64, &platform.runtimeRoot()[511], .acquire) & ~@as(u64, 32)) == (linked.toInt() | 3);
    }
    pub fn pdp() *const [512]u64 {
        return platform.runtimePdp();
    }
    pub fn table(pa: u64) ?*const [512]u64 {
        return physical.tablePointer(.fromInt(pa));
    }
};

pub fn physicalAddress(address: physical.KernelVirtualAddress) ?physical.PhysicalAddress {
    const pa = physicalWith(NativeReader, address.toInt(), next, end, &roots) orelse return null;
    return .fromInt(pa);
}

test "direct map runtime pointer translation follows owned noncontiguous pages and rejects holes or foreign parents" {
    const Fixture = struct {
        var linked = true;
        var root: [512]u64 = @splat(0);
        var pd: [512]u64 = @splat(0);
        var pt: [512]u64 = @splat(0);
        pub fn rootLinked() bool {
            return linked;
        }
        pub fn pdp() *const [512]u64 {
            return &root;
        }
        pub fn table(pa: u64) ?*const [512]u64 {
            return switch (pa) {
                0x1000 => &pd,
                0x2000 => &pt,
                else => null,
            };
        }
    };
    const F = Fixture;
    const low: u64 = 0xffff_ffff_4000_0000;
    const high = low + 3 * 4096;
    var owned: [512]u64 = @splat(0);
    F.root = @splat(0);
    F.pd = @splat(0);
    F.pt = @splat(0);
    F.linked = true;
    owned[(low >> 30) & 511] = 0x1003;
    F.root[(low >> 30) & 511] = 0x1023; // Hardware A bit does not change ownership.
    F.pd[0] = 0x2023;
    F.pt[0] = 0x8000 | 0x63 | platform.page_nx;
    F.pt[1] = 0xb001; // RX text, physically noncontiguous with the previous page.
    try std.testing.expectEqual(@as(?u64, 0x8fff), physicalWith(F, low + 4095, low, high, &owned));
    try std.testing.expectEqual(@as(?u64, 0xb123), physicalWith(F, low + 4096 + 0x123, low, high, &owned));
    try std.testing.expect(physicalWith(F, low - 1, low, high, &owned) == null);
    try std.testing.expect(physicalWith(F, high, low, high, &owned) == null);
    try std.testing.expect(physicalWith(F, low + 8192, low, high, &owned) == null);
    F.linked = false;
    try std.testing.expect(physicalWith(F, low, low, high, &owned) == null);
    F.linked = true;
    F.root[(low >> 30) & 511] = 0x3003;
    try std.testing.expect(physicalWith(F, low, low, high, &owned) == null);
    F.root[(low >> 30) & 511] = 0x1003;
    for ([_]u64{ 0, 0x2007, 0x2083, 0x3003 }) |bad| {
        F.pd[0] = bad;
        try std.testing.expect(physicalWith(F, low, low, high, &owned) == null);
    }
    F.pd[0] = 0x2003;
    for ([_]u64{ 0, 0x8007, 3 }) |bad| {
        F.pt[0] = bad;
        try std.testing.expect(physicalWith(F, low, low, high, &owned) == null);
    }
}
