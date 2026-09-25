const std = @import("std");
const builtin = @import("builtin");
const ivrs = @import("acpi_ivrs.zig");
const iova = @import("iova.zig");
const pci = @import("pci.zig");
const smp = @import("smp.zig");
const types = @import("state/types.zig");
const user_copy = @import("user_copy.zig");
const log = @import("kernel_log.zig");
const physical_layout = @import("arch/x86_64/physical_layout.zig");
const x86_platform = @import("arch/x86_64/platform.zig");
const mtrr = @import("arch/x86_64/mtrr.zig");

const page_size: u64 = 4096;
const four_gib: u64 = 0x1_0000_0000;
const page_mask: u64 = 0x000f_ffff_ffff_f000;
const read_bit: u64 = @as(u64, 1) << 61;
const write_bit: u64 = @as(u64, 1) << 62;
const present_bit: u64 = 1;
const access_bits: u64 = present_bit | read_bit | write_bit;
const max_domains: usize = 256;
const max_leaves: usize = 4096;
const leaf_index_slots: usize = max_leaves * 2;
const poll_limit: usize = 1_000_000;

const devtab_base_reg: u64 = 0x0000;
const command_base_reg: u64 = 0x0008;
const event_base_reg: u64 = 0x0010;
const control_reg: u64 = 0x0018;
const command_head_reg: u64 = 0x2000;
const command_tail_reg: u64 = 0x2008;
const event_head_reg: u64 = 0x2010;
const event_tail_reg: u64 = 0x2018;
const status_reg: u64 = 0x2020;
const ctrl_iommu_en: u64 = 1;
const ctrl_event_en: u64 = 1 << 2;
const ctrl_coherent: u64 = 1 << 10;
const ctrl_command_en: u64 = 1 << 12;
const status_event_overflow: u64 = 1;
const status_event_run: u64 = 1 << 3;
const status_command_run: u64 = 1 << 4;
const command_ring_bytes: u16 = 4096;
const event_ring_bytes: u16 = 4096;

const Page = [512]u64;
const Dte = [4]u64;
const Command = [4]u32;
const Event = [4]u32;

const UnitState = struct {
    register_base: u64 = 0,
    device_table_paddr: u64 = 0,
    device_table_pages: usize = 0,
    command_paddr: u64 = 0,
    event_paddr: u64 = 0,
    completion_paddr: u64 = 0,
    command_tail: u16 = 0,
    completion_sequence: u64 = 0,
    active: bool = false,
};

const Domain = struct {
    device: types.DmaDeviceId = types.invalid_dma_device_id,
    source_id: u16 = 0,
    did: u16 = 0,
    unit_index: u8 = 0,
    root_paddr: u64 = 0,
    allocator: iova.Allocator = .{},
    quarantined: bool = false,
    enabled: bool = true,
};

const Leaf = struct {
    table_paddr: u64 = 0,
    refcounts_paddr: u64 = 0,
    domain_id: u16 = 0,
    quarantined: bool = false,
};

const Candidate = struct {
    device: types.DmaDeviceId = types.invalid_dma_device_id,
    source_id: u16 = 0,
    unit_index: u8 = 0,
    settings: u8 = 0,
};

const DriverState = struct {
    info: ivrs.Info = .{},
    free_list: ?*types.FreePageList = null,
    units: [ivrs.max_units]UnitState = [_]UnitState{.{}} ** ivrs.max_units,
    domains: [max_domains]Domain = [_]Domain{.{}} ** max_domains,
    candidates: [max_domains]Candidate = [_]Candidate{.{}} ** max_domains,
    candidate_count: usize = 0,
    domain_count: usize = 0,
    leaves: [max_leaves]Leaf = [_]Leaf{.{}} ** max_leaves,
    leaf_index: [leaf_index_slots]u16 = [_]u16{0} ** leaf_index_slots,
    leaf_count: usize = 0,
    iova_end: u64 = iova.window_ceiling,
    iova_spares: ?*iova.Bank = null,
    has_quarantine: bool = false,
    active: bool = false,
    lock_word: u8 = 0,
};

var state: DriverState = .{};
var test_fail_completion: bool = false;

const unit_register_bytes: u64 = 0x3000;

fn bootUnitMmioRangeSupported(snapshot: *const mtrr.CacheSnapshot, base: u64) bool {
    return base < physical_layout.identity_limit and
        unit_register_bytes <= physical_layout.identity_limit - base and
        snapshot.rangeSupportsUncachedMapping(base, unit_register_bytes);
}

/// The IOMMU uses the low identity VA for its register block. Validate IVRS
/// with the production parser and make only its selected register pages UC
/// before APs start; a late cache-type remap would need global TLB/cache
/// coordination and could leave an incompatible WB alias behind.
pub fn prepareBootMmio(rsdp_paddr: u64) bool {
    const table = ivrs.find(rsdp_paddr) orelse return true;
    ivrs.parse(table, &state.info) catch |err| {
        log.writeFmt("amd-iommu: early IVRS rejected error={s}\n", .{@errorName(err)});
        return false;
    };
    var snapshot: mtrr.CacheSnapshot = .{};
    if (!mtrr.captureCacheSnapshot(&snapshot)) {
        log.write("amd-iommu: BSP PAT/MTRR proof unavailable before AP startup\n");
        return false;
    }
    for (state.info.units[0..state.info.unit_count], 0..) |unit, index| {
        if (!bootUnitMmioRangeSupported(&snapshot, unit.mmio_base) or
            !x86_platform.mapBootMmioIdentityRange(unit.mmio_base, unit_register_bytes))
        {
            log.writeFmt("amd-iommu: boot UC mapping failed unit={} base=0x{x}\n", .{ index, unit.mmio_base });
            return false;
        }
        log.writeFmt("amd-iommu: boot UC register map unit={} base=0x{x} pages=3\n", .{ index, unit.mmio_base });
    }
    return true;
}

test "AMD IOMMU boot MMIO range admission is generic and bounded" {
    const snapshot = mtrr.CacheSnapshot{
        .pat = 6 | (@as(u64, 7) << 16),
        .physical_bits = 46,
        .default_type = (1 << 11) | 6,
    };
    try std.testing.expect(bootUnitMmioRangeSupported(&snapshot, 0xfd50_0000));
    try std.testing.expect(bootUnitMmioRangeSupported(&snapshot, 0x1_0000_0000));
    try std.testing.expect(!bootUnitMmioRangeSupported(&snapshot, 0xfd50_0001));
    try std.testing.expect(!bootUnitMmioRangeSupported(&snapshot, physical_layout.identity_limit - 0x2000));
    try std.testing.expect(!bootUnitMmioRangeSupported(&snapshot, physical_layout.identity_limit));
}

pub fn kernelStaticStorageStartAddr() usize {
    return @intFromPtr(&state);
}

pub fn kernelStaticStorageEndAddr() usize {
    return @intFromPtr(&state) + @sizeOf(DriverState);
}

fn lock() void {
    while (true) {
        if (@cmpxchgWeak(u8, &state.lock_word, 0, 1, .acquire, .monotonic) == null) return;
        while (@atomicLoad(u8, &state.lock_word, .monotonic) != 0) asm volatile ("pause");
    }
}

fn unlock() void {
    @atomicStore(u8, &state.lock_word, 0, .release);
}

fn mmioRead64(unit: *const UnitState, offset: u64) u64 {
    return @as(*volatile u64, @ptrFromInt(unit.register_base + offset)).*;
}

fn mmioWrite64(unit: *const UnitState, offset: u64, value: u64) void {
    @as(*volatile u64, @ptrFromInt(unit.register_base + offset)).* = value;
}

fn pageAt(paddr: u64) *Page {
    return @ptrFromInt(paddr);
}

fn deviceTableAt(unit: *const UnitState, source: u16) *Dte {
    return @ptrFromInt(unit.device_table_paddr + @as(u64, source) * @sizeOf(Dte));
}

fn pageAddress(entry: u64) u64 {
    return entry & page_mask;
}

fn allocZeroPage() ?u64 {
    const free_list = state.free_list orelse return null;
    const paddr = if (builtin.is_test)
        free_list.popFront() catch return null
    else
        free_list.popFrontBelow(four_gib) catch return null;
    if (builtin.is_test) {
        @memset(pageAt(paddr)[0..], 0);
    } else if (!user_copy.zeroPhysicalPage(paddr)) {
        free_list.appendPage(0, paddr) catch {};
        return null;
    }
    return paddr;
}

fn allocIovaBank(_: ?*anyopaque) ?*iova.Bank {
    if (state.iova_spares) |bank| {
        state.iova_spares = bank.next;
        return bank;
    }
    return @ptrFromInt(allocZeroPage() orelse return null);
}

fn freeIovaBank(_: ?*anyopaque, bank: *iova.Bank) void {
    const free_list = state.free_list orelse unreachable;
    free_list.appendPage(0, @intFromPtr(bank)) catch {
        bank.next = state.iova_spares;
        state.iova_spares = bank;
    };
}

fn domainForDevice(device: types.DmaDeviceId) ?*Domain {
    for (state.domains[0..state.domain_count]) |*domain| {
        if (domain.device == device) return domain;
    }
    return null;
}

fn ensureAllocator(domain: *Domain) bool {
    if (domain.allocator.storage != null) return true;
    if (state.iova_end <= iova.window_start) return false;
    domain.allocator = iova.Allocator.init(iova.window_start, state.iova_end, .{
        .allocate = allocIovaBank,
        .release = freeIovaBank,
    });
    return true;
}

fn pte(paddr: u64, next_level: u3, readable: bool, writable: bool) u64 {
    return (paddr & page_mask) | present_bit | (@as(u64, next_level) << 9) |
        (if (readable) read_bit else 0) | (if (writable) write_bit else 0);
}

fn ensureChild(parent: *Page, index: usize, next_level: u3, enabled: bool) ?u64 {
    const old = pageAddress(parent[index]);
    if (old != 0) return old;
    const child = allocZeroPage() orelse return null;
    parent[index] = pte(child, next_level, enabled, enabled);
    return child;
}

fn leafFor(table_paddr: u64, create: bool, did: u16) ?*Leaf {
    const start: usize = @intCast((table_paddr >> 12) & (leaf_index_slots - 1));
    var probe: usize = 0;
    while (probe < leaf_index_slots) : (probe += 1) {
        const slot = (start + probe) & (leaf_index_slots - 1);
        const encoded = state.leaf_index[slot];
        if (encoded != 0) {
            const metadata = &state.leaves[encoded - 1];
            if (metadata.table_paddr == table_paddr) return metadata;
            continue;
        }
        if (!create or state.leaf_count == max_leaves) return null;
        const refs = allocZeroPage() orelse return null;
        const index = state.leaf_count;
        state.leaves[index] = .{ .table_paddr = table_paddr, .refcounts_paddr = refs, .domain_id = did };
        state.leaf_count += 1;
        state.leaf_index[slot] = @intCast(index + 1);
        return &state.leaves[index];
    }
    return null;
}

const Walk = struct { table: *Page, metadata: *Leaf, index: usize };

fn walk(domain: *const Domain, address: u64, create: bool) ?Walk {
    const root = pageAt(domain.root_paddr);
    const l4: usize = @intCast((address >> 39) & 511);
    const l3_paddr = if (create)
        ensureChild(root, l4, 3, domain.enabled) orelse return null
    else
        pageAddress(root[l4]);
    if (l3_paddr == 0) return null;
    const l3 = pageAt(l3_paddr);
    const l3_index: usize = @intCast((address >> 30) & 511);
    const l2_paddr = if (create)
        ensureChild(l3, l3_index, 2, true) orelse return null
    else
        pageAddress(l3[l3_index]);
    if (l2_paddr == 0) return null;
    const l2 = pageAt(l2_paddr);
    const l2_index: usize = @intCast((address >> 21) & 511);
    const l1_paddr = if (create)
        ensureChild(l2, l2_index, 1, true) orelse return null
    else
        pageAddress(l2[l2_index]);
    if (l1_paddr == 0) return null;
    return .{
        .table = pageAt(l1_paddr),
        .metadata = leafFor(l1_paddr, create, domain.did) orelse return null,
        .index = @intCast((address >> 12) & 511),
    };
}

fn refcounts(paddr: u64) *[512]u16 {
    return @ptrFromInt(paddr);
}

fn mapPage(domain: *const Domain, address: u64, paddr: u64, readable: bool, writable: bool) bool {
    const leaf = walk(domain, address, true) orelse return false;
    const refs = refcounts(leaf.metadata.refcounts_paddr);
    if (refs[leaf.index] == std.math.maxInt(u16)) return false;
    const desired = pte(paddr, 0, readable, writable);
    const existing = leaf.table[leaf.index];
    if (existing != 0 and existing != desired) return false;
    if (existing == 0) leaf.table[leaf.index] = desired;
    refs[leaf.index] += 1;
    return true;
}

fn withdrawPage(domain: *const Domain, address: u64) void {
    const leaf = walk(domain, address, false) orelse return;
    const refs = refcounts(leaf.metadata.refcounts_paddr);
    if (refs[leaf.index] == 1) leaf.table[leaf.index] &= page_mask;
}

fn removePage(domain: *const Domain, address: u64) void {
    const leaf = walk(domain, address, false) orelse return;
    const refs = refcounts(leaf.metadata.refcounts_paddr);
    if (refs[leaf.index] == 0) return;
    refs[leaf.index] -= 1;
    if (refs[leaf.index] == 0) leaf.table[leaf.index] = 0;
}

fn stopSourceBusMasters(domain: *const Domain) void {
    if (builtin.is_test) return;
    // IVRS aliases can make a bridge share the endpoint's DTE. If a drain
    // fails, quiesce every present PCI function using that source ID.
    for (0..256) |bus| {
        for (0..32) |device| {
            const first = pci.Location{ .bus = @intCast(bus), .device = @intCast(device), .function = 0 };
            if (pci.readVendorId(first) == 0xffff) continue;
            const count: usize = if ((pci.readHeaderType(first) & 0x80) != 0) 8 else 1;
            for (0..count) |function| {
                const location = pci.Location{ .bus = @intCast(bus), .device = @intCast(device), .function = @intCast(function) };
                if (pci.readVendorId(location) == 0xffff) continue;
                const id: u16 = @intCast((bus << 8) | (device << 3) | function);
                const resolved = ruleForPci(id) orelse continue;
                if (resolved.unit_index != domain.unit_index or resolved.rule.sourceFor(id).? != domain.source_id) continue;
                const command = pci.readConfigU16(location, 0x04);
                pci.writeConfigU16(location, 0x04, command & ~@as(u16, 4));
                _ = pci.readConfigU16(location, 0x04);
            }
        }
    }
}

fn quarantineDomain(domain: *Domain) void {
    if (domain.quarantined) return;
    domain.quarantined = true;
    // Quiesce the endpoint first. Withdrawal is then safe even if the command
    // queue is broken: failed invalidation retains all referenced pages.
    stopSourceBusMasters(domain);
    const root = pageAt(domain.root_paddr);
    for (root) |*entry| entry.* &= ~access_bits;
    domain.enabled = false;
    asm volatile ("mfence" ::: .{ .memory = true });
    _ = invalidateDomain(domain);
    for (state.leaves[0..state.leaf_count]) |*leaf| {
        if (leaf.domain_id == domain.did) @atomicStore(bool, &leaf.quarantined, true, .release);
    }
    @atomicStore(bool, &state.has_quarantine, true, .release);
    // A command failure cannot prove that cached DMA translations are gone.
    // Keep every referenced page pinned and stop new PCI bus-master traffic.
    if (!builtin.is_test) {
        log.writeFmt("amd-iommu: quarantined device=0x{x} source=0x{x} did={}\n", .{
            domain.device, domain.source_id, domain.did,
        });
    }
}

fn quarantineUnit(unit_index: usize) void {
    for (state.domains[0..state.domain_count]) |*domain| {
        if (domain.unit_index == unit_index) quarantineDomain(domain);
    }
}

fn quarantineEvent(unit_index: usize, code: u32, source: u16) void {
    if (code == 5 or code == 6) {
        quarantineUnit(unit_index);
    } else if (code != 0) {
        for (state.domains[0..state.domain_count]) |*domain| {
            if (domain.unit_index == unit_index and domain.source_id == source)
                quarantineDomain(domain);
        }
    }
}

fn pollEventsLocked() void {
    if (builtin.is_test) return;
    for (state.units[0..state.info.unit_count], 0..) |*unit, unit_index| {
        if (!unit.active) continue;
        const status = mmioRead64(unit, status_reg);
        if ((status & status_event_overflow) != 0) {
            log.writeFmt("amd-iommu: event overflow unit={}\n", .{unit_index});
            quarantineUnit(unit_index);
        }
        var head: u16 = @intCast(mmioRead64(unit, event_head_reg) & 0xffff);
        const tail: u16 = @intCast(mmioRead64(unit, event_tail_reg) & 0xffff);
        if ((head & 15) != 0 or (tail & 15) != 0 or head >= event_ring_bytes or tail >= event_ring_bytes) {
            quarantineUnit(unit_index);
            continue;
        }
        const events: *[256]Event = @ptrFromInt(unit.event_paddr);
        while (head != tail) {
            const event = events[head / 16];
            const code = event[1] >> 28;
            const source: u16 = @truncate(event[0]);
            log.writeFmt("amd-iommu: event unit={} code={} source=0x{x} word1=0x{x} address=0x{x}\n", .{
                unit_index, code, source, event[1], @as(u64, event[2]) | (@as(u64, event[3]) << 32),
            });
            quarantineEvent(unit_index, code, source);
            head = (head + 16) & (event_ring_bytes - 1);
            mmioWrite64(unit, event_head_reg, head);
        }
    }
}

fn submit(unit: *UnitState, command: Command) bool {
    if (builtin.is_test) return !test_fail_completion;
    const head: u16 = @intCast(mmioRead64(unit, command_head_reg) & 0xffff);
    const next = (unit.command_tail + 16) & (command_ring_bytes - 1);
    if (next == head) return false;
    const ring: *[256]Command = @ptrFromInt(unit.command_paddr);
    ring[unit.command_tail / 16] = command;
    asm volatile ("mfence" ::: .{ .memory = true });
    unit.command_tail = next;
    mmioWrite64(unit, command_tail_reg, next);
    return true;
}

fn completionWait(unit: *UnitState) bool {
    unit.completion_sequence +%= 1;
    if (unit.completion_sequence == 0) unit.completion_sequence = 1;
    const marker: *volatile u64 = @ptrFromInt(unit.completion_paddr);
    marker.* = 0;
    asm volatile ("mfence" ::: .{ .memory = true });
    const address = unit.completion_paddr;
    const sequence = unit.completion_sequence;
    if (!submit(unit, .{
        @truncate(address | 1),
        (@as(u32, 1) << 28) | @as(u32, @intCast((address >> 32) & 0xfffff)),
        @truncate(sequence),
        @truncate(sequence >> 32),
    })) return false;
    if (builtin.is_test and !test_fail_completion) marker.* = sequence;
    var spins: usize = 0;
    while (spins < poll_limit) : (spins += 1) {
        if (marker.* == sequence) return true;
        if (!builtin.is_test and (mmioRead64(unit, status_reg) & status_command_run) == 0) return false;
        asm volatile ("pause");
    }
    return false;
}

fn invalidateDomain(domain: *const Domain) bool {
    const unit = &state.units[domain.unit_index];
    const all: u64 = 0x7fff_ffff_ffff_f000;
    // S=1, PDE=1 and all-one address invalidates the entire domain, including
    // cached directory links. The following wait drains in-flight DMA writes.
    if (!submit(unit, .{ 0, (@as(u32, 3) << 28) | domain.did, @truncate(all | 3), @truncate(all >> 32) }))
        return false;
    return completionWait(unit);
}

fn invalidateDevice(domain: *const Domain) bool {
    const unit = &state.units[domain.unit_index];
    if (!submit(unit, .{ domain.source_id, @as(u32, 2) << 28, 0, 0 })) return false;
    if (!invalidateDomain(domain)) return false;
    return true;
}

fn withdrawRange(domain: *Domain, first: u64, page_count: usize) bool {
    if (domain.quarantined) return false;
    if (page_count == 0) return true;
    for (0..page_count) |index| {
        withdrawPage(domain, first + @as(u64, @intCast(index)) * page_size);
    }
    asm volatile ("mfence" ::: .{ .memory = true });
    if (!invalidateDomain(domain)) {
        quarantineDomain(domain);
        return false;
    }
    for (0..page_count) |index| {
        removePage(domain, first + @as(u64, @intCast(index)) * page_size);
    }
    return true;
}

pub fn physicalRangeQuarantined(paddr: u64, page_count: usize) bool {
    if (!@atomicLoad(bool, &state.has_quarantine, .acquire) or page_count == 0) return false;
    const bytes, const overflow = @mulWithOverflow(@as(u64, @intCast(page_count)), page_size);
    const end, const end_overflow = @addWithOverflow(paddr, bytes);
    if (overflow != 0 or end_overflow != 0) return true;
    for (&state.leaves) |*leaf| {
        if (!@atomicLoad(bool, &leaf.quarantined, .acquire)) continue;
        const entries = pageAt(leaf.table_paddr);
        const refs = refcounts(leaf.refcounts_paddr);
        for (entries, 0..) |entry, index| {
            if (refs[index] == 0) continue;
            const page = pageAddress(entry);
            if (page < end and page + page_size > paddr) return true;
        }
    }
    return false;
}

pub fn deviceQuarantined(device: types.DmaDeviceId) bool {
    if (!state.active) return false;
    lock();
    defer unlock();
    pollEventsLocked();
    const domain = domainForDevice(device) orelse return false;
    return domain.quarantined;
}

const ResolvedRule = struct { unit_index: u8, rule: ivrs.Rule };

fn ruleForPci(device_id: u16) ?ResolvedRule {
    var found: ?ResolvedRule = null;
    for (state.info.units[0..state.info.unit_count], 0..) |*unit, index| {
        if (unit.segment != 0) continue;
        const rule = unit.ruleFor(device_id) orelse continue;
        if (found != null) return null;
        found = .{ .unit_index = @intCast(index), .rule = rule };
    }
    return found;
}

fn platformOnlyPciFunction(info: *const ivrs.Info, device_id: u16, class_code: u8) bool {
    // Bridges are never exposed as DMA capabilities, and the IVHD's own PCI
    // function is the IOMMU, not a peripheral behind its translation unit.
    // Neither needs a device rule; check this before fail-closed lookup so a
    // firmware range may legitimately begin after the root-complex BDFs.
    if (class_code == 0x06) return true;
    for (info.units[0..info.unit_count]) |unit| {
        if (unit.segment == 0 and unit.device_id == device_id) return true;
    }
    return false;
}

test "AMD IOMMU skips only topology bridges and its own PCI function" {
    const info = try std.testing.allocator.create(ivrs.Info);
    defer std.testing.allocator.destroy(info);
    info.* = .{};
    info.unit_count = 1;
    info.units[0].device_id = 0x0002;
    info.units[0].segment = 0;
    try std.testing.expect(platformOnlyPciFunction(info, 0x0000, 0x06));
    try std.testing.expect(platformOnlyPciFunction(info, 0x0002, 0x08));
    try std.testing.expect(!platformOnlyPciFunction(info, 0x0001, 0x08));
    try std.testing.expect(!platformOnlyPciFunction(info, 0x0008, 0x01));
    info.units[0].segment = 1;
    try std.testing.expect(!platformOnlyPciFunction(info, 0x0002, 0x08));
}

fn discoverCandidates() bool {
    var bus: usize = 0;
    while (bus < 256) : (bus += 1) {
        var dev: usize = 0;
        while (dev < 32) : (dev += 1) {
            const first = pci.Location{ .bus = @intCast(bus), .device = @intCast(dev), .function = 0 };
            if (pci.readVendorId(first) == 0xffff) continue;
            const count: usize = if ((pci.readHeaderType(first) & 0x80) != 0) 8 else 1;
            for (0..count) |func| {
                const loc = pci.Location{ .bus = @intCast(bus), .device = @intCast(dev), .function = @intCast(func) };
                if (pci.readVendorId(loc) == 0xffff) continue;
                const id: u16 = @intCast((bus << 8) | (dev << 3) | func);
                if (platformOnlyPciFunction(&state.info, id, pci.readClassCode(loc))) continue;
                const resolved = ruleForPci(id) orelse {
                    log.writeFmt("amd-iommu: IVRS missing or overlapping bdf={}:{}:{}\n", .{ bus, dev, func });
                    return false;
                };
                if (state.candidate_count == max_domains) return false;
                state.candidates[state.candidate_count] = .{
                    .device = pci.resourceIdFromLocation(loc),
                    .source_id = resolved.rule.sourceFor(id).?,
                    .unit_index = resolved.unit_index,
                    .settings = resolved.rule.settings,
                };
                state.candidate_count += 1;
            }
        }
    }
    return true;
}

fn sourceIsExclusive(candidate: Candidate) bool {
    var count: usize = 0;
    for (state.candidates[0..state.candidate_count]) |other| {
        if (other.unit_index == candidate.unit_index and other.source_id == candidate.source_id) count += 1;
    }
    return count == 1;
}

fn translatedPreparedDte(definition: *const ivrs.Unit, source: u16,
    current: Dte, root: u64, did: u16) ?Dte
{
    const initial = initialDteWords(definition, source);
    // A SysMgt=1 requester starts with IW set for AMD Erratum 63. Compare
    // against the IVRS-derived initial state, not a hard-coded bare V bit;
    // still reject any unexpected or already-translated DTE contents.
    if (current[0] != initial[0] or current[1] != initial[1] or
        current[2] != 0 or current[3] != 0 or root == 0 or
        (root & ~page_mask) != 0 or did == 0) return null;
    return .{
        root | (@as(u64, 4) << 9) | 3 | read_bit | write_bit,
        @as(u64, did) | initial[1],
        0,
        0,
    };
}

fn installDomains() bool {
    for (state.candidates[0..state.candidate_count]) |candidate| {
        if (!sourceIsExclusive(candidate)) {
            log.writeFmt("amd-iommu: shared source denied device=0x{x} source=0x{x}\n", .{ candidate.device, candidate.source_id });
            continue;
        }
        if (state.domain_count == max_domains) {
            log.write("amd-iommu: domain capacity exhausted\n");
            return false;
        }
        const root = allocZeroPage() orelse {
            log.writeFmt("amd-iommu: domain root allocation failed source=0x{x}\n", .{candidate.source_id});
            return false;
        };
        const did: u16 = @intCast(state.domain_count + 1);
        const unit = &state.units[candidate.unit_index];
        const source = candidate.source_id;
        if (@as(usize, source) >= unit.device_table_pages * 128) {
            log.writeFmt("amd-iommu: source outside DTE table source=0x{x}\n", .{source});
            return false;
        }
        const dte = deviceTableAt(unit, source);
        dte.* = translatedPreparedDte(&state.info.units[candidate.unit_index],
            source, dte.*, root, did) orelse {
            log.writeFmt("amd-iommu: prepared DTE mismatch source=0x{x} word0=0x{x} word1=0x{x}\n",
                .{ source, dte[0], dte[1] });
            return false;
        };
        state.domains[state.domain_count] = .{
            .device = candidate.device,
            .source_id = source,
            .did = did,
            .unit_index = candidate.unit_index,
            .root_paddr = root,
        };
        state.domain_count += 1;
    }
    return true;
}

fn freeListOverlaps(base: u64, end: u64) bool {
    const list = state.free_list orelse return true;
    for (list.ranges[0..list.range_len]) |range| {
        const first = range.physical_start;
        const last = first + @as(u64, @intCast(range.len)) * page_size;
        if (first < end and base < last) return true;
    }
    return false;
}

fn validateMemoryDefinitions() bool {
    for (state.info.memory_ranges[0..state.info.memory_range_count]) |memory| {
        // ExclusionRange bypasses translation and access checks; it cannot be
        // implemented as a normal identity PTE without granting wrong access.
        if ((memory.flags & 0xf8) != 0) return false;
        if (!memory.all_segments) {
            var found = false;
            for (state.info.units[0..state.info.unit_count]) |unit| {
                if (unit.segment == memory.segment) found = true;
            }
            if (!found) return false;
        }
        if ((memory.flags & 0x07) == 0x01) return false;
        const end = memory.base + memory.length;
        if (end > (@as(u64, 1) << 48)) return false;
        const first_page = memory.base & ~(page_size - 1);
        const last_page = std.mem.alignForward(u64, end, page_size);
        const readable = (memory.flags & 0x02) != 0;
        const writable = (memory.flags & 0x04) != 0;
        // Check before allocating IOMMU metadata: otherwise a conflicting
        // free page could disappear from the free list and escape detection.
        if ((readable or writable) and freeListOverlaps(first_page, last_page)) return false;
    }
    return true;
}

fn installMemoryDefinitions() bool {
    for (state.info.memory_ranges[0..state.info.memory_range_count]) |memory| {
        if ((memory.flags & 0xf8) != 0) return false;
        const end = memory.base + memory.length;
        const first_page = memory.base & ~(page_size - 1);
        if (end > (@as(u64, 1) << 48)) return false;
        const last_page = std.mem.alignForward(u64, end, page_size);
        state.iova_end = iova.exclude(iova.window_start, state.iova_end, first_page, last_page - 1);
        const readable = (memory.flags & 0x02) != 0;
        const writable = (memory.flags & 0x04) != 0;
        if (!readable and !writable) continue;
        for (state.domains[0..state.domain_count]) |*domain| {
            if (!memory.all_segments and state.info.units[domain.unit_index].segment != memory.segment) continue;
            if (domain.source_id < memory.first_device or domain.source_id > memory.last_device) continue;
            var page = first_page;
            while (page < last_page) : (page += page_size) {
                if (!mapPage(domain, page, page, readable, writable)) return false;
            }
        }
    }
    return true;
}

fn initialDteWords(definition: *const ivrs.Unit, source: u16) [2]u64 {
    const settings = definition.settingsForSource(source);
    const sysmgt: u64 = (settings >> 4) & 3;
    // AMD Erratum 63 requires IW when IVRS requests SysMgt=1, including
    // special-only requester IDs that will not receive a DMA domain later.
    return .{ 1 | (if (sysmgt == 1) write_bit else 0), sysmgt << 40 };
}

fn prepareUnit(index: usize) bool {
    const definition = &state.info.units[index];
    const unit = &state.units[index];
    const mmio_status = smp.explicitUcIdentityMmioStatus(definition.mmio_base, unit_register_bytes);
    if (mmio_status != .ready) {
        const evidence = smp.mmioCacheEvidenceMasks();
        log.writeFmt("amd-iommu: unit={} MMIO UC proof failed reason={s} base=0x{x} verified=0x{x} matching=0x{x} online=0x{x}\n",
            .{ index, @tagName(mmio_status), definition.mmio_base, evidence.verified, evidence.matching, evidence.online });
        return false;
    }
    unit.register_base = definition.mmio_base;
    if ((mmioRead64(unit, control_reg) & ctrl_iommu_en) != 0) {
        log.writeFmt("amd-iommu: unit={} already enabled\n", .{index});
        return false;
    }

    var largest: u16 = if (definition.default_settings != null) 0xffff else 0;
    for (definition.rules[0..definition.rule_count]) |rule| {
        largest = @max(largest, rule.last);
        if (rule.alias) largest = @max(largest, rule.source);
    }
    for (definition.special_sources[0..definition.special_count]) |special| largest = @max(largest, special.source);
    const pages = (@as(usize, largest) / 128) + 1;
    const list = state.free_list orelse return false;
    const base = list.popContiguousBelow(pages, four_gib) catch return false;
    unit.device_table_paddr = base;
    unit.device_table_pages = pages;
    for (0..pages) |page_index| {
        const paddr = base + @as(u64, @intCast(page_index)) * page_size;
        if (builtin.is_test) {
            @memset(pageAt(paddr)[0..], 0);
        } else if (!user_copy.zeroPhysicalPage(paddr)) return false;
    }
    // DTE[V]=0 passes ordinary DMA through. Fill even unused slots with
    // V=1, TV=0 so they fault rather than bypass the isolation boundary.
    // Resolve source settings once per DTE, independent of IVHD entry order.
    for (0..pages * 128) |source| {
        const dte = deviceTableAt(unit, @intCast(source));
        const initial = initialDteWords(definition, @intCast(source));
        dte[0] = initial[0];
        dte[1] = initial[1];
    }
    unit.command_paddr = allocZeroPage() orelse return false;
    unit.event_paddr = allocZeroPage() orelse return false;
    unit.completion_paddr = allocZeroPage() orelse return false;
    return true;
}

fn waitRunning(unit: *const UnitState) bool {
    var spin: usize = 0;
    while (spin < poll_limit) : (spin += 1) {
        const status = mmioRead64(unit, status_reg);
        if ((status & (status_command_run | status_event_run)) == (status_command_run | status_event_run)) return true;
        asm volatile ("pause");
    }
    return false;
}

fn enableUnit(index: usize) bool {
    const unit = &state.units[index];
    mmioWrite64(unit, devtab_base_reg, unit.device_table_paddr | @as(u64, @intCast(unit.device_table_pages - 1)));
    mmioWrite64(unit, command_base_reg, unit.command_paddr | (@as(u64, 8) << 56));
    mmioWrite64(unit, event_base_reg, unit.event_paddr | (@as(u64, 8) << 56));
    mmioWrite64(unit, command_head_reg, 0);
    mmioWrite64(unit, command_tail_reg, 0);
    mmioWrite64(unit, event_head_reg, 0);
    mmioWrite64(unit, event_tail_reg, 0);
    if ((mmioRead64(unit, devtab_base_reg) & page_mask) != unit.device_table_paddr or
        (mmioRead64(unit, command_base_reg) & page_mask) != unit.command_paddr or
        (mmioRead64(unit, event_base_reg) & page_mask) != unit.event_paddr) return false;
    asm volatile ("mfence" ::: .{ .memory = true });
    const flags = state.info.units[index].flags;
    const recommended =
        (if ((flags & 0x02) != 0) @as(u64, 1) << 8 else 0) |
        (if ((flags & 0x04) != 0) @as(u64, 1) << 9 else 0) |
        (if ((flags & 0x08) != 0) @as(u64, 1) << 11 else 0) |
        (if (state.info.units[index].block_type != 0x40 and (flags & 0x01) != 0) @as(u64, 1) << 1 else 0);
    mmioWrite64(unit, control_reg, ctrl_iommu_en | ctrl_command_en | ctrl_event_en | ctrl_coherent | recommended);
    if ((mmioRead64(unit, control_reg) & ctrl_coherent) == 0) return false;
    if (!waitRunning(unit)) return false;
    unit.active = true;
    return true;
}

pub fn init(rsdp_paddr: u64, free_list: *types.FreePageList, comptime checkpoint: fn (u8) void) bool {
    if (state.active or state.has_quarantine) return state.active;
    const table = ivrs.find(rsdp_paddr) orelse return false;
    state = .{};
    state.free_list = free_list;
    ivrs.parse(table, &state.info) catch |err| {
        checkpoint('N');
        log.writeFmt("amd-iommu: IVRS rejected error={s}\n", .{@errorName(err)});
        return false;
    };
    checkpoint('J');
    if (!discoverCandidates()) {
        log.write("amd-iommu: PCI source discovery failed\n");
        return false;
    }
    if (!validateMemoryDefinitions()) {
        log.write("amd-iommu: IVMD overlaps free RAM or is unsupported\n");
        return false;
    }
    checkpoint('K');
    for (0..state.info.unit_count) |index| {
        if (!prepareUnit(index)) {
            log.writeFmt("amd-iommu: unit preparation failed index={}\n", .{index});
            return false;
        }
    }
    if (!installDomains() or !installMemoryDefinitions()) {
        log.write("amd-iommu: domain or IVMD construction failed\n");
        return false;
    }
    state.iova_end = pci.bootDmaWindowEnd(iova.window_start, state.iova_end);
    if (state.iova_end <= iova.window_start) {
        log.write("amd-iommu: no IOVA aperture\n");
        return false;
    }
    checkpoint('L');
    for (0..state.info.unit_count) |index| {
        if (!enableUnit(index)) {
            // A previous unit may already be translating. Returning to boot
            // would expose unprotected devices behind a different unit.
            @import("halt.zig").haltWithMessage("AMD IOMMU partial enable failed");
        }
    }
    for (state.domains[0..state.domain_count]) |*domain| {
        if (!invalidateDevice(domain)) {
            @import("halt.zig").haltWithMessage("AMD IOMMU initial invalidation failed");
        }
    }
    @atomicStore(bool, &state.active, true, .release);
    log.writeFmt("amd-iommu: translated units={} domains={} iova_end=0x{x}\n", .{
        state.info.unit_count, state.domain_count, state.iova_end,
    });
    return true;
}

pub fn isActive() bool {
    return @atomicLoad(bool, &state.active, .acquire);
}

fn validIovaRange(address: u64, size: u64) bool {
    if (size == 0 or address < iova.window_start or address >= state.iova_end) return false;
    const end, const overflow = @addWithOverflow(address, size);
    return overflow == 0 and end <= state.iova_end;
}

pub fn deviceIovaWindow(device: types.DmaDeviceId) struct { start: u64 = 0, size: u64 = 0 } {
    if (!isActive() or domainForDevice(device) == null) return .{};
    return .{ .start = iova.window_start, .size = state.iova_end - iova.window_start };
}

pub fn allocIova(device: types.DmaDeviceId, page_count: usize) ?u64 {
    if (!isActive()) return null;
    lock();
    defer unlock();
    pollEventsLocked();
    const domain = domainForDevice(device) orelse return null;
    if (domain.quarantined or !ensureAllocator(domain)) return null;
    return domain.allocator.alloc(page_count);
}

pub fn reserveIova(device: types.DmaDeviceId, address: u64, page_count: usize) bool {
    if (!isActive()) return false;
    lock();
    defer unlock();
    pollEventsLocked();
    const domain = domainForDevice(device) orelse return false;
    if (domain.quarantined or !ensureAllocator(domain)) return false;
    return domain.allocator.reserve(address, page_count);
}

pub fn freeIova(device: types.DmaDeviceId, address: u64, page_count: usize) void {
    if (!isActive()) return;
    lock();
    defer unlock();
    pollEventsLocked();
    const domain = domainForDevice(device) orelse return;
    if (domain.quarantined) return;
    _ = domain.allocator.free(address, page_count);
}

pub fn mapPages(
    device: types.DmaDeviceId,
    address: u64,
    paddrs: []const u64,
    readable: bool,
    writable: bool,
) bool {
    if (!isActive() or paddrs.len == 0 or (!readable and !writable) or (address & (page_size - 1)) != 0)
        return false;
    const size, const overflow = @mulWithOverflow(@as(u64, @intCast(paddrs.len)), page_size);
    if (overflow != 0 or !validIovaRange(address, size)) return false;
    for (paddrs) |paddr| {
        if ((paddr & (page_size - 1)) != 0 or (paddr & ~page_mask) != 0) return false;
    }
    lock();
    defer unlock();
    pollEventsLocked();
    const domain = domainForDevice(device) orelse return false;
    if (domain.quarantined) return false;
    var mapped: usize = 0;
    while (mapped < paddrs.len) : (mapped += 1) {
        if (!mapPage(domain, address + @as(u64, @intCast(mapped)) * page_size, paddrs[mapped], readable, writable)) {
            _ = withdrawRange(domain, address, mapped);
            return false;
        }
    }
    asm volatile ("mfence" ::: .{ .memory = true });
    if (!invalidateDomain(domain)) {
        _ = withdrawRange(domain, address, mapped);
        return false;
    }
    return true;
}

pub fn unmapRangeForDevice(device: types.DmaDeviceId, address: u64, size: u64) bool {
    if (!isActive() or !validIovaRange(address, size)) return false;
    const first = address & ~(page_size - 1);
    const last = (address + size - 1) & ~(page_size - 1);
    const pages: usize = @intCast((last - first) / page_size + 1);
    lock();
    defer unlock();
    pollEventsLocked();
    const domain = domainForDevice(device) orelse return false;
    return withdrawRange(domain, first, pages);
}

pub fn setDeviceDmaEnabled(device: types.DmaDeviceId, enabled: bool) error{
    Unsupported,
    NoDevice,
    Quarantined,
    DrainFailed,
}!void {
    if (!isActive()) return error.Unsupported;
    lock();
    defer unlock();
    pollEventsLocked();
    const domain = domainForDevice(device) orelse return error.NoDevice;
    if (domain.quarantined) return error.Quarantined;
    const root = pageAt(domain.root_paddr);
    for (root) |*entry| {
        if (pageAddress(entry.*) == 0) continue;
        if (enabled) entry.* |= access_bits else entry.* &= ~access_bits;
    }
    domain.enabled = enabled;
    asm volatile ("mfence" ::: .{ .memory = true });
    if (invalidateDomain(domain)) return;
    // An unacknowledged permission change cannot release any backing page.
    for (root) |*entry| entry.* &= ~access_bits;
    domain.enabled = false;
    quarantineDomain(domain);
    return error.DrainFailed;
}

pub fn dmaSnapshotFlags(device: types.DmaDeviceId) u64 {
    if (!isActive()) return 0;
    lock();
    defer unlock();
    pollEventsLocked();
    const domain = domainForDevice(device) orelse return 0;
    const abi = @import("kernel_abi_root").capsule_abi;
    return if (domain.quarantined) abi.snapshot_flag_dma_quarantined else abi.snapshot_flag_dma_translated;
}

pub const TestSupport = if (builtin.is_test) struct {
    pub fn begin(free_list: *types.FreePageList, device: types.DmaDeviceId) !void {
        state = .{ .free_list = free_list };
        test_fail_completion = false;
        state.units[0] = .{
            .command_paddr = allocZeroPage() orelse return error.OutOfMemory,
            .completion_paddr = allocZeroPage() orelse return error.OutOfMemory,
            .active = true,
        };
        state.info.unit_count = 1;
        state.domains[0] = .{
            .device = device,
            .source_id = 0x10,
            .did = 1,
            .root_paddr = allocZeroPage() orelse return error.OutOfMemory,
        };
        state.domain_count = 1;
        state.active = true;
    }

    pub fn end() void {
        state = .{};
        test_fail_completion = false;
    }

    pub fn failCompletion(fail: bool) void {
        test_fail_completion = fail;
    }
} else struct {};

test "AMD IVRS DTE settings ignore entry order and apply SysMgt erratum" {
    var definition = ivrs.Unit{};
    const alias = ivrs.Rule{ .first = 0x0010, .last = 0x001f, .source = 0x8000, .settings = 0x10, .alias = true };
    const ordinary = ivrs.Rule{ .first = 0x8000, .last = 0x80ff, .settings = 0x10 };
    definition.rules[0] = alias;
    definition.rules[1] = ordinary;
    definition.rule_count = 2;
    definition.special_sources[0] = .{ .source = 0x00a0, .settings = 0xd7 };
    definition.special_count = 1;
    const before = initialDteWords(&definition, 0x8000);
    definition.rules[0] = ordinary;
    definition.rules[1] = alias;
    try std.testing.expectEqualDeep(before, initialDteWords(&definition, 0x8000));
    try std.testing.expectEqual(@as(u64, 1 | write_bit), before[0]);
    try std.testing.expectEqual(@as(u64, 1) << 40, before[1]);
    const special = initialDteWords(&definition, 0x00a0);
    try std.testing.expectEqual(@as(u64, 1 | write_bit), special[0]);
    try std.testing.expectEqual(@as(u64, 1) << 40, special[1]);
    try std.testing.expectEqual(@as(u64, 1), initialDteWords(&definition, 0x0050)[0]);
}

test "AMD IOMMU domain accepts IVRS SysMgt IW without dropping it" {
    var definition = ivrs.Unit{};
    definition.special_sources[0] = .{ .source = 0x00a0, .settings = 0xd7 };
    definition.special_count = 1;
    const initial = initialDteWords(&definition, 0x00a0);
    const prepared = Dte{ initial[0], initial[1], 0, 0 };
    const translated = translatedPreparedDte(&definition, 0x00a0,
        prepared, 0x0040_0000, 7) orelse return error.ExpectedTranslatedDte;
    try std.testing.expectEqual(@as(u64, 0x0040_0000 | (@as(u64, 4) << 9) | 3 | read_bit | write_bit), translated[0]);
    try std.testing.expectEqual(@as(u64, 7 | (@as(u64, 1) << 40)), translated[1]);
    try std.testing.expect(translatedPreparedDte(&definition, 0x00a0,
        .{ 1, initial[1], 0, 0 }, 0x0040_0000, 7) == null);
    try std.testing.expect(translatedPreparedDte(&definition, 0x00a0,
        .{ initial[0], initial[1], 1, 0 }, 0x0040_0000, 7) == null);
    try std.testing.expect(translatedPreparedDte(&definition, 0x00a0,
        prepared, 0x0040_0001, 7) == null);
}

test "AMD IOMMU DTE puts DMA permissions in first word" {
    const allocator = std.testing.allocator;
    const backing = try allocator.alignedAlloc(u8, .fromByteUnits(4096), 8 * 4096);
    defer allocator.free(backing);
    const free_list = try allocator.create(types.FreePageList);
    defer allocator.destroy(free_list);
    free_list.* = .{};
    try free_list.appendContiguousRange(0, @intFromPtr(backing.ptr), 8);
    defer TestSupport.end();
    state = .{ .free_list = free_list };
    state.units[0].device_table_paddr = allocZeroPage() orelse return error.OutOfMemory;
    state.units[0].device_table_pages = 1;
    state.info.unit_count = 1;
    state.info.units[0].special_sources[0] = .{ .source = 2, .settings = 0x20 };
    state.info.units[0].special_count = 1;
    const dte = deviceTableAt(&state.units[0], 2);
    const initial = initialDteWords(&state.info.units[0], 2);
    dte.* = .{ initial[0], initial[1], 0, 0 };
    state.candidates[0] = .{ .device = 42, .source_id = 2 };
    state.candidate_count = 1;
    try std.testing.expect(installDomains());
    try std.testing.expectEqual(@as(u64, read_bit | write_bit), dte[0] & (read_bit | write_bit));
    try std.testing.expectEqual(@as(u64, 1 | (@as(u64, 2) << 40)), dte[1]);
}

test "AMD IOMMU map unmap and failed drain pin pages" {
    const allocator = std.testing.allocator;
    const backing = try allocator.alignedAlloc(u8, .fromByteUnits(4096), 64 * 4096);
    defer allocator.free(backing);
    const free_list = try allocator.create(types.FreePageList);
    defer allocator.destroy(free_list);
    free_list.* = .{};
    try free_list.appendContiguousRange(0, @intFromPtr(backing.ptr), 64);
    const device = pci.resourceIdFromLocation(.{ .bus = 0, .device = 2, .function = 0 });
    try TestSupport.begin(free_list, device);
    defer TestSupport.end();
    const page = try free_list.popFront();
    const address = iova.window_start;
    try std.testing.expect(reserveIova(device, address, 1));
    try std.testing.expect(mapPages(device, address, &.{page}, true, false));
    try std.testing.expectEqual(page, pageAddress(walk(&state.domains[0], address, false).?.table[0]));
    try std.testing.expect(unmapRangeForDevice(device, address, page_size));
    try std.testing.expectEqual(@as(u64, 0), walk(&state.domains[0], address, false).?.table[0]);
    try std.testing.expect(mapPages(device, address, &.{page}, true, true));
    const root_entry = &pageAt(state.domains[0].root_paddr)[@as(usize, @intCast((address >> 39) & 511))];
    try setDeviceDmaEnabled(device, false);
    try std.testing.expectEqual(@as(u64, 3), (root_entry.* >> 9) & 7);
    try std.testing.expectEqual(@as(u64, 0), root_entry.* & access_bits);
    try setDeviceDmaEnabled(device, true);
    try std.testing.expectEqual(@as(u64, 3), (root_entry.* >> 9) & 7);
    try std.testing.expectEqual(access_bits, root_entry.* & access_bits);
    TestSupport.failCompletion(true);
    try std.testing.expect(!unmapRangeForDevice(device, address, page_size));
    try std.testing.expect(deviceQuarantined(device));
    try std.testing.expect(physicalRangeQuarantined(page, 1));
    try std.testing.expectError(types.KernelError.InvalidState, free_list.appendPage(0, page));
    try std.testing.expect(!mapPages(device, address + page_size, &.{page}, true, true));
}

test "AMD IOMMU page fault quarantines only its source" {
    const allocator = std.testing.allocator;
    const backing = try allocator.alignedAlloc(u8, .fromByteUnits(4096), 32 * 4096);
    defer allocator.free(backing);
    const free_list = try allocator.create(types.FreePageList);
    defer allocator.destroy(free_list);
    free_list.* = .{};
    try free_list.appendContiguousRange(0, @intFromPtr(backing.ptr), 32);
    try TestSupport.begin(free_list, 42);
    defer TestSupport.end();
    state.domains[1] = .{
        .device = 43,
        .source_id = 0x18,
        .did = 2,
        .root_paddr = allocZeroPage() orelse return error.OutOfMemory,
    };
    state.domain_count = 2;
    const first = try free_list.popFront();
    const second = try free_list.popFront();
    try std.testing.expect(mapPages(42, iova.window_start, &.{first}, true, true));
    try std.testing.expect(mapPages(43, iova.window_start, &.{second}, true, true));
    quarantineEvent(0, 2, 0x10);
    try std.testing.expect(deviceQuarantined(42));
    try std.testing.expect(!deviceQuarantined(43));
    try std.testing.expect(physicalRangeQuarantined(first, 1));
    try std.testing.expect(!physicalRangeQuarantined(second, 1));
}
