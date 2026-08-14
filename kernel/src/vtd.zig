const std = @import("std");
const acpi_dmar = @import("acpi_dmar.zig");
const kernel_log = @import("kernel_log.zig");
const pci = @import("pci.zig");
const types = @import("state/types.zig");
const user_copy = @import("user_copy.zig");
const tables = @import("vtd_tables.zig");

pub const Discovery = struct {
    table_paddr: u64,
    info: acpi_dmar.DmarInfo,
};

const page_size: u64 = 4096;
const four_gib: u64 = 4 * 1024 * 1024 * 1024;
const domain_id: u16 = 1;
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
const iotlb_iaig_shift: u6 = 57;
const fault_record_valid: u64 = @as(u64, 1) << 63;

const mtrr_cap_msr: u32 = 0x0000_00fe;
const mtrr_def_type_msr: u32 = 0x0000_02ff;
const mtrr_physbase0_msr: u32 = 0x0000_0200;
const mtrr_enabled: u64 = 1 << 11;
const mtrr_valid: u64 = 1 << 11;
const memory_type_uc: u8 = 0;

const LeafMetadata = struct {
    table_paddr: u64 = 0,
    refcounts_paddr: u64 = 0,
};

const DriverState = struct {
    discovery: ?Discovery = null,
    free_list: ?*types.FreePageList = null,
    register_base: u64 = 0,
    cap: u64 = 0,
    ecap: u64 = 0,
    root_paddr: u64 = 0,
    second_level_root_paddr: u64 = 0,
    context_table_paddrs: [256]u64 = [_]u64{0} ** 256,
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
    active: bool = false,
    lock_word: u8 = 0,
};

var driver_state: DriverState = .{};

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

fn rdmsr(msr: u32) u64 {
    var low: u32 = 0;
    var high: u32 = 0;
    asm volatile ("rdmsr"
        : [low] "={eax}" (low),
          [high] "={edx}" (high),
        : [msr] "{ecx}" (msr),
    );
    return (@as(u64, high) << 32) | low;
}

fn physicalAddressBits() u8 {
    if (cpuid(0x8000_0000).eax < 0x8000_0008) return 36;
    const reported: u8 = @truncate(cpuid(0x8000_0008).eax);
    return @min(reported, 52);
}

/// The kernel's low identity map covers this register page. Its paging entry
/// uses the normal WB encoding, so firmware MTRRs must make the MMIO address
/// UC before it is safe to dereference as a register block.
fn mmioIsUncachedByMtrr(paddr: u64) bool {
    if ((cpuid(1).edx & (@as(u32, 1) << 12)) == 0) return false;
    const mtrr_cap = rdmsr(mtrr_cap_msr);
    const def_type = rdmsr(mtrr_def_type_msr);
    if ((def_type & mtrr_enabled) == 0) return false;

    const address_bits = physicalAddressBits();
    const physical_mask = ((@as(u64, 1) << @intCast(address_bits)) - 1) & tables.page_address_mask;
    const variable_count: usize = @intCast(mtrr_cap & 0xff);
    var matched = false;
    var index: usize = 0;
    while (index < variable_count) : (index += 1) {
        const base = rdmsr(mtrr_physbase0_msr + @as(u32, @intCast(index * 2)));
        const mask = rdmsr(mtrr_physbase0_msr + @as(u32, @intCast(index * 2 + 1)));
        if ((mask & mtrr_valid) == 0) continue;
        const range_mask = mask & physical_mask;
        if ((paddr & range_mask) != (base & range_mask)) continue;
        matched = true;
        if (@as(u8, @truncate(base)) == memory_type_uc) return true;
    }
    return !matched and @as(u8, @truncate(def_type)) == memory_type_uc;
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
    const ptr: *volatile u64 = @ptrFromInt(driver_state.register_base + offset);
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
    const paddr = free_list.popFrontBelow(four_gib) catch return null;
    if (!user_copy.zeroPhysicalPage(paddr)) {
        free_list.appendPage(0, paddr) catch {};
        return null;
    }
    return paddr;
}

fn tableAt(paddr: u64) *tables.TablePage {
    return @ptrFromInt(paddr);
}

fn pageAddress(entry: u64) u64 {
    return entry & tables.page_address_mask;
}

fn ensureChildTable(parent: *tables.TablePage, index: usize) ?u64 {
    const existing = parent[index];
    if ((existing & (tables.second_level_read | tables.second_level_write)) != 0) {
        const paddr = pageAddress(existing);
        return if (paddr != 0) paddr else null;
    }
    const child = allocZeroPage() orelse return null;
    if (!tables.setSecondLevelEntry(parent, index, child, true, true)) return null;
    return child;
}

fn leafMetadata(table_paddr: u64, create: bool) ?*LeafMetadata {
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
        result.* = .{ .table_paddr = table_paddr, .refcounts_paddr = refcounts };
        driver_state.leaf_count += 1;
        driver_state.leaf_index[slot_index] = @intCast(metadata_index + 1);
        return result;
    }
    return null;
}

fn refcountsAt(paddr: u64) *[512]u16 {
    return @ptrFromInt(paddr);
}

fn walkToLeaf(iova: u64, create: bool) ?struct { table: *tables.TablePage, metadata: *LeafMetadata, index: usize } {
    const pml4 = tableAt(driver_state.second_level_root_paddr);
    const pml4_index: usize = @intCast((iova >> 39) & 0x1ff);
    const pdp_paddr = if (create)
        ensureChildTable(pml4, pml4_index) orelse return null
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
    const metadata = leafMetadata(pt_paddr, create) orelse return null;
    return .{
        .table = tableAt(pt_paddr),
        .metadata = metadata,
        .index = @intCast((iova >> 12) & 0x1ff),
    };
}

fn mapIdentityPage(paddr: u64) bool {
    const leaf = walkToLeaf(paddr, true) orelse return false;
    const refs = refcountsAt(leaf.metadata.refcounts_paddr);
    if (refs[leaf.index] == std.math.maxInt(u16)) return false;
    const existing = leaf.table[leaf.index];
    if (existing != 0 and pageAddress(existing) != paddr) return false;
    if (existing == 0 and !tables.setSecondLevelEntry(leaf.table, leaf.index, paddr, true, true)) return false;
    if (refs[leaf.index] == 0) driver_state.mapped_page_count += 1;
    refs[leaf.index] += 1;
    driver_state.mapping_ref_count += 1;
    return true;
}

fn unmapIdentityPage(paddr: u64) void {
    const leaf = walkToLeaf(paddr, false) orelse return;
    const refs = refcountsAt(leaf.metadata.refcounts_paddr);
    if (refs[leaf.index] == 0) return;
    refs[leaf.index] -= 1;
    driver_state.mapping_ref_count -= 1;
    if (refs[leaf.index] == 0) {
        leaf.table[leaf.index] = 0;
        driver_state.mapped_page_count -= 1;
    }
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

fn flushTranslationChanges() bool {
    asm volatile ("mfence" ::: .{ .memory = true });
    if (!writeBufferFlush()) return false;
    return invalidateIotlbGlobal();
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
    kernel_log.writeFmt("vtd: enable failed reason={s}; pass-through active=0\n", .{reason});
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

fn installPciContexts() bool {
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
                if (driver_state.context_table_paddrs[bus_number] == 0) {
                    const context_paddr = allocZeroPage() orelse return false;
                    if (!tables.setRootEntry(root, @intCast(bus_number), context_paddr)) return false;
                    driver_state.context_table_paddrs[bus_number] = context_paddr;
                }
                const context = tableAt(driver_state.context_table_paddrs[bus_number]);
                const device_function: u8 = @intCast(device_number * 8 + function_number);
                if (!tables.setContextEntry(context, device_function, driver_state.second_level_root_paddr, domain_id)) return false;
                driver_state.context_count += 1;
                kernel_log.writeFmt(
                    "vtd: context bdf={}:{}:{} vendor=0x{x} device=0x{x} domain={}\n",
                    .{ bus_number, device_number, function_number, vendor, pci.readDeviceId(location), domain_id },
                );
            }
        }
    }
    return driver_state.context_count != 0;
}

pub fn init(rsdp_paddr: u64, free_list: *types.FreePageList) void {
    driver_state = .{};
    const table = acpi_dmar.findDmar(rsdp_paddr) orelse {
        kernel_log.write("vtd: DMAR not found active=0 faults=0\n");
        return;
    };
    const info = acpi_dmar.parseDmar(table.bytes) catch |err| {
        kernel_log.writeFmt("vtd: DMAR invalid paddr=0x{x} error={s} active=0 faults=0\n", .{ table.paddr, @errorName(err) });
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
    if (drhd.segment != 0 or drhd.register_base == 0 or (drhd.register_base & (page_size - 1)) != 0) {
        logInitFailure("unsupported DRHD segment or register base");
        return;
    }
    if (drhd.register_base >= 16 * 1024 * 1024 * 1024) {
        logInitFailure("DRHD register base is outside kernel identity map");
        return;
    }
    if (!mmioIsUncachedByMtrr(drhd.register_base)) {
        logInitFailure("DRHD register page is not confirmed uncached by MTRR");
        return;
    }
    driver_state.register_base = drhd.register_base;
    kernel_log.writeFmt("vtd: mmio base=0x{x} identity_mapped=1 cache=UC source=MTRR\n", .{drhd.register_base});

    driver_state.cap = mmioRead64(cap_reg);
    driver_state.ecap = mmioRead64(ecap_reg);
    const sagaw: u5 = @truncate(driver_state.cap >> 8);
    const nd: u3 = @truncate(driver_state.cap);
    const cm = (driver_state.cap & (@as(u64, 1) << 7)) != 0;
    driver_state.rwbf = (driver_state.cap & (@as(u64, 1) << 4)) != 0;
    driver_state.fault_record_offset = ((driver_state.cap >> 24) & 0x3ff) * 16;
    driver_state.fault_record_count = @as(usize, @intCast((driver_state.cap >> 40) & 0xff)) + 1;
    driver_state.iotlb_offset = ((driver_state.ecap >> 8) & 0x3ff) * 16;
    const qi = (driver_state.ecap & (@as(u64, 1) << 1)) != 0;
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

    driver_state.root_paddr = allocZeroPage() orelse {
        logInitFailure("root table allocation failed");
        return;
    };
    driver_state.second_level_root_paddr = allocZeroPage() orelse {
        logInitFailure("second-level root allocation failed");
        return;
    };
    if (!installPciContexts()) {
        logInitFailure("PCI context construction failed");
        return;
    }
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
        "vtd: translation enabled active=1 domain={} contexts={} root=0x{x} slpt=0x{x}\n",
        .{ domain_id, driver_state.context_count, driver_state.root_paddr, driver_state.second_level_root_paddr },
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

pub fn isAddressableRange(paddr: u64, size: u64) bool {
    if (!isActive()) return true;
    if (size == 0 or paddr >= (@as(u64, 1) << 48)) return false;
    const last, const overflow = @addWithOverflow(paddr, size - 1);
    return overflow == 0 and last < (@as(u64, 1) << 48);
}

pub fn mapRange(iova: u64, paddr: u64, size: u64) bool {
    if (!isActive()) return true;
    if (iova != paddr) {
        kernel_log.writeFmt("vtd: map rejected identity mismatch iova=0x{x} paddr=0x{x} size=0x{x}\n", .{ iova, paddr, size });
        dumpFaults();
        return false;
    }
    if (!isAddressableRange(paddr, size)) {
        kernel_log.writeFmt("vtd: map rejected range paddr=0x{x} size=0x{x}\n", .{ paddr, size });
        dumpFaults();
        return false;
    }
    const first = paddr & ~(page_size - 1);
    const last_byte = paddr + size - 1;
    const page_count = ((last_byte & ~(page_size - 1)) - first) / page_size + 1;

    lock();
    defer unlock();
    var mapped: u64 = 0;
    while (mapped < page_count) : (mapped += 1) {
        if (!mapIdentityPage(first + mapped * page_size)) {
            var rollback: u64 = 0;
            while (rollback < mapped) : (rollback += 1) {
                unmapIdentityPage(first + rollback * page_size);
            }
            _ = flushTranslationChanges();
            kernel_log.writeFmt("vtd: map failed paddr=0x{x} pages={} at={}\n", .{ paddr, page_count, mapped });
            dumpFaultsLocked();
            return false;
        }
    }
    if (!flushTranslationChanges()) {
        var rollback: u64 = 0;
        while (rollback < page_count) : (rollback += 1) {
            unmapIdentityPage(first + rollback * page_size);
        }
        _ = flushTranslationChanges();
        kernel_log.writeFmt("vtd: map failed invalidation paddr=0x{x} pages={}\n", .{ paddr, page_count });
        dumpFaultsLocked();
        return false;
    }
    return true;
}

pub fn unmapRange(iova: u64, size: u64) void {
    if (!isActive() or size == 0 or !isAddressableRange(iova, size)) return;
    const first = iova & ~(page_size - 1);
    const last_byte = iova + size - 1;
    const page_count = ((last_byte & ~(page_size - 1)) - first) / page_size + 1;
    lock();
    defer unlock();
    var index: u64 = 0;
    while (index < page_count) : (index += 1) {
        unmapIdentityPage(first + index * page_size);
    }
    if (!flushTranslationChanges()) {
        kernel_log.writeFmt("vtd: unmap invalidation failed iova=0x{x} pages={}\n", .{ iova, page_count });
        dumpFaultsLocked();
    }
}

fn dumpFaultsLocked() void {
    if (!driver_state.active and driver_state.register_base == 0) return;
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
