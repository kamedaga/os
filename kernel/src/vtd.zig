const IovaAddress = @import("memory/addresses.zig").IovaAddress;
const mmio = @import("memory/mmio.zig");
const physical_layout = @import("arch/x86_64/physical_layout.zig");
const lock_diagnostics = @import("sync/lock_diagnostics.zig");
const std = @import("std");
const builtin = @import("builtin");
const acpi_dmar = @import("acpi_dmar.zig");
const kernel_log = @import("kernel_log.zig");
const pci = @import("pci.zig");
const smp = @import("smp.zig");
const types = @import("state/types.zig");
const user_copy = @import("user_copy.zig");
const tables = @import("vtd_tables.zig");
const iova_storage = @import("iova.zig");

pub const Discovery = struct {
    table_paddr: u64,
    info: acpi_dmar.DmarInfo,
};

const page_size: u64 = 4096;
const four_gib: u64 = @import("arch/x86_64/physical_layout.zig").low_memory_limit;
pub const iova_window_start = tables.iova_window_start;
pub const iova_window_end = tables.iova_window_end;
const slot_table = @import("memory/slot_table.zig");
// Segment zero has 256 buses, 32 devices and 8 functions. DIDs are
// independently bounded by each unit; software ownership covers every BDF.
const max_domain_count: usize = 256 * 32 * 8;
// Hash bucket count controls distribution, not the number of retained leaves.
// Collisions use stable SlotTable bodies rather than finite open-address slots.
const leaf_index_buckets: usize = 8192;
const poll_limit: usize = 1_000_000;

const cap_reg: u64 = 0x08;
const cap_caching_mode: u64 = @as(u64, 1) << 7;
const ecap_reg: u64 = 0x10;
const gcmd_reg: u64 = 0x18;
const gsts_reg: u64 = 0x1c;
const rtaddr_reg: u64 = 0x20;
const ccmd_reg: u64 = 0x28;
const fsts_reg: u64 = 0x34;

const gcmd_te: u32 = 1 << 31;
const gcmd_srtp: u32 = 1 << 30;
const gcmd_wbf: u32 = 1 << 27;
const gsts_tes: u32 = 1 << 31;
const gsts_rtps: u32 = 1 << 30;
const gsts_wbfs: u32 = 1 << 27;

const ccmd_icc: u64 = @as(u64, 1) << 63;
const ccmd_cirg_global: u64 = @as(u64, 1) << 61;
const ccmd_caig_shift: u6 = 59;
const iotlb_ivt: u64 = @as(u64, 1) << 63;
const iotlb_iirg_global: u64 = @as(u64, 1) << 60;
const iotlb_iirg_domain: u64 = @as(u64, 2) << 60;
const iotlb_did_shift: u6 = 32;
const iotlb_iaig_shift: u6 = 57;
const iotlb_read_drain: u64 = @as(u64, 1) << 49;
const iotlb_write_drain: u64 = @as(u64, 1) << 48;
const fault_record_valid: u64 = @as(u64, 1) << 63;

const LeafMetadata = struct {
    next: ?*LeafMetadata = null,
    hash_next: ?*LeafMetadata = null,
    table_paddr: u64 = 0,
    refcounts_paddr: u64 = 0,
    domain_id: u16 = 0,
    unit_index: usize = 0,
    quarantined: bool = false,
};

pub const IovaAllocator = tables.IovaAllocator;

const Domain = struct {
    unit_index: usize = 0,
    device: types.DmaDeviceId = types.invalid_dma_device_id,
    did: u16 = 0,
    second_level_root_paddr: u64 = 0,
    allocator: IovaAllocator = .{},
    quarantined: bool = false,
    enabled: bool = true,
};

// Domain bodies are hardware owners: keep them stable until all DMA is drained.
// Runtime lookup and growth share the driver lock; no directory loan escapes it.
fn poolAllocator() @import("memory/owned_storage.zig").Allocator {
    if (builtin.is_test) return TestSupport.pool_allocator_override orelse slot_table.hostTestAllocator();
    return @import("memory/identity_allocator.zig").allocator(driver_state.free_list.?);
}
fn poolInitializationStarted() bool {
    return driver_state.domains.directory_owner.bound_address != null or driver_state.leaf_metadata.directory_owner.bound_address != null or driver_state.units.directory_owner.bound_address != null;
}
fn prepareDomain(index: usize) ?*Domain {
    if (index >= max_domain_count) return null;
    driver_state.domains.prepareIndex(@intCast(index), poolAllocator()) catch return null;
    return driver_state.domains.claimIndex(@intCast(index)) catch return null;
}
fn domainAt(index: usize) *Domain {
    return driver_state.domains.get(@intCast(index)) orelse unreachable;
}

// Unit bodies never move after contexts are published. The driver lock protects
// their registers and domains; a domain belongs to exactly one PCI requester
// and its unique routing unit, so it cannot acquire a second hardware owner.
const UnitState = struct {
    drhd: acpi_dmar.Drhd = .{},
    published: bool = false,
    prepared: bool = false,
    table_cache_line_bytes: usize = 0,
    register_base: u64 = 0,
    registers: mmio.Borrow = undefined,
    cap: u64 = 0,
    ecap: u64 = 0,
    root_paddr: u64 = 0,
    context_table_paddrs: [256]u64 = [_]u64{0} ** 256,
    hardware_domain_count: u32 = 0,
    fault_record_count: usize = 0,
    fault_record_offset: u64 = 0,
    iotlb_offset: u64 = 0,
    gcmd_shadow: u32 = 0,
    context_count: usize = 0,
    rwbf: bool = false,
};

const DriverState = struct {
    discovery: ?Discovery = null,
    free_list: ?*types.FreePageList = null,
    domains: slot_table.SlotTable(Domain) = .{},
    domain_count: usize = 0,
    leaf_metadata: slot_table.SlotTable(LeafMetadata) = .{},
    leaf_head: ?*LeafMetadata = null,
    leaf_index: [leaf_index_buckets]?*LeafMetadata = .{null} ** leaf_index_buckets,
    leaf_count: usize = 0,
    mapped_page_count: usize = 0,
    mapping_ref_count: usize = 0,
    /// Zero for a coherent table walker, otherwise the CPUID CLFLUSH stride.
    table_cache_line_bytes: usize = 0,
    active: bool = false,
    iova_end: u64 = iova_window_end,
    // PMM can temporarily reject a return when its free-range metadata is
    // fragmented. Keep ownership and reuse such banks instead of leaking them.
    iova_spares: ?*iova_storage.Bank = null,
    // A quarantine rejection is permanent, unlike PMM descriptor exhaustion.
    iova_quarantined: ?*iova_storage.Bank = null,
    has_quarantine: bool = false,
    lock_word: lock_diagnostics.Byte = .{},
    first_unit: UnitState = .{},
    units: slot_table.SlotTable(UnitState) = .{},
    unit_count: usize = 0,
};

var driver_state: DriverState = .{};
fn unitAt(index: usize) *UnitState {
    return if (index == 0) &driver_state.first_unit else driver_state.units.get(@intCast(index)) orelse unreachable;
}

/// Read only, after successful initialization and before user dispatch.
/// GSTS.IRES (bit25) forbids legacy-format IOAPIC programming; RTPS/TES do
/// not prove anything about interrupt remapping. No global command is changed.
pub fn legacyInterruptRoutingAllowed() bool {
    for (0..driver_state.unit_count) |index| {
        const unit = unitAt(index);
        if (unit.register_base == 0 or (mmioRead32(unit, gsts_reg) & (1 << 25)) != 0) return false;
    }
    return true;
}
fn prepareUnit(index: usize) ?*UnitState {
    if (index == 0) return &driver_state.first_unit;
    driver_state.units.prepareIndex(@intCast(index), poolAllocator()) catch return null;
    return driver_state.units.claimIndex(@intCast(index)) catch return null;
}
var test_fail_invalidation: bool = false;
var test_last_invalidation: u64 = 0;
const CacheFlushEvent = struct { address: u64 = 0, length: usize = 0, first_word: u64 = 0 };
var test_cache_flush_events: [256]CacheFlushEvent = [_]CacheFlushEvent{.{}} ** 256;
var test_cache_flush_count: usize = 0;
var test_flush_count_at_invalidation: usize = 0;

pub fn kernelStaticStorageStartAddr() usize {
    return @intFromPtr(&driver_state);
}

pub fn kernelStaticStorageEndAddr() usize {
    return @intFromPtr(&driver_state) + @sizeOf(@TypeOf(driver_state));
}

fn lock() void {
    while (true) {
        const diagnostic_if = lock_diagnostics.maskInterrupts();
        lock_diagnostics.beforeAcquire(&driver_state.lock_word.record, .{ .class = .vtd });
        if (@cmpxchgWeak(u8, &driver_state.lock_word.value, 0, 1, .acquire, .monotonic) == null) {
            lock_diagnostics.acquired(&driver_state.lock_word.record, .{ .class = .vtd });
            lock_diagnostics.restoreInterrupts(diagnostic_if);
            return;
        }
        lock_diagnostics.restoreInterrupts(diagnostic_if);
        while (@atomicLoad(u8, &driver_state.lock_word.value, .monotonic) != 0) {
            asm volatile ("pause");
        }
    }
}

fn unlock() void {
    const diagnostic_if = lock_diagnostics.maskInterrupts();
    defer lock_diagnostics.restoreInterrupts(diagnostic_if);
    lock_diagnostics.releasing(&driver_state.lock_word.record);
    @atomicStore(u8, &driver_state.lock_word.value, 0, .release);
}

fn cpuid(leaf: u32) struct { eax: u32, ebx: u32, ecx: u32, edx: u32 } {
    var eax: u32 = 0;
    var ebx: u32 = 0;
    var ecx: u32 = 0;
    var edx: u32 = 0;
    asm volatile ("cpuid"
        : [eax] "={eax}" (eax),
          [ebx] "={ebx}" (ebx),
          [ecx] "={ecx}" (ecx),
          [edx] "={edx}" (edx),
        : [leaf] "{eax}" (leaf),
          [subleaf] "{ecx}" (@as(u32, 0)),
    );
    return .{ .eax = eax, .ebx = ebx, .ecx = ecx, .edx = edx };
}

fn tableCacheLineBytes(ecap: u64, cpu_ebx: u32, cpu_edx: u32) ?usize {
    if ((ecap & 1) != 0) return 0;
    if ((cpu_edx & (@as(u32, 1) << 19)) == 0) return null;
    const bytes: usize = ((cpu_ebx >> 8) & 0xff) * 8;
    if (bytes == 0 or bytes > page_size or !std.math.isPowerOfTwo(bytes)) return null;
    return bytes;
}

/// ECAP.C describes the remapping hardware's accesses to CPU-owned tables,
/// not the coherency of device payload DMA. CLFLUSH writes those cache lines
/// back before table links or invalidation commands may expose their contents.
fn flushTableCache(pointer: *const anyopaque, length: usize) void {
    const address = @intFromPtr(pointer);
    const stride = driver_state.table_cache_line_bytes;
    if (stride == 0 or length == 0) return;
    if (builtin.is_test) {
        if (test_cache_flush_count < test_cache_flush_events.len) {
            test_cache_flush_events[test_cache_flush_count] = .{
                .address = address,
                .length = length,
                .first_word = @as(*const u64, @ptrCast(@alignCast(pointer))).*,
            };
        }
        test_cache_flush_count += 1;
    }
    const end = address + length;
    var current = address & ~(@as(u64, @intCast(stride)) - 1);
    asm volatile ("mfence" ::: .{ .memory = true });
    while (current < end) : (current += stride) {
        asm volatile ("clflush (%%rax)"
            :
            : [address] "{rax}" (current),
            : .{ .memory = true });
    }
    asm volatile ("mfence" ::: .{ .memory = true });
}

fn mmioRead32(unit: *UnitState, offset: u64) u32 {
    return unit.registers.read(u32, offset);
}

fn mmioWrite32(unit: *UnitState, offset: u64, value: u32) void {
    unit.registers.write(u32, offset, value);
    if (builtin.is_test) {
        if (TestSupport.fake_register_mode and offset == gcmd_reg and TestSupport.fail_unit != unit)
            unit.registers.write(u32, gsts_reg, value & (gcmd_te | gcmd_srtp));
    }
}

fn mmioRead64(unit: *UnitState, offset: u64) u64 {
    return mmioRead64At(&unit.registers, offset);
}

fn mmioRead64At(registers: *const mmio.Borrow, offset: u64) u64 {
    return registers.read(u64, offset);
}

fn mmioWrite64(unit: *UnitState, offset: u64, value: u64) void {
    unit.registers.write(u64, offset, value);
    if (builtin.is_test) {
        if (TestSupport.fake_register_mode) {
            if (offset == ccmd_reg or offset == unit.iotlb_offset + 8) {
                unit.registers.write(u64, 0x300, unit.registers.read(u64, 0x300) + 1);
                if (TestSupport.fail_unit != unit) unit.registers.write(u64, offset, if (offset == ccmd_reg) @as(u64, 1) << ccmd_caig_shift else @as(u64, 1) << iotlb_iaig_shift);
            }
        }
    }
}

fn wait32Set(unit: *UnitState, offset: u64, mask: u32) bool {
    var spins: usize = 0;
    while (spins < poll_limit) : (spins += 1) {
        if ((mmioRead32(unit, offset) & mask) == mask) return true;
        asm volatile ("pause");
    }
    return false;
}

fn wait32Clear(unit: *UnitState, offset: u64, mask: u32) bool {
    var spins: usize = 0;
    while (spins < poll_limit) : (spins += 1) {
        if ((mmioRead32(unit, offset) & mask) == 0) return true;
        asm volatile ("pause");
    }
    return false;
}

fn wait64Clear(unit: *UnitState, offset: u64, mask: u64) ?u64 {
    for (0..poll_limit) |_| {
        const value = mmioRead64(unit, offset);
        if ((value & mask) == 0) return value;
        asm volatile ("pause");
    }
    return null;
}

fn wait64ClearWithReader(offset: u64, mask: u64, comptime limit: usize, comptime read: fn (u64) u64) ?u64 {
    var spins: usize = 0;
    while (spins < limit) : (spins += 1) {
        const value = read(offset);
        // VT-d reports CAIG/IAIG when ICC/IVT clears (11.4.6.1/11.4.6.3).
        // Keep that sample so checking the result needs no second MMIO read.
        if ((value & mask) == 0) return value;
        asm volatile ("pause");
    }
    return null;
}

fn allocZeroPage() ?u64 {
    const free_list = driver_state.free_list orelse return null;
    const paddr = if (builtin.is_test)
        free_list.popFront() catch return null
    else
        free_list.popFrontBelow(four_gib) catch return null;
    if (builtin.is_test) {
        @memset(tableAt(paddr)[0..], 0);
    } else if (!user_copy.zeroPhysicalPage(paddr)) {
        free_list.appendPage(paddr) catch |err| switch (err) {
            error.PageQuarantined => {},
            else => @panic("invalid PMM return"),
        };
        return null;
    }
    return paddr;
}

fn allocTablePage() ?u64 {
    const paddr = allocZeroPage() orelse return null;
    flushTableCache(tableAt(paddr), page_size);
    return paddr;
}

// Bank bookkeeping is reinitialized by iova.Allocator. Keep the original
// PMM grant outside Bank so a later direct-map change cannot corrupt returns.
const IovaBankBacking = struct {
    bank: iova_storage.Bank,
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

fn allocIovaBank(_: ?*anyopaque) ?*iova_storage.Bank {
    if (driver_state.iova_spares) |bank| {
        driver_state.iova_spares = bank.next;
        return bank;
    }
    const physical = physical_layout.PhysicalAddress.fromInt(allocZeroPage() orelse return null);
    const backing = borrowRamObject(IovaBankBacking, physical) orelse unreachable;
    backing.physical = physical;
    return &backing.bank;
}

fn freeIovaBank(_: ?*anyopaque, bank: *iova_storage.Bank) void {
    const free_list = driver_state.free_list orelse unreachable;
    // Save the grant before PMM can publish/reuse the page.
    const backing: *IovaBankBacking = @fieldParentPtr("bank", bank);
    const physical = backing.physical;
    free_list.appendPage(physical.toInt()) catch |err| switch (err) {
        error.PageQuarantined => {
            // PMM rejection forbids reuse by another IOVA owner as well as PMM
            // reuse. Keep the ownership record off the reusable spare list.
            bank.next = driver_state.iova_quarantined;
            driver_state.iova_quarantined = bank;
        },
        else => @panic("invalid PMM return"),
    };
}

inline fn tableAt(paddr: u64) *tables.TablePage {
    return borrowRamObject(tables.TablePage, .fromInt(paddr)) orelse unreachable;
}

fn pageAddress(entry: u64) u64 {
    return entry & tables.page_address_mask;
}

fn domainForDevice(device: types.DmaDeviceId) ?*Domain {
    for (0..driver_state.domain_count) |domain_index| {
        const domain = domainAt(domain_index);
        if (domain.device == device) return domain;
    }
    return null;
}

fn ensureDomainAllocator(domain: *Domain) bool {
    if (domain.allocator.storage != null) return true;
    if (driver_state.iova_end <= iova_window_start) return false;
    domain.allocator = IovaAllocator.init(iova_window_start, driver_state.iova_end, .{
        .allocate = allocIovaBank,
        .release = freeIovaBank,
    });
    return true;
}

fn ensureChildTable(parent: *tables.TablePage, index: usize) ?u64 {
    const existing = parent[index];
    if ((existing & (tables.second_level_read | tables.second_level_write)) != 0) {
        const paddr = pageAddress(existing);
        return if (paddr != 0) paddr else null;
    }
    const child = allocTablePage() orelse return null;
    if (!tables.setSecondLevelEntry(parent, index, child, true, true)) return null;
    flushTableCache(&parent[index], @sizeOf(u64));
    return child;
}

/// A disabled domain still owns its entire tree. Unlike ordinary child
/// links, its root links intentionally carry an address with no R/W bits.
fn ensureDomainRootChild(domain: *const Domain, root: *tables.TablePage, index: usize) ?u64 {
    // The walk already borrowed this root under the same domain lock. Reuse
    // that borrow instead of repeating the physical extent validation.
    const existing = pageAddress(root[index]);
    if (existing != 0) return existing;
    const child = allocTablePage() orelse return null;
    root[index] = child | (if (domain.enabled)
        tables.second_level_read | tables.second_level_write
    else
        @as(u64, 0));
    flushTableCache(&root[index], @sizeOf(u64));
    return child;
}

fn leafMetadata(table_paddr: u64, create: bool, domain_id: u16, unit_index: usize) ?*LeafMetadata {
    const bucket: usize = @intCast((table_paddr >> 12) & (leaf_index_buckets - 1));
    var current = driver_state.leaf_index[bucket];
    while (current) |metadata| : (current = metadata.hash_next) {
        if (metadata.table_paddr == table_paddr) return metadata;
    }
    if (!create) return null;
    // Reserve metadata before acquiring the physical refcount page. On OOM,
    // an unused prepared slot remains owned and can serve the next attempt.
    const index = driver_state.leaf_metadata.prepareFree(poolAllocator()) catch return null;
    const refs = allocZeroPage() orelse return null;
    const result = driver_state.leaf_metadata.claimIndex(index) catch unreachable;
    // PMM readers follow only fully initialized stable bodies, never a
    // replaceable SlotTable directory. Its old paths can therefore retire
    // during growth without a PMM-reader grace period. Published next links
    // never change; old heads remain valid and contain all earlier quarantine.
    // Never assign the publication flag here or recycle a published leaf.
    result.table_paddr = table_paddr;
    result.refcounts_paddr = refs;
    result.domain_id = domain_id;
    result.unit_index = unit_index;
    result.next = driver_state.leaf_head;
    result.hash_next = driver_state.leaf_index[bucket];
    driver_state.leaf_count += 1;
    driver_state.leaf_index[bucket] = result;
    @atomicStore(?*LeafMetadata, &driver_state.leaf_head, result, .release);
    return result;
}

inline fn refcountsAt(paddr: u64) *[512]u16 {
    return borrowRamObject([512]u16, .fromInt(paddr)) orelse unreachable;
}

fn walkToLeaf(domain: *const Domain, iova: u64, create: bool) ?struct { table: *tables.TablePage, metadata: *LeafMetadata, index: usize } {
    const pml4 = tableAt(domain.second_level_root_paddr);
    const pml4_index: usize = @intCast((iova >> 39) & 0x1ff);
    const pdp_paddr = if (create)
        ensureDomainRootChild(domain, pml4, pml4_index) orelse return null
    else
        pageAddress(pml4[pml4_index]);
    if (pdp_paddr == 0) return null;

    const pdp = tableAt(pdp_paddr);
    const pdp_index: usize = @intCast((iova >> 30) & 0x1ff);
    const pd_paddr = if (create)
        ensureChildTable(pdp, pdp_index) orelse return null
    else
        pageAddress(pdp[pdp_index]);
    if (pd_paddr == 0) return null;

    const pd = tableAt(pd_paddr);
    const pd_index: usize = @intCast((iova >> 21) & 0x1ff);
    const pt_paddr = if (create)
        ensureChildTable(pd, pd_index) orelse return null
    else
        pageAddress(pd[pd_index]);
    if (pt_paddr == 0) return null;
    const metadata = leafMetadata(pt_paddr, create, domain.did, domain.unit_index) orelse return null;
    return .{
        .table = tableAt(pt_paddr),
        .metadata = metadata,
        .index = @intCast((iova >> 12) & 0x1ff),
    };
}

fn mapPage(domain: *const Domain, iova_address: IovaAddress, paddr: u64, readable: bool, writable: bool) bool {
    const iova = iova_address.toInt();
    const leaf = walkToLeaf(domain, iova, true) orelse return false;
    const refs = refcountsAt(leaf.metadata.refcounts_paddr);
    if (refs[leaf.index] == std.math.maxInt(u16)) return false;
    const existing = leaf.table[leaf.index];
    const desired = paddr |
        (if (readable) tables.second_level_read else 0) |
        (if (writable) tables.second_level_write else 0);
    if (existing != 0 and existing != desired) return false;
    if (existing == 0) {
        if (!tables.setSecondLevelEntry(leaf.table, leaf.index, paddr, readable, writable)) return false;
        flushTableCache(&leaf.table[leaf.index], @sizeOf(u64));
    }
    if (refs[leaf.index] == 0) driver_state.mapped_page_count += 1;
    refs[leaf.index] += 1;
    driver_state.mapping_ref_count += 1;
    return true;
}

fn unmapPage(domain: *const Domain, iova: u64) void {
    const leaf = walkToLeaf(domain, iova, false) orelse return;
    const refs = refcountsAt(leaf.metadata.refcounts_paddr);
    if (refs[leaf.index] == 0) return;
    refs[leaf.index] -= 1;
    driver_state.mapping_ref_count -= 1;
    if (refs[leaf.index] == 0) {
        leaf.table[leaf.index] = 0;
        flushTableCache(&leaf.table[leaf.index], @sizeOf(u64));
        driver_state.mapped_page_count -= 1;
    }
}

/// Remove hardware permission before invalidation, but preserve the physical
/// address and reference ledger until the IOMMU has acknowledged the drain.
fn withdrawPage(domain: *const Domain, iova: u64) void {
    const leaf = walkToLeaf(domain, iova, false) orelse return;
    const refs = refcountsAt(leaf.metadata.refcounts_paddr);
    if (refs[leaf.index] == 1) {
        leaf.table[leaf.index] &= tables.page_address_mask;
        flushTableCache(&leaf.table[leaf.index], @sizeOf(u64));
    }
}

fn quarantineDomain(domain: *Domain) void {
    domain.quarantined = true;
    var cursor = driver_state.leaf_head;
    while (cursor) |metadata| : (cursor = metadata.next) {
        if (metadata.domain_id == domain.did and metadata.unit_index == domain.unit_index) @atomicStore(bool, &metadata.quarantined, true, .release);
    }
    @atomicStore(bool, &driver_state.has_quarantine, true, .release);
    // Never disable IOMMU translation on a runtime failure: that would turn
    // stale accesses into unrestricted DMA. Stop bus mastering instead.
    if (!builtin.is_test) {
        if (pci.locationFromResourceId(domain.device)) |loc| {
            const command = pci.readConfigU16(loc, 0x04);
            pci.writeConfigU16(loc, 0x04, command & ~@as(u16, 4));
            _ = pci.readConfigU16(loc, 0x04);
        }
        kernel_log.writeFmt("vtd: quarantined device=0x{x} did={} backing-retained=1\n", .{ domain.device, domain.did });
    }
}

/// The retained leaf ledger is the physical-page quarantine owner, independent
/// of FDs, VMAs and process lifetime. It is never cleared after a failed drain.
/// Quarantined leaves become immutable before publication, so the PMM can
/// inspect them without inverting the VT-d -> PMM allocation lock order.
/// Admission check for an owner AFTER its synchronous withdrawal returns.
/// A concurrent quarantine publication may still be observed as false here;
/// the caller must retain ownership/pins until withdrawal finishes. Acquire
/// publishes immutable leaves, not exclusion against an early parallel return.
pub fn physicalRangeQuarantined(paddr: u64, page_count: usize) bool {
    if (!@atomicLoad(bool, &driver_state.has_quarantine, .acquire)) return false;
    if (page_count == 0) return false;
    const length, const size_overflow = @mulWithOverflow(@as(u64, @intCast(page_count)), page_size);
    const end, const end_overflow = @addWithOverflow(paddr, length);
    if (size_overflow != 0 or end_overflow != 0) return true;
    var cursor = @atomicLoad(?*LeafMetadata, &driver_state.leaf_head, .acquire);
    while (cursor) |metadata| : (cursor = metadata.next) {
        if (!@atomicLoad(bool, &metadata.quarantined, .acquire)) continue;
        const entries = tableAt(metadata.table_paddr);
        const refs = refcountsAt(metadata.refcounts_paddr);
        for (entries, 0..) |entry, index| {
            if (refs[index] == 0) continue;
            const page = pageAddress(entry);
            if (page < end and page + page_size > paddr) return true;
        }
    }
    return false;
}

pub fn deviceQuarantined(device: types.DmaDeviceId) bool {
    if (!@atomicLoad(bool, &driver_state.has_quarantine, .acquire)) return false;
    lock();
    defer unlock();
    const domain = domainForDevice(device) orelse return false;
    return domain.quarantined;
}

fn setDomainRootPermissions(domain: *Domain, enabled: bool) void {
    const root = tableAt(domain.second_level_root_paddr);
    for (root) |*entry| {
        if (pageAddress(entry.*) == 0) continue;
        entry.* = pageAddress(entry.*) | (if (enabled)
            tables.second_level_read | tables.second_level_write
        else
            @as(u64, 0));
    }
    domain.enabled = enabled;
    flushTableCache(tableAt(domain.second_level_root_paddr), page_size);
}

/// Shared device-domain state, including all same-authority FD aliases.
/// Legacy second-level contexts only (no Device-TLB/ATS or PASID domains).
/// Keep the context, IOVA allocations, leaf permissions and RAM ledger intact.
/// PCI bus-master writes cannot bypass withdrawn second-level root permission.
pub fn setDeviceDmaEnabled(device: types.DmaDeviceId, enabled: bool) error{
    Unsupported,
    NoDevice,
    Quarantined,
    DrainFailed,
}!void {
    if (!isActive()) return error.Unsupported;
    lock();
    defer unlock();
    const domain = domainForDevice(device) orelse return error.NoDevice;
    if (domain.quarantined) return error.Quarantined;
    const unit = unitAt(domain.unit_index);
    const required_drain = (@as(u64, 1) << 55) | (@as(u64, 1) << 54);
    if ((unit.cap & required_drain) != required_drain) return error.Unsupported;
    setDomainRootPermissions(domain, enabled);
    if (flushTranslationChanges(domain)) return;
    // A failed enable may already have exposed translations. Remove root
    // permission again, but never claim a successful stop without its drain.
    setDomainRootPermissions(domain, false);
    quarantineDomain(domain);
    return error.DrainFailed;
}

pub fn dmaSnapshotFlags(device: types.DmaDeviceId) u64 {
    if (!isActive()) return 0;
    lock();
    defer unlock();
    const domain = domainForDevice(device) orelse return 0;
    const abi = @import("kernel_abi_root").capsule_abi;
    return if (domain.quarantined) abi.snapshot_flag_dma_quarantined else abi.snapshot_flag_dma_translated;
}

pub fn domainInvalidationCommand(cap: u64, did: u16) u64 {
    return iotlb_ivt | iotlb_iirg_domain | (@as(u64, did) << iotlb_did_shift) |
        (if ((cap & (@as(u64, 1) << 55)) != 0) iotlb_read_drain else 0) |
        (if ((cap & (@as(u64, 1) << 54)) != 0) iotlb_write_drain else 0);
}

fn domainInvalidationCompleted(value: u64) bool {
    if ((value & iotlb_ivt) != 0) return false;
    const actual = (value >> iotlb_iaig_shift) & 3;
    // Hardware may promote a domain flush to a stronger global flush.
    return actual == 1 or actual == 2;
}

fn writeBufferFlush(unit: *UnitState) bool {
    if (!unit.rwbf) return true;
    mmioWrite32(unit, gcmd_reg, unit.gcmd_shadow | gcmd_wbf);
    return wait32Clear(unit, gsts_reg, gsts_wbfs);
}

fn invalidateContextCacheGlobal(unit: *UnitState) bool {
    mmioWrite64(unit, ccmd_reg, ccmd_icc | ccmd_cirg_global);
    const completed = wait64Clear(unit, ccmd_reg, ccmd_icc) orelse return false;
    return ((completed >> ccmd_caig_shift) & 0x3) == 1;
}

fn invalidateIotlbGlobal(unit: *UnitState) bool {
    const register = unit.iotlb_offset + 8;
    mmioWrite64(unit, register, iotlb_ivt | iotlb_iirg_global);
    const completed = wait64Clear(unit, register, iotlb_ivt) orelse return false;
    return ((completed >> iotlb_iaig_shift) & 0x3) == 1;
}

fn invalidateIotlbDomain(unit: *UnitState, did: u16) bool {
    const command = domainInvalidationCommand(unit.cap, did);
    if (builtin.is_test) {
        if (!TestSupport.fake_register_mode) {
            test_last_invalidation = command;
            test_flush_count_at_invalidation = test_cache_flush_count;
            if (TestSupport.before_ack) |observe| observe();
            const completed = TestSupport.invalidation_result orelse (@as(u64, 2) << iotlb_iaig_shift);
            const success = !test_fail_invalidation and domainInvalidationCompleted(completed);
            TestSupport.ack_completed = success;
            return success;
        }
    }
    const register = unit.iotlb_offset + 8;
    mmioWrite64(unit, register, command);
    const completed = wait64Clear(unit, register, iotlb_ivt) orelse return false;
    return domainInvalidationCompleted(completed);
}

fn flushTranslationChanges(domain: *const Domain) bool {
    const unit = unitAt(domain.unit_index);
    const did = domain.did;
    // VT-d Rev. 5.0 sections 6.5.4 and 11.4.2: IVT completion drains
    // non-committed DMA only with supported DR/DW requests. Major >= 2 also
    // reports both capability bits as 1; no version-based exception is needed.
    // A caller withdrawing reachable pages must quarantine on missing proof.
    const required_drain = (@as(u64, 1) << 55) | (@as(u64, 1) << 54);
    if ((unit.cap & required_drain) != required_drain) return false;
    asm volatile ("mfence" ::: .{ .memory = true });
    if (!writeBufferFlush(unit)) return false;
    return invalidateIotlbDomain(unit, did);
}

fn flushMapChanges(domain: *const Domain) bool {
    const unit = unitAt(domain.unit_index);
    const did = domain.did;
    asm volatile ("mfence" ::: .{ .memory = true });
    if (!builtin.is_test and !writeBufferFlush(unit)) return false;
    // mapPage only publishes absent entries or retains identical mappings.
    // Without negative caching, publication (including WBF) is sufficient.
    // Withdrawal and device stop still require invalidation and DMA drain.
    if ((unit.cap & cap_caching_mode) == 0) return true;
    return invalidateIotlbDomain(unit, did);
}

fn stopBootBusMasters() void {
    if (builtin.is_test) return;
    // Init can fail before domain construction (including firmware-enabled
    // translation). Stop every present function, not only constructed domains.
    for (0..256) |bus| {
        for (0..32) |device| {
            var loc = pci.Location{ .bus = @intCast(bus), .device = @intCast(device), .function = 0 };
            if (pci.readVendorId(loc) == 0xffff) continue;
            const functions: usize = if ((pci.readHeaderType(loc) & 0x80) != 0) 8 else 1;
            for (0..functions) |function| {
                loc.function = @intCast(function);
                if (pci.readVendorId(loc) == 0xffff) continue;
                const command = pci.readConfigU16(loc, 0x04);
                pci.writeConfigU16(loc, 0x04, command & ~@as(u16, 4));
                _ = pci.readConfigU16(loc, 0x04);
            }
        }
    }
}

fn disableTranslationAfterFailure(unit: *UnitState) error{DisableTimedOut}!void {
    unit.gcmd_shadow &= ~gcmd_te;
    mmioWrite32(unit, gcmd_reg, unit.gcmd_shadow);
    if (!wait32Clear(unit, gsts_reg, gsts_tes)) {
        // An unacknowledged disable is not pass-through. Retain all tables,
        // stop bus masters and force boot to halt instead of admitting raw DMA.
        stopBootBusMasters();
        for (0..driver_state.domain_count) |index| quarantineDomain(domainAt(index));
        @atomicStore(bool, &driver_state.has_quarantine, true, .release);
        return error.DisableTimedOut;
    }
    driver_state.active = false;
}

fn logInitFailure(reason: []const u8) void {
    kernel_log.writeFmt("vtd: initialization-failed detail={s}\n", .{reason});
    if (initializationRequiresHalt()) {
        stopBootBusMasters();
        for (0..driver_state.domain_count) |d| quarantineDomain(domainAt(d));
        @import("halt.zig").haltWithMessage("VT-d partial initialization; bus masters stopped and tables retained");
    }
}

fn initializationRequiresHalt() bool {
    for (0..driver_state.unit_count) |index| {
        const unit = unitAt(index);
        // SRTP alone exposes references; even a failed enable must not release
        // tables or revert the already active subset to unrestricted DMA.
        if (unit.published or (unit.register_base != 0 and (mmioRead32(unit, gsts_reg) & gsts_tes) != 0)) return true;
    }
    return false;
}

fn domainCount(nd: u3) u32 {
    const shift: u5 = @intCast(4 + @as(u8, nd) * 2);
    return @as(u32, 1) << shift;
}

fn logDeviceScopes(info: *const acpi_dmar.DmarInfo) void {
    var drhds = info.drhds();
    var drhd_index: usize = 0;
    while (drhds.next()) |drhd| : (drhd_index += 1) {
        if (drhd.segment != 0) continue;
        kernel_log.writeFmt(
            "vtd: drhd index={} length={} base=0x{x} segment={} include_pci_all={} scopes={}\n",
            .{ drhd_index, drhd.length, drhd.register_base, drhd.segment, @intFromBool(drhd.include_pci_all), drhd.scope_count },
        );
        var scopes = drhd.scopes();
        var scope_index: usize = 0;
        while (scopes.next()) |scope| : (scope_index += 1) {
            kernel_log.writeFmt(
                "vtd: scope drhd={} index={} type={} enumeration_id={} start_bus={} path=",
                .{ drhd_index, scope_index, scope.scope_type, scope.enumeration_id, scope.start_bus },
            );
            for (scope.path[0..scope.path_count], 0..) |path, path_index| {
                if (path_index != 0) kernel_log.write("/");
                kernel_log.writeFmt("{}:{}", .{ path.device, path.function });
            }
            kernel_log.write("\n");

            var bus = scope.start_bus;
            var resolved: ?pci.Location = null;
            for (scope.path[0..scope.path_count], 0..) |path, path_index| {
                const location = pci.Location{ .bus = bus, .device = path.device, .function = path.function };
                resolved = location;
                if (path_index + 1 < scope.path_count) {
                    bus = pci.readConfigU8(location, 0x19);
                }
            }
            if (resolved) |location| {
                kernel_log.writeFmt(
                    "vtd: scope_resolved drhd={} index={} bdf={}:{}:{} vendor=0x{x} device=0x{x}\n",
                    .{ drhd_index, scope_index, location.bus, location.device, location.function, pci.readVendorId(location), pci.readDeviceId(location) },
                );
            } else {
                kernel_log.writeFmt("vtd: scope_resolved drhd={} index={} none\n", .{ drhd_index, scope_index });
            }
        }
    }
}

fn samePciLocation(left: pci.Location, right: pci.Location) bool {
    return left.bus == right.bus and
        left.device == right.device and
        left.function == right.function;
}

fn resolveEndpointScope(scope: *const acpi_dmar.DeviceScope) ?pci.Location {
    return resolveScopeWith(pci, scope);
}
fn resolveScopeWith(comptime Reader: type, scope: *const acpi_dmar.DeviceScope) ?pci.Location {
    if ((scope.scope_type != 1 and scope.scope_type != 2) or scope.path_count == 0) return null;

    var bus = scope.start_bus;
    for (scope.path[0..scope.path_count], 0..) |path, path_index| {
        if (path.device >= 32 or path.function >= 8) return null;
        const location = pci.Location{
            .bus = bus,
            .device = path.device,
            .function = path.function,
        };
        if (path_index + 1 == scope.path_count) {
            // A subhierarchy is meaningful only at a real PCI bridge. Otherwise
            // falling through to INCLUDE_PCI_ALL could select the wrong owner.
            if (scope.scope_type == 2 and (Reader.readVendorId(location) == 0xffff or
                Reader.readClassCode(location) != 6 or Reader.readSubclass(location) != 4 or
                Reader.readConfigU8(location, 0x19) == 0 or
                Reader.readConfigU8(location, 0x19) > Reader.readConfigU8(location, 0x1a))) return null;
            return location;
        }

        if (Reader.readVendorId(location) == 0xffff) return null;
        if (Reader.readClassCode(location) != 0x06 or
            Reader.readSubclass(location) != 0x04)
        {
            return null;
        }
        const secondary = Reader.readConfigU8(location, 0x19);
        const subordinate = Reader.readConfigU8(location, 0x1a);
        if (secondary == 0 or secondary > subordinate) return null;
        bus = secondary;
    }
    return null;
}

// Explicit scopes precede INCLUDE_PCI_ALL. Ambiguity is a firmware error,
// never resolved by array order: that could attach a device to the wrong root.
fn explicitScopeMatches(drhd: acpi_dmar.Drhd, location: pci.Location) bool {
    return explicitScopeMatchesWith(pci, drhd, location);
}
fn explicitScopeMatchesWith(comptime Reader: type, drhd: acpi_dmar.Drhd, location: pci.Location) bool {
    return scopesMatchWith(Reader, drhd.scopes(), location);
}
fn scopesMatchWith(comptime Reader: type, initial: acpi_dmar.Scopes, location: pci.Location) bool {
    var scopes = initial;
    while (scopes.next()) |scope| {
        const endpoint = resolveScopeWith(Reader, &scope) orelse continue;
        if (samePciLocation(endpoint, location)) return true;
        if (scope.scope_type == 2) {
            if (Reader.readClassCode(endpoint) != 6 or Reader.readSubclass(endpoint) != 4) continue;
            const secondary = Reader.readConfigU8(endpoint, 0x19);
            const subordinate = Reader.readConfigU8(endpoint, 0x1a);
            if (secondary != 0 and secondary <= subordinate and
                location.bus >= secondary and location.bus <= subordinate) return true;
        }
    }
    return false;
}
fn ownerForLocation(location: pci.Location) ?usize {
    var explicit: ?usize = null;
    var fallback: ?usize = null;
    for (0..driver_state.unit_count) |index| {
        const drhd = unitAt(index).drhd;
        if (drhd.segment != 0) continue;
        if (drhd.include_pci_all) {
            if (fallback != null) return null;
            fallback = index;
        }
        if (explicitScopeMatches(drhd, location)) {
            if (explicit != null) return null;
            explicit = index;
        }
    }
    return explicit orelse fallback;
}

fn pciFunctionCount() ?usize {
    var count: usize = 0;
    var bus_number: usize = 0;
    while (bus_number < 256) : (bus_number += 1) {
        var device_number: usize = 0;
        while (device_number < 32) : (device_number += 1) {
            const function_zero = pci.Location{
                .bus = @intCast(bus_number),
                .device = @intCast(device_number),
                .function = 0,
            };
            if (pci.readVendorId(function_zero) == 0xffff) continue;
            const function_count: usize = if ((pci.readHeaderType(function_zero) & 0x80) != 0) 8 else 1;
            var function_number: usize = 0;
            while (function_number < function_count) : (function_number += 1) {
                const location = pci.Location{
                    .bus = @intCast(bus_number),
                    .device = @intCast(device_number),
                    .function = @intCast(function_number),
                };
                if (pci.readVendorId(location) == 0xffff) continue;
                if (ownerForLocation(location) == null) {
                    kernel_log.writeFmt(
                        "vtd: scope coverage failed bdf={}:{}:{}\n",
                        .{ bus_number, device_number, function_number },
                    );
                    return null;
                }
                count += 1;
            }
        }
    }
    return count;
}

fn installPciContexts(unit: *UnitState, unit_index: usize, total_function_count: usize) bool {
    if (total_function_count == 0) {
        kernel_log.write("vtd: context construction failed reason=no-pci-functions\n");
        return false;
    }
    var bus_number: usize = 0;
    while (bus_number < 256) : (bus_number += 1) {
        var device_number: usize = 0;
        while (device_number < 32) : (device_number += 1) {
            const function_zero = pci.Location{
                .bus = @intCast(bus_number),
                .device = @intCast(device_number),
                .function = 0,
            };
            if (pci.readVendorId(function_zero) == 0xffff) continue;
            const function_count: usize = if ((pci.readHeaderType(function_zero) & 0x80) != 0) 8 else 1;
            var function_number: usize = 0;
            while (function_number < function_count) : (function_number += 1) {
                const location = pci.Location{
                    .bus = @intCast(bus_number),
                    .device = @intCast(device_number),
                    .function = @intCast(function_number),
                };
                const vendor = pci.readVendorId(location);
                if (vendor == 0xffff) continue;
                const owner = ownerForLocation(location) orelse return false;
                if (owner != unit_index) continue;
                if (!installPciContext(unit, unit_index, location)) return false;
                const did = domainAt(driver_state.domain_count - 1).did;
                kernel_log.writeFmt(
                    "vtd: context bdf={}:{}:{} vendor=0x{x} device=0x{x} domain={}\n",
                    .{ bus_number, device_number, function_number, vendor, pci.readDeviceId(location), did },
                );
            }
        }
    }
    return true;
}

fn installPciContext(unit: *UnitState, unit_index: usize, location: pci.Location) bool {
    const root = tableAt(unit.root_paddr);
    if (unit.context_table_paddrs[location.bus] == 0) {
        const context_paddr = allocTablePage() orelse return false;
        if (!tables.setRootEntry(root, @intCast(location.bus), context_paddr)) return false;
        unit.context_table_paddrs[location.bus] = context_paddr;
    }
    const context = tableAt(unit.context_table_paddrs[location.bus]);
    const device_function: u8 = @intCast(@as(usize, location.device) * 8 + location.function);
    const domain_index = driver_state.domain_count;
    if (domain_index >= max_domain_count or unit.context_count + 1 >= unit.hardware_domain_count or
        unit.context_count >= std.math.maxInt(u16)) return false;
    _ = prepareDomain(domain_index) orelse return false;
    const second_level_root_paddr = allocTablePage() orelse return false;
    const did: u16 = @intCast(unit.context_count + 1);
    const resource_id = pci.resourceIdFromLocation(location);
    domainAt(domain_index).* = .{
        .unit_index = unit_index,
        .device = resource_id,
        .did = did,
        .second_level_root_paddr = second_level_root_paddr,
    };
    if (!tables.setContextEntry(context, device_function, second_level_root_paddr, did)) return false;
    driver_state.domain_count += 1;
    unit.context_count += 1;
    return true;
}

/// Context contents reach RAM before the root entries that point at them;
/// both are visible before SRTP or translation enable touches either table.
fn flushInitialTables(unit: *UnitState) void {
    for (unit.context_table_paddrs) |context| {
        if (context != 0) flushTableCache(tableAt(context), page_size);
    }
    flushTableCache(tableAt(unit.root_paddr), page_size);
}

pub fn init(rsdp_paddr: u64, free_list: *types.FreePageList) void {
    // A second initialization must never discard retained hardware references.
    if (driver_state.active or driver_state.has_quarantine) return;
    // Even an unpublished failed growth can retain rejected PMM grants.
    // Transaction.finish binds the directory Owner on every rollback; root
    // alone is insufficient evidence that resetting the state is safe.
    if (driver_state.first_unit.root_paddr != 0 or poolInitializationStarted()) return;
    if (comptime lock_diagnostics.enabled) {
        if (driver_state.lock_word.record.linked) lock_diagnostics.failure(error.RecordStillLinked);
    }
    driver_state = .{};
    const table = acpi_dmar.findDmar(rsdp_paddr) orelse {
        kernel_log.write("vtd: mode=pass-through reason=dmar-not-found active=0 faults=0\n");
        return;
    };
    const info = acpi_dmar.parseDmar(table.bytes) catch |err| {
        kernel_log.writeFmt(
            "vtd: mode=pass-through reason=dmar-invalid paddr=0x{x} error={s} active=0 faults=0\n",
            .{ table.paddr.toInt(), @errorName(err) },
        );
        return;
    };
    driver_state.discovery = .{ .table_paddr = table.paddr.toInt(), .info = info };
    driver_state.free_list = free_list;

    kernel_log.writeFmt(
        "vtd: DMAR paddr=0x{x} host_address_width={} raw_haw={} flags=0x{x} drhds={}\n",
        .{ table.paddr.toInt(), info.hostAddressWidthBits(), info.host_address_width, info.flags, info.drhd_count },
    );
    defer if (!driver_state.active and !initializationRequiresHalt()) releaseDraftTables();
    // All scopes of a DRHD belong to its segment. Nonzero segments are not
    // enumerated or published by PCI today; ignoring them cannot remove the
    // mandatory ownership check for every present segment-zero requester.
    if (!selectUnits(info)) {
        logInitFailure("no segment-zero DRHD or unit allocation failed");
        return;
    }
    driver_state.iova_end = pci.bootDmaWindowEnd(iova_window_start, info.dma_window_end);
    for (0..driver_state.unit_count) |index| {
        prepareHardwareUnit(unitAt(index));
        if (!unitAt(index).prepared) return;
    }
    for (0..driver_state.unit_count) |index| {
        var scopes = unitAt(index).drhd.scopes();
        while (scopes.next()) |scope| {
            if ((scope.scope_type == 1 or scope.scope_type == 2) and resolveEndpointScope(&scope) == null) {
                logInitFailure("unresolvable PCI device scope");
                return;
            }
        }
    }
    const pci_function_count = pciFunctionCount() orelse {
        logInitFailure("segment-zero PCI ownership missing or ambiguous");
        return;
    };
    for (0..driver_state.unit_count) |index| {
        const unit = unitAt(index);
        unit.root_paddr = allocTablePage() orelse {
            logInitFailure("root allocation failed");
            return;
        };
        if (!installPciContexts(unit, index, pci_function_count)) {
            logInitFailure("PCI context construction failed");
            return;
        }
    }
    if (driver_state.domain_count != pci_function_count) {
        logInitFailure("PCI coverage changed");
        return;
    }
    if (!installFirmwareReservedMappings(info)) {
        // All tables are still unpublished. Do not enable a partial RMRR
        // policy or silently continue without firmware-required DMA access.
        logInitFailure("firmware reserved DMA grant rejected");
        releaseDraftTables();
        @import("halt.zig").haltWithMessage("VT-d RMRR construction failed before publication");
    }
    for (0..driver_state.unit_count) |index| {
        const unit = unitAt(index);
        if (!enableHardwareUnit(unit)) {
            logInitFailure("unit activation failed");
            return;
        }
    }
    driver_state.active = true;
    kernel_log.writeFmt("vtd: mode=translated reason=enabled active=1 domains={} contexts={} units={}\n", .{
        driver_state.domain_count, driver_state.domain_count, driver_state.unit_count,
    });
    dumpFaults();
}

/// Opt-in pre-userland evidence only. Never call prepareHardwareUnit: its
/// rejection path can stop bus masters and erase the very display we observe.
/// All MMIO is read-only, with the same RAM exclusion and all-CPU UC proof as
/// production. No driver state, DMA tables, fault flags or PCI command is changed.
pub fn diagnosticSnapshot(rsdp_paddr: u64) void {
    const platform = @import("arch/x86_64/platform.zig");
    const table = acpi_dmar.findDmar(rsdp_paddr) orelse {
        kernel_log.write("VTD SNAPSHOT: DMAR missing\n");
        return;
    };
    const info = acpi_dmar.parseDmar(table.bytes) catch {
        kernel_log.write("VTD SNAPSHOT: DMAR invalid; no MMIO read\n");
        return;
    };
    kernel_log.writeFmt("VTD SNAPSHOT: DMAR=0x{x} HAW={} flags=0x{x} units={}\n", .{
        table.paddr.toInt(), info.hostAddressWidthBits(), info.flags, info.drhd_count,
    });
    var units = info.drhds();
    var index: usize = 0;
    while (units.next()) |unit| : (index += 1) {
        const base = unit.register_base;
        if (unit.segment != 0 or base == 0 or base % page_size != 0 or
            !platform.kernelMmioRangeFits(base, page_size) or
            !smp.ucMinusMmioAllowed(base, page_size) or
            !platform.mapBootMmioRange(base, page_size))
        {
            kernel_log.writeFmt("VTD{} base=0x{x} SKIPPED: segment/range/cache/map proof failed\n", .{ index, base });
            continue;
        }
        const registers = platform.kernelMmioBorrow(base, page_size) orelse {
            kernel_log.writeFmt("VTD{} SKIPPED: borrow unavailable\n", .{index});
            continue;
        };
        kernel_log.writeFmt("VTD{} base=0x{x} VER=0x{x} CAP=0x{x} ECAP=0x{x}\n", .{
            index, base, registers.read(u32, 0), registers.read(u64, cap_reg), registers.read(u64, ecap_reg),
        });
        kernel_log.writeFmt("VTD{} GSTS=0x{x} RTADDR=0x{x} FSTS=0x{x} all={}\n", .{
            index, registers.read(u32, gsts_reg), registers.read(u64, rtaddr_reg), registers.read(u32, fsts_reg), @intFromBool(unit.include_pci_all),
        });
        var scopes = unit.scopes();
        while (scopes.next()) |scope| diagnosticScope(scope);
    }
    // parseDmar validated every record and scope before this bounded traversal.
    var bytes = info.structures;
    while (bytes.len != 0) {
        const length = std.mem.readInt(u16, bytes[2..4], .little);
        const record = bytes[0..length];
        bytes = bytes[length..];
        if (std.mem.readInt(u16, record[0..2], .little) != 1) continue;
        kernel_log.writeFmt("RMRR segment={} base=0x{x} limit=0x{x}\n", .{
            std.mem.readInt(u16, record[6..8], .little), std.mem.readInt(u64, record[8..16], .little), std.mem.readInt(u64, record[16..24], .little),
        });
        var scopes = acpi_dmar.Scopes{ .bytes = record[24..] };
        while (scopes.next()) |scope| diagnosticScope(scope);
    }
    const igd = pci.Location{ .bus = 0, .device = 2, .function = 0 };
    if (pci.readVendorId(igd) != 0x8086 or pci.readClassCode(igd) != 3) {
        kernel_log.write("PCI 00:02.0 is not an Intel display function\n");
        return;
    }
    kernel_log.writeFmt("PCI 00:02.0 vendor=0x{x} device=0x{x} command=0x{x}\n", .{
        pci.readVendorId(igd), pci.readDeviceId(igd), pci.readConfigU16(igd, 4),
    });
}

fn diagnosticScope(scope: acpi_dmar.DeviceScope) void {
    kernel_log.writeFmt(" scope type={} bus={} path=", .{ scope.scope_type, scope.start_bus });
    for (scope.path[0..scope.path_count]) |path|
        kernel_log.writeFmt("{x}:{x}/", .{ path.device, path.function });
    kernel_log.write("\n");
}

// Only unpublished boot tables can be returned here; runtime withdrawal has
// separate drain/quarantine ownership. Keep a rejected grant in its field.
fn returnDraftPage(address: *u64) bool {
    if (address.* == 0) return true;
    driver_state.free_list.?.appendPage(address.*) catch return false;
    address.* = 0;
    return true;
}
fn releaseDraftTables() void {
    std.debug.assert(!initializationRequiresHalt());
    var released = true;
    for (0..driver_state.domain_count) |index| {
        if (!releaseDraftSecondLevel(&domainAt(index).second_level_root_paddr, 4)) released = false;
    }
    for (0..driver_state.unit_count) |index| {
        const unit = unitAt(index);
        for (&unit.context_table_paddrs) |*address| {
            if (!returnDraftPage(address)) released = false;
        }
        if (!returnDraftPage(&unit.root_paddr)) released = false;
    }
    if (released) {
        driver_state.domains.deinit();
        driver_state.units.deinit();
        driver_state.first_unit = .{};
        driver_state.domain_count = 0;
        driver_state.unit_count = 0;
    }
}

/// Only unpublished second-level tables enter here. Level-one targets are
/// firmware data, not allocations: release tables recursively, never those PFNs.
/// A rejected PMM return keeps the live parent link and all remaining ownership.
fn releaseDraftSecondLevel(address: *u64, level: u3) bool {
    return releaseDraftSecondLevelWith(address, level, returnDraftPage);
}
fn releaseDraftSecondLevelWith(address: *u64, level: u3, comptime release: fn (*u64) bool) bool {
    if (address.* == 0) return true;
    if (level > 1) {
        const page = tableAt(address.*);
        var released = true;
        for (page) |*entry| {
            var child = pageAddress(entry.*);
            if (child == 0) continue;
            if (releaseDraftSecondLevelWith(&child, level - 1, release)) entry.* = 0 else released = false;
        }
        if (!released) return false;
    }
    return release(address);
}

const firmware_dma = @import("firmware_dma.zig");

test "VT-d RMRR mappings stay outside dynamic APIs and preserve neighboring pages" {
    const fixture = try DmaWithdrawalFixture.init();
    defer fixture.deinit();
    try fixture.list.installRegions(fixture.regions);
    try fixture.list.seedContiguousRange(@intFromPtr(fixture.backing.ptr), 128);
    try TestSupport.begin(fixture.list, DmaWithdrawalFixture.device);
    const other = prepareDomain(1).?;
    other.* = .{ .device = 222, .did = 2, .second_level_root_paddr = allocTablePage().? };
    driver_state.domain_count = 2;
    const start: u64 = 0x66001000;
    driver_state.iova_end = start;
    const grant = firmware_dma.ReservedGrant{ .start = start, .end = start + 8192 };
    try std.testing.expect(mapFirmwareReserved(domainAt(0), grant));
    try std.testing.expectEqual(start | 3, reservedLeafEntry(domainAt(0), start));
    try std.testing.expectEqual((start + 4096) | 3, reservedLeafEntry(domainAt(0), start + 4096));
    try std.testing.expectEqual(@as(u64, 0), reservedLeafEntry(domainAt(0), start - 4096));
    try std.testing.expectEqual(@as(u64, 0), reservedLeafEntry(domainAt(0), grant.end));
    try std.testing.expectEqual(@as(u64, 0), reservedLeafEntry(other, start));
    try std.testing.expect(!mapPages(DmaWithdrawalFixture.device, .fromInt(start), &.{start}, true, true));
    try std.testing.expect(!mapPages(DmaWithdrawalFixture.device, .fromInt(start - 4096), &.{ start, start + 4096 }, true, true));
    try std.testing.expect(!unmapRangeForDevice(DmaWithdrawalFixture.device, .fromInt(start), 4096));
    try std.testing.expect(!reserveIova(DmaWithdrawalFixture.device, .fromInt(start), 1));
    freeIova(DmaWithdrawalFixture.device, .fromInt(start), 1);
    try std.testing.expectEqual(start | 3, reservedLeafEntry(domainAt(0), start));
    try std.testing.expectEqual(@as(usize, 0), driver_state.leaf_count);
    // The last dynamic IOVA and first reserved IOVA share the same PT page.
    const data = try fixture.list.popFront();
    try std.testing.expect(mapPages(DmaWithdrawalFixture.device, .fromInt(start - 4096), &.{data}, true, true));
    try std.testing.expect(unmapRangeForDevice(DmaWithdrawalFixture.device, .fromInt(start - 4096), 4096));
    try std.testing.expectEqual(start | 3, reservedLeafEntry(domainAt(0), start));
}

test "VT-d unpublished RMRR table OOM returns every allocated table and never data" {
    for (0..4) |available| {
        const fixture = try DmaWithdrawalFixture.init();
        defer fixture.deinit();
        try fixture.list.installRegions(fixture.regions);
        try fixture.list.seedContiguousRange(@intFromPtr(fixture.backing.ptr), 128);
        try TestSupport.begin(fixture.list, DmaWithdrawalFixture.device);
        while (fixture.list.pageCount() > available) _ = try fixture.list.popFront();
        const grant = firmware_dma.ReservedGrant{ .start = 0x66000000, .end = 0x66001000 };
        try std.testing.expectEqual(available == 3, mapFirmwareReserved(domainAt(0), grant));
        driver_state.active = false;
        try std.testing.expect(releaseDraftSecondLevel(&domainAt(0).second_level_root_paddr, 4));
        try std.testing.expectEqual(@as(u64, 0), domainAt(0).second_level_root_paddr);
        try std.testing.expectEqual(available + 1, fixture.list.pageCount());
        try std.testing.expect(!fixture.isFree(grant.start));
    }
}

test "VT-d RMRR deep return rejection retains ancestors and permits one retry" {
    const fixture = try DmaWithdrawalFixture.init();
    defer fixture.deinit();
    try fixture.list.installRegions(fixture.regions);
    try fixture.list.seedContiguousRange(@intFromPtr(fixture.backing.ptr), 128);
    try TestSupport.begin(fixture.list, DmaWithdrawalFixture.device);
    const before = fixture.list.pageCount();
    const root = &domainAt(0).second_level_root_paddr;
    try std.testing.expect(mapFirmwareReserved(domainAt(0), .{ .start = 0x66001000, .end = 0x66002000 }));
    const Reject = struct {
        var calls: usize = 0;
        fn release(address: *u64) bool {
            calls += 1;
            if (calls == 1) return false;
            return returnDraftPage(address);
        }
    };
    Reject.calls = 0;
    const root_before = root.*;
    try std.testing.expect(!releaseDraftSecondLevelWith(root, 4, Reject.release));
    try std.testing.expectEqual(@as(usize, 1), Reject.calls);
    try std.testing.expectEqual(root_before, root.*);
    try std.testing.expectEqual(before - 3, fixture.list.pageCount());
    try std.testing.expect(releaseDraftSecondLevel(root, 4));
    try std.testing.expectEqual(before + 1, fixture.list.pageCount());
    try std.testing.expectEqual(@as(u64, 0), root.*);
}

fn reservedLeafEntry(domain: *const Domain, iova: u64) u64 {
    var page = tableAt(domain.second_level_root_paddr);
    for ([_]u6{ 39, 30, 21 }) |shift| {
        const child = pageAddress(page[@intCast((iova >> shift) & 511)]);
        if (child == 0) return 0;
        page = tableAt(child);
    }
    return page[@intCast((iova >> 12) & 511)];
}

/// Boot-owned mappings have no dynamic lease/refcount. Their IOVAs are outside
/// the allocation window clipped by parseDmar, so runtime map/unmap cannot
/// withdraw them. A later domain stop still removes root permission normally.
fn mapFirmwareReserved(domain: *const Domain, grant: firmware_dma.ReservedGrant) bool {
    var physical = grant.start;
    while (physical < grant.end) : (physical += page_size) {
        var page = tableAt(domain.second_level_root_paddr);
        for ([_]u6{ 39, 30, 21 }) |shift| {
            const next = ensureChildTable(page, @intCast((physical >> shift) & 511)) orelse return false;
            page = tableAt(next);
        }
        const index: usize = @intCast((physical >> 12) & 511);
        const wanted = physical | tables.second_level_read | tables.second_level_write;
        if (page[index] != 0 and page[index] != wanted) return false;
        if (!tables.setSecondLevelEntry(page, index, physical, true, true)) return false;
        flushTableCache(&page[index], @sizeOf(u64));
    }
    return true;
}

fn installFirmwareReservedMappings(info: acpi_dmar.DmarInfo) bool {
    var reservations = info.rmrrs();
    while (reservations.next()) |reservation| {
        if (reservation.segment != 0) return false;
        const layout = @import("arch/x86_64/boot_direct_map.zig").layout orelse return false;
        const image = @import("boot/image_range.zig");
        const physical_limit = firmware_dma.platformLimit(info.hostAddressWidthBits(), layout.limit) catch return false;
        const grant = firmware_dma.validate(layout.regions,
            .{ .start = image.base_paddr, .end = image.base_paddr + image.size_bytes },
            reservation.base, reservation.last, physical_limit) catch return false;
        var scopes = reservation.scopes();
        while (scopes.next()) |scope| {
            if (resolveEndpointScope(&scope) == null) return false;
        }
        var matched = false;
        for (0..driver_state.domain_count) |index| {
            const domain = domainAt(index);
            const location = pci.locationFromResourceId(domain.device) orelse return false;
            if (!scopesMatchWith(pci, reservation.scopes(), location)) continue;
            if (ownerForLocation(location) != domain.unit_index) return false;
            const unit = unitAt(domain.unit_index);
            const bits: u7 = @as(u7, @intCast((unit.cap >> 16) & 63)) + 1;
            if (bits < 64 and grant.end > (@as(u64, 1) << @as(u6, @intCast(bits)))) return false;
            if (!mapFirmwareReserved(domain, grant)) return false;
            matched = true;
            kernel_log.writeFmt("vtd: RMRR bdf={x}:{x}:{x} identity=0x{x}..0x{x} RW scope-only\n", .{
                location.bus, location.device, location.function, grant.start, grant.end,
            });
        }
        if (!matched) return false;
    }
    return true;
}

fn selectUnits(info: acpi_dmar.DmarInfo) bool {
    var records = info.drhds();
    while (records.next()) |drhd| {
        if (drhd.segment != 0) {
            if (!builtin.is_test) kernel_log.writeFmt("vtd: unsupported DRHD segment={} base=0x{x} ignored=1\n", .{ drhd.segment, drhd.register_base });
            continue;
        }
        const unit = prepareUnit(driver_state.unit_count) orelse {
            return false;
        };
        unit.drhd = drhd;
        driver_state.unit_count += 1;
    }
    if (driver_state.unit_count == 0) {
        return false;
    }
    return true;
}

fn prepareHardwareUnit(unit: *UnitState) void {
    const drhd = unit.drhd;
    if (drhd.register_base == 0 or (drhd.register_base & (page_size - 1)) != 0) {
        logInitFailure("invalid DRHD register base");
        return;
    }
    for (0..driver_state.unit_count) |index| {
        const other = unitAt(index);
        if (other != unit and other.drhd.register_base == drhd.register_base) {
            logInitFailure("duplicate DRHD register base");
            return;
        }
    }
    if (!@import("arch/x86_64/platform.zig").kernelMmioRangeFits(drhd.register_base, page_size)) {
        logInitFailure("DRHD register page exceeds the physical address width");
        return;
    }
    if (!smp.ucMinusMmioAllowed(drhd.register_base, page_size)) {
        logInitFailure("DRHD register page lacks an all-CPU uncached proof");
        return;
    }
    // Cache evidence does not create a VA. Reserve the sparse high mapping
    // before borrowing; no device registers are read on allocation failure.
    if (!@import("arch/x86_64/platform.zig").mapBootMmioRange(drhd.register_base, page_size)) {
        logInitFailure("DRHD initial register mapping failed");
        return;
    }
    const initial_registers = @import("arch/x86_64/platform.zig").kernelMmioBorrow(drhd.register_base, page_size) orelse {
        logInitFailure("DRHD initial register borrow unavailable");
        return;
    };
    const cap = mmioRead64At(&initial_registers, cap_reg);
    const ecap = mmioRead64At(&initial_registers, ecap_reg);
    const cache_cpu = cpuid(1);
    const table_cache_line_bytes = tableCacheLineBytes(ecap, cache_cpu.ebx, cache_cpu.edx) orelse {
        logInitFailure("non-coherent page-table walker requires CLFLUSH");
        return;
    };
    const sagaw: u5 = @truncate(cap >> 8);
    const nd: u3 = @truncate(cap);
    const cm = (cap & (@as(u64, 1) << 7)) != 0;
    const rwbf = (cap & (@as(u64, 1) << 4)) != 0;
    const fault_record_offset = ((cap >> 24) & 0x3ff) * 16;
    const fault_record_count = @as(usize, @intCast((cap >> 40) & 0xff)) + 1;
    const iotlb_offset = ((ecap >> 8) & 0x3ff) * 16;
    const fault_end = fault_record_offset + @as(u64, @intCast(fault_record_count)) * 16;
    const iotlb_end = iotlb_offset + 16;
    const register_bytes = std.mem.alignForward(
        u64,
        @max(page_size, @max(fault_end, iotlb_end)),
        page_size,
    );
    if (!@import("arch/x86_64/platform.zig").kernelMmioRangeFits(drhd.register_base, register_bytes) or
        !smp.ucMinusMmioAllowed(drhd.register_base, register_bytes))
    {
        logInitFailure("DRHD extended registers lack an all-CPU uncached mapping");
        return;
    }

    if (!@import("arch/x86_64/platform.zig").mapBootMmioRange(drhd.register_base, register_bytes)) {
        logInitFailure("DRHD extended register mapping failed");
        return;
    }
    const registers = @import("arch/x86_64/platform.zig").kernelMmioBorrow(drhd.register_base, register_bytes) orelse {
        logInitFailure("DRHD extended register borrow unavailable");
        return;
    };
    // Publish only after the complete aperture has both cache evidence and
    // an owned high VA; the physical address is never a register pointer.
    unit.register_base = drhd.register_base;
    unit.registers = registers;
    unit.cap = cap;
    unit.ecap = ecap;
    unit.table_cache_line_bytes = table_cache_line_bytes;
    // CPU CLFLUSH stride is common to all walkers. Flush shared table helpers
    // conservatively when any unit is noncoherent; no coherent-only assumption
    // may leak from the first unit into another unit's page-table publication.
    driver_state.table_cache_line_bytes = @max(driver_state.table_cache_line_bytes, table_cache_line_bytes);
    unit.rwbf = rwbf;
    unit.fault_record_offset = fault_record_offset;
    unit.fault_record_count = fault_record_count;
    unit.iotlb_offset = iotlb_offset;
    unit.hardware_domain_count = domainCount(nd);
    const qi = (ecap & (@as(u64, 1) << 1)) != 0;
    kernel_log.writeFmt("vtd: mmio base=0x{x} identity_mapped=1 cache=UC source=MTRR\n", .{drhd.register_base});
    kernel_log.writeFmt(
        "vtd: CAP=0x{x} ECAP=0x{x} SAGAW=0x{x} aw48={} CM={} RWBF={} ND={} domains={} FRO=0x{x} IRO=0x{x} QI={}\n",
        .{ unit.cap, unit.ecap, sagaw, @intFromBool((sagaw & (1 << 2)) != 0), @intFromBool(cm), @intFromBool(unit.rwbf), nd, domainCount(nd), unit.fault_record_offset, unit.iotlb_offset, @intFromBool(qi) },
    );
    if ((sagaw & (1 << 2)) == 0) {
        logInitFailure("four-level second-level translation unsupported");
        return;
    }
    if ((mmioRead32(unit, gsts_reg) & gsts_tes) != 0) {
        logInitFailure("translation was already enabled");
        return;
    }

    // Finalize once, before any reservation or device FD publication. Neither
    // captureBarApertures (which runs later) nor live driver BAR writes define
    // this window. CAP.MGAW constrains input addresses independently of SAGAW.
    const mgaw: u7 = @as(u7, @intCast((cap >> 16) & 0x3f)) + 1;
    if (mgaw < 32) {
        logInitFailure("DMA input address width is below the 32-bit contract");
        return;
    }
    kernel_log.writeFmt("vtd: IOVA window start=0x{x} end=0x{x} sparse-banks=1\n", .{
        iova_window_start, driver_state.iova_end,
    });
    if (driver_state.iova_end <= iova_window_start) {
        logInitFailure("no continuous DMA window outside firmware reservations");
        return;
    }

    unit.prepared = true;
}

fn enableHardwareUnit(unit: *UnitState) bool {
    flushInitialTables(unit);
    asm volatile ("mfence" ::: .{ .memory = true });

    unit.published = true;
    mmioWrite64(unit, rtaddr_reg, unit.root_paddr);
    mmioWrite32(unit, gcmd_reg, unit.gcmd_shadow | gcmd_srtp);
    if (!wait32Set(unit, gsts_reg, gsts_rtps)) {
        return false;
    }
    if (!invalidateContextCacheGlobal(unit)) {
        return false;
    }
    if (!invalidateIotlbGlobal(unit)) {
        return false;
    }

    unit.gcmd_shadow |= gcmd_te;
    mmioWrite32(unit, gcmd_reg, unit.gcmd_shadow);
    if (!wait32Set(unit, gsts_reg, gsts_tes)) {
        return false;
    }
    return true;
}

pub fn discovered() ?*const Discovery {
    if (driver_state.discovery) |*value| return value;
    return null;
}

pub fn isActive() bool {
    return @atomicLoad(bool, &driver_state.active, .acquire);
}

fn validIovaRange(iova: u64, size: u64) bool {
    if (size == 0 or iova < iova_window_start or iova >= driver_state.iova_end) return false;
    const end, const overflow = @addWithOverflow(iova, size);
    return overflow == 0 and end <= driver_state.iova_end;
}

/// The device snapshot describes one usable continuous IOVA window, not the
/// endpoint's DMA mask. Untranslated/unknown devices have no such window.
pub fn deviceIovaWindow(device: types.DmaDeviceId) struct { start: u64 = 0, size: u64 = 0 } {
    if (!isActive() or domainForDevice(device) == null) return .{};
    return .{ .start = iova_window_start, .size = driver_state.iova_end - iova_window_start };
}

pub fn allocIova(device: types.DmaDeviceId, page_count: usize) ?u64 {
    if (!isActive()) return null;
    lock();
    defer unlock();
    const domain = domainForDevice(device) orelse {
        kernel_log.writeFmt("vtd: iova alloc failed device=0x{x} reason=no-domain\n", .{device});
        return null;
    };
    if (domain.quarantined) return null;
    if (!ensureDomainAllocator(domain)) {
        kernel_log.writeFmt("vtd: iova alloc failed device=0x{x} did={} reason=no-window\n", .{ device, domain.did });
        return null;
    }
    const iova = domain.allocator.alloc(page_count) orelse {
        kernel_log.writeFmt(
            "vtd: iova alloc failed device=0x{x} did={} reason=exhausted pages={} used_pages={} window_pages={}\n",
            .{ device, domain.did, page_count, domain.allocator.used_pages, domain.allocator.page_count },
        );
        return null;
    };
    return iova;
}

pub fn reserveIova(device: types.DmaDeviceId, iova_address: IovaAddress, page_count: usize) bool {
    const iova = iova_address.toInt();
    if (!isActive()) return false;
    lock();
    defer unlock();
    const domain = domainForDevice(device) orelse return false;
    if (domain.quarantined or !ensureDomainAllocator(domain)) return false;
    return domain.allocator.reserve(iova, page_count);
}

pub fn freeIova(device: types.DmaDeviceId, iova_address: IovaAddress, page_count: usize) void {
    const iova = iova_address.toInt();
    if (!isActive()) return;
    lock();
    defer unlock();
    const domain = domainForDevice(device) orelse return;
    if (domain.quarantined) return;
    _ = domain.allocator.free(iova, page_count);
}

pub fn mapPages(
    device: types.DmaDeviceId,
    iova_address: IovaAddress,
    paddrs: []const u64,
    readable: bool,
    writable: bool,
) bool {
    const iova = iova_address.toInt();
    if (!isActive()) return true;
    if (paddrs.len == 0 or (!readable and !writable) or (iova & (page_size - 1)) != 0) return false;
    const size = @as(u64, @intCast(paddrs.len)) * page_size;
    if (!validIovaRange(iova, size)) return false;
    for (paddrs) |paddr| {
        if ((paddr & (page_size - 1)) != 0 or paddr >= (@as(u64, 1) << 48)) return false;
    }

    lock();
    defer unlock();
    const domain = domainForDevice(device) orelse {
        kernel_log.writeFmt("vtd: map failed device=0x{x} reason=no-domain\n", .{device});
        return false;
    };
    if (domain.quarantined) return false;
    var mapped: usize = 0;
    while (mapped < paddrs.len) : (mapped += 1) {
        const page_iova = iova + @as(u64, @intCast(mapped)) * page_size;
        if (!mapPage(domain, IovaAddress.fromInt(page_iova), paddrs[mapped], readable, writable)) {
            _ = withdrawRange(domain, iova, mapped);
            if (!builtin.is_test) kernel_log.writeFmt(
                "vtd: map failed device=0x{x} did={} iova=0x{x} pages={} at={}\n",
                .{ device, domain.did, iova, paddrs.len, mapped },
            );
            if (!builtin.is_test) dumpFaultsLocked();
            return false;
        }
    }
    if (!flushMapChanges(domain)) {
        for (0..paddrs.len) |index| withdrawPage(domain, iova + index * page_size);
        quarantineDomain(domain);
        if (!builtin.is_test) kernel_log.writeFmt(
            "vtd: map failed invalidation device=0x{x} did={} iova=0x{x} pages={}\n",
            .{ device, domain.did, iova, paddrs.len },
        );
        if (!builtin.is_test) dumpFaultsLocked();
        return false;
    }
    return true;
}

fn withdrawRange(domain: *Domain, first: u64, page_count: usize) bool {
    if (domain.quarantined) return false;
    if (page_count == 0) return true;
    var index: usize = 0;
    while (index < page_count) : (index += 1) {
        withdrawPage(domain, first + @as(u64, @intCast(index)) * page_size);
    }
    if (!flushTranslationChanges(domain)) {
        quarantineDomain(domain);
        return false;
    }
    index = 0;
    while (index < page_count) : (index += 1) {
        unmapPage(domain, first + @as(u64, @intCast(index)) * page_size);
    }
    return true;
}

pub fn unmapRangeForDevice(device: types.DmaDeviceId, iova_address: IovaAddress, size: u64) bool {
    const iova = iova_address.toInt();
    if (!isActive()) return true;
    if (!validIovaRange(iova, size)) return false;
    const first = iova & ~(page_size - 1);
    const last_byte = iova + size - 1;
    const page_count = ((last_byte & ~(page_size - 1)) - first) / page_size + 1;
    lock();
    defer unlock();
    const domain = domainForDevice(device) orelse return false;
    if (!withdrawRange(domain, first, @intCast(page_count))) {
        if (!builtin.is_test) kernel_log.writeFmt(
            "vtd: unmap invalidation failed device=0x{x} did={} iova=0x{x} pages={}\n",
            .{ device, domain.did, iova, page_count },
        );
        if (!builtin.is_test) dumpFaultsLocked();
        return false;
    }
    return true;
}

fn dumpUnitFaultsLocked(unit: *UnitState) void {
    if (unit.register_base == 0) return;
    const fsts = mmioRead32(unit, fsts_reg);
    var fault_count: usize = 0;
    var record_index: usize = 0;
    while (record_index < unit.fault_record_count) : (record_index += 1) {
        const offset = unit.fault_record_offset + @as(u64, @intCast(record_index)) * 16;
        const low = mmioRead64(unit, offset);
        const high = mmioRead64(unit, offset + 8);
        if ((high & fault_record_valid) == 0) continue;
        fault_count += 1;
        kernel_log.writeFmt(
            "vtd: fault unit=0x{x} index={} source_id=0x{x} reason=0x{x} address=0x{x} type={} raw_low=0x{x} raw_high=0x{x}\n",
            .{ unit.register_base, record_index, high & 0xffff, (high >> 32) & 0xff, low & tables.page_address_mask, (high >> 62) & 0x1, low, high },
        );
        mmioWrite64(unit, offset + 8, fault_record_valid);
    }
    if (fsts != 0) mmioWrite32(unit, fsts_reg, fsts);
    kernel_log.writeFmt("vtd: faults count={} fsts=0x{x}\n", .{ fault_count, fsts });
}

fn dumpFaultsLocked() void {
    for (0..driver_state.unit_count) |index| dumpUnitFaultsLocked(unitAt(index));
}

pub fn dumpFaults() void {
    if (driver_state.unit_count == 0) return;
    lock();
    defer unlock();
    dumpFaultsLocked();
}

/// Process teardown is outside the DMA map/unmap hot path and gives smoke
/// tests an active view of both late hardware faults and mapping lifetime.
pub fn dumpRuntimeCheckpoint() void {
    if (driver_state.unit_count == 0) return;
    lock();
    defer unlock();
    kernel_log.writeFmt(
        "vtd: runtime checkpoint active={} mapped_pages={} refs={}\n",
        .{ @intFromBool(driver_state.active), driver_state.mapped_page_count, driver_state.mapping_ref_count },
    );
    dumpFaultsLocked();
}

/// Fault injection replaces only the register acknowledgement. Page tables,
/// allocation, rollback, quarantine and PMM admission remain production paths.
pub const TestSupport = if (builtin.is_test) struct {
    var fake_register_mode: bool = false;
    var fail_unit: ?*UnitState = null;
    var pool_allocator_override: ?@import("memory/owned_storage.zig").Allocator = null;
    pub var before_ack: ?*const fn () void = null;
    pub var ack_completed: bool = false;
    pub var invalidation_result: ?u64 = null;

    pub fn begin(free_list: *types.FreePageList, device: types.DmaDeviceId) !void {
        try beginWithTableCache(free_list, device, true);
    }

    pub fn beginWithTableCache(free_list: *types.FreePageList, device: types.DmaDeviceId, coherent: bool) !void {
        driver_state = .{ .free_list = free_list, .unit_count = 1, .first_unit = .{ .ecap = if (coherent) 1 else 0xf42 } };
        const cpu = cpuid(1);
        driver_state.table_cache_line_bytes = tableCacheLineBytes(driver_state.first_unit.ecap, cpu.ebx, cpu.edx) orelse
            return error.CacheMaintenanceUnavailable;
        test_fail_invalidation = false;
        before_ack = null;
        ack_completed = false;
        invalidation_result = null;
        test_last_invalidation = 0;
        test_cache_flush_count = 0;
        test_flush_count_at_invalidation = 0;
        _ = prepareDomain(0) orelse return error.OutOfMemory;
        domainAt(0).* = .{
            .device = device,
            .did = 1,
            .second_level_root_paddr = allocTablePage() orelse return error.OutOfMemory,
        };
        driver_state.domain_count = 1;
        // Default to negative caching so existing map failure/publication
        // tests exercise the invalidation path. CM=0 is tested separately.
        driver_state.first_unit.cap = cap_caching_mode | (@as(u64, 1) << 55) | (@as(u64, 1) << 54);
        driver_state.active = true;
    }

    pub fn end() void {
        fake_register_mode = false;
        fail_unit = null;
        pool_allocator_override = null;
        driver_state.leaf_metadata.deinit();
        driver_state.domains.deinit();
        driver_state.units.deinit();
        driver_state = .{};
        test_fail_invalidation = false;
        before_ack = null;
        ack_completed = false;
        invalidation_result = null;
    }

    pub fn failInvalidation(fail: bool) void {
        test_fail_invalidation = fail;
    }

    pub fn lastInvalidation() u64 {
        return test_last_invalidation;
    }

    pub fn mappingReferences() usize {
        return driver_state.mapping_ref_count;
    }

    pub fn usedPages(device: types.DmaDeviceId) usize {
        return (domainForDevice(device) orelse return 0).allocator.used_pages;
    }
} else void;

test "VT-d completion polling returns the clear sample without a second read" {
    const Reader = struct {
        var values: []const u64 = undefined;
        var reads: usize = 0;
        var last_offset: u64 = 0;

        fn read(offset: u64) u64 {
            last_offset = offset;
            const value = values[reads];
            reads += 1;
            return value;
        }
    };
    const completed = @as(u64, 2) << iotlb_iaig_shift;
    const cases = [_]struct { samples: []const u64, expected: ?u64 }{
        .{ .samples = &.{completed}, .expected = completed },
        .{ .samples = &.{ iotlb_ivt, iotlb_ivt | completed, completed }, .expected = completed },
        .{ .samples = &.{ iotlb_ivt, iotlb_ivt, iotlb_ivt }, .expected = null },
        // Zero is a completed observation, not a timeout; the caller must
        // still reject its missing actual invalidation granularity.
        .{ .samples = &.{0}, .expected = 0 },
    };
    for (cases) |case| {
        Reader.values = case.samples;
        Reader.reads = 0;
        try std.testing.expectEqual(case.expected, wait64ClearWithReader(0x108, iotlb_ivt, 3, Reader.read));
        try std.testing.expectEqual(case.samples.len, Reader.reads);
        try std.testing.expectEqual(@as(u64, 0x108), Reader.last_offset);
    }
}

test "VT-d IOVA bank returns backing without a metadata capacity failure" {
    const backing = try std.testing.allocator.alignedAlloc(u8, .fromByteUnits(4096), 8 * 4096);
    defer std.testing.allocator.free(backing);
    var free_list: types.FreePageList = .{};
    const base = @intFromPtr(backing.ptr);
    try free_list.installRegions(&.{.{ .id = 1, .start = base, .end = base + backing.len }});
    try free_list.seedContiguousRange(base, 8);
    const device = pci.resourceIdFromLocation(.{ .bus = 0, .device = 2, .function = 0 });
    try TestSupport.begin(&free_list, device);
    defer TestSupport.end();
    const bank = allocIovaBank(null).?;
    const pa = @intFromPtr(bank);
    freeIovaBank(null, bank);
    try std.testing.expect(driver_state.iova_spares == null);
    try std.testing.expect(free_list.overlaps(pa, pa + 4096));
}

test "VT-d device aperture admits beyond old fixed window and clips the real boundary" {
    const allocator = std.testing.allocator;
    const backing = try allocator.alignedAlloc(u8, .fromByteUnits(4096), 32 * 4096);
    defer allocator.free(backing);
    const free_list = try allocator.create(types.FreePageList);
    defer allocator.destroy(free_list);
    free_list.* = .{};
    try free_list.installRegions(&.{.{ .id = 0, .start = @intFromPtr(backing.ptr), .end = (@intFromPtr(backing.ptr)) + (32) * 4096 }});
    try free_list.seedContiguousRange(@intFromPtr(backing.ptr), 32);
    const device = pci.resourceIdFromLocation(.{ .bus = 0, .device = 2, .function = 0 });
    try TestSupport.begin(free_list, device);
    defer TestSupport.end();
    driver_state.iova_end = 0xb0000000;
    const window = deviceIovaWindow(device);
    try std.testing.expectEqual(iova_window_start, window.start);
    try std.testing.expectEqual(driver_state.iova_end - iova_window_start, window.size);
    try std.testing.expectEqual(@as(u64, 0), deviceIovaWindow(device + 1).size);
    try std.testing.expect(!reserveIova(device, IovaAddress.fromInt(0xb0000000), 1));
    const page = try free_list.popFront();
    const address = 0xa0000000;
    try std.testing.expect(reserveIova(device, IovaAddress.fromInt(address), 1));
    try std.testing.expect(mapPages(device, IovaAddress.fromInt(address), &.{page}, true, true));
    try std.testing.expect(unmapRangeForDevice(device, IovaAddress.fromInt(address), page_size));
    freeIova(device, IovaAddress.fromInt(address), 1);
    try std.testing.expectEqual(@as(usize, 0), TestSupport.usedPages(device));
    try std.testing.expect(domainForDevice(device).?.allocator.banks == null);
}

test "VT-d CM=0 map publication skips invalidation but withdrawal still drains" {
    const allocator = std.testing.allocator;
    const backing = try allocator.alignedAlloc(u8, .fromByteUnits(4096), 32 * 4096);
    defer allocator.free(backing);
    const free_list = try allocator.create(types.FreePageList);
    defer allocator.destroy(free_list);
    free_list.* = .{};
    try free_list.installRegions(&.{.{ .id = 0, .start = @intFromPtr(backing.ptr), .end = (@intFromPtr(backing.ptr)) + (32) * 4096 }});
    try free_list.seedContiguousRange(@intFromPtr(backing.ptr), 32);
    const device = pci.resourceIdFromLocation(.{ .bus = 0, .device = 2, .function = 0 });
    try TestSupport.begin(free_list, device);
    defer TestSupport.end();
    driver_state.first_unit.cap &= ~cap_caching_mode;
    const page = try free_list.popFront();
    const other = try free_list.popFront();
    try std.testing.expect(reserveIova(device, IovaAddress.fromInt(iova_window_start), 1));
    TestSupport.failInvalidation(true);
    try std.testing.expect(mapPages(device, IovaAddress.fromInt(iova_window_start), &.{page}, true, true));
    try std.testing.expect(mapPages(device, IovaAddress.fromInt(iova_window_start), &.{page}, true, true));
    try std.testing.expect(!mapPages(device, IovaAddress.fromInt(iova_window_start), &.{other}, true, true));
    try std.testing.expect(!mapPages(device, IovaAddress.fromInt(iova_window_start), &.{page}, true, false));
    try std.testing.expectEqual(@as(u64, 0), TestSupport.lastInvalidation());
    try std.testing.expectEqual(@as(usize, 2), TestSupport.mappingReferences());
    try std.testing.expect(!unmapRangeForDevice(device, IovaAddress.fromInt(iova_window_start), page_size));
    try std.testing.expectEqual(domainInvalidationCommand(driver_state.first_unit.cap, 1), TestSupport.lastInvalidation());
    try std.testing.expect(deviceQuarantined(device));
    try std.testing.expectEqual(@as(usize, 2), TestSupport.mappingReferences());
    try std.testing.expectError(types.KernelError.PageQuarantined, free_list.appendPage(page));
}

test "VT-d endpoint scope coverage is exact and excludes non-PCI scopes" {
    const location = pci.Location{ .bus = 0, .device = 3, .function = 0 };
    var drhd = acpi_dmar.Drhd{};

    try std.testing.expect(!explicitScopeMatches(drhd, location));
    var scope_bytes = [_]u8{ 1, 8, 0, 0, 0, 0, 3, 0 };
    drhd.scope_bytes = &scope_bytes;
    drhd.scope_count = 1;
    try std.testing.expect(explicitScopeMatches(drhd, location));
    try std.testing.expect(!explicitScopeMatches(
        drhd,
        .{ .bus = 0, .device = 4, .function = 0 },
    ));

    scope_bytes[0] = 3;
    try std.testing.expect(!explicitScopeMatches(drhd, location));
    scope_bytes[0] = 1;
    scope_bytes[6] = 32;
    try std.testing.expect(!explicitScopeMatches(drhd, location));
    // Fallback is tested through ownerForLocation, not the explicit matcher.
}

test "VT-d lifetime domain stop retains tree and permits blocked mapping changes" {
    const allocator = std.testing.allocator;
    const backing = try allocator.alignedAlloc(u8, .fromByteUnits(4096), 64 * 4096);
    defer allocator.free(backing);
    const free_list = try allocator.create(types.FreePageList);
    defer allocator.destroy(free_list);
    free_list.* = .{};
    try free_list.installRegions(&.{.{ .id = 0, .start = @intFromPtr(backing.ptr), .end = (@intFromPtr(backing.ptr)) + (64) * 4096 }});
    try free_list.seedContiguousRange(@intFromPtr(backing.ptr), 64);
    const device = pci.resourceIdFromLocation(.{ .bus = 0, .device = 2, .function = 0 });
    const other = pci.resourceIdFromLocation(.{ .bus = 0, .device = 3, .function = 0 });
    try TestSupport.beginWithTableCache(free_list, device, false);
    defer TestSupport.end();
    _ = prepareDomain(1) orelse return error.OutOfMemory;
    domainAt(1).* = .{
        .device = other,
        .did = 2,
        .second_level_root_paddr = allocTablePage() orelse return error.OutOfMemory,
    };
    driver_state.domain_count = 2;
    const page = try free_list.popFront();
    const iova = iova_window_start;
    try std.testing.expect(reserveIova(device, IovaAddress.fromInt(iova), 2));
    try std.testing.expect(reserveIova(other, IovaAddress.fromInt(iova), 1));
    try std.testing.expect(mapPages(other, IovaAddress.fromInt(iova), &.{page}, true, true));
    const other_root = tableAt(domainAt(1).second_level_root_paddr).*;
    const domain = domainAt(0);
    const root = tableAt(domain.second_level_root_paddr);
    try setDeviceDmaEnabled(device, false);
    try std.testing.expect(mapPages(device, IovaAddress.fromInt(iova), &.{page}, true, false));
    const child = pageAddress(root[0]);
    try std.testing.expect(child != 0);
    try std.testing.expectEqual(@as(u64, 0), root[0] & 3);
    const first_leaf = walkToLeaf(domain, iova, false).?;
    const first_entry = first_leaf.table[first_leaf.index];
    for (0..8) |_| {
        // Alias the same RAM at a distinct IOVA while the original map lives.
        try std.testing.expect(mapPages(device, IovaAddress.fromInt(iova + page_size), &.{page}, false, true));
        try std.testing.expectEqual(child, root[0]);
        try std.testing.expectEqual(first_entry, first_leaf.table[first_leaf.index]);
        test_cache_flush_count = 0;
        try setDeviceDmaEnabled(device, true);
        try std.testing.expectEqual(child | 3, root[0]);
        try std.testing.expectEqual(@as(usize, 3), TestSupport.mappingReferences());
        try std.testing.expectEqual(@as(usize, 2), TestSupport.usedPages(device));
        try std.testing.expectEqual(@as(usize, 1), test_flush_count_at_invalidation);
        try std.testing.expectEqual(domain.second_level_root_paddr, test_cache_flush_events[0].address);
        try std.testing.expectEqual(child | 3, test_cache_flush_events[0].first_word);
        try setDeviceDmaEnabled(device, false);
        try std.testing.expectEqual(child, root[0]);
        try std.testing.expectEqual(iotlb_read_drain | iotlb_write_drain, TestSupport.lastInvalidation() & (iotlb_read_drain | iotlb_write_drain));
        try std.testing.expect(unmapRangeForDevice(device, IovaAddress.fromInt(iova + page_size), page_size));
        try std.testing.expectEqual(@as(usize, 2), TestSupport.mappingReferences());
        try std.testing.expectEqualSlices(u64, &other_root, tableAt(domainAt(1).second_level_root_paddr));
    }
    try std.testing.expect(unmapRangeForDevice(device, IovaAddress.fromInt(iova), page_size));
    try setDeviceDmaEnabled(device, true);
    try setDeviceDmaEnabled(device, false);
    try std.testing.expectEqual(@as(usize, 1), TestSupport.mappingReferences());
}

test "VT-d lifetime domain stop fails closed and requires both drain capabilities" {
    const allocator = std.testing.allocator;
    const backing = try allocator.alignedAlloc(u8, .fromByteUnits(4096), 32 * 4096);
    defer allocator.free(backing);
    const free_list = try allocator.create(types.FreePageList);
    defer allocator.destroy(free_list);
    const device = pci.resourceIdFromLocation(.{ .bus = 0, .device = 2, .function = 0 });
    inline for (.{ false, true }) |enable| {
        free_list.* = .{};
        try free_list.installRegions(&.{.{ .id = 0, .start = @intFromPtr(backing.ptr), .end = (@intFromPtr(backing.ptr)) + (32) * 4096 }});
        try free_list.seedContiguousRange(@intFromPtr(backing.ptr), 32);
        try TestSupport.begin(free_list, device);
        defer TestSupport.end();
        const page = try free_list.popFront();
        const iova = iova_window_start;
        try std.testing.expect(reserveIova(device, IovaAddress.fromInt(iova), 1));
        try std.testing.expect(mapPages(device, IovaAddress.fromInt(iova), &.{page}, true, true));
        const root = tableAt(domainAt(0).second_level_root_paddr);
        const entry = root[0];
        for ([_]u64{ 0, @as(u64, 1) << 54, @as(u64, 1) << 55 }) |cap| {
            driver_state.first_unit.cap = cap;
            try std.testing.expectError(error.Unsupported, setDeviceDmaEnabled(device, false));
            try std.testing.expectEqual(entry, root[0]);
            try std.testing.expect(!deviceQuarantined(device));
        }
        driver_state.first_unit.cap = (@as(u64, 1) << 54) | (@as(u64, 1) << 55);
        try setDeviceDmaEnabled(device, !enable);
        TestSupport.failInvalidation(true);
        try std.testing.expectError(error.DrainFailed, setDeviceDmaEnabled(device, enable));
        try std.testing.expect(deviceQuarantined(device));
        try std.testing.expectEqual(entry & tables.page_address_mask, root[0]);
        try std.testing.expectEqual(@as(usize, 1), TestSupport.mappingReferences());
        try std.testing.expectEqual(@as(usize, 1), TestSupport.usedPages(device));
        try std.testing.expect(physicalRangeQuarantined(page, 1));
        TestSupport.failInvalidation(false);
        try std.testing.expectError(error.Quarantined, setDeviceDmaEnabled(device, true));
        try std.testing.expect(!mapPages(device, IovaAddress.fromInt(iova + page_size), &.{page}, true, true));
        try std.testing.expect(!unmapRangeForDevice(device, IovaAddress.fromInt(iova), page_size));
        try std.testing.expectError(types.KernelError.PageQuarantined, free_list.appendPage(page));
    }
}

test "VT-d lifetime reserves exact IOVA and releases SG pages only after drain" {
    try std.testing.expect(domainInvalidationCompleted(@as(u64, 1) << iotlb_iaig_shift));
    try std.testing.expect(domainInvalidationCompleted(@as(u64, 2) << iotlb_iaig_shift));
    try std.testing.expect(!domainInvalidationCompleted(0));
    try std.testing.expect(!domainInvalidationCompleted(@as(u64, 3) << iotlb_iaig_shift));
    try std.testing.expect(!domainInvalidationCompleted(iotlb_ivt | (@as(u64, 2) << iotlb_iaig_shift)));
    const allocator = std.testing.allocator;
    const backing = try allocator.alignedAlloc(u8, .fromByteUnits(4096), 32 * 4096);
    defer allocator.free(backing);
    const free_list = try allocator.create(types.FreePageList);
    defer allocator.destroy(free_list);
    free_list.* = .{};
    try free_list.installRegions(&.{.{ .id = 0, .start = @intFromPtr(backing.ptr), .end = (@intFromPtr(backing.ptr)) + (32) * 4096 }});
    try free_list.seedContiguousRange(@intFromPtr(backing.ptr), 32);
    const device = pci.resourceIdFromLocation(.{ .bus = 0, .device = 2, .function = 0 });
    try TestSupport.begin(free_list, device);
    defer TestSupport.end();
    const first = try free_list.popFront();
    _ = try free_list.popFront();
    const second = try free_list.popFront();
    const iova = iova_window_start + 0x4000;
    try std.testing.expect(reserveIova(device, IovaAddress.fromInt(iova), 2));
    try std.testing.expect(!reserveIova(device, IovaAddress.fromInt(iova + page_size), 2));
    try std.testing.expect(!reserveIova(device, IovaAddress.fromInt(iova_window_end), 1));
    try std.testing.expect(!reserveIova(device, IovaAddress.fromInt(iova + 1), 1));
    try std.testing.expectEqual(@as(usize, 2), TestSupport.usedPages(device));
    try std.testing.expect(mapPages(device, IovaAddress.fromInt(iova), &.{ first, second }, true, true));
    try std.testing.expectEqual(@as(usize, 0), test_cache_flush_count);
    try std.testing.expectEqual(@as(usize, 2), TestSupport.mappingReferences());
    try std.testing.expect(unmapRangeForDevice(device, IovaAddress.fromInt(iova), 2 * page_size));
    try std.testing.expectEqual(iotlb_read_drain | iotlb_write_drain, TestSupport.lastInvalidation() & (iotlb_read_drain | iotlb_write_drain));
    try std.testing.expectEqual(@as(usize, 0), TestSupport.mappingReferences());
    freeIova(device, IovaAddress.fromInt(iova), 2);
    try std.testing.expectEqual(@as(usize, 0), TestSupport.usedPages(device));
    try std.testing.expect(reserveIova(device, IovaAddress.fromInt(iova), 2));
    try free_list.appendPage(first);
    try free_list.appendPage(second);
}

test "VT-d lifetime failed drain retains IOVA and actual PMM backing" {
    const allocator = std.testing.allocator;
    const backing = try allocator.alignedAlloc(u8, .fromByteUnits(4096), 32 * 4096);
    defer allocator.free(backing);
    const free_list = try allocator.create(types.FreePageList);
    defer allocator.destroy(free_list);
    free_list.* = .{};
    try free_list.installRegions(&.{.{ .id = 0, .start = @intFromPtr(backing.ptr), .end = (@intFromPtr(backing.ptr)) + (32) * 4096 }});
    try free_list.seedContiguousRange(@intFromPtr(backing.ptr), 32);
    const device = pci.resourceIdFromLocation(.{ .bus = 0, .device = 2, .function = 0 });
    try TestSupport.begin(free_list, device);
    defer TestSupport.end();
    const first = try free_list.popFront();
    _ = try free_list.popFront();
    const second = try free_list.popFront();
    const iova = iova_window_start + 0x8000;
    try std.testing.expect(reserveIova(device, IovaAddress.fromInt(iova), 2));
    try std.testing.expect(mapPages(device, IovaAddress.fromInt(iova), &.{ first, second }, true, true));
    try std.testing.expectEqual(@import("kernel_abi_root").capsule_abi.snapshot_flag_dma_translated, dmaSnapshotFlags(device));
    try std.testing.expectEqual(@as(u64, 0), dmaSnapshotFlags(device + 1));
    TestSupport.failInvalidation(true);
    try std.testing.expect(!unmapRangeForDevice(device, IovaAddress.fromInt(iova), 2 * page_size));
    try std.testing.expect(deviceQuarantined(device));
    try std.testing.expectEqual(@import("kernel_abi_root").capsule_abi.snapshot_flag_dma_quarantined, dmaSnapshotFlags(device));
    try std.testing.expectEqual(@as(usize, 2), TestSupport.mappingReferences());
    freeIova(device, IovaAddress.fromInt(iova), 2);
    try std.testing.expectEqual(@as(usize, 2), TestSupport.usedPages(device));
    try std.testing.expect(!reserveIova(device, IovaAddress.fromInt(iova), 2));
    try std.testing.expect(!mapPages(device, IovaAddress.fromInt(iova), &.{ first, second }, true, true));
    const free_pages = free_list.pageCount();
    try std.testing.expectError(types.KernelError.PageQuarantined, free_list.appendPage(first));
    try std.testing.expectError(types.KernelError.PageQuarantined, free_list.appendContiguousRange(first, 3));
    try std.testing.expectEqual(free_pages, free_list.pageCount());
    try std.testing.expect(physicalRangeQuarantined(first + 1, 1));
    try std.testing.expect(!physicalRangeQuarantined(first + page_size, 1));
    try std.testing.expect(physicalRangeQuarantined(std.math.maxInt(u64) - 1, 2));
    // Reinitialization and a later successful register response must not
    // discard the retained references of an already-faulted device.
    init(0, free_list);
    TestSupport.failInvalidation(false);
    try std.testing.expect(!unmapRangeForDevice(device, IovaAddress.fromInt(iova), 2 * page_size));
    try std.testing.expect(physicalRangeQuarantined(first, 1));
}

test "VT-d lifetime failed map rollback retains the published physical pages" {
    const allocator = std.testing.allocator;
    const backing = try allocator.alignedAlloc(u8, .fromByteUnits(4096), 32 * 4096);
    defer allocator.free(backing);
    const free_list = try allocator.create(types.FreePageList);
    defer allocator.destroy(free_list);
    free_list.* = .{};
    try free_list.installRegions(&.{.{ .id = 0, .start = @intFromPtr(backing.ptr), .end = (@intFromPtr(backing.ptr)) + (32) * 4096 }});
    try free_list.seedContiguousRange(@intFromPtr(backing.ptr), 32);
    const device = pci.resourceIdFromLocation(.{ .bus = 0, .device = 2, .function = 0 });
    try TestSupport.begin(free_list, device);
    defer TestSupport.end();
    const page = try free_list.popFront();
    const iova = iova_window_start;
    try std.testing.expect(reserveIova(device, IovaAddress.fromInt(iova), 1));
    TestSupport.failInvalidation(true);
    try std.testing.expect(!mapPages(device, IovaAddress.fromInt(iova), &.{page}, true, true));
    freeIova(device, IovaAddress.fromInt(iova), 1);
    try std.testing.expectEqual(@as(usize, 1), TestSupport.usedPages(device));
    try std.testing.expectError(types.KernelError.PageQuarantined, free_list.appendPage(page));
}

test "VT-d lifetime partial map allocation failure rolls back or quarantines only published pages" {
    inline for (.{ false, true }) |fail_drain| {
        const allocator = std.testing.allocator;
        const backing = try allocator.alignedAlloc(u8, .fromByteUnits(4096), 32 * 4096);
        defer allocator.free(backing);
        const free_list = try allocator.create(types.FreePageList);
        defer allocator.destroy(free_list);
        free_list.* = .{};
        try free_list.installRegions(&.{.{ .id = 0, .start = @intFromPtr(backing.ptr), .end = (@intFromPtr(backing.ptr)) + (32) * 4096 }});
        try free_list.seedContiguousRange(@intFromPtr(backing.ptr), 32);
        const device = pci.resourceIdFromLocation(.{ .bus = 0, .device = 2, .function = 0 });
        try TestSupport.begin(free_list, device);
        defer TestSupport.end();
        const first = try free_list.popFront();
        const second = try free_list.popFront();
        // A range crossing a PT boundary needs an additional leaf and ledger
        // for its second page. Leave RAM for only the first page's walk.
        const iova = iova_window_start + 2 * 1024 * 1024 - page_size;
        try std.testing.expect(reserveIova(device, IovaAddress.fromInt(iova), 2));
        while (free_list.pageCount() > 4) _ = try free_list.popFront();
        TestSupport.failInvalidation(fail_drain);
        try std.testing.expect(!mapPages(device, IovaAddress.fromInt(iova), &.{ first, second }, true, true));
        try std.testing.expectEqual(@as(usize, @intFromBool(fail_drain)), TestSupport.mappingReferences());
        freeIova(device, IovaAddress.fromInt(iova), 2);
        try std.testing.expectEqual(@as(usize, if (fail_drain) 2 else 0), TestSupport.usedPages(device));
        if (fail_drain) {
            try std.testing.expectError(types.KernelError.PageQuarantined, free_list.appendPage(first));
        } else {
            try free_list.appendPage(first);
        }
        try free_list.appendPage(second);
    }
}

test "VT-d lifetime failed close retains FD and owner death cannot free backing" {
    const kernel = @import("kernel.zig");
    const allocator = std.testing.allocator;
    const backing = try allocator.alignedAlloc(u8, .fromByteUnits(4096), 32 * 4096);
    defer allocator.free(backing);
    const runtime = try allocator.alignedAlloc(u8, .fromByteUnits(4096), kernel.runtimeStorageBytes());
    defer allocator.free(runtime);
    try std.testing.expect(kernel.initRuntimeStorage(runtime));
    const state = try allocator.create(kernel.KernelState);
    defer allocator.destroy(state);
    try state.initFromDetectedRegionsInPlace(1);
    const free_list = try allocator.create(types.FreePageList);
    defer allocator.destroy(free_list);
    free_list.* = .{};
    try free_list.installRegions(&.{.{ .id = 0, .start = @intFromPtr(backing.ptr), .end = (@intFromPtr(backing.ptr)) + (32) * 4096 }});
    try free_list.seedContiguousRange(@intFromPtr(backing.ptr), 32);
    const owner = kernel.processPrincipalFromIndex(0).?;
    const device = pci.resourceIdFromLocation(.{ .bus = 0, .device = 2, .function = 0 });
    try TestSupport.begin(free_list, device);
    defer TestSupport.end();
    const page = try free_list.popFront();
    const vmo_fd = try state.createAnonymousVmoFd(owner, page_size, .{ .close = true, .map_read = true, .map_write = true }, .{}, 16);
    const vmo = state.nativeVmoRefForFd(owner, vmo_fd).?;
    try state.installNativeVmoPages(vmo, 0, &.{page}, free_list);
    const va: u64 = 0x4400_0000;
    _ = try state.mmapFd(owner, vmo_fd, va, page_size, .{ .read = true, .write = true }, .{ .anonymous = true }, 0);
    const iova = iova_window_start;
    try std.testing.expect(reserveIova(device, IovaAddress.fromInt(iova), 1));
    try std.testing.expect(mapPages(device, IovaAddress.fromInt(iova), &.{page}, true, true));
    const dma_fd = try state.createDmaMappingFd(owner, .{ .device = device, .user_va = va, .iova = iova, .size = page_size }, .{ .close = true }, .{}, 16);
    try state.closeFdWithFreeList(owner, vmo_fd, free_list);
    TestSupport.failInvalidation(true);
    try std.testing.expectError(types.KernelError.InvalidState, state.closeFdWithFreeList(owner, dma_fd, free_list));
    try std.testing.expect(state.fdEntryConst(owner, dma_fd) != null);
    const free_pages = free_list.pageCount();
    state.releasePrincipalNativeMemory(owner, free_list);
    try std.testing.expect(state.fdEntryConst(owner, dma_fd) == null);
    try std.testing.expectEqual(@as(?u32, null), state.nativeVmoRefCount(vmo));
    try std.testing.expectEqual(free_pages, free_list.pageCount());
    try std.testing.expectEqual(@as(usize, 1), TestSupport.mappingReferences());
    try std.testing.expect(physicalRangeQuarantined(page, 1));
    try std.testing.expectError(types.KernelError.PageQuarantined, free_list.appendPage(page));
}

test "VT-d lifetime noncoherent table cache publication precedes link and invalidation" {
    try std.testing.expectEqual(@as(?usize, 0), tableCacheLineBytes(1, 0, 0));
    try std.testing.expectEqual(@as(?usize, 64), tableCacheLineBytes(0xf42, 8 << 8, 1 << 19));
    try std.testing.expectEqual(@as(?usize, null), tableCacheLineBytes(0xf42, 8 << 8, 0));
    try std.testing.expectEqual(@as(?usize, null), tableCacheLineBytes(0xf42, 0, 1 << 19));
    try std.testing.expectEqual(@as(?usize, null), tableCacheLineBytes(0xf42, 3 << 8, 1 << 19));
    const allocator = std.testing.allocator;
    const backing = try allocator.alignedAlloc(u8, .fromByteUnits(4096), 32 * 4096);
    defer allocator.free(backing);
    const free_list = try allocator.create(types.FreePageList);
    defer allocator.destroy(free_list);
    free_list.* = .{};
    try free_list.installRegions(&.{.{ .id = 0, .start = @intFromPtr(backing.ptr), .end = (@intFromPtr(backing.ptr)) + (32) * 4096 }});
    try free_list.seedContiguousRange(@intFromPtr(backing.ptr), 32);
    const device = pci.resourceIdFromLocation(.{ .bus = 0, .device = 2, .function = 0 });
    try TestSupport.beginWithTableCache(free_list, device, false);
    defer TestSupport.end();
    try std.testing.expect(driver_state.table_cache_line_bytes != 0);
    // Exercise the same initial-root publication helper that precedes SRTP.
    driver_state.first_unit.root_paddr = allocTablePage().?;
    const context = allocTablePage().?;
    driver_state.first_unit.context_table_paddrs[0] = context;
    try std.testing.expect(tables.setRootEntry(tableAt(driver_state.first_unit.root_paddr), 0, context));
    try std.testing.expect(tables.setContextEntry(tableAt(context), 0, domainAt(0).second_level_root_paddr, 1));
    test_cache_flush_count = 0;
    flushInitialTables(&driver_state.first_unit);
    try std.testing.expectEqual(@as(usize, 2), test_cache_flush_count);
    try std.testing.expectEqual(context, test_cache_flush_events[0].address);
    try std.testing.expectEqual(driver_state.first_unit.root_paddr, test_cache_flush_events[1].address);
    try std.testing.expectEqual(context | tables.root_present, test_cache_flush_events[1].first_word);

    const first = try free_list.popFront();
    const second = try free_list.popFront();
    const iova = iova_window_start;
    try std.testing.expect(reserveIova(device, IovaAddress.fromInt(iova), 2));
    test_cache_flush_count = 0;
    try std.testing.expect(mapPages(device, IovaAddress.fromInt(iova), &.{ first, second }, true, true));
    try std.testing.expectEqual(@as(usize, 8), test_cache_flush_count);
    try std.testing.expectEqual(test_cache_flush_count, test_flush_count_at_invalidation);
    // Each zeroed child reaches RAM before a parent entry publishes it.
    for (0..3) |level| {
        const child = test_cache_flush_events[level * 2];
        const parent = test_cache_flush_events[level * 2 + 1];
        try std.testing.expectEqual(@as(usize, page_size), child.length);
        try std.testing.expectEqual(@as(u64, 0), child.first_word);
        try std.testing.expectEqual(child.address | tables.second_level_read | tables.second_level_write, parent.first_word);
    }
    try std.testing.expectEqual(first | tables.second_level_read | tables.second_level_write, test_cache_flush_events[6].first_word);
    try std.testing.expectEqual(second | tables.second_level_read | tables.second_level_write, test_cache_flush_events[7].first_word);
    test_cache_flush_count = 0;
    try std.testing.expect(unmapRangeForDevice(device, IovaAddress.fromInt(iova), 2 * page_size));
    try std.testing.expectEqual(@as(usize, 2), test_flush_count_at_invalidation);
    try std.testing.expectEqual(@as(usize, 4), test_cache_flush_count);
    try std.testing.expectEqual(first, test_cache_flush_events[0].first_word);
    try std.testing.expectEqual(second, test_cache_flush_events[1].first_word);
    try std.testing.expectEqual(@as(u64, 0), test_cache_flush_events[2].first_word);
    try std.testing.expectEqual(@as(u64, 0), test_cache_flush_events[3].first_word);
    freeIova(device, IovaAddress.fromInt(iova), 2);
    try free_list.appendPage(first);
    try free_list.appendPage(second);
}

// withdrawal contract fixtures use real owner cleanup and PMM admission. Only the existing
// invalidation ACK boundary is substituted; no production observer is added.
const DmaWithdrawalFixture = if (builtin.is_test) struct {
    backing: []align(4096) u8,
    list: *types.FreePageList,
    regions: []@import("memory/free_page_list.zig").RegionBoundary,
    const device: types.DmaDeviceId = 0x1001;
    const address: u64 = iova_window_start;

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

test "VT-d withdrawal contract owner cleanup waits for ACK and failed cleanup retains backing" {
    const kernel = @import("kernel.zig");
    const Observer = struct {
        var target: u64 = 0;
        var fixture: *const DmaWithdrawalFixture = undefined;
        var before_count: usize = 0;
        var returned: usize = 0;
        var violated: bool = false;
        fn beforeAck() void {
            before_count += 1;
            const leaf = walkToLeaf(domainAt(0), DmaWithdrawalFixture.address, false).?;
            // Permissions must already be gone, but physical identity and the
            // last reference must survive until the ACK model completes.
            if (leaf.table[leaf.index] != target or refcountsAt(leaf.metadata.refcounts_paddr)[leaf.index] != 1 or fixture.isFree(target)) violated = true;
        }
        fn beforeReturn(start: u64, pages: usize) void {
            if (target < start or target >= start + pages * page_size) return;
            returned += 1;
            if (!TestSupport.ack_completed or before_count == 0 or
                @atomicLoad(u8, &driver_state.lock_word.value, .acquire) != 0)
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
        const retained_leaf = walkToLeaf(domainAt(0), DmaWithdrawalFixture.address, false).?;
        const retained_root = domainAt(0).second_level_root_paddr;
        const retained_l3 = pageAddress(tableAt(retained_root)[0]);
        const retained_l2 = pageAddress(tableAt(retained_l3)[0]);
        const retained_pages = [_]u64{ retained_root, retained_l3, retained_l2, retained_leaf.metadata.table_paddr, retained_leaf.metadata.refcounts_paddr };
        Observer.target = page;
        Observer.fixture = &fixture;
        Observer.before_count = 0;
        Observer.returned = 0;
        Observer.violated = false;
        TestSupport.ack_completed = false;
        TestSupport.before_ack = Observer.beforeAck;
        @import("memory/free_page_list.zig").ReturnProbe.before_publish = Observer.beforeReturn;
        TestSupport.failInvalidation(fail);
        // This is the actual forced owner cleanup, without a preceding failed
        // explicit close that could hide a missing destructor drain.
        owner_state.releasePrincipalNativeMemory(owner, fixture.list);
        TestSupport.before_ack = null;
        for (retained_pages) |retained| try std.testing.expect(!fixture.isFree(retained));
        try std.testing.expect(!Observer.violated);
        try std.testing.expect(Observer.before_count > 0);
        try std.testing.expectEqual(@as(usize, if (fail) 0 else 1), Observer.returned);
        try std.testing.expectEqual(!fail, fixture.isFree(page));
        if (fail) {
            TestSupport.failInvalidation(false);
            try std.testing.expectError(types.KernelError.PageQuarantined, fixture.list.appendPage(page));
            try std.testing.expectError(types.KernelError.PageQuarantined, fixture.list.appendContiguousRange(page, 2));
            while (fixture.list.popFront()) |available| {
                try std.testing.expect(available != page);
            } else |_| {}
        }
    }
}

test "VT-d withdrawal contract quarantine leaves stay immutable while another domain adds leaves" {
    const fixture = try DmaWithdrawalFixture.init();
    defer fixture.deinit();
    try fixture.list.installRegions(fixture.regions);
    try fixture.list.seedContiguousRange(@intFromPtr(fixture.backing.ptr), 128);
    try TestSupport.begin(fixture.list, DmaWithdrawalFixture.device);
    const page = try fixture.list.popFront();
    try std.testing.expect(reserveIova(DmaWithdrawalFixture.device, IovaAddress.fromInt(DmaWithdrawalFixture.address), 1));
    try std.testing.expect(mapPages(DmaWithdrawalFixture.device, IovaAddress.fromInt(DmaWithdrawalFixture.address), &.{page}, true, true));
    const fail = true;
    TestSupport.failInvalidation(fail);
    try std.testing.expect(!unmapRangeForDevice(DmaWithdrawalFixture.device, IovaAddress.fromInt(DmaWithdrawalFixture.address), page_size));
    TestSupport.failInvalidation(false);
    const leaf = walkToLeaf(domainAt(0), DmaWithdrawalFixture.address, false).?;
    const old_entries = leaf.table.*;
    const old_refs = refcountsAt(leaf.metadata.refcounts_paddr).*;
    const old_count = driver_state.leaf_count;
    const old_used = domainAt(0).allocator.used_pages;
    try std.testing.expect(!mapPages(DmaWithdrawalFixture.device, IovaAddress.fromInt(DmaWithdrawalFixture.address), &.{page}, true, true));
    try std.testing.expect(!unmapRangeForDevice(DmaWithdrawalFixture.device, IovaAddress.fromInt(DmaWithdrawalFixture.address), page_size));
    try std.testing.expectError(error.Quarantined, setDeviceDmaEnabled(DmaWithdrawalFixture.device, true));
    try std.testing.expect(allocIova(DmaWithdrawalFixture.device, 1) == null);
    try std.testing.expect(!reserveIova(DmaWithdrawalFixture.device, IovaAddress.fromInt(DmaWithdrawalFixture.address + page_size), 1));
    freeIova(DmaWithdrawalFixture.device, IovaAddress.fromInt(DmaWithdrawalFixture.address), 1);
    try std.testing.expectEqual(old_used, domainAt(0).allocator.used_pages);
    try std.testing.expectEqual(old_count, driver_state.leaf_count);
    const other: types.DmaDeviceId = 0x2001;
    _ = prepareDomain(1) orelse return error.OutOfMemory;
    domainAt(1).* = .{ .device = other, .did = 2, .second_level_root_paddr = allocTablePage() orelse return error.OutOfMemory };
    driver_state.domain_count = 2;
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
    try std.testing.expectEqualSlices(u16, &old_refs, refcountsAt(leaf.metadata.refcounts_paddr));
    try std.testing.expectError(types.KernelError.PageQuarantined, fixture.list.appendPage(page));
}

test "VT-d withdrawal contract leaf initialization never overwrites the atomic publication flag" {
    // Dynamic interleaving alone cannot prove the absence of a data race.
    // Bound this source check to the publication boundary read by PMM; do not police unrelated assignments.
    const source = @embedFile("vtd.zig");
    const first = std.mem.indexOf(u8, source, "    // PMM readers follow only fully initialized stable bodies").?;
    const last = std.mem.indexOfPos(u8, source, first, "leaf_count += 1;").?;
    const initialization = source[first..last];
    try std.testing.expect(std.mem.indexOf(u8, initialization, "result.* =") == null);
    try std.testing.expect(std.mem.indexOf(u8, initialization, "result.quarantined =") == null);
    try std.testing.expect(std.mem.indexOf(u8, initialization, "result.table_paddr = table_paddr;") != null);
}

test "VT-d withdrawal contract quarantine rejected IOVA banks never enter reusable spares" {
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
    TestSupport.failInvalidation(fail);
    try std.testing.expect(!unmapRangeForDevice(DmaWithdrawalFixture.device, IovaAddress.fromInt(DmaWithdrawalFixture.address), page_size));
    TestSupport.failInvalidation(false);
    freeIovaBank(null, bank);
    try std.testing.expectEqual(bank, driver_state.iova_quarantined.?);
    try std.testing.expect(driver_state.iova_spares == null);
    const replacement = allocIovaBank(null).?;
    try std.testing.expect(replacement != bank);
    try std.testing.expect(!fixture.isFree(page));
    try std.testing.expectError(types.KernelError.PageQuarantined, fixture.list.appendPage(page));

    const replacement_pa = @intFromPtr(replacement);
    freeIovaBank(null, replacement);
    try std.testing.expect(fixture.isFree(replacement_pa));
    try std.testing.expectEqual(bank, driver_state.iova_quarantined.?);
}

test "VT-d withdrawal contract missing DMA drain proof quarantines runtime withdrawal" {
    for ([_]u64{ 0, @as(u64, 1) << 54, @as(u64, 1) << 55, 0xd2008c222f0606 }) |cap| {
        const fixture = try DmaWithdrawalFixture.init();
        defer fixture.deinit();
        try fixture.list.installRegions(fixture.regions);
        try fixture.list.seedContiguousRange(@intFromPtr(fixture.backing.ptr), 128);
        try TestSupport.begin(fixture.list, DmaWithdrawalFixture.device);
        const page = try fixture.list.popFront();
        try std.testing.expect(mapPages(DmaWithdrawalFixture.device, IovaAddress.fromInt(DmaWithdrawalFixture.address), &.{page}, true, true));
        driver_state.first_unit.cap = cap;
        const supported = cap == 0xd2008c222f0606;
        try std.testing.expectEqual(supported, unmapRangeForDevice(DmaWithdrawalFixture.device, IovaAddress.fromInt(DmaWithdrawalFixture.address), page_size));
        try std.testing.expectEqual(!supported, deviceQuarantined(DmaWithdrawalFixture.device));
        if (supported) try fixture.list.appendPage(page) else try std.testing.expectError(error.PageQuarantined, fixture.list.appendPage(page));
    }
}

test "VT-d withdrawal contract invalid completion samples retain backing" {
    for ([_]u64{ iotlb_ivt, 0, @as(u64, 3) << iotlb_iaig_shift }) |sample| {
        const fixture = try DmaWithdrawalFixture.init();
        defer fixture.deinit();
        try fixture.list.installRegions(fixture.regions);
        try fixture.list.seedContiguousRange(@intFromPtr(fixture.backing.ptr), 128);
        try TestSupport.begin(fixture.list, DmaWithdrawalFixture.device);
        const page = try fixture.list.popFront();
        try std.testing.expect(mapPages(DmaWithdrawalFixture.device, IovaAddress.fromInt(DmaWithdrawalFixture.address), &.{page}, true, true));
        TestSupport.invalidation_result = sample;
        try std.testing.expect(!unmapRangeForDevice(DmaWithdrawalFixture.device, IovaAddress.fromInt(DmaWithdrawalFixture.address), page_size));
        try std.testing.expectError(error.PageQuarantined, fixture.list.appendPage(page));
    }
}

test "VT-d withdrawal contract boot disable timeout retains state and requires halt" {
    for ([_]bool{ false, true }) |timeout| {
        const fixture = try DmaWithdrawalFixture.init();
        defer fixture.deinit();
        try fixture.list.installRegions(fixture.regions);
        try fixture.list.seedContiguousRange(@intFromPtr(fixture.backing.ptr), 128);
        try TestSupport.begin(fixture.list, DmaWithdrawalFixture.device);
        const page = try fixture.list.popFront();
        try std.testing.expect(mapPages(DmaWithdrawalFixture.device, IovaAddress.fromInt(DmaWithdrawalFixture.address), &.{page}, true, true));
        var registers: [128]u64 = @splat(0);
        driver_state.first_unit.register_base = @intFromPtr(&registers);
        driver_state.first_unit.registers = mmio.mappedAfterValidation(@ptrCast(&registers), @sizeOf(@TypeOf(registers)), .uc);
        driver_state.first_unit.gcmd_shadow = gcmd_te;
        mmioWrite32(&driver_state.first_unit, gsts_reg, if (timeout) gsts_tes else 0);
        if (timeout) {
            try std.testing.expectError(error.DisableTimedOut, disableTranslationAfterFailure(&driver_state.first_unit));
            try std.testing.expect(driver_state.active);
            try std.testing.expect(driver_state.has_quarantine);
            try std.testing.expectError(error.PageQuarantined, fixture.list.appendPage(page));
        } else {
            try disableTranslationAfterFailure(&driver_state.first_unit);
            try std.testing.expect(!driver_state.active);
        }
    }
}

test "VT-d withdrawal contract buffer and mapping aliases close only after the last ACK" {
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
            TestSupport.failInvalidation(fail);
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

test "VT-d withdrawal contract write buffer timeout quarantines before IOTLB success" {
    const fixture = try DmaWithdrawalFixture.init();
    defer fixture.deinit();
    try fixture.list.installRegions(fixture.regions);
    try fixture.list.seedContiguousRange(@intFromPtr(fixture.backing.ptr), 128);
    try TestSupport.begin(fixture.list, DmaWithdrawalFixture.device);
    const page = try fixture.list.popFront();
    try std.testing.expect(mapPages(DmaWithdrawalFixture.device, IovaAddress.fromInt(DmaWithdrawalFixture.address), &.{page}, true, true));
    var registers: [128]u64 = @splat(0);
    driver_state.first_unit.register_base = @intFromPtr(&registers);
    driver_state.first_unit.registers = mmio.mappedAfterValidation(@ptrCast(&registers), @sizeOf(@TypeOf(registers)), .uc);
    driver_state.first_unit.rwbf = true;
    mmioWrite32(&driver_state.first_unit, gsts_reg, gsts_wbfs);
    TestSupport.ack_completed = false;
    try std.testing.expect(!unmapRangeForDevice(DmaWithdrawalFixture.device, IovaAddress.fromInt(DmaWithdrawalFixture.address), page_size));
    try std.testing.expect(!TestSupport.ack_completed);
    try std.testing.expect(deviceQuarantined(DmaWithdrawalFixture.device));
    try std.testing.expectError(error.PageQuarantined, fixture.list.appendPage(page));
}

test "VT-d physical access nonidentity bank grant survives allocator reset and returns physical" {
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
    const saved = driver_state;
    defer driver_state = saved;
    driver_state = .{ .free_list = free_list };
    var allocator = iova_storage.Allocator.init(0x1000000, 0x2000000, .{ .allocate = allocIovaBank, .release = freeIovaBank });
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

test "VT-d physical access nonidentity table flush uses borrowed KVA" {
    const fixture = @import("memory/physical_access_test.zig");
    const memory = try std.testing.allocator.alignedAlloc(u8, .fromByteUnits(4096), 4096);
    defer std.testing.allocator.free(memory);
    const physical: u64 = 0x140000;
    fixture.mapping = .{ .physical = physical, .bytes = memory };
    defer fixture.mapping = null;
    const saved = driver_state.table_cache_line_bytes;
    defer driver_state.table_cache_line_bytes = saved;
    driver_state.table_cache_line_bytes = 64;
    const table = tableAt(physical);
    table[0] = 0x9876003;
    test_cache_flush_count = 0;
    flushTableCache(table, 4096);
    try std.testing.expectEqual(@as(usize, 1), test_cache_flush_count);
    try std.testing.expectEqual(@intFromPtr(memory.ptr), test_cache_flush_events[0].address);
    try std.testing.expect(test_cache_flush_events[0].address != physical);
    try std.testing.expectEqual(table[0], test_cache_flush_events[0].first_word);
}

test "VT-d pools exceed domain and leaf limits during quarantine reads" {
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
    const first = leafMetadata(table_page, true, 1, 0) orelse return error.OutOfMemory;
    tableAt(table_page)[0] = 0x12345000;
    refcountsAt(first.refcounts_paddr)[0] = 1;
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
            const leaf = leafMetadata(key, true, 2, 0) orelse return error.OutOfMemory;
            try std.testing.expectEqual(leaf, leafMetadata(key, false, 2, 0).?);
        }
    }
    try std.testing.expectEqual(@as(usize, 4097), driver_state.leaf_count);
    try std.testing.expectEqual(first, leafMetadata(table_page, false, 1, 0).?);
    try std.testing.expectEqual(@as(u64, 0x12345000), tableAt(table_page)[0]);
    try std.testing.expectEqual(@as(u16, 1), refcountsAt(first.refcounts_paddr)[0]);
}

test "VT-d pool growth failure preserves unpublished ownership and permits retry" {
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
    try std.testing.expect(leafMetadata(4096, true, 1, 0) == null);
    try std.testing.expectEqual(@as(usize, 0), driver_state.leaf_count);
    try std.testing.expect(driver_state.leaf_head == null);
    TestSupport.pool_allocator_override = null;
    const first = prepareDomain(0) orelse return error.OutOfMemory;
    first.did = 123;
    TestSupport.pool_allocator_override = .{ .context = &context, .acquire = Fail.acquire, .release = Fail.release };
    const boundary: usize = @intCast(driver_state.domains.capacity);
    try std.testing.expect(prepareDomain(boundary) == null);
    try std.testing.expectEqual(first, domainAt(0));
    try std.testing.expectEqual(@as(u16, 123), first.did);
    TestSupport.pool_allocator_override = null;
    try std.testing.expect(prepareDomain(boundary) != null);
}

// The fixture passes through the firmware parser and real unit selection. The
// fourth record must never steal a same-BDF requester from segment zero.
fn syntheticDmar(bytes: *[48 + 4 * 24]u8, include_all: bool) !acpi_dmar.DmarInfo {
    @memset(bytes, 0);
    @memcpy(bytes[0..4], "DMAR");
    std.mem.writeInt(u32, bytes[4..8], bytes.len, .little);
    for (0..4) |index| {
        const record = bytes[48 + index * 24 ..][0..24];
        std.mem.writeInt(u16, record[2..4], 24, .little);
        record[4] = @intFromBool(include_all and index == 2);
        std.mem.writeInt(u16, record[6..8], if (index == 3) 1 else 0, .little);
        std.mem.writeInt(u64, record[8..16], 0xfed00000 + index * 4096, .little);
        record[16] = 1;
        record[17] = 8;
        record[22] = if (index == 3) 0 else @intCast(index);
    }
    var sum: u8 = 0;
    for (bytes) |byte| sum +%= byte;
    bytes[9] = 0 -% sum;
    return acpi_dmar.parseDmar(bytes);
}

test "VT-d multiple DRHD explicit ownership fallback ambiguity and segment isolation" {
    for ([_]bool{ false, true }) |include_all| {
        defer TestSupport.end();
        var bytes: [48 + 4 * 24]u8 = undefined;
        const info = try syntheticDmar(&bytes, include_all);
        try std.testing.expect(selectUnits(info));
        try std.testing.expectEqual(@as(usize, 3), driver_state.unit_count);
        for (0..3) |index| try std.testing.expectEqual(index, ownerForLocation(.{ .bus = 0, .device = @intCast(index), .function = 0 }).?);
        try std.testing.expectEqual(if (include_all) @as(?usize, 2) else null, ownerForLocation(.{ .bus = 0, .device = 5, .function = 0 }));
        unitAt(1).drhd.scope_bytes = unitAt(0).drhd.scope_bytes;
        try std.testing.expect(ownerForLocation(.{ .bus = 0, .device = 0, .function = 0 }) == null);
        unitAt(0).drhd.include_pci_all = true;
        unitAt(1).drhd.include_pci_all = true;
        try std.testing.expect(ownerForLocation(.{ .bus = 0, .device = 5, .function = 0 }) == null);
    }
}

test "VT-d multi-unit fake registers owner completion and quarantine are isolated" {
    for (0..3) |failed| {
        for ([_]bool{ false, true }) |fail_map| {
            const fixture = try DmaWithdrawalFixture.init();
            defer fixture.deinit();
            try fixture.list.installRegions(fixture.regions);
            try fixture.list.seedContiguousRange(@intFromPtr(fixture.backing.ptr), 128);
            try TestSupport.begin(fixture.list, 100);
            var registers: [3][128]u64 = @splat(@splat(0));
            for (0..3) |index| {
                const unit = prepareUnit(index).?;
                unit.cap = cap_caching_mode | (@as(u64, 3) << 54);
                unit.iotlb_offset = 0x100;
                unit.register_base = @intFromPtr(&registers[index]);
                unit.registers = mmio.mappedAfterValidation(@ptrCast(&registers[index]), 1024, .uc);
                if (index != 0) {
                    const domain = prepareDomain(index).?;
                    domain.* = .{ .device = 100 + index, .did = 1, .unit_index = index, .second_level_root_paddr = allocTablePage().? };
                }
            }
            driver_state.unit_count = 3;
            driver_state.domain_count = 3;
            TestSupport.fake_register_mode = true;
            var pages: [3]u64 = undefined;
            for (0..3) |index| {
                pages[index] = try fixture.list.popFront();
                try std.testing.expect(mapPages(100 + index, .fromInt(DmaWithdrawalFixture.address), &.{pages[index]}, true, true));
                try std.testing.expectEqual(@as(u64, 1), registers[index][0x300 / 8]);
            }
            TestSupport.fail_unit = unitAt(failed);
            for (0..3) |index| {
                const success = if (fail_map)
                    mapPages(100 + index, .fromInt(DmaWithdrawalFixture.address), &.{pages[index]}, true, true)
                else
                    unmapRangeForDevice(100 + index, .fromInt(DmaWithdrawalFixture.address), page_size);
                try std.testing.expectEqual(index != failed, success);
                try std.testing.expectEqual(index == failed, deviceQuarantined(100 + index));
                try std.testing.expectEqual(index == failed, physicalRangeQuarantined(pages[index], 1));
                if (index == failed) try std.testing.expectError(error.PageQuarantined, fixture.list.appendPage(pages[index]));
            }
            // Even a single published unit makes initialization failure fatal.
            try std.testing.expect(!initializationRequiresHalt());
            unitAt(failed).published = true;
            try std.testing.expect(initializationRequiresHalt());
        }
    }
}

test "VT-d bridge scope follows hierarchy and excludes unrelated buses" {
    const Reader = struct {
        pub fn readVendorId(_: pci.Location) u16 {
            return 0x8086;
        }
        pub fn readClassCode(_: pci.Location) u8 {
            return 6;
        }
        pub fn readSubclass(_: pci.Location) u8 {
            return 4;
        }
        pub fn readConfigU8(_: pci.Location, offset: u8) u8 {
            return if (offset == 0x19) 4 else 7;
        }
    };
    const scope = [_]u8{ 2, 8, 0, 0, 0, 0, 1, 0 };
    const drhd = acpi_dmar.Drhd{ .scope_bytes = &scope, .scope_count = 1 };
    for (0..9) |bus| try std.testing.expectEqual(bus >= 4 and bus <= 7, explicitScopeMatchesWith(Reader, drhd, .{ .bus = @intCast(bus), .device = 3, .function = 0 }));
    const path = [_]acpi_dmar.DevicePath{ .{ .device = 1 }, .{ .device = 2 } };
    const endpoint = resolveScopeWith(Reader, &.{ .scope_type = 1, .path = &path, .path_count = 2 }).?;
    try std.testing.expectEqual(@as(u8, 4), endpoint.bus);
    try std.testing.expectEqual(@as(u8, 2), endpoint.device);
    const rmrr = acpi_dmar.Rmrr{ .segment = 0, .base = 0x66000000, .last = 0x687fffff, .scope_bytes = &scope };
    try std.testing.expect(scopesMatchWith(Reader, rmrr.scopes(), .{ .bus = 0, .device = 1, .function = 0 }));
    const endpoint_bytes = [_]u8{ 1, 8, 0, 0, 0, 0, 2, 0 };
    for ([_]pci.Location{
        .{ .bus = 0, .device = 2, .function = 0 }, .{ .bus = 1, .device = 2, .function = 0 },
        .{ .bus = 0, .device = 3, .function = 0 }, .{ .bus = 0, .device = 2, .function = 1 },
    }, 0..) |location, i| try std.testing.expectEqual(i == 0,
        scopesMatchWith(Reader, .{ .bytes = &endpoint_bytes }, location));
    const invalid_bytes = [_]u8{ 1, 8, 0, 0, 0, 0, 32, 0 };
    try std.testing.expect(!scopesMatchWith(Reader, .{ .bytes = &invalid_bytes }, .{ .bus = 0, .device = 0, .function = 0 }));
}

test "VT-d multiple roots reuse local DIDs and unpublished rollback returns tables" {
    const fixture = try DmaWithdrawalFixture.init();
    defer fixture.deinit();
    try fixture.list.installRegions(fixture.regions);
    try fixture.list.seedContiguousRange(@intFromPtr(fixture.backing.ptr), 128);
    driver_state.free_list = fixture.list;
    var bytes: [48 + 4 * 24]u8 = undefined;
    try std.testing.expect(selectUnits(try syntheticDmar(&bytes, false)));
    var registers: [3][128]u64 = @splat(@splat(0));
    const before = fixture.list.len;
    for (0..3) |index| {
        const unit = unitAt(index);
        unit.hardware_domain_count = 16;
        unit.root_paddr = allocTablePage().?;
        unit.cap = cap_caching_mode | (@as(u64, 3) << 54);
        unit.iotlb_offset = 0x100;
        unit.register_base = @intFromPtr(&registers[index]);
        unit.registers = mmio.mappedAfterValidation(@ptrCast(&registers[index]), 1024, .uc);
        try std.testing.expect(installPciContext(unit, index, .{ .bus = 0, .device = @intCast(index), .function = 0 }));
        try std.testing.expectEqual(@as(u16, 1), domainAt(index).did);
        try std.testing.expectEqual(index, domainAt(index).unit_index);
        if (index != 0) try std.testing.expect(unit.root_paddr != unitAt(0).root_paddr);
    }
    try std.testing.expectEqual(before - 9, fixture.list.len);
    releaseDraftTables();
    try std.testing.expectEqual(before, fixture.list.len);
    try std.testing.expectEqual(@as(usize, 0), driver_state.unit_count);
}

test "VT-d two and three unit activation requires all completions and retains failed publication" {
    for ([_]usize{ 2, 3 }) |count| {
        for (0..count + 1) |failure| {
            const fixture = try DmaWithdrawalFixture.init();
            defer fixture.deinit();
            try fixture.list.installRegions(fixture.regions);
            try fixture.list.seedContiguousRange(@intFromPtr(fixture.backing.ptr), 128);
            driver_state.free_list = fixture.list;
            var registers: [3][128]u64 = @splat(@splat(0));
            for (0..count) |index| {
                const unit = prepareUnit(index).?;
                unit.root_paddr = allocTablePage().?;
                unit.register_base = @intFromPtr(&registers[index]);
                unit.registers = mmio.mappedAfterValidation(@ptrCast(&registers[index]), 1024, .uc);
                unit.iotlb_offset = 0x100;
            }
            driver_state.unit_count = count;
            TestSupport.fake_register_mode = true;
            TestSupport.fail_unit = if (failure < count) unitAt(failure) else null;
            for (0..count) |index| {
                try std.testing.expectEqual(index != failure, enableHardwareUnit(unitAt(index)));
                try std.testing.expect(unitAt(index).published);
                try std.testing.expectEqual(unitAt(index).root_paddr, registers[index][rtaddr_reg / 8]);
                if (index == failure) break;
                try std.testing.expectEqual(@as(u64, 2), registers[index][0x300 / 8]);
            }
            try std.testing.expect(initializationRequiresHalt());
            for (0..count) |index| try std.testing.expect(!fixture.isFree(unitAt(index).root_paddr));
        }
    }
}

test "VT-d nonzero-only DMAR cannot supply segment-zero protection" {
    defer TestSupport.end();
    var bytes: [48 + 4 * 24]u8 = undefined;
    _ = try syntheticDmar(&bytes, true);
    for (0..4) |index| bytes[48 + index * 24 + 6] = 1;
    bytes[9] = 0;
    var sum: u8 = 0;
    for (bytes) |byte| sum +%= byte;
    bytes[9] = 0 -% sum;
    try std.testing.expect(!selectUnits(try acpi_dmar.parseDmar(&bytes)));
    try std.testing.expectEqual(@as(usize, 0), driver_state.unit_count);
    try std.testing.expect(ownerForLocation(.{ .bus = 0, .device = 0, .function = 0 }) == null);
}

test "VT-d unit catalog grows beyond former parser limit" {
    defer TestSupport.end();
    var bytes: [48 + 33 * 16]u8 = @splat(0);
    @memcpy(bytes[0..4], "DMAR");
    std.mem.writeInt(u32, bytes[4..8], bytes.len, .little);
    for (0..33) |index| {
        const block = bytes[48 + index * 16 ..][0..16];
        std.mem.writeInt(u16, block[2..4], 16, .little);
        std.mem.writeInt(u64, block[8..16], 0xfed00000 + index * 4096, .little);
    }
    var sum: u8 = 0;
    for (bytes) |byte| sum +%= byte;
    bytes[9] = 0 -% sum;
    try std.testing.expect(selectUnits(try acpi_dmar.parseDmar(&bytes)));
    try std.testing.expectEqual(@as(usize, 33), driver_state.unit_count);
    for (0..33) |index| try std.testing.expectEqual(@as(u64, 0xfed00000) + index * 4096, unitAt(index).drhd.register_base);
}
