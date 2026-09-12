const std = @import("std");
const builtin = @import("builtin");
const acpi_dmar = @import("acpi_dmar.zig");
const kernel_log = @import("kernel_log.zig");
const pci = @import("pci.zig");
const smp = @import("smp.zig");
const types = @import("state/types.zig");
const user_copy = @import("user_copy.zig");
const tables = @import("vtd_tables.zig");

pub const Discovery = struct {
    table_paddr: u64,
    info: acpi_dmar.DmarInfo,
};

const page_size: u64 = 4096;
const four_gib: u64 = 4 * 1024 * 1024 * 1024;
pub const iova_window_start = tables.iova_window_start;
pub const iova_window_end = tables.iova_window_end;
const iova_page_count = tables.iova_page_count;
const iova_bitmap_bytes = tables.iova_bitmap_bytes;
const iova_bitmap_pages: usize = iova_bitmap_bytes / @as(usize, @intCast(page_size));
const max_domains: usize = 256;
const max_leaf_tables: usize = 4096;
const leaf_index_slots: usize = max_leaf_tables * 2;
const poll_limit: usize = 1_000_000;

const cap_reg: u64 = 0x08;
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
    table_paddr: u64 = 0,
    refcounts_paddr: u64 = 0,
    domain_id: u16 = 0,
    quarantined: bool = false,
};

pub const IovaAllocator = tables.IovaAllocator;

const Domain = struct {
    device: types.DmaDeviceId = types.invalid_dma_device_id,
    did: u16 = 0,
    second_level_root_paddr: u64 = 0,
    allocator: IovaAllocator = .{},
    quarantined: bool = false,
    enabled: bool = true,
};

comptime {
    if (@sizeOf([max_domains]Domain) > 64 * 1024)
        @compileError("VT-d domain metadata exceeds the static-storage budget");
}

const DriverState = struct {
    discovery: ?Discovery = null,
    free_list: ?*types.FreePageList = null,
    register_base: u64 = 0,
    cap: u64 = 0,
    ecap: u64 = 0,
    root_paddr: u64 = 0,
    context_table_paddrs: [256]u64 = [_]u64{0} ** 256,
    domains: [max_domains]Domain = [_]Domain{.{}} ** max_domains,
    domain_count: usize = 0,
    hardware_domain_count: u32 = 0,
    leaf_metadata: [max_leaf_tables]LeafMetadata = [_]LeafMetadata{.{}} ** max_leaf_tables,
    /// Open-addressed table of leaf_metadata indices plus one; zero is empty.
    leaf_index: [leaf_index_slots]u16 = [_]u16{0} ** leaf_index_slots,
    leaf_count: usize = 0,
    mapped_page_count: usize = 0,
    mapping_ref_count: usize = 0,
    fault_record_count: usize = 0,
    fault_record_offset: u64 = 0,
    iotlb_offset: u64 = 0,
    gcmd_shadow: u32 = 0,
    context_count: usize = 0,
    rwbf: bool = false,
    /// Zero for a coherent table walker, otherwise the CPUID CLFLUSH stride.
    table_cache_line_bytes: usize = 0,
    active: bool = false,
    has_quarantine: bool = false,
    lock_word: u8 = 0,
};

var driver_state: DriverState = .{};
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
        if (@cmpxchgWeak(u8, &driver_state.lock_word, 0, 1, .acquire, .monotonic) == null) return;
        while (@atomicLoad(u8, &driver_state.lock_word, .monotonic) != 0) {
            asm volatile ("pause");
        }
    }
}

fn unlock() void {
    @atomicStore(u8, &driver_state.lock_word, 0, .release);
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
fn flushTableCache(address: u64, length: usize) void {
    const stride = driver_state.table_cache_line_bytes;
    if (stride == 0 or length == 0) return;
    if (builtin.is_test) {
        if (test_cache_flush_count < test_cache_flush_events.len) {
            test_cache_flush_events[test_cache_flush_count] = .{
                .address = address,
                .length = length,
                .first_word = @as(*const u64, @ptrFromInt(address)).*,
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

fn mmioRead32(offset: u64) u32 {
    const ptr: *volatile u32 = @ptrFromInt(driver_state.register_base + offset);
    return ptr.*;
}

fn mmioWrite32(offset: u64, value: u32) void {
    const ptr: *volatile u32 = @ptrFromInt(driver_state.register_base + offset);
    ptr.* = value;
}

fn mmioRead64(offset: u64) u64 {
    return mmioRead64At(driver_state.register_base, offset);
}

fn mmioRead64At(register_base: u64, offset: u64) u64 {
    const ptr: *volatile u64 = @ptrFromInt(register_base + offset);
    return ptr.*;
}

fn mmioWrite64(offset: u64, value: u64) void {
    const ptr: *volatile u64 = @ptrFromInt(driver_state.register_base + offset);
    ptr.* = value;
}

fn wait32Set(offset: u64, mask: u32) bool {
    var spins: usize = 0;
    while (spins < poll_limit) : (spins += 1) {
        if ((mmioRead32(offset) & mask) == mask) return true;
        asm volatile ("pause");
    }
    return false;
}

fn wait32Clear(offset: u64, mask: u32) bool {
    var spins: usize = 0;
    while (spins < poll_limit) : (spins += 1) {
        if ((mmioRead32(offset) & mask) == 0) return true;
        asm volatile ("pause");
    }
    return false;
}

fn wait64Clear(offset: u64, mask: u64) bool {
    var spins: usize = 0;
    while (spins < poll_limit) : (spins += 1) {
        if ((mmioRead64(offset) & mask) == 0) return true;
        asm volatile ("pause");
    }
    return false;
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
        free_list.appendPage(0, paddr) catch {};
        return null;
    }
    return paddr;
}

fn allocTablePage() ?u64 {
    const paddr = allocZeroPage() orelse return null;
    flushTableCache(paddr, page_size);
    return paddr;
}

fn allocIovaBitmap() ?*[iova_bitmap_bytes]u8 {
    const free_list = driver_state.free_list orelse return null;
    const paddr = if (builtin.is_test)
        free_list.popContiguousBelow(iova_bitmap_pages, std.math.maxInt(u64)) catch return null
    else
        free_list.popContiguousBelow(iova_bitmap_pages, four_gib) catch return null;
    var page_index: usize = 0;
    while (page_index < iova_bitmap_pages) : (page_index += 1) {
        const page_paddr = paddr + @as(u64, @intCast(page_index)) * page_size;
        if (builtin.is_test) {
            @memset(tableAt(page_paddr)[0..], 0);
        } else if (!user_copy.zeroPhysicalPage(page_paddr)) {
            free_list.appendContiguousRange(0, paddr, iova_bitmap_pages) catch {};
            return null;
        }
    }
    return @ptrFromInt(paddr);
}

fn tableAt(paddr: u64) *tables.TablePage {
    return @ptrFromInt(paddr);
}

fn pageAddress(entry: u64) u64 {
    return entry & tables.page_address_mask;
}

fn domainForDevice(device: types.DmaDeviceId) ?*Domain {
    for (driver_state.domains[0..driver_state.domain_count]) |*domain| {
        if (domain.device == device) return domain;
    }
    return null;
}

fn ensureDomainAllocator(domain: *Domain) bool {
    if (domain.allocator.bitmap != null) return true;
    const bitmap = allocIovaBitmap() orelse return false;
    domain.allocator = IovaAllocator.init(bitmap);
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
    flushTableCache(@intFromPtr(&parent[index]), @sizeOf(u64));
    return child;
}

/// A disabled domain still owns its entire tree. Unlike ordinary child
/// links, its root links intentionally carry an address with no R/W bits.
fn ensureDomainRootChild(domain: *const Domain, index: usize) ?u64 {
    const root = tableAt(domain.second_level_root_paddr);
    const existing = pageAddress(root[index]);
    if (existing != 0) return existing;
    const child = allocTablePage() orelse return null;
    root[index] = child | (if (domain.enabled)
        tables.second_level_read | tables.second_level_write
    else
        @as(u64, 0));
    flushTableCache(@intFromPtr(&root[index]), @sizeOf(u64));
    return child;
}

fn leafMetadata(table_paddr: u64, create: bool, domain_id: u16) ?*LeafMetadata {
    const start: usize = @intCast((table_paddr >> 12) & (leaf_index_slots - 1));
    var probe: usize = 0;
    while (probe < driver_state.leaf_index.len) : (probe += 1) {
        const slot_index = (start + probe) & (leaf_index_slots - 1);
        const encoded = driver_state.leaf_index[slot_index];
        if (encoded != 0) {
            const metadata = &driver_state.leaf_metadata[encoded - 1];
            if (metadata.table_paddr == table_paddr) return metadata;
            continue;
        }
        if (!create or driver_state.leaf_count >= driver_state.leaf_metadata.len) return null;
        const refcounts = allocZeroPage() orelse return null;
        const metadata_index = driver_state.leaf_count;
        const result = &driver_state.leaf_metadata[metadata_index];
        result.* = .{ .table_paddr = table_paddr, .refcounts_paddr = refcounts, .domain_id = domain_id };
        driver_state.leaf_count += 1;
        driver_state.leaf_index[slot_index] = @intCast(metadata_index + 1);
        return result;
    }
    return null;
}

fn refcountsAt(paddr: u64) *[512]u16 {
    return @ptrFromInt(paddr);
}

fn walkToLeaf(domain: *const Domain, iova: u64, create: bool) ?struct { table: *tables.TablePage, metadata: *LeafMetadata, index: usize } {
    const pml4 = tableAt(domain.second_level_root_paddr);
    const pml4_index: usize = @intCast((iova >> 39) & 0x1ff);
    const pdp_paddr = if (create)
        ensureDomainRootChild(domain, pml4_index) orelse return null
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
    const metadata = leafMetadata(pt_paddr, create, domain.did) orelse return null;
    return .{
        .table = tableAt(pt_paddr),
        .metadata = metadata,
        .index = @intCast((iova >> 12) & 0x1ff),
    };
}

fn mapPage(domain: *const Domain, iova: u64, paddr: u64, readable: bool, writable: bool) bool {
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
        flushTableCache(@intFromPtr(&leaf.table[leaf.index]), @sizeOf(u64));
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
        flushTableCache(@intFromPtr(&leaf.table[leaf.index]), @sizeOf(u64));
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
        flushTableCache(@intFromPtr(&leaf.table[leaf.index]), @sizeOf(u64));
    }
}

fn quarantineDomain(domain: *Domain) void {
    domain.quarantined = true;
    for (driver_state.leaf_metadata[0..driver_state.leaf_count]) |*metadata| {
        if (metadata.domain_id == domain.did) @atomicStore(bool, &metadata.quarantined, true, .release);
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
pub fn physicalRangeQuarantined(paddr: u64, page_count: usize) bool {
    if (!@atomicLoad(bool, &driver_state.has_quarantine, .acquire)) return false;
    if (page_count == 0) return false;
    const length, const size_overflow = @mulWithOverflow(@as(u64, @intCast(page_count)), page_size);
    const end, const end_overflow = @addWithOverflow(paddr, length);
    if (size_overflow != 0 or end_overflow != 0) return true;
    for (&driver_state.leaf_metadata) |*metadata| {
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
    flushTableCache(domain.second_level_root_paddr, page_size);
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
    const required_drain = (@as(u64, 1) << 55) | (@as(u64, 1) << 54);
    if ((driver_state.cap & required_drain) != required_drain) return error.Unsupported;
    setDomainRootPermissions(domain, enabled);
    if (flushTranslationChanges(domain.did)) return;
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

fn writeBufferFlush() bool {
    if (!driver_state.rwbf) return true;
    mmioWrite32(gcmd_reg, driver_state.gcmd_shadow | gcmd_wbf);
    return wait32Clear(gsts_reg, gsts_wbfs);
}

fn invalidateContextCacheGlobal() bool {
    mmioWrite64(ccmd_reg, ccmd_icc | ccmd_cirg_global);
    if (!wait64Clear(ccmd_reg, ccmd_icc)) return false;
    return ((mmioRead64(ccmd_reg) >> ccmd_caig_shift) & 0x3) == 1;
}

fn invalidateIotlbGlobal() bool {
    const register = driver_state.iotlb_offset + 8;
    mmioWrite64(register, iotlb_ivt | iotlb_iirg_global);
    if (!wait64Clear(register, iotlb_ivt)) return false;
    return ((mmioRead64(register) >> iotlb_iaig_shift) & 0x3) == 1;
}

fn invalidateIotlbDomain(did: u16) bool {
    const command = domainInvalidationCommand(driver_state.cap, did);
    if (builtin.is_test) {
        test_last_invalidation = command;
        test_flush_count_at_invalidation = test_cache_flush_count;
        return !test_fail_invalidation;
    }
    const register = driver_state.iotlb_offset + 8;
    mmioWrite64(register, command);
    if (!wait64Clear(register, iotlb_ivt)) return false;
    return domainInvalidationCompleted(mmioRead64(register));
}

fn flushTranslationChanges(did: u16) bool {
    asm volatile ("mfence" ::: .{ .memory = true });
    if (builtin.is_test) return invalidateIotlbDomain(did);
    if (!writeBufferFlush()) return false;
    return invalidateIotlbDomain(did);
}

fn disableTranslationAfterFailure() void {
    driver_state.gcmd_shadow &= ~gcmd_te;
    mmioWrite32(gcmd_reg, driver_state.gcmd_shadow);
    if (!wait32Clear(gsts_reg, gsts_tes)) {
        kernel_log.write("vtd: disable after failure timed out; hardware state uncertain\n");
    }
    driver_state.active = false;
}

fn logInitFailure(reason: []const u8) void {
    kernel_log.writeFmt(
        "vtd: mode=pass-through reason=initialization-failed detail={s} active=0 faults=0\n",
        .{reason},
    );
    if (driver_state.register_base != 0 and
        ((driver_state.gcmd_shadow & gcmd_te) != 0 or (mmioRead32(gsts_reg) & gsts_tes) != 0))
    {
        disableTranslationAfterFailure();
    }
}

fn domainCount(nd: u3) u32 {
    const shift: u5 = @intCast(4 + @as(u8, nd) * 2);
    return @as(u32, 1) << shift;
}

fn logDeviceScopes(info: *const acpi_dmar.DmarInfo) void {
    for (info.drhds[0..info.drhd_count], 0..) |drhd, drhd_index| {
        kernel_log.writeFmt(
            "vtd: drhd index={} length={} base=0x{x} segment={} include_pci_all={} scopes={}\n",
            .{ drhd_index, drhd.length, drhd.register_base, drhd.segment, @intFromBool(drhd.include_pci_all), drhd.scope_count },
        );
        for (drhd.scopes[0..drhd.scope_count], 0..) |scope, scope_index| {
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
    if (scope.scope_type != 1 or scope.path_count == 0) return null;

    var bus = scope.start_bus;
    for (scope.path[0..scope.path_count], 0..) |path, path_index| {
        if (path.device >= 32 or path.function >= 8) return null;
        const location = pci.Location{
            .bus = bus,
            .device = path.device,
            .function = path.function,
        };
        if (path_index + 1 == scope.path_count) return location;

        if (pci.readVendorId(location) == 0xffff) return null;
        if (pci.readClassCode(location) != 0x06 or
            pci.readSubclass(location) != 0x04)
        {
            return null;
        }
        const secondary = pci.readConfigU8(location, 0x19);
        const subordinate = pci.readConfigU8(location, 0x1a);
        if (secondary == 0 or secondary > subordinate) return null;
        bus = secondary;
    }
    return null;
}

fn drhdCoversPciFunction(drhd: *const acpi_dmar.Drhd, location: pci.Location) bool {
    if (drhd.include_pci_all) return true;
    for (drhd.scopes[0..drhd.scope_count]) |*scope| {
        const endpoint = resolveEndpointScope(scope) orelse continue;
        if (samePciLocation(endpoint, location)) return true;
    }
    return false;
}

fn pciFunctionCount(drhd: *const acpi_dmar.Drhd) ?usize {
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
                if (!drhdCoversPciFunction(drhd, location)) {
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

fn installPciContexts(drhd: *const acpi_dmar.Drhd, total_function_count: usize) bool {
    if (total_function_count == 0) {
        kernel_log.write("vtd: context construction failed reason=no-pci-functions\n");
        return false;
    }
    if (total_function_count > max_domains) {
        kernel_log.writeFmt(
            "vtd: context construction failed reason=domain-slots-exhausted functions={} slots={}\n",
            .{ total_function_count, max_domains },
        );
        return false;
    }
    if (total_function_count >= driver_state.hardware_domain_count) {
        kernel_log.writeFmt(
            "vtd: context construction failed reason=hardware-did-exhausted functions={} dids={}\n",
            .{ total_function_count, driver_state.hardware_domain_count },
        );
        return false;
    }

    const root = tableAt(driver_state.root_paddr);
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
                if (!drhdCoversPciFunction(drhd, location)) {
                    kernel_log.writeFmt(
                        "vtd: context construction failed reason=scope-changed bdf={}:{}:{}\n",
                        .{ bus_number, device_number, function_number },
                    );
                    return false;
                }
                if (driver_state.context_table_paddrs[bus_number] == 0) {
                    const context_paddr = allocTablePage() orelse return false;
                    if (!tables.setRootEntry(root, @intCast(bus_number), context_paddr)) return false;
                    driver_state.context_table_paddrs[bus_number] = context_paddr;
                }
                const context = tableAt(driver_state.context_table_paddrs[bus_number]);
                const device_function: u8 = @intCast(device_number * 8 + function_number);
                const domain_index = driver_state.domain_count;
                if (domain_index >= driver_state.domains.len) return false;
                const second_level_root_paddr = allocTablePage() orelse return false;
                const did: u16 = @intCast(domain_index + 1);
                const resource_id = pci.resourceIdFromLocation(location);
                driver_state.domains[domain_index] = .{
                    .device = resource_id,
                    .did = did,
                    .second_level_root_paddr = second_level_root_paddr,
                };
                if (!tables.setContextEntry(context, device_function, second_level_root_paddr, did)) return false;
                driver_state.domain_count += 1;
                driver_state.context_count += 1;
                kernel_log.writeFmt(
                    "vtd: context bdf={}:{}:{} vendor=0x{x} device=0x{x} domain={}\n",
                    .{ bus_number, device_number, function_number, vendor, pci.readDeviceId(location), did },
                );
            }
        }
    }
    return driver_state.context_count == total_function_count and driver_state.domain_count == total_function_count;
}

/// Context contents reach RAM before the root entries that point at them;
/// both are visible before SRTP or translation enable touches either table.
fn flushInitialTables() void {
    for (driver_state.context_table_paddrs) |context| {
        if (context != 0) flushTableCache(context, page_size);
    }
    flushTableCache(driver_state.root_paddr, page_size);
}

pub fn init(rsdp_paddr: u64, free_list: *types.FreePageList) void {
    // A second initialization must never discard retained hardware references.
    if (driver_state.active or driver_state.has_quarantine) return;
    driver_state = .{};
    const table = acpi_dmar.findDmar(rsdp_paddr) orelse {
        kernel_log.write("vtd: mode=pass-through reason=dmar-not-found active=0 faults=0\n");
        return;
    };
    const info = acpi_dmar.parseDmar(table.bytes) catch |err| {
        kernel_log.writeFmt(
            "vtd: mode=pass-through reason=dmar-invalid paddr=0x{x} error={s} active=0 faults=0\n",
            .{ table.paddr, @errorName(err) },
        );
        return;
    };
    driver_state.discovery = .{ .table_paddr = table.paddr, .info = info };
    driver_state.free_list = free_list;

    kernel_log.writeFmt(
        "vtd: DMAR paddr=0x{x} host_address_width={} raw_haw={} flags=0x{x} drhds={}\n",
        .{ table.paddr, info.hostAddressWidthBits(), info.host_address_width, info.flags, info.drhd_count },
    );
    logDeviceScopes(&info);
    if (info.drhd_count != 1) {
        logInitFailure("requires exactly one DRHD in R8");
        return;
    }
    const drhd = info.drhds[0];
    if (drhd.segment != 0 or
        drhd.register_base == 0 or (drhd.register_base & (page_size - 1)) != 0)
    {
        logInitFailure("unsupported DRHD segment or register base");
        return;
    }
    const pci_function_count = pciFunctionCount(&drhd) orelse {
        logInitFailure("DRHD scopes do not cover every present PCI function");
        return;
    };
    const identity_limit = @import("arch/x86_64/physical_layout.zig").identity_limit;
    if (drhd.register_base >= identity_limit or
        page_size > identity_limit - drhd.register_base)
    {
        logInitFailure("DRHD register page is outside kernel identity map");
        return;
    }
    if (!smp.ucMinusMmioAllowed(drhd.register_base, page_size)) {
        logInitFailure("DRHD register page lacks an all-CPU uncached proof");
        return;
    }
    const cap = mmioRead64At(drhd.register_base, cap_reg);
    const ecap = mmioRead64At(drhd.register_base, ecap_reg);
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
    if (register_bytes > identity_limit - drhd.register_base or
        !smp.ucMinusMmioAllowed(drhd.register_base, register_bytes))
    {
        logInitFailure("DRHD extended registers lack an all-CPU uncached mapping");
        return;
    }

    // Publish the MMIO state only after every register range used by runtime
    // fault reporting and invalidation has passed the identity/cache proof.
    driver_state.register_base = drhd.register_base;
    driver_state.cap = cap;
    driver_state.ecap = ecap;
    driver_state.table_cache_line_bytes = table_cache_line_bytes;
    driver_state.rwbf = rwbf;
    driver_state.fault_record_offset = fault_record_offset;
    driver_state.fault_record_count = fault_record_count;
    driver_state.iotlb_offset = iotlb_offset;
    driver_state.hardware_domain_count = domainCount(nd);
    const qi = (ecap & (@as(u64, 1) << 1)) != 0;
    kernel_log.writeFmt("vtd: mmio base=0x{x} identity_mapped=1 cache=UC source=MTRR\n", .{drhd.register_base});
    kernel_log.writeFmt(
        "vtd: CAP=0x{x} ECAP=0x{x} SAGAW=0x{x} aw48={} CM={} RWBF={} ND={} domains={} FRO=0x{x} IRO=0x{x} QI={}\n",
        .{ driver_state.cap, driver_state.ecap, sagaw, @intFromBool((sagaw & (1 << 2)) != 0), @intFromBool(cm), @intFromBool(driver_state.rwbf), nd, domainCount(nd), driver_state.fault_record_offset, driver_state.iotlb_offset, @intFromBool(qi) },
    );
    if ((sagaw & (1 << 2)) == 0) {
        logInitFailure("four-level second-level translation unsupported");
        return;
    }
    if ((mmioRead32(gsts_reg) & gsts_tes) != 0) {
        logInitFailure("translation was already enabled");
        return;
    }

    driver_state.root_paddr = allocTablePage() orelse {
        logInitFailure("root table allocation failed");
        return;
    };
    if (!installPciContexts(&drhd, pci_function_count)) {
        logInitFailure("PCI context construction failed");
        return;
    }
    flushInitialTables();
    asm volatile ("mfence" ::: .{ .memory = true });

    mmioWrite64(rtaddr_reg, driver_state.root_paddr);
    mmioWrite32(gcmd_reg, driver_state.gcmd_shadow | gcmd_srtp);
    if (!wait32Set(gsts_reg, gsts_rtps)) {
        logInitFailure("SRTP/RTPS timeout");
        return;
    }
    if (!invalidateContextCacheGlobal()) {
        logInitFailure("global context-cache invalidation failed");
        return;
    }
    if (!invalidateIotlbGlobal()) {
        logInitFailure("global IOTLB invalidation failed");
        return;
    }

    driver_state.gcmd_shadow |= gcmd_te;
    mmioWrite32(gcmd_reg, driver_state.gcmd_shadow);
    if (!wait32Set(gsts_reg, gsts_tes)) {
        logInitFailure("TE/TES timeout");
        return;
    }
    driver_state.active = true;
    kernel_log.writeFmt(
        "vtd: mode=translated reason=enabled active=1 domains={} contexts={} root=0x{x}\n",
        .{ driver_state.domain_count, driver_state.context_count, driver_state.root_paddr },
    );
    dumpFaults();
}

pub fn discovered() ?*const Discovery {
    if (driver_state.discovery) |*value| return value;
    return null;
}

pub fn isActive() bool {
    return @atomicLoad(bool, &driver_state.active, .acquire);
}

fn validIovaRange(iova: u64, size: u64) bool {
    if (size == 0 or iova < iova_window_start or iova >= iova_window_end) return false;
    const end, const overflow = @addWithOverflow(iova, size);
    return overflow == 0 and end <= iova_window_end;
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
        kernel_log.writeFmt("vtd: iova alloc failed device=0x{x} did={} reason=bitmap-allocation\n", .{ device, domain.did });
        return null;
    }
    const iova = domain.allocator.alloc(page_count) orelse {
        kernel_log.writeFmt(
            "vtd: iova alloc failed device=0x{x} did={} reason=exhausted pages={} used_pages={} window_pages={}\n",
            .{ device, domain.did, page_count, domain.allocator.used_pages, iova_page_count },
        );
        return null;
    };
    return iova;
}

pub fn reserveIova(device: types.DmaDeviceId, iova: u64, page_count: usize) bool {
    if (!isActive()) return false;
    lock();
    defer unlock();
    const domain = domainForDevice(device) orelse return false;
    if (domain.quarantined or !ensureDomainAllocator(domain)) return false;
    return domain.allocator.reserve(iova, page_count);
}

pub fn freeIova(device: types.DmaDeviceId, iova: u64, page_count: usize) void {
    if (!isActive()) return;
    lock();
    defer unlock();
    const domain = domainForDevice(device) orelse return;
    if (domain.quarantined) return;
    _ = domain.allocator.free(iova, page_count);
}

pub fn mapPages(
    device: types.DmaDeviceId,
    iova: u64,
    paddrs: []const u64,
    readable: bool,
    writable: bool,
) bool {
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
        if (!mapPage(domain, page_iova, paddrs[mapped], readable, writable)) {
            _ = withdrawRange(domain, iova, mapped);
            if (!builtin.is_test) kernel_log.writeFmt(
                "vtd: map failed device=0x{x} did={} iova=0x{x} pages={} at={}\n",
                .{ device, domain.did, iova, paddrs.len, mapped },
            );
            if (!builtin.is_test) dumpFaultsLocked();
            return false;
        }
    }
    if (!flushTranslationChanges(domain.did)) {
        _ = withdrawRange(domain, iova, paddrs.len);
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
    if (!flushTranslationChanges(domain.did)) {
        quarantineDomain(domain);
        return false;
    }
    index = 0;
    while (index < page_count) : (index += 1) {
        unmapPage(domain, first + @as(u64, @intCast(index)) * page_size);
    }
    return true;
}

pub fn unmapRangeForDevice(device: types.DmaDeviceId, iova: u64, size: u64) bool {
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

fn dumpFaultsLocked() void {
    if (driver_state.register_base == 0) return;
    const fsts = mmioRead32(fsts_reg);
    var fault_count: usize = 0;
    var record_index: usize = 0;
    while (record_index < driver_state.fault_record_count) : (record_index += 1) {
        const offset = driver_state.fault_record_offset + @as(u64, @intCast(record_index)) * 16;
        const low = mmioRead64(offset);
        const high = mmioRead64(offset + 8);
        if ((high & fault_record_valid) == 0) continue;
        fault_count += 1;
        kernel_log.writeFmt(
            "vtd: fault index={} source_id=0x{x} reason=0x{x} address=0x{x} type={} raw_low=0x{x} raw_high=0x{x}\n",
            .{ record_index, high & 0xffff, (high >> 32) & 0xff, low & tables.page_address_mask, (high >> 62) & 0x1, low, high },
        );
        mmioWrite64(offset + 8, fault_record_valid);
    }
    if (fsts != 0) mmioWrite32(fsts_reg, fsts);
    kernel_log.writeFmt("vtd: faults count={} fsts=0x{x}\n", .{ fault_count, fsts });
}

pub fn dumpFaults() void {
    if (driver_state.register_base == 0) return;
    lock();
    defer unlock();
    dumpFaultsLocked();
}

/// Process teardown is outside the DMA map/unmap hot path and gives smoke
/// tests an active view of both late hardware faults and mapping lifetime.
pub fn dumpRuntimeCheckpoint() void {
    if (driver_state.register_base == 0) return;
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
    pub fn begin(free_list: *types.FreePageList, device: types.DmaDeviceId) !void {
        try beginWithTableCache(free_list, device, true);
    }

    pub fn beginWithTableCache(free_list: *types.FreePageList, device: types.DmaDeviceId, coherent: bool) !void {
        driver_state = .{ .free_list = free_list, .ecap = if (coherent) 1 else 0xf42 };
        const cpu = cpuid(1);
        driver_state.table_cache_line_bytes = tableCacheLineBytes(driver_state.ecap, cpu.ebx, cpu.edx) orelse
            return error.CacheMaintenanceUnavailable;
        test_fail_invalidation = false;
        test_last_invalidation = 0;
        test_cache_flush_count = 0;
        test_flush_count_at_invalidation = 0;
        driver_state.domains[0] = .{
            .device = device,
            .did = 1,
            .second_level_root_paddr = allocTablePage() orelse return error.OutOfMemory,
        };
        driver_state.domain_count = 1;
        driver_state.cap = (@as(u64, 1) << 55) | (@as(u64, 1) << 54);
        driver_state.active = true;
    }

    pub fn end() void {
        driver_state = .{};
        test_fail_invalidation = false;
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

test "VT-d endpoint scope coverage is exact and excludes non-PCI scopes" {
    const location = pci.Location{ .bus = 0, .device = 3, .function = 0 };
    var drhd = acpi_dmar.Drhd{};

    try std.testing.expect(!drhdCoversPciFunction(&drhd, location));
    drhd.scopes[0] = .{
        .scope_type = 1,
        .start_bus = 0,
        .path = blk: {
            var path = [_]acpi_dmar.DevicePath{.{}} ** acpi_dmar.max_device_scope_path_entries;
            path[0] = .{ .device = 3, .function = 0 };
            break :blk path;
        },
        .path_count = 1,
    };
    drhd.scope_count = 1;
    try std.testing.expect(drhdCoversPciFunction(&drhd, location));
    try std.testing.expect(!drhdCoversPciFunction(
        &drhd,
        .{ .bus = 0, .device = 4, .function = 0 },
    ));

    drhd.scopes[0].scope_type = 3;
    try std.testing.expect(!drhdCoversPciFunction(&drhd, location));
    drhd.scopes[0].scope_type = 1;
    drhd.scopes[0].path[0].device = 32;
    try std.testing.expect(!drhdCoversPciFunction(&drhd, location));
    drhd.include_pci_all = true;
    try std.testing.expect(drhdCoversPciFunction(&drhd, location));
}

test "VT-d lifetime domain stop retains tree and permits blocked mapping changes" {
    const allocator = std.testing.allocator;
    const backing = try allocator.alignedAlloc(u8, .fromByteUnits(4096), 64 * 4096);
    defer allocator.free(backing);
    const free_list = try allocator.create(types.FreePageList);
    defer allocator.destroy(free_list);
    free_list.* = .{};
    try free_list.appendContiguousRange(0, @intFromPtr(backing.ptr), 64);
    const device = pci.resourceIdFromLocation(.{ .bus = 0, .device = 2, .function = 0 });
    const other = pci.resourceIdFromLocation(.{ .bus = 0, .device = 3, .function = 0 });
    try TestSupport.beginWithTableCache(free_list, device, false);
    defer TestSupport.end();
    driver_state.domains[1] = .{
        .device = other,
        .did = 2,
        .second_level_root_paddr = allocTablePage() orelse return error.OutOfMemory,
    };
    driver_state.domain_count = 2;
    const page = try free_list.popFront();
    const iova = iova_window_start;
    try std.testing.expect(reserveIova(device, iova, 2));
    try std.testing.expect(reserveIova(other, iova, 1));
    try std.testing.expect(mapPages(other, iova, &.{page}, true, true));
    const other_root = tableAt(driver_state.domains[1].second_level_root_paddr).*;
    const domain = &driver_state.domains[0];
    const root = tableAt(domain.second_level_root_paddr);
    try setDeviceDmaEnabled(device, false);
    try std.testing.expect(mapPages(device, iova, &.{page}, true, false));
    const child = pageAddress(root[0]);
    try std.testing.expect(child != 0);
    try std.testing.expectEqual(@as(u64, 0), root[0] & 3);
    const first_leaf = walkToLeaf(domain, iova, false).?;
    const first_entry = first_leaf.table[first_leaf.index];
    for (0..8) |_| {
        // Alias the same RAM at a distinct IOVA while the original map lives.
        try std.testing.expect(mapPages(device, iova + page_size, &.{page}, false, true));
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
        try std.testing.expect(unmapRangeForDevice(device, iova + page_size, page_size));
        try std.testing.expectEqual(@as(usize, 2), TestSupport.mappingReferences());
        try std.testing.expectEqualSlices(u64, &other_root, tableAt(driver_state.domains[1].second_level_root_paddr));
    }
    try std.testing.expect(unmapRangeForDevice(device, iova, page_size));
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
        try free_list.appendContiguousRange(0, @intFromPtr(backing.ptr), 32);
        try TestSupport.begin(free_list, device);
        defer TestSupport.end();
        const page = try free_list.popFront();
        const iova = iova_window_start;
        try std.testing.expect(reserveIova(device, iova, 1));
        try std.testing.expect(mapPages(device, iova, &.{page}, true, true));
        const root = tableAt(driver_state.domains[0].second_level_root_paddr);
        const entry = root[0];
        for ([_]u64{ 0, @as(u64, 1) << 54, @as(u64, 1) << 55 }) |cap| {
            driver_state.cap = cap;
            try std.testing.expectError(error.Unsupported, setDeviceDmaEnabled(device, false));
            try std.testing.expectEqual(entry, root[0]);
            try std.testing.expect(!deviceQuarantined(device));
        }
        driver_state.cap = (@as(u64, 1) << 54) | (@as(u64, 1) << 55);
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
        try std.testing.expect(!mapPages(device, iova + page_size, &.{page}, true, true));
        try std.testing.expect(!unmapRangeForDevice(device, iova, page_size));
        try std.testing.expectError(types.KernelError.InvalidState, free_list.appendPage(0, page));
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
    try free_list.appendContiguousRange(0, @intFromPtr(backing.ptr), 32);
    const device = pci.resourceIdFromLocation(.{ .bus = 0, .device = 2, .function = 0 });
    try TestSupport.begin(free_list, device);
    defer TestSupport.end();
    const first = try free_list.popFront();
    _ = try free_list.popFront();
    const second = try free_list.popFront();
    const iova = iova_window_start + 0x4000;
    try std.testing.expect(reserveIova(device, iova, 2));
    try std.testing.expect(!reserveIova(device, iova + page_size, 2));
    try std.testing.expect(!reserveIova(device, iova_window_end, 1));
    try std.testing.expect(!reserveIova(device, iova + 1, 1));
    try std.testing.expectEqual(@as(usize, 2), TestSupport.usedPages(device));
    try std.testing.expect(mapPages(device, iova, &.{ first, second }, true, true));
    try std.testing.expectEqual(@as(usize, 0), test_cache_flush_count);
    try std.testing.expectEqual(@as(usize, 2), TestSupport.mappingReferences());
    try std.testing.expect(unmapRangeForDevice(device, iova, 2 * page_size));
    try std.testing.expectEqual(iotlb_read_drain | iotlb_write_drain, TestSupport.lastInvalidation() & (iotlb_read_drain | iotlb_write_drain));
    try std.testing.expectEqual(@as(usize, 0), TestSupport.mappingReferences());
    freeIova(device, iova, 2);
    try std.testing.expectEqual(@as(usize, 0), TestSupport.usedPages(device));
    try std.testing.expect(reserveIova(device, iova, 2));
    try free_list.appendPage(0, first);
    try free_list.appendPage(0, second);
}

test "VT-d lifetime failed drain retains IOVA and actual PMM backing" {
    const allocator = std.testing.allocator;
    const backing = try allocator.alignedAlloc(u8, .fromByteUnits(4096), 32 * 4096);
    defer allocator.free(backing);
    const free_list = try allocator.create(types.FreePageList);
    defer allocator.destroy(free_list);
    free_list.* = .{};
    try free_list.appendContiguousRange(0, @intFromPtr(backing.ptr), 32);
    const device = pci.resourceIdFromLocation(.{ .bus = 0, .device = 2, .function = 0 });
    try TestSupport.begin(free_list, device);
    defer TestSupport.end();
    const first = try free_list.popFront();
    _ = try free_list.popFront();
    const second = try free_list.popFront();
    const iova = iova_window_start + 0x8000;
    try std.testing.expect(reserveIova(device, iova, 2));
    try std.testing.expect(mapPages(device, iova, &.{ first, second }, true, true));
    try std.testing.expectEqual(@import("kernel_abi_root").capsule_abi.snapshot_flag_dma_translated, dmaSnapshotFlags(device));
    try std.testing.expectEqual(@as(u64, 0), dmaSnapshotFlags(device + 1));
    TestSupport.failInvalidation(true);
    try std.testing.expect(!unmapRangeForDevice(device, iova, 2 * page_size));
    try std.testing.expect(deviceQuarantined(device));
    try std.testing.expectEqual(@import("kernel_abi_root").capsule_abi.snapshot_flag_dma_quarantined, dmaSnapshotFlags(device));
    try std.testing.expectEqual(@as(usize, 2), TestSupport.mappingReferences());
    freeIova(device, iova, 2);
    try std.testing.expectEqual(@as(usize, 2), TestSupport.usedPages(device));
    try std.testing.expect(!reserveIova(device, iova, 2));
    try std.testing.expect(!mapPages(device, iova, &.{ first, second }, true, true));
    const free_pages = free_list.pageCount();
    try std.testing.expectError(types.KernelError.InvalidState, free_list.appendPage(0, first));
    try std.testing.expectError(types.KernelError.InvalidState, free_list.appendContiguousRange(0, first, 3));
    try std.testing.expectEqual(free_pages, free_list.pageCount());
    try std.testing.expect(physicalRangeQuarantined(first + 1, 1));
    try std.testing.expect(!physicalRangeQuarantined(first + page_size, 1));
    try std.testing.expect(physicalRangeQuarantined(std.math.maxInt(u64) - 1, 2));
    // Reinitialization and a later successful register response must not
    // discard the retained references of an already-faulted device.
    init(0, free_list);
    TestSupport.failInvalidation(false);
    try std.testing.expect(!unmapRangeForDevice(device, iova, 2 * page_size));
    try std.testing.expect(physicalRangeQuarantined(first, 1));
}

test "VT-d lifetime failed map rollback retains the published physical pages" {
    const allocator = std.testing.allocator;
    const backing = try allocator.alignedAlloc(u8, .fromByteUnits(4096), 32 * 4096);
    defer allocator.free(backing);
    const free_list = try allocator.create(types.FreePageList);
    defer allocator.destroy(free_list);
    free_list.* = .{};
    try free_list.appendContiguousRange(0, @intFromPtr(backing.ptr), 32);
    const device = pci.resourceIdFromLocation(.{ .bus = 0, .device = 2, .function = 0 });
    try TestSupport.begin(free_list, device);
    defer TestSupport.end();
    const page = try free_list.popFront();
    const iova = iova_window_start;
    try std.testing.expect(reserveIova(device, iova, 1));
    TestSupport.failInvalidation(true);
    try std.testing.expect(!mapPages(device, iova, &.{page}, true, true));
    freeIova(device, iova, 1);
    try std.testing.expectEqual(@as(usize, 1), TestSupport.usedPages(device));
    try std.testing.expectError(types.KernelError.InvalidState, free_list.appendPage(0, page));
}

test "VT-d lifetime partial map allocation failure rolls back or quarantines only published pages" {
    inline for (.{ false, true }) |fail_drain| {
        const allocator = std.testing.allocator;
        const backing = try allocator.alignedAlloc(u8, .fromByteUnits(4096), 32 * 4096);
        defer allocator.free(backing);
        const free_list = try allocator.create(types.FreePageList);
        defer allocator.destroy(free_list);
        free_list.* = .{};
        try free_list.appendContiguousRange(0, @intFromPtr(backing.ptr), 32);
        const device = pci.resourceIdFromLocation(.{ .bus = 0, .device = 2, .function = 0 });
        try TestSupport.begin(free_list, device);
        defer TestSupport.end();
        const first = try free_list.popFront();
        const second = try free_list.popFront();
        // A range crossing a PT boundary needs an additional leaf and ledger
        // for its second page. Leave RAM for only the first page's walk.
        const iova = iova_window_start + 2 * 1024 * 1024 - page_size;
        try std.testing.expect(reserveIova(device, iova, 2));
        while (free_list.pageCount() > 4) _ = try free_list.popFront();
        TestSupport.failInvalidation(fail_drain);
        try std.testing.expect(!mapPages(device, iova, &.{ first, second }, true, true));
        try std.testing.expectEqual(@as(usize, @intFromBool(fail_drain)), TestSupport.mappingReferences());
        freeIova(device, iova, 2);
        try std.testing.expectEqual(@as(usize, if (fail_drain) 2 else 0), TestSupport.usedPages(device));
        if (fail_drain) {
            try std.testing.expectError(types.KernelError.InvalidState, free_list.appendPage(0, first));
        } else {
            try free_list.appendPage(0, first);
        }
        try free_list.appendPage(0, second);
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
    state.* = try kernel.KernelState.initFromDetectedRegions(1);
    const free_list = try allocator.create(types.FreePageList);
    defer allocator.destroy(free_list);
    free_list.* = .{};
    try free_list.appendContiguousRange(0, @intFromPtr(backing.ptr), 32);
    const owner = kernel.processPrincipalFromIndex(0).?;
    const device = pci.resourceIdFromLocation(.{ .bus = 0, .device = 2, .function = 0 });
    try TestSupport.begin(free_list, device);
    defer TestSupport.end();
    const page = try free_list.popFront();
    const vmo_fd = try state.createAnonymousVmoFd(owner, page_size, .{ .close = true, .map_read = true, .map_write = true }, .{}, 16);
    const vmo = state.nativeVmoRefForFd(owner, vmo_fd).?;
    try state.installNativeVmoPages(vmo, 0, &.{page});
    const va: u64 = 0x4400_0000;
    _ = try state.mmapFd(owner, vmo_fd, va, page_size, .{ .read = true, .write = true }, .{ .anonymous = true }, 0);
    const iova = iova_window_start;
    try std.testing.expect(reserveIova(device, iova, 1));
    try std.testing.expect(mapPages(device, iova, &.{page}, true, true));
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
    try std.testing.expectError(types.KernelError.InvalidState, free_list.appendPage(0, page));
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
    try free_list.appendContiguousRange(0, @intFromPtr(backing.ptr), 32);
    const device = pci.resourceIdFromLocation(.{ .bus = 0, .device = 2, .function = 0 });
    try TestSupport.beginWithTableCache(free_list, device, false);
    defer TestSupport.end();
    try std.testing.expect(driver_state.table_cache_line_bytes != 0);
    // Exercise the same initial-root publication helper that precedes SRTP.
    driver_state.root_paddr = allocTablePage().?;
    const context = allocTablePage().?;
    driver_state.context_table_paddrs[0] = context;
    try std.testing.expect(tables.setRootEntry(tableAt(driver_state.root_paddr), 0, context));
    try std.testing.expect(tables.setContextEntry(tableAt(context), 0, driver_state.domains[0].second_level_root_paddr, 1));
    test_cache_flush_count = 0;
    flushInitialTables();
    try std.testing.expectEqual(@as(usize, 2), test_cache_flush_count);
    try std.testing.expectEqual(context, test_cache_flush_events[0].address);
    try std.testing.expectEqual(driver_state.root_paddr, test_cache_flush_events[1].address);
    try std.testing.expectEqual(context | tables.root_present, test_cache_flush_events[1].first_word);

    const first = try free_list.popFront();
    const second = try free_list.popFront();
    const iova = iova_window_start;
    try std.testing.expect(reserveIova(device, iova, 2));
    test_cache_flush_count = 0;
    try std.testing.expect(mapPages(device, iova, &.{ first, second }, true, true));
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
    try std.testing.expect(unmapRangeForDevice(device, iova, 2 * page_size));
    try std.testing.expectEqual(@as(usize, 2), test_flush_count_at_invalidation);
    try std.testing.expectEqual(@as(usize, 4), test_cache_flush_count);
    try std.testing.expectEqual(first, test_cache_flush_events[0].first_word);
    try std.testing.expectEqual(second, test_cache_flush_events[1].first_word);
    try std.testing.expectEqual(@as(u64, 0), test_cache_flush_events[2].first_word);
    try std.testing.expectEqual(@as(u64, 0), test_cache_flush_events[3].first_word);
    freeIova(device, iova, 2);
    try free_list.appendPage(0, first);
    try free_list.appendPage(0, second);
}
