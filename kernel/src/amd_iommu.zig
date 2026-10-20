const IovaAddress = @import("memory/addresses.zig").IovaAddress;
const mmio = @import("memory/mmio.zig");
const lock_diagnostics = @import("sync/lock_diagnostics.zig");
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
const four_gib: u64 = @import("arch/x86_64/physical_layout.zig").low_memory_limit;
const page_mask: u64 = 0x000f_ffff_ffff_f000;
const read_bit: u64 = @as(u64, 1) << 61;
const write_bit: u64 = @as(u64, 1) << 62;
const present_bit: u64 = 1;
const access_bits: u64 = present_bit | read_bit | write_bit;
const slot_table = @import("memory/slot_table.zig");
// DID zero is reserved; the remaining u16 values are hardware identifiers.
const max_domain_count: usize = std.math.maxInt(u16);
// Hash bucket count controls distribution, not the number of retained leaves.
// Collisions use stable SlotTable bodies rather than finite open-address slots.
const leaf_index_buckets: usize = 8192;
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

const storage = @import("memory/owned_storage.zig");
const UnitState = struct {
    next: ?*UnitState = null,
    definition: ivrs.Unit = .{},
    ordinal: usize = 0,
    register_base: u64 = 0,
    registers: mmio.Borrow = undefined,
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
    unit: *UnitState = undefined,
    root_paddr: u64 = 0,
    allocator: iova.Allocator = .{},
    quarantined: bool = false,
    enabled: bool = true,
};

const Leaf = struct {
    next: ?*Leaf = null,
    hash_next: ?*Leaf = null,
    table_paddr: u64 = 0,
    refcounts_paddr: u64 = 0,
    domain_id: u16 = 0,
    quarantined: bool = false,
};

const Candidate = struct {
    device: types.DmaDeviceId = types.invalid_dma_device_id,
    source_id: u16 = 0,
    unit: *UnitState = undefined,
    settings: u8 = 0,
};

// Domain bodies are hardware owners: keep them stable until all DMA is drained.
// Runtime lookup and growth share the driver lock; no directory loan escapes it.
fn poolAllocator() @import("memory/owned_storage.zig").Allocator {
    if (builtin.is_test) return TestSupport.pool_allocator_override orelse slot_table.hostTestAllocator();
    return @import("memory/identity_allocator.zig").allocator(state.free_list.?);
}
fn poolInitializationStarted() bool {
    return state.domains.directory_owner.bound_address != null or state.candidates.directory_owner.bound_address != null or state.leaves.directory_owner.bound_address != null;
}
fn prepareDomain(index: usize) ?*Domain {
    if (index >= max_domain_count) return null;
    state.domains.prepareIndex(@intCast(index), poolAllocator()) catch return null;
    return state.domains.claimIndex(@intCast(index)) catch return null;
}
fn domainAt(index: usize) *Domain {
    return state.domains.get(@intCast(index)) orelse unreachable;
}

const DriverState = struct {
    info: ivrs.Info = .{},
    free_list: ?*types.FreePageList = null,
    units: ?*UnitState = null,
    domains: slot_table.SlotTable(Domain) = .{},
    candidates: slot_table.SlotTable(Candidate) = .{},
    candidate_count: usize = 0,
    domain_count: usize = 0,
    leaves: slot_table.SlotTable(Leaf) = .{},
    leaf_head: ?*Leaf = null,
    leaf_index: [leaf_index_buckets]?*Leaf = .{null} ** leaf_index_buckets,
    leaf_count: usize = 0,
    iova_end: u64 = iova.window_ceiling,
    iova_spares: ?*iova.Bank = null,
    // A quarantine rejection is permanent, unlike PMM descriptor exhaustion.
    iova_quarantined: ?*iova.Bank = null,
    has_quarantine: bool = false,
    active: bool = false,
    lock_word: lock_diagnostics.Byte = .{},
};

// Separate from resettable driver scalars: failed PMM returns must survive
// initialization failure/retry. Once active, unit references remain stable for
// domains, command completion and quarantine until hardware has been drained.
var unit_owner: storage.Owner = .{};
const units_per_page = blk: {
    @setEvalBranchQuota(10000);
    var n: usize = 1;
    while ((storage.requiredPages(UnitState, n + 1) catch unreachable) == 1) : (n += 1) {}
    break :blk n;
};
fn stageUnits(tx: *storage.Transaction, info: ivrs.Info) !?*UnitState {
    var head: ?*UnitState = null;
    var link = &head;
    var definitions = info.units();
    var index: usize = 0;
    while (index < info.unit_count) {
        const count = @min(units_per_page, info.unit_count - index);
        const nodes = try tx.reserve(UnitState, count, .replaceable);
        for (nodes) |*node| {
            node.* = .{ .definition = definitions.next().?, .ordinal = index };
            link.* = node;
            link = &node.next;
            index += 1;
        }
    }
    return head;
}
var state: DriverState = .{};
var test_fail_completion: bool = false;

const unit_register_bytes: u64 = 0x3000;

fn bootUnitMmioRangeSupported(snapshot: *const mtrr.CacheSnapshot, base: u64) bool {
    return snapshot.rangeSupportsUncachedMapping(base, unit_register_bytes);
}

/// Validate IVRS with the production parser and reserve only its register
/// pages in the high UC window before AP startup. Device ownership never
/// authorizes a WB RAM alias or treating its physical address as a pointer.
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
    var units = state.info.units();
    var index: usize = 0;
    while (units.next()) |unit| : (index += 1) {
        if (!bootUnitMmioRangeSupported(&snapshot, unit.mmio_base) or
            !x86_platform.mapBootMmioRange(unit.mmio_base, unit_register_bytes))
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
    try std.testing.expect(!bootUnitMmioRangeSupported(&snapshot, (@as(u64, 1) << 48) - 0x2000));
    try std.testing.expect(!bootUnitMmioRangeSupported(&snapshot, (@as(u64, 1) << 52)));
}

pub fn kernelStaticStorageStartAddr() usize {
    return @min(@intFromPtr(&state), @intFromPtr(&unit_owner));
}

pub fn kernelStaticStorageEndAddr() usize {
    return @max(@intFromPtr(&state) + @sizeOf(DriverState), @intFromPtr(&unit_owner) + @sizeOf(@TypeOf(unit_owner)));
}

fn lock() void {
    while (true) {
        const diagnostic_if = lock_diagnostics.maskInterrupts();
        lock_diagnostics.beforeAcquire(&state.lock_word.record, .{ .class = .amd_iommu });
        if (@cmpxchgWeak(u8, &state.lock_word.value, 0, 1, .acquire, .monotonic) == null) {
            lock_diagnostics.acquired(&state.lock_word.record, .{ .class = .amd_iommu });
            lock_diagnostics.restoreInterrupts(diagnostic_if);
            return;
        }
        lock_diagnostics.restoreInterrupts(diagnostic_if);
        while (@atomicLoad(u8, &state.lock_word.value, .monotonic) != 0) asm volatile ("pause");
    }
}

fn unlock() void {
    const diagnostic_if = lock_diagnostics.maskInterrupts();
    defer lock_diagnostics.restoreInterrupts(diagnostic_if);
    lock_diagnostics.releasing(&state.lock_word.record);
    @atomicStore(u8, &state.lock_word.value, 0, .release);
}

fn mmioRead64(unit: *const UnitState, offset: u64) u64 {
    return unit.registers.read(u64, offset);
}

fn mmioWrite64(unit: *const UnitState, offset: u64, value: u64) void {
    unit.registers.write(u64, offset, value);
}

inline fn pageAt(paddr: u64) *Page {
    return borrowRamObject(Page, .fromInt(paddr)) orelse unreachable;
}

fn deviceTableAt(unit: *const UnitState, source: u16) *Dte {
    const address = physical_layout.PhysicalAddress.fromInt(unit.device_table_paddr).offset(@as(u64, source) * @sizeOf(Dte)) orelse unreachable;
    return borrowRamObject(Dte, address) orelse unreachable;
}

fn pageAddress(entry: u64) u64 {
    return entry & page_mask;
}

fn allocZeroPage() ?u64 {
    return allocZeroPageForPreparation(null);
}

const Preparation = struct { unit: usize, stage: []const u8 };

fn preparationFailure(context: Preparation, reason: []const u8, paddr: u64, pages: usize) void {
    log.writeFmt("amd-iommu: unit={} stage={s} error={s}\n", .{ context.unit, context.stage, reason });
    log.writeFmt("amd-iommu: requested_pages={} paddr=0x{x}\n", .{ pages, paddr });
    if (state.free_list) |list| {
        const available = list.availableBelow(four_gib);
        log.writeFmt("amd-iommu: below4G free_pages={} largest_run={}\n", .{ available.total, available.largest_run });
    }
}

fn allocZeroPageForPreparation(context: ?Preparation) ?u64 {
    const free_list = state.free_list orelse {
        if (context) |where| preparationFailure(where, "MissingFreeList", 0, 1);
        return null;
    };
    const paddr = if (builtin.is_test)
        free_list.popFront() catch return null
    else
        free_list.popFrontBelow(four_gib) catch |err| {
            if (context) |where| preparationFailure(where, @errorName(err), 0, 1);
            return null;
        };
    if (builtin.is_test) {
        @memset(pageAt(paddr)[0..], 0);
    } else if (!user_copy.zeroPhysicalPage(paddr)) {
        if (context) |where| preparationFailure(where, "PhysicalZeroFailed", paddr, 1);
        free_list.appendPage(paddr) catch |err| switch (err) {
            error.PageQuarantined => {},
            else => @panic("invalid PMM return"),
        };
        return null;
    }
    return paddr;
}

// Bank bookkeeping is reinitialized by iova.Allocator. Keep the original
// PMM grant outside Bank so a later direct-map change cannot corrupt returns.
const IovaBankBacking = struct {
    bank: iova.Bank,
    physical: physical_layout.PhysicalAddress,
};
comptime {
    if (@sizeOf(IovaBankBacking) > 4096) @compileError("bank backing must fit one page");
}

// Hardware-owned tables remain pinned; this accessor proves the entire CPU
// access extent, not ownership or completion of hardware accesses.
inline fn borrowRamObject(comptime T: type, physical: physical_layout.PhysicalAddress) ?*T {
    const objects = if (builtin.is_test)
        @import("memory/physical_access_test.zig").slice(T, physical.toInt(), 1)
    else
        @call(.always_inline, physical_layout.identitySlice, .{ T, physical, 1 });
    return &(objects orelse return null)[0];
}

fn allocIovaBank(_: ?*anyopaque) ?*iova.Bank {
    if (state.iova_spares) |bank| {
        state.iova_spares = bank.next;
        return bank;
    }
    const physical = physical_layout.PhysicalAddress.fromInt(allocZeroPage() orelse return null);
    const backing = borrowRamObject(IovaBankBacking, physical) orelse unreachable;
    backing.physical = physical;
    return &backing.bank;
}

fn freeIovaBank(_: ?*anyopaque, bank: *iova.Bank) void {
    const free_list = state.free_list orelse unreachable;
    // Save the grant before PMM can publish/reuse the page.
    const backing: *IovaBankBacking = @fieldParentPtr("bank", bank);
    const physical = backing.physical;
    free_list.appendPage(physical.toInt()) catch |err| switch (err) {
        error.PageQuarantined => {
            // PMM rejection forbids reuse by another IOVA owner as well as PMM
            // reuse. Keep the ownership record off the reusable spare list.
            bank.next = state.iova_quarantined;
            state.iova_quarantined = bank;
        },
        else => @panic("invalid PMM return"),
    };
}

fn domainForDevice(device: types.DmaDeviceId) ?*Domain {
    for (0..state.domain_count) |domain_index| {
        const domain = domainAt(domain_index);
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
    const bucket: usize = @intCast((table_paddr >> 12) & (leaf_index_buckets - 1));
    var current = state.leaf_index[bucket];
    while (current) |metadata| : (current = metadata.hash_next) {
        if (metadata.table_paddr == table_paddr) return metadata;
    }
    if (!create) return null;
    // Reserve metadata before acquiring the physical refcount page. On OOM,
    // an unused prepared slot remains owned and can serve the next attempt.
    const index = state.leaves.prepareFree(poolAllocator()) catch return null;
    const refs = allocZeroPage() orelse return null;
    const result = state.leaves.claimIndex(index) catch unreachable;
    // PMM readers follow only fully initialized stable bodies, never a
    // replaceable SlotTable directory. Its old paths can therefore retire
    // during growth without a PMM-reader grace period. Published next links
    // never change; old heads remain valid and contain all earlier quarantine.
    // Never assign the publication flag here or recycle a published leaf.
    result.table_paddr = table_paddr;
    result.refcounts_paddr = refs;
    result.domain_id = did;
    result.next = state.leaf_head;
    result.hash_next = state.leaf_index[bucket];
    state.leaf_count += 1;
    state.leaf_index[bucket] = result;
    @atomicStore(?*Leaf, &state.leaf_head, result, .release);
    return result;
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

inline fn refcounts(paddr: u64) *[512]u16 {
    return borrowRamObject([512]u16, .fromInt(paddr)) orelse unreachable;
}

fn mapPage(domain: *const Domain, iova_address: IovaAddress, paddr: u64, readable: bool, writable: bool) bool {
    const address = iova_address.toInt();
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
                if (resolved.unit != domain.unit or resolved.rule.sourceFor(id).? != domain.source_id) continue;
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
    var cursor = state.leaf_head;
    while (cursor) |leaf| : (cursor = leaf.next) {
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
    for (0..state.domain_count) |domain_index| {
        const domain = domainAt(domain_index);
        if (domain.unit.ordinal == unit_index) quarantineDomain(domain);
    }
}

fn quarantineEvent(unit_index: usize, code: u32, source: u16) void {
    if (code == 5 or code == 6) {
        quarantineUnit(unit_index);
    } else if (code != 0) {
        for (0..state.domain_count) |domain_index| {
            const domain = domainAt(domain_index);
            if (domain.unit.ordinal == unit_index and domain.source_id == source)
                quarantineDomain(domain);
        }
    }
}

fn pollEventsLocked() void {
    if (builtin.is_test) return;
    var cursor = state.units;
    while (cursor) |unit| : (cursor = unit.next) {
        const unit_index = unit.ordinal;
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
        const events = borrowRamObject([256]Event, .fromInt(unit.event_paddr)) orelse unreachable;
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
    const ring = borrowRamObject([256]Command, .fromInt(unit.command_paddr)) orelse return false;
    ring[unit.command_tail / 16] = command;
    asm volatile ("mfence" ::: .{ .memory = true });
    unit.command_tail = next;
    mmioWrite64(unit, command_tail_reg, next);
    return true;
}

fn completionWait(unit: *UnitState) bool {
    unit.completion_sequence +%= 1;
    if (unit.completion_sequence == 0) unit.completion_sequence = 1;
    const marker: *volatile u64 = borrowRamObject(u64, .fromInt(unit.completion_paddr)) orelse return false;
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
    if (builtin.is_test) {
        if (TestSupport.before_ack) |observe| observe();
        if (!TestSupport.withhold_marker) marker.* = TestSupport.marker_override orelse sequence;
    }
    var spins: usize = 0;
    while (spins < poll_limit) : (spins += 1) {
        if (marker.* == sequence) {
            if (builtin.is_test) TestSupport.ack_completed = true;
            return true;
        }
        if (builtin.is_test) {
            if (TestSupport.command_stopped) return false;
        } else if ((mmioRead64(unit, status_reg) & status_command_run) == 0) return false;
        asm volatile ("pause");
    }
    return false;
}

fn invalidateDomain(domain: *const Domain) bool {
    const unit = domain.unit;
    const all: u64 = 0x7fff_ffff_ffff_f000;
    // S=1, PDE=1 and all-one address invalidates the entire domain, including
    // cached directory links. The following wait drains in-flight DMA writes.
    if (!submit(unit, .{ 0, (@as(u32, 3) << 28) | domain.did, @truncate(all | 3), @truncate(all >> 32) }))
        return false;
    return completionWait(unit);
}

fn invalidateDevice(domain: *const Domain) bool {
    const unit = domain.unit;
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

/// Admission check for an owner AFTER its synchronous withdrawal returns.
/// A concurrent quarantine publication may still be observed as false here;
/// the caller must retain ownership/pins until withdrawal finishes. Acquire
/// publishes immutable leaves, not exclusion against an early parallel return.
pub fn physicalRangeQuarantined(paddr: u64, page_count: usize) bool {
    if (!@atomicLoad(bool, &state.has_quarantine, .acquire) or page_count == 0) return false;
    const bytes, const overflow = @mulWithOverflow(@as(u64, @intCast(page_count)), page_size);
    const end, const end_overflow = @addWithOverflow(paddr, bytes);
    if (overflow != 0 or end_overflow != 0) return true;
    var cursor = @atomicLoad(?*Leaf, &state.leaf_head, .acquire);
    while (cursor) |leaf| : (cursor = leaf.next) {
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

const ResolvedRule = struct { unit: *UnitState, rule: ivrs.Rule };

fn ruleForPci(device_id: u16) ?ResolvedRule {
    var found: ?ResolvedRule = null;
    var cursor = state.units;
    while (cursor) |node| : (cursor = node.next) {
        const unit = &node.definition;
        if (unit.segment != 0) continue;
        const rule = unit.ruleFor(device_id) orelse continue;
        if (found != null) return null;
        found = .{ .unit = node, .rule = rule };
    }
    return found;
}

fn platformOnlyPciFunction(info: *const ivrs.Info, device_id: u16, class_code: u8) bool {
    // Bridges are never exposed as DMA capabilities, and the IVHD's own PCI
    // function is the IOMMU, not a peripheral behind its translation unit.
    // Neither needs a device rule; check this before fail-closed lookup so a
    // firmware range may legitimately begin after the root-complex BDFs.
    if (class_code == 0x06) return true;
    var units = info.units();
    while (units.next()) |unit| {
        if (unit.segment == 0 and unit.device_id == device_id) return true;
    }
    return false;
}

test "AMD IOMMU skips only topology bridges and its own PCI function" {
    const allocator = std.testing.allocator;
    var definition = ivrs.Unit{ .device_id = 2, .default_settings = 0 };
    const bytes = try ivrs.testTable(allocator, &.{definition});
    defer allocator.free(bytes);
    var info: ivrs.Info = .{};
    try ivrs.parse(bytes, &info);
    try std.testing.expect(platformOnlyPciFunction(&info, 0x0002, 0x08));
    try std.testing.expect(platformOnlyPciFunction(&info, 0x0001, 0x06));
    try std.testing.expect(!platformOnlyPciFunction(&info, 0x0001, 0x08));
    try std.testing.expect(!platformOnlyPciFunction(&info, 0x0008, 0x01));
    definition.segment = 1;
    const other = try ivrs.testTable(allocator, &.{definition});
    defer allocator.free(other);
    try ivrs.parse(other, &info);
    try std.testing.expect(!platformOnlyPciFunction(&info, 0x0002, 0x08));
}

fn prepareCandidate(index: usize) ?*Candidate {
    if (index > std.math.maxInt(u32)) return null;
    state.candidates.prepareIndex(@intCast(index), poolAllocator()) catch return null;
    return state.candidates.claimIndex(@intCast(index)) catch return null;
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
                const candidate = prepareCandidate(state.candidate_count) orelse return false;
                candidate.* = .{
                    .device = pci.resourceIdFromLocation(loc),
                    .source_id = resolved.rule.sourceFor(id).?,
                    .unit = resolved.unit,
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
    for (0..state.candidate_count) |candidate_index| {
        const other = state.candidates.get(@intCast(candidate_index)).?.*;
        if (other.unit == candidate.unit and other.source_id == candidate.source_id) count += 1;
    }
    return count == 1;
}

fn translatedPreparedDte(definition: *const ivrs.Unit, source: u16, current: Dte, root: u64, did: u16) ?Dte {
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
    for (0..state.candidate_count) |candidate_index| {
        const candidate = state.candidates.get(@intCast(candidate_index)).?.*;
        if (!sourceIsExclusive(candidate)) {
            log.writeFmt("amd-iommu: shared source denied device=0x{x} source=0x{x}\n", .{ candidate.device, candidate.source_id });
            continue;
        }
        if (state.domain_count == max_domain_count) {
            log.write("amd-iommu: domain capacity exhausted\n");
            return false;
        }
        _ = prepareDomain(state.domain_count) orelse return false;
        const root = allocZeroPage() orelse {
            log.writeFmt("amd-iommu: domain root allocation failed source=0x{x}\n", .{candidate.source_id});
            return false;
        };
        const did: u16 = @intCast(state.domain_count + 1);
        const unit = candidate.unit;
        const source = candidate.source_id;
        if (@as(usize, source) >= unit.device_table_pages * 128) {
            log.writeFmt("amd-iommu: source outside DTE table source=0x{x}\n", .{source});
            return false;
        }
        const dte = deviceTableAt(unit, source);
        dte.* = translatedPreparedDte(&candidate.unit.definition, source, dte.*, root, did) orelse {
            log.writeFmt("amd-iommu: prepared DTE mismatch source=0x{x} word0=0x{x} word1=0x{x}\n", .{ source, dte[0], dte[1] });
            return false;
        };
        domainAt(state.domain_count).* = .{
            .device = candidate.device,
            .source_id = source,
            .did = did,
            .unit = candidate.unit,
            .root_paddr = root,
        };
        state.domain_count += 1;
    }
    // Domains copied all candidate fields; hardware never borrows this
    // discovery table. Retire it now, retaining rejected returns in its Owner.
    state.candidates.deinit();
    state.candidate_count = 0;
    return true;
}

fn freeListOverlaps(base: u64, end: u64) bool {
    const list = state.free_list orelse return true;
    return list.overlaps(base, end);
}

fn validateMemoryDefinitions() bool {
    var memories = state.info.memories();
    while (memories.next()) |memory| {
        // ExclusionRange bypasses translation and access checks; it cannot be
        // implemented as a normal identity PTE without granting wrong access.
        if ((memory.flags & 0xf8) != 0) return false;
        if (!memory.all_segments) {
            var found = false;
            var units = state.info.units();
            while (units.next()) |unit| {
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
    var memories = state.info.memories();
    while (memories.next()) |memory| {
        if ((memory.flags & 0xf8) != 0) return false;
        const end = memory.base + memory.length;
        const first_page = memory.base & ~(page_size - 1);
        if (end > (@as(u64, 1) << 48)) return false;
        const last_page = std.mem.alignForward(u64, end, page_size);
        state.iova_end = iova.exclude(iova.window_start, state.iova_end, first_page, last_page - 1);
        const readable = (memory.flags & 0x02) != 0;
        const writable = (memory.flags & 0x04) != 0;
        if (!readable and !writable) continue;
        for (0..state.domain_count) |domain_index| {
            const domain = domainAt(domain_index);
            if (!memory.all_segments and domain.unit.definition.segment != memory.segment) continue;
            if (domain.source_id < memory.first_device or domain.source_id > memory.last_device) continue;
            var page = first_page;
            while (page < last_page) : (page += page_size) {
                if (!mapPage(domain, IovaAddress.fromInt(page), page, readable, writable)) return false;
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

fn prepareUnit(unit: *UnitState) bool {
    const index = unit.ordinal;
    const definition = &unit.definition;
    const mmio_status = smp.explicitUcMmioStatus(definition.mmio_base, unit_register_bytes);
    if (mmio_status != .ready) {
        const evidence = smp.mmioCacheEvidenceCounts();
        log.writeFmt("amd-iommu: unit={} MMIO UC proof failed reason={s} base=0x{x} verified-count={} matching-count={} online-count={}\n", .{ index, @tagName(mmio_status), definition.mmio_base, evidence.verified, evidence.matching, evidence.online });
        return false;
    }
    unit.register_base = definition.mmio_base;
    unit.registers = (@import("arch/x86_64/platform.zig").kernelMmioBorrow(definition.mmio_base, unit_register_bytes) orelse unreachable);
    if ((mmioRead64(unit, control_reg) & ctrl_iommu_en) != 0) {
        log.writeFmt("amd-iommu: unit={} already enabled\n", .{index});
        return false;
    }

    var largest: u16 = if (definition.default_settings != null) 0xffff else 0;
    var rules = definition.rawRules();
    while (rules.next()) |rule| {
        largest = @max(largest, rule.last);
        if (rule.alias) largest = @max(largest, rule.source);
    }
    var specials = definition.specials();
    while (specials.next()) |special| largest = @max(largest, special.source);
    const pages = (@as(usize, largest) / 128) + 1;
    const list = state.free_list orelse {
        preparationFailure(.{ .unit = index, .stage = "device-table" }, "MissingFreeList", 0, pages);
        return false;
    };
    const base = list.popContiguousBelow(pages, four_gib) catch |err| {
        preparationFailure(.{ .unit = index, .stage = "device-table" }, @errorName(err), 0, pages);
        return false;
    };
    unit.device_table_paddr = base;
    unit.device_table_pages = pages;
    for (0..pages) |page_index| {
        const paddr = base + @as(u64, @intCast(page_index)) * page_size;
        if (builtin.is_test) {
            @memset(pageAt(paddr)[0..], 0);
        } else if (!user_copy.zeroPhysicalPage(paddr)) {
            preparationFailure(.{ .unit = index, .stage = "device-table-zero" }, "PhysicalZeroFailed", paddr, pages);
            return false;
        }
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
    unit.command_paddr = allocZeroPageForPreparation(.{ .unit = index, .stage = "command" }) orelse return false;
    unit.event_paddr = allocZeroPageForPreparation(.{ .unit = index, .stage = "event" }) orelse return false;
    unit.completion_paddr = allocZeroPageForPreparation(.{ .unit = index, .stage = "completion" }) orelse return false;
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

fn enableUnit(unit: *UnitState) bool {
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
    const flags = unit.definition.flags;
    const recommended =
        (if ((flags & 0x02) != 0) @as(u64, 1) << 8 else 0) |
        (if ((flags & 0x04) != 0) @as(u64, 1) << 9 else 0) |
        (if ((flags & 0x08) != 0) @as(u64, 1) << 11 else 0) |
        (if (unit.definition.block_type != 0x40 and (flags & 0x01) != 0) @as(u64, 1) << 1 else 0);
    mmioWrite64(unit, control_reg, ctrl_iommu_en | ctrl_command_en | ctrl_event_en | ctrl_coherent | recommended);
    if ((mmioRead64(unit, control_reg) & ctrl_coherent) == 0) return false;
    if (!waitRunning(unit)) return false;
    unit.active = true;
    return true;
}

pub fn init(rsdp_paddr: u64, free_list: *types.FreePageList, comptime checkpoint: fn (u8) void) bool {
    if (state.active or state.has_quarantine) return state.active;
    // Even an unpublished failed growth can retain rejected PMM grants.
    // Transaction.finish binds the directory Owner on every rollback; root
    // alone is insufficient evidence that resetting the state is safe.
    if (poolInitializationStarted()) return false;
    // A failed hardware preparation still owns its pages. Never reset away
    // their unit metadata on retry; production fails boot in this state.
    if (state.units != null) return false;
    const table = ivrs.find(rsdp_paddr) orelse return false;
    if (comptime lock_diagnostics.enabled) {
        if (state.lock_word.record.linked) lock_diagnostics.failure(error.RecordStillLinked);
    }
    state = .{};
    state.free_list = free_list;
    ivrs.parse(table, &state.info) catch |err| {
        checkpoint('N');
        log.writeFmt("amd-iommu: IVRS rejected error={s}\n", .{@errorName(err)});
        return false;
    };
    // All metadata is staged before device discovery or any MMIO enable.
    var tx = storage.Transaction.init(&unit_owner, @import("memory/identity_allocator.zig").allocator(free_list));
    var committed = false;
    defer if (!committed) {
        // No hardware preparation has started. Discard every draft reference
        // before returning metadata; rejected returns stay in unit_owner.
        state.units = null;
        state.candidate_count = 0;
        tx.rollback() catch @panic("AMD unit rollback failed");
    };
    state.units = stageUnits(&tx, state.info) catch return false;
    checkpoint('J');
    if (!discoverCandidates()) {
        log.write("amd-iommu: PCI source discovery failed\n");
        return false;
    }
    if (!validateMemoryDefinitions()) {
        log.write("amd-iommu: IVMD overlaps free RAM or is unsupported\n");
        return false;
    }
    tx.commit() catch @panic("AMD unit commit failed");
    committed = true;
    checkpoint('K');
    var prepare_cursor = state.units;
    while (prepare_cursor) |unit| : (prepare_cursor = unit.next) {
        if (!prepareUnit(unit)) {
            const index = unit.ordinal;
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
    var enable_cursor = state.units;
    while (enable_cursor) |unit| : (enable_cursor = unit.next) {
        if (!enableUnit(unit)) {
            // A previous unit may already be translating. Returning to boot
            // would expose unprotected devices behind a different unit.
            @import("halt.zig").haltWithMessage("AMD IOMMU partial enable failed");
        }
    }
    for (0..state.domain_count) |domain_index| {
        const domain = domainAt(domain_index);
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

pub fn reserveIova(device: types.DmaDeviceId, iova_address: IovaAddress, page_count: usize) bool {
    const address = iova_address.toInt();
    if (!isActive()) return false;
    lock();
    defer unlock();
    pollEventsLocked();
    const domain = domainForDevice(device) orelse return false;
    if (domain.quarantined or !ensureAllocator(domain)) return false;
    return domain.allocator.reserve(address, page_count);
}

pub fn freeIova(device: types.DmaDeviceId, iova_address: IovaAddress, page_count: usize) void {
    const address = iova_address.toInt();
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
    iova_address: IovaAddress,
    paddrs: []const u64,
    readable: bool,
    writable: bool,
) bool {
    const address = iova_address.toInt();
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
        if (!mapPage(domain, IovaAddress.fromInt(address + @as(u64, @intCast(mapped)) * page_size), paddrs[mapped], readable, writable)) {
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

pub fn unmapRangeForDevice(device: types.DmaDeviceId, iova_address: IovaAddress, size: u64) bool {
    const address = iova_address.toInt();
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
    var pool_allocator_override: ?@import("memory/owned_storage.zig").Allocator = null;
    var unit: UnitState = .{};
    pub var before_ack: ?*const fn () void = null;
    pub var withhold_marker: bool = false;
    pub var marker_override: ?u64 = null;
    pub var command_stopped: bool = false;
    pub var ack_completed: bool = false;

    pub fn begin(free_list: *types.FreePageList, device: types.DmaDeviceId) !void {
        state = .{ .free_list = free_list };
        test_fail_completion = false;
        before_ack = null;
        withhold_marker = false;
        marker_override = null;
        command_stopped = false;
        ack_completed = false;
        unit = .{};
        state.units = &unit;
        state.units.?.* = .{
            .command_paddr = allocZeroPage() orelse return error.OutOfMemory,
            .completion_paddr = allocZeroPage() orelse return error.OutOfMemory,
            .active = true,
        };
        state.info.unit_count = 1;
        _ = prepareDomain(0) orelse return error.OutOfMemory;
        domainAt(0).* = .{
            .device = device,
            .source_id = 0x10,
            .unit = &unit,
            .did = 1,
            .root_paddr = allocZeroPage() orelse return error.OutOfMemory,
        };
        state.domain_count = 1;
        state.active = true;
    }

    pub fn end() void {
        pool_allocator_override = null;
        state.leaves.deinit();
        state.domains.deinit();
        state.candidates.deinit();
        state = .{};
        test_fail_completion = false;
        before_ack = null;
        withhold_marker = false;
        marker_override = null;
        command_stopped = false;
        ack_completed = false;
    }

    pub fn failCompletion(fail: bool) void {
        test_fail_completion = fail;
    }
} else struct {};

test "AMD IVRS DTE settings ignore entry order and apply SysMgt erratum" {
    var fixture = ivrs.TestEntries{};
    defer fixture.deinit();
    const definition = &fixture.unit;
    const alias = ivrs.Rule{ .first = 0x0010, .last = 0x001f, .source = 0x8000, .settings = 0x10, .alias = true };
    const ordinary = ivrs.Rule{ .first = 0x8000, .last = 0x80ff, .settings = 0x10 };
    try fixture.appendRule(alias);
    try fixture.appendRule(ordinary);
    try fixture.appendSpecial(0x00a0, 0xd7);
    const before = initialDteWords(definition, 0x8000);
    fixture.clear();
    try fixture.appendRule(ordinary);
    try fixture.appendRule(alias);
    try fixture.appendSpecial(0x00a0, 0xd7);
    try std.testing.expectEqualDeep(before, initialDteWords(definition, 0x8000));
    try std.testing.expectEqual(@as(u64, 1 | write_bit), before[0]);
    try std.testing.expectEqual(@as(u64, 1) << 40, before[1]);
    const special = initialDteWords(definition, 0x00a0);
    try std.testing.expectEqual(@as(u64, 1 | write_bit), special[0]);
    try std.testing.expectEqual(@as(u64, 1) << 40, special[1]);
    try std.testing.expectEqual(@as(u64, 1), initialDteWords(definition, 0x0050)[0]);
}

test "AMD IOMMU domain accepts IVRS SysMgt IW without dropping it" {
    var fixture = ivrs.TestEntries{};
    defer fixture.deinit();
    const definition = &fixture.unit;
    try fixture.appendSpecial(0x00a0, 0xd7);
    const initial = initialDteWords(definition, 0x00a0);
    const prepared = Dte{ initial[0], initial[1], 0, 0 };
    const translated = translatedPreparedDte(definition, 0x00a0, prepared, 0x0040_0000, 7) orelse return error.ExpectedTranslatedDte;
    try std.testing.expectEqual(@as(u64, 0x0040_0000 | (@as(u64, 4) << 9) | 3 | read_bit | write_bit), translated[0]);
    try std.testing.expectEqual(@as(u64, 7 | (@as(u64, 1) << 40)), translated[1]);
    try std.testing.expect(translatedPreparedDte(definition, 0x00a0, .{ 1, initial[1], 0, 0 }, 0x0040_0000, 7) == null);
    try std.testing.expect(translatedPreparedDte(definition, 0x00a0, .{ initial[0], initial[1], 1, 0 }, 0x0040_0000, 7) == null);
    try std.testing.expect(translatedPreparedDte(definition, 0x00a0, prepared, 0x0040_0001, 7) == null);
}

test "AMD IOMMU DTE puts DMA permissions in first word" {
    const allocator = std.testing.allocator;
    const backing = try allocator.alignedAlloc(u8, .fromByteUnits(4096), 8 * 4096);
    defer allocator.free(backing);
    const free_list = try allocator.create(types.FreePageList);
    defer allocator.destroy(free_list);
    free_list.* = .{};
    try free_list.installRegions(&.{.{ .id = 0, .start = @intFromPtr(backing.ptr), .end = (@intFromPtr(backing.ptr)) + (8) * 4096 }});
    try free_list.seedContiguousRange(@intFromPtr(backing.ptr), 8);
    defer TestSupport.end();
    state = .{ .free_list = free_list, .units = &TestSupport.unit };
    TestSupport.unit = .{};
    state.units.?.*.device_table_paddr = allocZeroPage() orelse return error.OutOfMemory;
    state.units.?.*.device_table_pages = 1;
    state.info.unit_count = 1;
    var fixture = ivrs.TestEntries{};
    defer fixture.deinit();
    try fixture.appendSpecial(2, 0x20);
    state.units.?.definition = fixture.unit;
    const dte = deviceTableAt(&state.units.?.*, 2);
    const initial = initialDteWords(&state.units.?.definition, 2);
    dte.* = .{ initial[0], initial[1], 0, 0 };
    (prepareCandidate(0) orelse return error.OutOfMemory).* = .{ .device = 42, .source_id = 2, .unit = &TestSupport.unit };
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
    try free_list.installRegions(&.{.{ .id = 0, .start = @intFromPtr(backing.ptr), .end = (@intFromPtr(backing.ptr)) + (64) * 4096 }});
    try free_list.seedContiguousRange(@intFromPtr(backing.ptr), 64);
    const device = pci.resourceIdFromLocation(.{ .bus = 0, .device = 2, .function = 0 });
    try TestSupport.begin(free_list, device);
    defer TestSupport.end();
    const page = try free_list.popFront();
    const address = iova.window_start;
    try std.testing.expect(reserveIova(device, IovaAddress.fromInt(address), 1));
    try std.testing.expect(mapPages(device, IovaAddress.fromInt(address), &.{page}, true, false));
    try std.testing.expectEqual(page, pageAddress(walk(domainAt(0), address, false).?.table[0]));
    try std.testing.expect(unmapRangeForDevice(device, IovaAddress.fromInt(address), page_size));
    try std.testing.expectEqual(@as(u64, 0), walk(domainAt(0), address, false).?.table[0]);
    try std.testing.expect(mapPages(device, IovaAddress.fromInt(address), &.{page}, true, true));
    const root_entry = &pageAt(domainAt(0).root_paddr)[@as(usize, @intCast((address >> 39) & 511))];
    try setDeviceDmaEnabled(device, false);
    try std.testing.expectEqual(@as(u64, 3), (root_entry.* >> 9) & 7);
    try std.testing.expectEqual(@as(u64, 0), root_entry.* & access_bits);
    try setDeviceDmaEnabled(device, true);
    try std.testing.expectEqual(@as(u64, 3), (root_entry.* >> 9) & 7);
    try std.testing.expectEqual(access_bits, root_entry.* & access_bits);
    TestSupport.failCompletion(true);
    try std.testing.expect(!unmapRangeForDevice(device, IovaAddress.fromInt(address), page_size));
    try std.testing.expect(deviceQuarantined(device));
    try std.testing.expect(physicalRangeQuarantined(page, 1));
    try std.testing.expectError(types.KernelError.PageQuarantined, free_list.appendPage(page));
    try std.testing.expect(!mapPages(device, IovaAddress.fromInt(address + page_size), &.{page}, true, true));
}

test "AMD IOMMU page fault quarantines only its source" {
    const allocator = std.testing.allocator;
    const backing = try allocator.alignedAlloc(u8, .fromByteUnits(4096), 32 * 4096);
    defer allocator.free(backing);
    const free_list = try allocator.create(types.FreePageList);
    defer allocator.destroy(free_list);
    free_list.* = .{};
    try free_list.installRegions(&.{.{ .id = 0, .start = @intFromPtr(backing.ptr), .end = (@intFromPtr(backing.ptr)) + (32) * 4096 }});
    try free_list.seedContiguousRange(@intFromPtr(backing.ptr), 32);
    try TestSupport.begin(free_list, 42);
    defer TestSupport.end();
    _ = prepareDomain(1) orelse return error.OutOfMemory;
    domainAt(1).* = .{
        .unit = &TestSupport.unit,
        .device = 43,
        .source_id = 0x18,
        .did = 2,
        .root_paddr = allocZeroPage() orelse return error.OutOfMemory,
    };
    state.domain_count = 2;
    const first = try free_list.popFront();
    const second = try free_list.popFront();
    try std.testing.expect(mapPages(42, IovaAddress.fromInt(iova.window_start), &.{first}, true, true));
    try std.testing.expect(mapPages(43, IovaAddress.fromInt(iova.window_start), &.{second}, true, true));
    quarantineEvent(0, 2, 0x10);
    try std.testing.expect(deviceQuarantined(42));
    try std.testing.expect(!deviceQuarantined(43));
    try std.testing.expect(physicalRangeQuarantined(first, 1));
    try std.testing.expect(!physicalRangeQuarantined(second, 1));
}

// withdrawal contract fixtures use real owner cleanup and PMM admission. Only the existing
// invalidation ACK boundary is substituted; no production observer is added.
const DmaWithdrawalFixture = if (builtin.is_test) struct {
    backing: []align(4096) u8,
    list: *types.FreePageList,
    regions: []@import("memory/free_page_list.zig").RegionBoundary,
    const device: types.DmaDeviceId = 0x1001;
    const address: u64 = iova.window_start;

    fn init() !@This() {
        const backing = try std.testing.allocator.alignedAlloc(u8, .fromByteUnits(4096), 128 * 4096);
        errdefer std.testing.allocator.free(backing);
        const list = try std.testing.allocator.create(types.FreePageList);
        errdefer std.testing.allocator.destroy(list);
        list.* = .{};
        // Store immutable region descriptors outside this returned value.
        const regions = try std.testing.allocator.alloc(@import("memory/free_page_list.zig").RegionBoundary, 1);
        errdefer std.testing.allocator.free(regions);
        regions[0] = .{ .id = 0, .start = @intFromPtr(backing.ptr), .end = @intFromPtr(backing.ptr) + backing.len };
        return .{ .backing = backing, .list = list, .regions = regions };
    }

    fn deinit(self: @This()) void {
        @import("memory/free_page_list.zig").ReturnProbe.before_publish = null;
        TestSupport.end();
        std.testing.allocator.free(self.regions);
        std.testing.allocator.destroy(self.list);
        std.testing.allocator.free(self.backing);
    }

    fn isFree(self: @This(), page: u64) bool {
        for (0..self.list.rangeCount()) |range_index| {
            const range = self.list.rangeAt(range_index);
            if (page >= range.physical_start and page < range.physical_start + range.len * page_size) return true;
        }
        return false;
    }
} else void;

test "AMD IOMMU withdrawal contract owner cleanup waits for ACK and failed cleanup retains backing" {
    const kernel = @import("kernel.zig");
    const Observer = struct {
        var target: u64 = 0;
        var fixture: *const DmaWithdrawalFixture = undefined;
        var before_count: usize = 0;
        var returned: usize = 0;
        var violated: bool = false;
        fn beforeAck() void {
            before_count += 1;
            const leaf = walk(domainAt(0), DmaWithdrawalFixture.address, false).?;
            // Permissions must already be gone, but physical identity and the
            // last reference must survive until the ACK model completes.
            if (leaf.table[leaf.index] != target or refcounts(leaf.metadata.refcounts_paddr)[leaf.index] != 1 or fixture.isFree(target)) violated = true;
        }
        fn beforeReturn(start: u64, pages: usize) void {
            if (target < start or target >= start + pages * page_size) return;
            returned += 1;
            if (!TestSupport.ack_completed or before_count == 0 or
                @atomicLoad(u8, &state.lock_word.value, .acquire) != 0)
                @panic("withdrawal contract return before IOMMU withdrawal completed");
        }
    };
    for ([_]bool{ false, true }) |fail| {
        const fixture = try DmaWithdrawalFixture.init();
        defer fixture.deinit();
        try fixture.list.installRegions(fixture.regions);
        try fixture.list.seedContiguousRange(@intFromPtr(fixture.backing.ptr), 128);
        try TestSupport.begin(fixture.list, DmaWithdrawalFixture.device);
        const runtime = try std.testing.allocator.alignedAlloc(u8, .fromByteUnits(4096), kernel.runtimeStorageBytes());
        defer std.testing.allocator.free(runtime);
        try std.testing.expect(kernel.initRuntimeStorage(runtime));
        const owner_state = try std.testing.allocator.create(kernel.KernelState);
        defer std.testing.allocator.destroy(owner_state);
        try owner_state.initFromDetectedRegionsInPlace(1);
        const owner = kernel.processPrincipalFromIndex(0).?;
        const page = try fixture.list.popFront();
        const vmo_fd = try owner_state.createAnonymousVmoFd(owner, page_size, .{ .close = true, .map_read = true, .map_write = true }, .{}, 16);
        const vmo = owner_state.nativeVmoRefForFd(owner, vmo_fd).?;
        try owner_state.installNativeVmoPages(vmo, 0, &.{page}, fixture.list);
        const va: u64 = 0x4400_0000;
        _ = try owner_state.mmapFd(owner, vmo_fd, va, page_size, .{ .read = true, .write = true }, .{ .anonymous = true }, 0);
        try std.testing.expect(reserveIova(DmaWithdrawalFixture.device, IovaAddress.fromInt(DmaWithdrawalFixture.address), 1));
        try std.testing.expect(mapPages(DmaWithdrawalFixture.device, IovaAddress.fromInt(DmaWithdrawalFixture.address), &.{page}, true, true));
        _ = try owner_state.createDmaMappingFd(owner, .{ .device = DmaWithdrawalFixture.device, .user_va = va, .iova = DmaWithdrawalFixture.address, .size = page_size }, .{ .close = true }, .{}, 16);
        try owner_state.closeFdWithFreeList(owner, vmo_fd, fixture.list);
        const retained_leaf = walk(domainAt(0), DmaWithdrawalFixture.address, false).?;
        const retained_root = domainAt(0).root_paddr;
        const retained_l3 = pageAddress(pageAt(retained_root)[0]);
        const retained_l2 = pageAddress(pageAt(retained_l3)[0]);
        const retained_pages = [_]u64{ retained_root, retained_l3, retained_l2, retained_leaf.metadata.table_paddr, retained_leaf.metadata.refcounts_paddr };
        Observer.target = page;
        Observer.fixture = &fixture;
        Observer.before_count = 0;
        Observer.returned = 0;
        Observer.violated = false;
        TestSupport.ack_completed = false;
        TestSupport.before_ack = Observer.beforeAck;
        @import("memory/free_page_list.zig").ReturnProbe.before_publish = Observer.beforeReturn;
        TestSupport.withhold_marker = fail;
        // This is the actual forced owner cleanup, without a preceding failed
        // explicit close that could hide a missing destructor drain.
        owner_state.releasePrincipalNativeMemory(owner, fixture.list);
        TestSupport.before_ack = null;
        for (retained_pages) |retained| try std.testing.expect(!fixture.isFree(retained));
        try std.testing.expect(!fixture.isFree(state.units.?.*.command_paddr));
        try std.testing.expect(!fixture.isFree(state.units.?.*.completion_paddr));
        try std.testing.expect(!Observer.violated);
        try std.testing.expect(Observer.before_count > 0);
        try std.testing.expectEqual(@as(usize, if (fail) 0 else 1), Observer.returned);
        try std.testing.expectEqual(!fail, fixture.isFree(page));
        if (fail) {
            TestSupport.withhold_marker = false;
            try std.testing.expectError(types.KernelError.PageQuarantined, fixture.list.appendPage(page));
            try std.testing.expectError(types.KernelError.PageQuarantined, fixture.list.appendContiguousRange(page, 2));
            while (fixture.list.popFront()) |available| {
                try std.testing.expect(available != page);
            } else |_| {}
        }
    }
}

test "AMD IOMMU withdrawal contract quarantine leaves stay immutable while another domain adds leaves" {
    const fixture = try DmaWithdrawalFixture.init();
    defer fixture.deinit();
    try fixture.list.installRegions(fixture.regions);
    try fixture.list.seedContiguousRange(@intFromPtr(fixture.backing.ptr), 128);
    try TestSupport.begin(fixture.list, DmaWithdrawalFixture.device);
    const page = try fixture.list.popFront();
    try std.testing.expect(reserveIova(DmaWithdrawalFixture.device, IovaAddress.fromInt(DmaWithdrawalFixture.address), 1));
    try std.testing.expect(mapPages(DmaWithdrawalFixture.device, IovaAddress.fromInt(DmaWithdrawalFixture.address), &.{page}, true, true));
    const fail = true;
    TestSupport.withhold_marker = fail;
    try std.testing.expect(!unmapRangeForDevice(DmaWithdrawalFixture.device, IovaAddress.fromInt(DmaWithdrawalFixture.address), page_size));
    TestSupport.withhold_marker = false;
    const leaf = walk(domainAt(0), DmaWithdrawalFixture.address, false).?;
    const old_entries = leaf.table.*;
    const old_refs = refcounts(leaf.metadata.refcounts_paddr).*;
    const old_count = state.leaf_count;
    const old_used = domainAt(0).allocator.used_pages;
    try std.testing.expect(!mapPages(DmaWithdrawalFixture.device, IovaAddress.fromInt(DmaWithdrawalFixture.address), &.{page}, true, true));
    try std.testing.expect(!unmapRangeForDevice(DmaWithdrawalFixture.device, IovaAddress.fromInt(DmaWithdrawalFixture.address), page_size));
    try std.testing.expectError(error.Quarantined, setDeviceDmaEnabled(DmaWithdrawalFixture.device, true));
    try std.testing.expect(allocIova(DmaWithdrawalFixture.device, 1) == null);
    try std.testing.expect(!reserveIova(DmaWithdrawalFixture.device, IovaAddress.fromInt(DmaWithdrawalFixture.address + page_size), 1));
    freeIova(DmaWithdrawalFixture.device, IovaAddress.fromInt(DmaWithdrawalFixture.address), 1);
    try std.testing.expectEqual(old_used, domainAt(0).allocator.used_pages);
    try std.testing.expectEqual(old_count, state.leaf_count);
    const other: types.DmaDeviceId = 0x2001;
    _ = prepareDomain(1) orelse return error.OutOfMemory;
    domainAt(1).* = .{ .unit = &TestSupport.unit, .device = other, .did = 2, .root_paddr = allocZeroPage() orelse return error.OutOfMemory };
    state.domain_count = 2;
    const Reader = struct {
        var stop: bool = false;
        var started: bool = false;
        var missed: bool = false;
        fn run(paddr: u64) void {
            @atomicStore(bool, &started, true, .release);
            while (!@atomicLoad(bool, &stop, .acquire)) {
                if (!physicalRangeQuarantined(paddr, 1)) missed = true;
            }
        }
    };
    Reader.stop = false;
    Reader.started = false;
    Reader.missed = false;
    const thread = try std.Thread.spawn(.{}, Reader.run, .{page});
    {
        defer {
            @atomicStore(bool, &Reader.stop, true, .release);
            thread.join();
        }
        while (!@atomicLoad(bool, &Reader.started, .acquire)) std.atomic.spinLoopHint();
        for (0..8) |index| {
            const other_page = try fixture.list.popFront();
            const address = DmaWithdrawalFixture.address + index * (1 << 21);
            try std.testing.expect(mapPages(other, IovaAddress.fromInt(address), &.{other_page}, true, true));
        }
    }
    try std.testing.expect(!Reader.missed);
    try std.testing.expectEqualSlices(u64, &old_entries, leaf.table);
    try std.testing.expectEqualSlices(u16, &old_refs, refcounts(leaf.metadata.refcounts_paddr));
    try std.testing.expectError(types.KernelError.PageQuarantined, fixture.list.appendPage(page));
}

test "AMD IOMMU withdrawal contract leaf initialization never overwrites the atomic publication flag" {
    // Dynamic interleaving alone cannot prove the absence of a data race.
    // Bound this source check to the publication boundary read by PMM; do not police unrelated assignments.
    const source = @embedFile("amd_iommu.zig");
    const first = std.mem.indexOf(u8, source, "    // PMM readers follow only fully initialized stable bodies").?;
    const last = std.mem.indexOfPos(u8, source, first, "leaf_count += 1;").?;
    const initialization = source[first..last];
    try std.testing.expect(std.mem.indexOf(u8, initialization, "result.* =") == null);
    try std.testing.expect(std.mem.indexOf(u8, initialization, "result.quarantined =") == null);
    try std.testing.expect(std.mem.indexOf(u8, initialization, "result.table_paddr = table_paddr;") != null);
}

test "AMD IOMMU withdrawal contract quarantine rejected IOVA banks never enter reusable spares" {
    const fixture = try DmaWithdrawalFixture.init();
    defer fixture.deinit();
    try fixture.list.installRegions(fixture.regions);
    try fixture.list.seedContiguousRange(@intFromPtr(fixture.backing.ptr), 128);
    try TestSupport.begin(fixture.list, DmaWithdrawalFixture.device);
    const bank = allocIovaBank(null).?;
    bank.* = .{};
    const page = @intFromPtr(bank);
    // Deliberately synthesize a bank/data alias to exercise the real PMM
    // quarantine rejection, without replacing the admission predicate.
    try std.testing.expect(mapPages(DmaWithdrawalFixture.device, IovaAddress.fromInt(DmaWithdrawalFixture.address), &.{page}, true, true));
    const fail = true;
    TestSupport.withhold_marker = fail;
    try std.testing.expect(!unmapRangeForDevice(DmaWithdrawalFixture.device, IovaAddress.fromInt(DmaWithdrawalFixture.address), page_size));
    TestSupport.withhold_marker = false;
    freeIovaBank(null, bank);
    try std.testing.expectEqual(bank, state.iova_quarantined.?);
    try std.testing.expect(state.iova_spares == null);
    const replacement = allocIovaBank(null).?;
    try std.testing.expect(replacement != bank);
    try std.testing.expect(!fixture.isFree(page));
    try std.testing.expectError(types.KernelError.PageQuarantined, fixture.list.appendPage(page));

    const replacement_pa = @intFromPtr(replacement);
    freeIovaBank(null, replacement);
    try std.testing.expect(fixture.isFree(replacement_pa));
    try std.testing.expectEqual(bank, state.iova_quarantined.?);
}

test "AMD IOMMU withdrawal contract old marker stopped queue and submit failure retain backing" {
    for (0..3) |mode| {
        const fixture = try DmaWithdrawalFixture.init();
        defer fixture.deinit();
        try fixture.list.installRegions(fixture.regions);
        try fixture.list.seedContiguousRange(@intFromPtr(fixture.backing.ptr), 128);
        try TestSupport.begin(fixture.list, DmaWithdrawalFixture.device);
        const page = try fixture.list.popFront();
        try std.testing.expect(mapPages(DmaWithdrawalFixture.device, IovaAddress.fromInt(DmaWithdrawalFixture.address), &.{page}, true, true));
        switch (mode) {
            0 => TestSupport.marker_override = state.units.?.*.completion_sequence,
            1 => {
                TestSupport.withhold_marker = true;
                TestSupport.command_stopped = true;
            },
            2 => TestSupport.failCompletion(true),
            else => unreachable,
        }
        try std.testing.expect(!unmapRangeForDevice(DmaWithdrawalFixture.device, IovaAddress.fromInt(DmaWithdrawalFixture.address), page_size));
        try std.testing.expectError(error.PageQuarantined, fixture.list.appendPage(page));
    }
}

test "AMD IOMMU withdrawal contract partial map rollback distinguishes unpublished pages" {
    for ([_]bool{ false, true }) |fail| {
        const fixture = try DmaWithdrawalFixture.init();
        defer fixture.deinit();
        try fixture.list.installRegions(fixture.regions);
        try fixture.list.seedContiguousRange(@intFromPtr(fixture.backing.ptr), 128);
        try TestSupport.begin(fixture.list, DmaWithdrawalFixture.device);
        const first = try fixture.list.popFront();
        const second = try fixture.list.popFront();
        const address = DmaWithdrawalFixture.address + (1 << 21) - page_size;
        try std.testing.expect(reserveIova(DmaWithdrawalFixture.device, IovaAddress.fromInt(address), 2));
        while (fixture.list.pageCount() > 4) _ = try fixture.list.popFront();
        TestSupport.withhold_marker = fail;
        try std.testing.expect(!mapPages(DmaWithdrawalFixture.device, IovaAddress.fromInt(address), &.{ first, second }, true, true));
        const leaf = walk(domainAt(0), address, false).?;
        try std.testing.expectEqual(@as(u16, @intFromBool(fail)), refcounts(leaf.metadata.refcounts_paddr)[leaf.index]);
        freeIova(DmaWithdrawalFixture.device, IovaAddress.fromInt(address), 2);
        try std.testing.expectEqual(@as(usize, if (fail) 2 else 0), domainAt(0).allocator.used_pages);
        if (fail) try std.testing.expectError(error.PageQuarantined, fixture.list.appendPage(first)) else try fixture.list.appendPage(first);
        try fixture.list.appendPage(second);
        try std.testing.expect(!mapPages(DmaWithdrawalFixture.device, IovaAddress.fromInt(address + 1), &.{second}, true, true));
    }
}

test "AMD IOMMU withdrawal contract buffer and mapping aliases close only after the last ACK" {
    const kernel = @import("kernel.zig");
    for ([_]bool{ false, true }) |buffer| {
        for ([_]bool{ false, true }) |fail| {
            const fixture = try DmaWithdrawalFixture.init();
            defer fixture.deinit();
            try fixture.list.installRegions(fixture.regions);
            try fixture.list.seedContiguousRange(@intFromPtr(fixture.backing.ptr), 128);
            try TestSupport.begin(fixture.list, DmaWithdrawalFixture.device);
            const runtime = try std.testing.allocator.alignedAlloc(u8, .fromByteUnits(4096), kernel.runtimeStorageBytes());
            defer std.testing.allocator.free(runtime);
            try std.testing.expect(kernel.initRuntimeStorage(runtime));
            const owner_state = try std.testing.allocator.create(kernel.KernelState);
            defer std.testing.allocator.destroy(owner_state);
            try owner_state.initFromDetectedRegionsInPlace(1);
            const owner = kernel.processPrincipalFromIndex(0).?;
            const page = try fixture.list.popFront();
            try std.testing.expect(reserveIova(DmaWithdrawalFixture.device, IovaAddress.fromInt(DmaWithdrawalFixture.address), 1));
            try std.testing.expect(mapPages(DmaWithdrawalFixture.device, IovaAddress.fromInt(DmaWithdrawalFixture.address), &.{page}, true, true));
            const rights: kernel.FdRights = .{ .close = true, .dup = true };
            const fd = if (buffer)
                try owner_state.createDmaBufferFd(owner, .{ .device = DmaWithdrawalFixture.device, .user_va = 0x44000000, .iova = DmaWithdrawalFixture.address, .size = page_size }, rights, .{}, 16)
            else
                try owner_state.createDmaMappingFd(owner, .{ .device = DmaWithdrawalFixture.device, .user_va = 0x44000000, .iova = DmaWithdrawalFixture.address, .size = page_size }, rights, .{}, 16);
            const alias = try owner_state.dupFd(owner, fd, 16, rights, .{});
            TestSupport.ack_completed = false;
            TestSupport.withhold_marker = fail;
            try owner_state.closeFdWithFreeList(owner, fd, fixture.list);
            try std.testing.expect(!TestSupport.ack_completed);
            try std.testing.expect(!deviceQuarantined(DmaWithdrawalFixture.device));
            if (fail) {
                try std.testing.expectError(error.InvalidState, owner_state.closeFdWithFreeList(owner, alias, fixture.list));
                try std.testing.expect(owner_state.fdEntryConst(owner, alias) != null);
                try std.testing.expectError(error.PageQuarantined, fixture.list.appendPage(page));
            } else {
                try owner_state.closeFdWithFreeList(owner, alias, fixture.list);
                try std.testing.expect(TestSupport.ack_completed);
                try std.testing.expect(owner_state.fdEntryConst(owner, alias) == null);
                try std.testing.expectEqual(@as(usize, 0), domainAt(0).allocator.used_pages);
                try fixture.list.appendPage(page);
            }
        }
    }
}

test "AMD IOMMU physical access nonidentity bank grant survives allocator reset and returns physical" {
    const fixture = @import("memory/physical_access_test.zig");
    const memory = try std.testing.allocator.alignedAlloc(u8, .fromByteUnits(4096), 4096);
    defer std.testing.allocator.free(memory);
    const physical: u64 = 0x120000;
    try std.testing.expect(physical != @intFromPtr(memory.ptr));
    fixture.mapping = .{ .physical = physical, .bytes = memory };
    defer fixture.mapping = null;
    const free_list = try std.testing.allocator.create(types.FreePageList);
    defer std.testing.allocator.destroy(free_list);
    free_list.* = .{};
    try free_list.installRegions(&.{.{ .id = 17, .start = physical, .end = physical + 4096 }});
    try free_list.seedContiguousRange(physical, 1);
    const saved = state;
    defer state = saved;
    state = .{ .free_list = free_list };
    var allocator = iova.Allocator.init(0x1000000, 0x2000000, .{ .allocate = allocIovaBank, .release = freeIovaBank });
    try std.testing.expect(allocator.reserve(0x1000000, 1));
    const bank = allocator.banks.?;
    const backing: *IovaBankBacking = @fieldParentPtr("bank", bank);
    try std.testing.expectEqual(physical, backing.physical.toInt());
    try std.testing.expect(allocator.free(0x1000000, 1));
    try std.testing.expectEqual(physical, try free_list.popFront());
    try std.testing.expect(borrowRamObject(u64, .fromInt(physical + 1)) == null);
    try std.testing.expect(borrowRamObject([4096]u8, .fromInt(physical + 1)) == null);
    try std.testing.expect(borrowRamObject(u64, .fromInt(std.math.maxInt(u64) - 7)) == null);
    try std.testing.expect(borrowRamObject(u64, .fromInt(physical + 4096)) == null);
}

test "AMD IOMMU physical access completion keeps hardware physical separate from CPU marker" {
    const fixture = @import("memory/physical_access_test.zig");
    const memory = try std.testing.allocator.alignedAlloc(u8, .fromByteUnits(4096), 4096);
    defer std.testing.allocator.free(memory);
    const physical: u64 = 0x160000;
    fixture.mapping = .{ .physical = physical, .bytes = memory };
    defer fixture.mapping = null;
    TestSupport.end();
    defer TestSupport.end();
    var unit: UnitState = .{ .completion_paddr = physical };
    try std.testing.expect(completionWait(&unit));
    const marker: *volatile u64 = @ptrCast(memory.ptr);
    try std.testing.expectEqual(unit.completion_sequence, marker.*);
    try std.testing.expectEqual(physical, unit.completion_paddr);
    try std.testing.expect(@intFromPtr(marker) != unit.completion_paddr);
    unit.completion_paddr = physical + 4096;
    try std.testing.expect(!completionWait(&unit));
    unit.completion_paddr = physical + 1;
    try std.testing.expect(!completionWait(&unit));
}

test "AMD unit metadata exceeds eight and u8 with single-page grants and stable references" {
    const allocator = std.testing.allocator;
    for ([_]usize{ 7, 8, 9, 257 }) |count| {
        const definitions = try allocator.alloc(ivrs.Unit, count);
        defer allocator.free(definitions);
        @memset(definitions, .{ .default_settings = 0 });
        const bytes = try ivrs.testTable(allocator, definitions);
        defer allocator.free(bytes);
        var info: ivrs.Info = .{};
        try ivrs.parse(bytes, &info);
        var backend: storage.TestAllocator = .{};
        defer backend.deinit();
        var owner: storage.Owner = .{};
        var tx = storage.Transaction.init(&owner, backend.interface());
        var cursor = try stageUnits(&tx, info);
        var count_seen: usize = 0;
        while (cursor) |unit| : (cursor = unit.next) {
            const domain = Domain{ .unit = unit };
            try std.testing.expectEqual(count_seen, domain.unit.ordinal);
            try std.testing.expectEqual(@as(u64, 0xfed0_0000) + count_seen * 0x4000, domain.unit.definition.mmio_base);
            count_seen += 1;
        }
        try std.testing.expectEqual(count, count_seen);
        try std.testing.expectEqual((count + units_per_page - 1) / units_per_page, backend.calls);
        try std.testing.expectEqual(@as(usize, 1), backend.last_pages);
        try tx.rollback();
        try std.testing.expectEqual(@as(usize, 0), backend.active());
    }
}

test "AMD unit metadata failed grants retain all refused returns in owner pending" {
    const count = units_per_page * 2 + 1;
    const definitions = try std.testing.allocator.alloc(ivrs.Unit, count);
    defer std.testing.allocator.free(definitions);
    @memset(definitions, .{ .default_settings = 0 });
    const bytes = try ivrs.testTable(std.testing.allocator, definitions);
    defer std.testing.allocator.free(bytes);
    var info: ivrs.Info = .{};
    try ivrs.parse(bytes, &info);
    for (1..4) |fail_at| {
        var backend = storage.TestAllocator{ .fail_at = fail_at, .fail_return = true };
        defer backend.deinit();
        var owner: storage.Owner = .{};
        var tx = storage.Transaction.init(&owner, backend.interface());
        try std.testing.expectError(error.OutOfMemory, stageUnits(&tx, info));
        try tx.rollback();
        try std.testing.expectEqual(fail_at - 1, backend.active());
        try owner.retryUnpublished();
        try std.testing.expectEqual(fail_at - 1, backend.active());
        backend.fail_return = false;
        try owner.retryUnpublished();
        try std.testing.expect(owner.pending == null);
        try std.testing.expectEqual(@as(usize, 0), backend.active());
    }
}

test "AMD IOMMU pools exceed domain and leaf limits during quarantine reads" {
    const allocator = std.testing.allocator;
    const backing = try allocator.alignedAlloc(u8, .fromByteUnits(4096), 8192 * 4096);
    defer allocator.free(backing);
    const list = try allocator.create(types.FreePageList);
    defer allocator.destroy(list);
    list.* = .{};
    try list.installRegions(&.{.{ .id = 0, .start = @intFromPtr(backing.ptr), .end = @intFromPtr(backing.ptr) + backing.len }});
    try list.seedContiguousRange(@intFromPtr(backing.ptr), 8192);
    try TestSupport.begin(list, 42);
    defer TestSupport.end();
    const first_domain = domainAt(0);
    for (1..257) |index| {
        const domain = prepareDomain(index) orelse return error.OutOfMemory;
        domain.did = @intCast(index + 1);
    }
    try std.testing.expectEqual(first_domain, domainAt(0));
    try std.testing.expectEqual(@as(u16, 257), domainAt(256).did);
    try std.testing.expect(prepareDomain(max_domain_count) == null);
    const table_page = allocZeroPage() orelse return error.OutOfMemory;
    const first = leafFor(table_page, true, 1) orelse return error.OutOfMemory;
    pageAt(table_page)[0] = 0x12345000;
    refcounts(first.refcounts_paddr)[0] = 1;
    quarantineDomain(first_domain);
    const Reader = struct {
        stop: bool = false,
        reads: usize = 0,
        fn run(self: *@This()) void {
            while (!@atomicLoad(bool, &self.stop, .acquire)) {
                std.debug.assert(physicalRangeQuarantined(0x12345000, 1));
                _ = @atomicRmw(usize, &self.reads, .Add, 1, .monotonic);
            }
        }
    };
    var reader = Reader{};
    const thread = try std.Thread.spawn(.{}, Reader.run, .{&reader});
    {
        defer {
            @atomicStore(bool, &reader.stop, true, .release);
            thread.join();
        }
        while (@atomicLoad(usize, &reader.reads, .monotonic) == 0) std.atomic.spinLoopHint();
        // Deliberate collisions prove buckets do not cap the leaf count.
        for (1..4097) |index| {
            const key = @as(u64, @intCast(index)) * leaf_index_buckets * page_size;
            const leaf = leafFor(key, true, 2) orelse return error.OutOfMemory;
            try std.testing.expectEqual(leaf, leafFor(key, false, 2).?);
        }
    }
    try std.testing.expectEqual(@as(usize, 4097), state.leaf_count);
    try std.testing.expectEqual(first, leafFor(table_page, false, 1).?);
    try std.testing.expectEqual(@as(u64, 0x12345000), pageAt(table_page)[0]);
    try std.testing.expectEqual(@as(u16, 1), refcounts(first.refcounts_paddr)[0]);
}

test "AMD IOMMU pool growth failure preserves unpublished ownership and permits retry" {
    const Fail = struct {
        fn acquire(_: *anyopaque, _: usize) ?@import("memory/owned_storage.zig").MappedExtent {
            return null;
        }
        fn release(_: *anyopaque, _: @import("memory/owned_storage.zig").PhysicalExtent) bool {
            unreachable;
        }
    };
    var context: u8 = 0;
    TestSupport.pool_allocator_override = .{ .context = &context, .acquire = Fail.acquire, .release = Fail.release };
    defer TestSupport.end();
    try std.testing.expect(prepareDomain(0) == null);
    try std.testing.expect(poolInitializationStarted());
    try std.testing.expect(leafFor(4096, true, 1) == null);
    try std.testing.expectEqual(@as(usize, 0), state.leaf_count);
    try std.testing.expect(state.leaf_head == null);
    TestSupport.pool_allocator_override = null;
    const first = prepareDomain(0) orelse return error.OutOfMemory;
    first.did = 123;
    TestSupport.pool_allocator_override = .{ .context = &context, .acquire = Fail.acquire, .release = Fail.release };
    const boundary: usize = @intCast(state.domains.capacity);
    try std.testing.expect(prepareDomain(boundary) == null);
    try std.testing.expectEqual(first, domainAt(0));
    try std.testing.expectEqual(@as(u16, 123), first.did);
    TestSupport.pool_allocator_override = null;
    try std.testing.expect(prepareDomain(boundary) != null);
}
