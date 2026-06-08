const std = @import("std");
const uefi = std.os.uefi;
const kernel_log = @import("kernel_log.zig");

const page_size: u64 = 4096;
const page_mask: u64 = ~(page_size - 1);

const root_entry_present: u64 = 1;
const context_present: u64 = 1;
const second_level_read: u64 = 1;
const second_level_write: u64 = 1 << 1;
const second_level_super_page: u64 = 1 << 7;

const context_agaw_39_bit: u64 = 1;
const agaw_39_iova_bits: u6 = 39;
const agaw_39_iova_limit: u64 = @as(u64, 1) << agaw_39_iova_bits;
const domain_id: u64 = 1;
const identity_gib: usize = 2;
const msi_identity_base: u64 = 0xfee0_0000;
const msi_identity_size: u64 = 0x0010_0000;

const max_units: usize = 8;
const max_dynamic_l2_tables: usize = 16;
const max_dynamic_l1_tables: usize = 256;
const max_translation_refs: usize = 2048;

const reg_cap: u64 = 0x08;
const reg_ecap: u64 = 0x10;
const reg_gcmd: u64 = 0x18;
const reg_gsts: u64 = 0x1c;
const reg_rtaddr: u64 = 0x20;
const reg_ccmd: u64 = 0x28;

const gcmd_te: u32 = 1 << 31;
const gcmd_srtp: u32 = 1 << 30;
const gsts_tes: u32 = 1 << 31;
const gsts_rtps: u32 = 1 << 30;

const ccmd_icc: u64 = 1 << 63;
const ccmd_cirg_global: u64 = 1 << 61;
const iotlb_ivt: u64 = 1 << 63;
const iotlb_iirg_global: u64 = 1 << 60;

const DrhdUnit = struct {
    reg_base: u64 = 0,
    segment: u16 = 0,
    include_all: bool = false,
    enabled: bool = false,
};

const DynamicL2Binding = struct {
    valid: bool = false,
    l3_index: u16 = 0,
};

const DynamicL1Binding = struct {
    valid: bool = false,
    l3_index: u16 = 0,
    l2_index: u16 = 0,
};

const TranslationRef = struct {
    valid: bool = false,
    iova_page: u64 = 0,
    paddr_page: u64 = 0,
    refs: u32 = 0,
};

var root_table: [512]u64 align(4096) = [_]u64{0} ** 512;
var context_table: [512]u64 align(4096) = [_]u64{0} ** 512;
var sl_l3_table: [512]u64 align(4096) = [_]u64{0} ** 512;
var identity_l2_tables: [identity_gib][512]u64 align(4096) =
    [_][512]u64{[_]u64{0} ** 512} ** identity_gib;
var dynamic_l2_tables: [max_dynamic_l2_tables][512]u64 align(4096) =
    [_][512]u64{[_]u64{0} ** 512} ** max_dynamic_l2_tables;
var dynamic_l1_tables: [max_dynamic_l1_tables][512]u64 align(4096) =
    [_][512]u64{[_]u64{0} ** 512} ** max_dynamic_l1_tables;
var dynamic_l2_bindings: [max_dynamic_l2_tables]DynamicL2Binding =
    [_]DynamicL2Binding{.{}} ** max_dynamic_l2_tables;
var dynamic_l1_bindings: [max_dynamic_l1_tables]DynamicL1Binding =
    [_]DynamicL1Binding{.{}} ** max_dynamic_l1_tables;
var translation_refs: [max_translation_refs]TranslationRef =
    [_]TranslationRef{.{}} ** max_translation_refs;
var units: [max_units]DrhdUnit = [_]DrhdUnit{.{}} ** max_units;
var unit_count: usize = 0;
var initialized: bool = false;
var trace_map_count: u32 = 0;
var trace_unmap_count: u32 = 0;

fn staticStorageStart(comptime T: type, ptr: *T) usize {
    return @intFromPtr(ptr);
}

fn staticStorageEnd(comptime T: type, ptr: *T) usize {
    return @intFromPtr(ptr) + @sizeOf(T);
}

fn minStaticStart(a: usize, b: usize) usize {
    return if (a < b) a else b;
}

fn maxStaticEnd(a: usize, b: usize) usize {
    return if (a > b) a else b;
}

pub fn kernelStaticStorageStartAddr() usize {
    var start = staticStorageStart(@TypeOf(root_table), &root_table);
    start = minStaticStart(start, staticStorageStart(@TypeOf(context_table), &context_table));
    start = minStaticStart(start, staticStorageStart(@TypeOf(sl_l3_table), &sl_l3_table));
    start = minStaticStart(start, staticStorageStart(@TypeOf(identity_l2_tables), &identity_l2_tables));
    start = minStaticStart(start, staticStorageStart(@TypeOf(dynamic_l2_tables), &dynamic_l2_tables));
    start = minStaticStart(start, staticStorageStart(@TypeOf(dynamic_l1_tables), &dynamic_l1_tables));
    start = minStaticStart(start, staticStorageStart(@TypeOf(dynamic_l2_bindings), &dynamic_l2_bindings));
    start = minStaticStart(start, staticStorageStart(@TypeOf(dynamic_l1_bindings), &dynamic_l1_bindings));
    start = minStaticStart(start, staticStorageStart(@TypeOf(translation_refs), &translation_refs));
    start = minStaticStart(start, staticStorageStart(@TypeOf(units), &units));
    return start;
}

pub fn kernelStaticStorageEndAddr() usize {
    var end = staticStorageEnd(@TypeOf(root_table), &root_table);
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(context_table), &context_table));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(sl_l3_table), &sl_l3_table));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(identity_l2_tables), &identity_l2_tables));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(dynamic_l2_tables), &dynamic_l2_tables));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(dynamic_l1_tables), &dynamic_l1_tables));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(dynamic_l2_bindings), &dynamic_l2_bindings));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(dynamic_l1_bindings), &dynamic_l1_bindings));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(translation_refs), &translation_refs));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(units), &units));
    return end;
}

fn readU8(addr: u64) u8 {
    const p: [*]const u8 = @ptrFromInt(addr);
    return p[0];
}

fn readU16(addr: u64) u16 {
    const p: [*]const u8 = @ptrFromInt(addr);
    return @as(u16, p[0]) | (@as(u16, p[1]) << 8);
}

fn readU32(addr: u64) u32 {
    const p: [*]const u8 = @ptrFromInt(addr);
    return @as(u32, p[0]) |
        (@as(u32, p[1]) << 8) |
        (@as(u32, p[2]) << 16) |
        (@as(u32, p[3]) << 24);
}

fn readU64(addr: u64) u64 {
    return @as(u64, readU32(addr)) | (@as(u64, readU32(addr + 4)) << 32);
}

fn bytesEqual(addr: u64, expected: []const u8) bool {
    const p: [*]const u8 = @ptrFromInt(addr);
    var i: usize = 0;
    while (i < expected.len) : (i += 1) {
        if (p[i] != expected[i]) return false;
    }
    return true;
}

fn checksumOk(addr: u64, len: usize) bool {
    const p: [*]const u8 = @ptrFromInt(addr);
    var sum: u8 = 0;
    var i: usize = 0;
    while (i < len) : (i += 1) sum +%= p[i];
    return sum == 0;
}

fn tableLength(addr: u64) usize {
    return @intCast(readU32(addr + 4));
}

fn findRsdp() ?u64 {
    const ct = uefi.tables.ConfigurationTable;
    const st = uefi.system_table;
    var fallback: ?u64 = null;
    var i: usize = 0;
    while (i < st.number_of_table_entries) : (i += 1) {
        const entry = st.configuration_table[i];
        if (std.meta.eql(entry.vendor_guid, ct.acpi_20_table_guid)) {
            return @intFromPtr(entry.vendor_table);
        }
        if (std.meta.eql(entry.vendor_guid, ct.acpi_10_table_guid)) {
            fallback = @intFromPtr(entry.vendor_table);
        }
    }
    return fallback;
}

fn findTableFromRoot(root_addr: u64, xsdt: bool, signature: []const u8) ?u64 {
    if (signature.len != 4) return null;
    if (!bytesEqual(root_addr, if (xsdt) "XSDT" else "RSDT")) return null;
    const len = tableLength(root_addr);
    if (len < 36 or !checksumOk(root_addr, len)) return null;
    const entry_size: usize = if (xsdt) 8 else 4;
    var off: usize = 36;
    while (off + entry_size <= len) : (off += entry_size) {
        const table_addr = if (xsdt) readU64(root_addr + off) else @as(u64, readU32(root_addr + off));
        if (table_addr == 0) continue;
        if (bytesEqual(table_addr, signature)) return table_addr;
    }
    return null;
}

fn findAcpiTable(signature: []const u8) ?u64 {
    const rsdp = findRsdp() orelse return null;
    if (!bytesEqual(rsdp, "RSD PTR ")) return null;
    const revision = readU8(rsdp + 15);
    if (!checksumOk(rsdp, 20)) return null;
    if (revision >= 2) {
        const len = readU32(rsdp + 20);
        const xsdt_addr = readU64(rsdp + 24);
        if (len >= 36 and xsdt_addr != 0 and checksumOk(rsdp, @intCast(len))) {
            if (findTableFromRoot(xsdt_addr, true, signature)) |table| return table;
        }
    }
    const rsdt_addr = readU32(rsdp + 16);
    if (rsdt_addr == 0) return null;
    return findTableFromRoot(rsdt_addr, false, signature);
}

fn mmioRead32(base: u64, off: u64) u32 {
    const ptr: *volatile u32 = @ptrFromInt(base + off);
    return ptr.*;
}

fn mmioWrite32(base: u64, off: u64, value: u32) void {
    const ptr: *volatile u32 = @ptrFromInt(base + off);
    ptr.* = value;
}

fn mmioRead64(base: u64, off: u64) u64 {
    const ptr: *volatile u64 = @ptrFromInt(base + off);
    return ptr.*;
}

fn mmioWrite64(base: u64, off: u64, value: u64) void {
    const ptr: *volatile u64 = @ptrFromInt(base + off);
    ptr.* = value;
}

fn pause() void {
    asm volatile ("pause");
}

fn waitStatus(base: u64, mask: u32, expected_set: bool) bool {
    var spins: usize = 0;
    while (spins < 1_000_000) : (spins += 1) {
        const value = mmioRead32(base, reg_gsts);
        const is_set = (value & mask) != 0;
        if (is_set == expected_set) return true;
        pause();
    }
    return false;
}

fn issueGlobalCommand(base: u64, command_bit: u32, status_bit: u32, expected_set: bool) bool {
    var command = mmioRead32(base, reg_gsts);
    if (expected_set) {
        command |= command_bit;
    } else {
        command &= ~command_bit;
    }
    mmioWrite32(base, reg_gcmd, command);
    return waitStatus(base, status_bit, expected_set);
}

fn iotlbRegisterOffset(base: u64) u64 {
    const ecap = mmioRead64(base, reg_ecap);
    return ((ecap >> 8) & 0x3ff) * 16;
}

fn invalidateContextGlobal(base: u64) void {
    mmioWrite64(base, reg_ccmd, ccmd_icc | ccmd_cirg_global);
    var spins: usize = 0;
    while (spins < 1_000_000) : (spins += 1) {
        if ((mmioRead64(base, reg_ccmd) & ccmd_icc) == 0) return;
        pause();
    }
}

fn invalidateIotlbGlobal(base: u64) void {
    const offset = iotlbRegisterOffset(base);
    if (offset == 0) return;
    mmioWrite64(base, offset + 8, 0);
    mmioWrite64(base, offset, iotlb_ivt | iotlb_iirg_global);
    var spins: usize = 0;
    while (spins < 1_000_000) : (spins += 1) {
        if ((mmioRead64(base, offset) & iotlb_ivt) == 0) return;
        pause();
    }
}

fn invalidateAllUnits() void {
    var i: usize = 0;
    while (i < unit_count) : (i += 1) {
        if (!units[i].enabled) continue;
        invalidateContextGlobal(units[i].reg_base);
        invalidateIotlbGlobal(units[i].reg_base);
    }
}

fn setupRootAndContextTables() void {
    @memset(root_table[0..], 0);
    @memset(context_table[0..], 0);

    const context_paddr = @intFromPtr(&context_table);
    var bus: usize = 0;
    while (bus < 256) : (bus += 1) {
        root_table[bus * 2] = (@as(u64, @intCast(context_paddr)) & page_mask) | root_entry_present;
        root_table[bus * 2 + 1] = 0;
    }

    const sl_root_paddr = @intFromPtr(&sl_l3_table);
    var devfn: usize = 0;
    while (devfn < 256) : (devfn += 1) {
        context_table[devfn * 2] = (@as(u64, @intCast(sl_root_paddr)) & page_mask) | context_present;
        context_table[devfn * 2 + 1] = (domain_id << 8) | context_agaw_39_bit;
    }
}

fn setupSecondLevelTables() void {
    @memset(sl_l3_table[0..], 0);
    @memset(dynamic_l2_bindings[0..], .{});
    @memset(dynamic_l1_bindings[0..], .{});
    @memset(translation_refs[0..], .{});

    var gib: usize = 0;
    while (gib < identity_gib) : (gib += 1) {
        @memset(identity_l2_tables[gib][0..], 0);
        var entry: usize = 0;
        while (entry < 512) : (entry += 1) {
            const paddr = (@as(u64, @intCast(gib)) << 30) + (@as(u64, @intCast(entry)) << 21);
            identity_l2_tables[gib][entry] =
                (paddr & ~@as(u64, 0x1f_ffff)) |
                second_level_read |
                second_level_write |
                second_level_super_page;
        }
        sl_l3_table[gib] = (@as(u64, @intCast(@intFromPtr(&identity_l2_tables[gib]))) & page_mask) |
            second_level_read |
            second_level_write;
    }

    var i: usize = 0;
    while (i < max_dynamic_l2_tables) : (i += 1) {
        @memset(dynamic_l2_tables[i][0..], 0);
    }
    i = 0;
    while (i < max_dynamic_l1_tables) : (i += 1) {
        @memset(dynamic_l1_tables[i][0..], 0);
    }

    mapStaticIdentityRange(msi_identity_base, msi_identity_size);
}

fn addUnit(reg_base: u64, segment: u16, include_all: bool) void {
    if (reg_base == 0 or unit_count >= max_units) return;
    units[unit_count] = .{
        .reg_base = reg_base,
        .segment = segment,
        .include_all = include_all,
        .enabled = false,
    };
    unit_count += 1;
}

fn parseDmar() bool {
    unit_count = 0;
    const dmar = findAcpiTable("DMAR") orelse return false;
    const len = tableLength(dmar);
    if (len < 48 or !checksumOk(dmar, len)) return false;

    var off: usize = 48;
    while (off + 4 <= len) {
        const entry_addr = dmar + @as(u64, @intCast(off));
        const entry_type = readU16(entry_addr);
        const entry_len = readU16(entry_addr + 2);
        if (entry_len < 4 or off + entry_len > len) break;
        if (entry_type == 0 and entry_len >= 16) {
            const flags = readU8(entry_addr + 4);
            const segment = readU16(entry_addr + 6);
            const reg_base = readU64(entry_addr + 8);
            addUnit(reg_base, segment, (flags & 0x1) != 0);
        }
        off += entry_len;
    }
    return unit_count != 0;
}

fn capSupportsAgaw39(base: u64) bool {
    const cap = mmioRead64(base, reg_cap);
    const sagaw = (cap >> 8) & 0x1f;
    return (sagaw & (1 << context_agaw_39_bit)) != 0;
}

fn enableUnit(unit: *DrhdUnit) bool {
    if (!capSupportsAgaw39(unit.reg_base)) {
        return false;
    }
    if ((mmioRead32(unit.reg_base, reg_gsts) & gsts_tes) != 0) {
        _ = issueGlobalCommand(unit.reg_base, gcmd_te, gsts_tes, false);
    }
    mmioWrite64(unit.reg_base, reg_rtaddr, @as(u64, @intCast(@intFromPtr(&root_table))) & page_mask);
    if (!issueGlobalCommand(unit.reg_base, gcmd_srtp, gsts_rtps, true)) {
        return false;
    }
    invalidateContextGlobal(unit.reg_base);
    invalidateIotlbGlobal(unit.reg_base);
    if (!issueGlobalCommand(unit.reg_base, gcmd_te, gsts_tes, true)) {
        return false;
    }
    unit.enabled = true;
    return true;
}

pub fn init() bool {
    if (initialized) return true;
    if (!parseDmar()) {
        return false;
    }
    setupSecondLevelTables();
    setupRootAndContextTables();

    var enabled_count: usize = 0;
    var i: usize = 0;
    while (i < unit_count) : (i += 1) {
        if (enableUnit(&units[i])) enabled_count += 1;
    }
    initialized = enabled_count != 0;
    if (initialized) {
        kernel_log.write("vtd: enabled hardware DMA translation\n");
    }
    return initialized;
}

pub fn isActive() bool {
    return initialized;
}

pub fn isAddressableRange(iova: u64, size: u64) bool {
    if (size == 0) return false;
    const last, const overflow = @addWithOverflow(iova, size - 1);
    if (overflow != 0) return false;
    return last < agaw_39_iova_limit;
}

fn l3Index(iova: u64) usize {
    return @intCast((iova >> 30) & 0x1ff);
}

fn l2Index(iova: u64) usize {
    return @intCast((iova >> 21) & 0x1ff);
}

fn l1Index(iova: u64) usize {
    return @intCast((iova >> 12) & 0x1ff);
}

fn dynamicL2For(index: usize, create: bool) ?*[512]u64 {
    var i: usize = 0;
    while (i < max_dynamic_l2_tables) : (i += 1) {
        if (dynamic_l2_bindings[i].valid and dynamic_l2_bindings[i].l3_index == index) {
            return &dynamic_l2_tables[i];
        }
    }
    if (!create) return null;
    i = 0;
    while (i < max_dynamic_l2_tables) : (i += 1) {
        if (dynamic_l2_bindings[i].valid) continue;
        @memset(dynamic_l2_tables[i][0..], 0);
        dynamic_l2_bindings[i] = .{ .valid = true, .l3_index = @intCast(index) };
        sl_l3_table[index] = (@as(u64, @intCast(@intFromPtr(&dynamic_l2_tables[i]))) & page_mask) |
            second_level_read |
            second_level_write;
        return &dynamic_l2_tables[i];
    }
    return null;
}

fn dynamicL1For(l3: usize, l2: usize, l2_table: *[512]u64, create: bool) ?*[512]u64 {
    var i: usize = 0;
    while (i < max_dynamic_l1_tables) : (i += 1) {
        if (dynamic_l1_bindings[i].valid and
            dynamic_l1_bindings[i].l3_index == l3 and
            dynamic_l1_bindings[i].l2_index == l2)
        {
            return &dynamic_l1_tables[i];
        }
    }
    if (!create) return null;
    i = 0;
    while (i < max_dynamic_l1_tables) : (i += 1) {
        if (dynamic_l1_bindings[i].valid) continue;
        @memset(dynamic_l1_tables[i][0..], 0);
        dynamic_l1_bindings[i] = .{
            .valid = true,
            .l3_index = @intCast(l3),
            .l2_index = @intCast(l2),
        };
        l2_table[l2] = (@as(u64, @intCast(@intFromPtr(&dynamic_l1_tables[i]))) & page_mask) |
            second_level_read |
            second_level_write;
        return &dynamic_l1_tables[i];
    }
    return null;
}

fn mapStaticIdentityPage(iova: u64) bool {
    const iova_page = iova & page_mask;
    const l3 = l3Index(iova_page);
    const l2 = l2Index(iova_page);
    const l1 = l1Index(iova_page);
    const l2_table = dynamicL2For(l3, true) orelse return false;
    if ((l2_table[l2] & second_level_super_page) != 0) return false;
    const l1_table = dynamicL1For(l3, l2, l2_table, true) orelse return false;
    l1_table[l1] = iova_page | second_level_read | second_level_write;
    return true;
}

fn mapStaticIdentityRange(base: u64, size: u64) void {
    var offset: u64 = 0;
    while (offset < size) : (offset += page_size) {
        _ = mapStaticIdentityPage(base + offset);
    }
}

fn addTranslationRef(iova_page: u64, paddr_page: u64) bool {
    var free_slot: ?usize = null;
    var i: usize = 0;
    while (i < max_translation_refs) : (i += 1) {
        if (!translation_refs[i].valid) {
            if (free_slot == null) free_slot = i;
            continue;
        }
        if (translation_refs[i].iova_page != iova_page) continue;
        if (translation_refs[i].paddr_page != paddr_page) return false;
        translation_refs[i].refs += 1;
        return true;
    }
    const slot = free_slot orelse return false;
    translation_refs[slot] = .{
        .valid = true,
        .iova_page = iova_page,
        .paddr_page = paddr_page,
        .refs = 1,
    };
    return true;
}

fn releaseTranslationRef(iova_page: u64) bool {
    var i: usize = 0;
    while (i < max_translation_refs) : (i += 1) {
        if (!translation_refs[i].valid or translation_refs[i].iova_page != iova_page) continue;
        if (translation_refs[i].refs > 1) {
            translation_refs[i].refs -= 1;
            return false;
        }
        translation_refs[i] = .{};
        return true;
    }
    return false;
}

fn mapPage(iova: u64, paddr: u64) bool {
    const iova_page = iova & page_mask;
    const paddr_page = paddr & page_mask;
    const l3 = l3Index(iova_page);
    const l2 = l2Index(iova_page);
    const l1 = l1Index(iova_page);

    if (l3 < identity_gib) {
        return iova_page == paddr_page;
    }

    const l2_table = dynamicL2For(l3, true) orelse return false;
    if ((l2_table[l2] & second_level_super_page) != 0) return false;
    const l1_table = dynamicL1For(l3, l2, l2_table, true) orelse return false;
    const old = l1_table[l1];
    if ((old & (second_level_read | second_level_write)) != 0 and (old & page_mask) != paddr_page) {
        return false;
    }
    if (!addTranslationRef(iova_page, paddr_page)) {
        return false;
    }
    l1_table[l1] = paddr_page | second_level_read | second_level_write;
    return true;
}

fn unmapPage(iova: u64) void {
    const iova_page = iova & page_mask;
    const l3 = l3Index(iova_page);
    const l2 = l2Index(iova_page);
    const l1 = l1Index(iova_page);
    if (l3 < identity_gib) return;
    if (!releaseTranslationRef(iova_page)) return;
    const l2_table = dynamicL2For(l3, false) orelse return;
    if ((l2_table[l2] & second_level_super_page) != 0) return;
    const l1_table = dynamicL1For(l3, l2, l2_table, false) orelse return;
    l1_table[l1] = 0;
}

pub fn mapRange(iova: u64, paddr: u64, size: u64) bool {
    if (!initialized) return true;
    if (iova == 0 or paddr == 0 or size == 0) return false;
    if (!isAddressableRange(iova, size)) return false;
    const offset = iova & (page_size - 1);
    if ((paddr & (page_size - 1)) != offset) return false;
    const start_iova = iova - offset;
    const start_paddr = paddr - offset;
    const span, const overflow = @addWithOverflow(offset, size);
    if (overflow != 0) return false;
    const page_count = (span + page_size - 1) / page_size;
    const should_trace = iova >= 0x100000000 and trace_map_count < 96;
    if (should_trace) {
        trace_map_count += 1;
        kernel_log.writeFmt(
            "vtd: map iova=0x{x} paddr=0x{x} size=0x{x} pages={}\n",
            .{ iova, paddr, size, page_count },
        );
    }
    var page: u64 = 0;
    while (page < page_count) : (page += 1) {
        if (!mapPage(start_iova + page * page_size, start_paddr + page * page_size)) {
            var rollback: u64 = 0;
            while (rollback < page) : (rollback += 1) {
                unmapPage(start_iova + rollback * page_size);
            }
            invalidateAllUnits();
            if (should_trace) {
                kernel_log.writeFmt(
                    "vtd: map failed iova=0x{x} page={}\n",
                    .{ iova, page },
                );
            }
            return false;
        }
    }
    invalidateAllUnits();
    return true;
}

pub fn unmapRange(iova: u64, size: u64) void {
    if (!initialized or iova == 0 or size == 0) return;
    if (!isAddressableRange(iova, size)) return;
    const offset = iova & (page_size - 1);
    const start_iova = iova - offset;
    const span, const overflow = @addWithOverflow(offset, size);
    if (overflow != 0) return;
    const page_count = (span + page_size - 1) / page_size;
    if (iova >= 0x100000000 and trace_unmap_count < 64) {
        trace_unmap_count += 1;
        kernel_log.writeFmt(
            "vtd: unmap iova=0x{x} size=0x{x} pages={}\n",
            .{ iova, size, page_count },
        );
    }
    var page: u64 = 0;
    while (page < page_count) : (page += 1) {
        unmapPage(start_iova + page * page_size);
    }
    invalidateAllUnits();
}
