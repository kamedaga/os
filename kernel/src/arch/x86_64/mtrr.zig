const mtrr_cap_msr: u32 = 0x0000_00fe;
const mtrr_def_type_msr: u32 = 0x0000_02ff;
const mtrr_physbase0_msr: u32 = 0x0000_0200;
const mtrr_enabled: u64 = 1 << 11;
const mtrr_valid: u64 = 1 << 11;
const memory_type_uc: u8 = 0;
const amd_syscfg_msr: u32 = 0xc001_0010;
const amd_top_mem2_msr: u32 = 0xc001_001d;
const amd_tom2_enable: u64 = 1 << 21;
const amd_tom2_force_wb: u64 = 1 << 22;
const amd_tom2_mask: u64 = 0x0000_ffff_ff80_0000;
const std = @import("std");

pub const MemoryType = enum(u8) {
    uc = 0,
    wc = 1,
    wt = 4,
    wp = 5,
    wb = 6,
};

/// Immutable boot-time proof for UC_MINUS BAR mappings. Fixed MTRRs are not
/// needed: admission excludes every address below 1 MiB. No MSR is written.
pub const CacheSnapshot = struct {
    pub const Range = struct { base: u64 = 0, mask: u64 = 0 };
    pat: u64 = 0,
    capability: u64 = 0,
    default_type: u64 = 0,
    physical_bits: u8 = 0,
    amd_pat_mtrr_semantics: bool = false,
    amd_syscfg: u64 = 0,
    amd_top_mem2: u64 = 0,
    ranges: [256]Range = [_]Range{.{}} ** 256,

    fn tom2DefaultWb(self: *const CacheSnapshot) bool {
        return self.amd_pat_mtrr_semantics and
            self.default_type & mtrr_enabled != 0 and
            self.amd_syscfg & (amd_tom2_enable | amd_tom2_force_wb) ==
                (amd_tom2_enable | amd_tom2_force_wb) and
            self.amd_top_mem2 > 0x1_0000_0000 and
            self.amd_top_mem2 <= (@as(u64, 1) << @intCast(self.physical_bits)) and
            self.amd_top_mem2 & ~amd_tom2_mask == 0;
    }

    fn defaultTypeAt(self: *const CacheSnapshot, address: u64) MemoryType {
        // AMD SYSCFG changes only the *unmatched* MTRR default in TOM2.
        // Variable MTRRs are still evaluated first, including UC overrides.
        if (self.tom2DefaultWb() and address >= 0x1_0000_0000 and
            address < self.amd_top_mem2) return .wb;
        return @enumFromInt(@as(u8, @truncate(self.default_type)));
    }

    fn physicalMask(self: *const CacheSnapshot) u64 {
        return ((@as(u64, 1) << @intCast(self.physical_bits)) - 1) & ~@as(u64, 4095);
    }

    pub fn valid(self: *const CacheSnapshot) bool {
        if (self.physical_bits < 32 or self.physical_bits > 52 or
            (self.pat & 0xff) != 6 or ((self.pat >> 16) & 0xff) != 7 or
            ((self.pat >> 24) & 0xff) != 0 or
            self.default_type & mtrr_enabled == 0 or self.default_type & ~@as(u64, 0xcff) != 0 or
            !validType(@truncate(self.default_type), self.capability)) return false;
        if (self.default_type & (1 << 10) != 0 and self.capability & (1 << 8) == 0) return false;
        if (self.amd_syscfg != 0 and !self.amd_pat_mtrr_semantics) return false;
        if (self.amd_syscfg & (amd_tom2_enable | amd_tom2_force_wb) ==
            (amd_tom2_enable | amd_tom2_force_wb))
        {
            if (!self.tom2DefaultWb()) return false;
        } else if (self.amd_top_mem2 != 0) return false;
        const physical_mask = self.physicalMask();
        for (self.ranges[0 .. self.capability & 0xff]) |range| {
            if (range.mask & mtrr_valid == 0) continue;
            if (range.mask & ~(physical_mask | mtrr_valid) != 0 or
                range.base & ~(physical_mask | @as(u64, 0xff)) != 0 or
                !validType(@truncate(range.base), self.capability)) return false;
            const size = (physical_mask & ~range.mask) + 4096;
            if (!std.math.isPowerOfTwo(size) or (range.base & physical_mask) & (size - 1) != 0)
                return false;
        }
        return true;
    }

    pub fn matches(self: *const CacheSnapshot, other: *const CacheSnapshot) bool {
        // valid() fixes entries 0/2/3 to WB/UC_MINUS/UC on both CPUs. These
        // are the only PAT entries used by the kernel identity and MMIO
        // aliases covered by this proof. Entries 1 and 4..7 need not match;
        // this is not a proof for mappings which select those entries.
        if (!self.valid() or !other.valid() or
            self.capability != other.capability or self.default_type != other.default_type or
            self.physical_bits != other.physical_bits or
            self.amd_pat_mtrr_semantics != other.amd_pat_mtrr_semantics or
            (self.amd_syscfg & (amd_tom2_enable | amd_tom2_force_wb)) !=
                (other.amd_syscfg & (amd_tom2_enable | amd_tom2_force_wb)) or
            self.amd_top_mem2 != other.amd_top_mem2) return false;
        for (self.ranges[0 .. self.capability & 0xff], other.ranges[0 .. other.capability & 0xff]) |a, b|
            if (a.base != b.base or a.mask != b.mask) return false;
        return true;
    }

    /// PAT entry 3 can encode explicit UC throughout this physical range.
    /// This does not prove that aliases using other PAT entries are also UC.
    pub fn rangeSupportsUncachedMapping(self: *const CacheSnapshot, start: u64, bytes: u64) bool {
        if (!self.valid() or bytes == 0 or start < 0x100000 or ((start | bytes) & 4095) != 0)
            return false;
        const limit = @as(u64, 1) << @intCast(self.physical_bits);
        return start < limit and bytes <= limit - start;
    }

    /// A PAT slot is usable for WC only when its actual MSR byte is WC. The
    /// existing snapshot proof fixes slots 0/2/3 but intentionally says
    /// nothing about the firmware-controlled entries used by a WC PTE.
    pub fn patSlotIsWc(self: *const CacheSnapshot, slot: u3) bool {
        return self.valid() and @as(u8, @truncate(self.pat >> (@as(u6, slot) * 8))) == @intFromEnum(MemoryType.wc);
    }

    /// Return one uniform effective variable-MTRR type for a BAR range.
    /// AMD APM's defined overlaps are identical types, UC overriding any
    /// other type, and WT+WB producing WT. All other overlaps fail closed.
    pub fn uniformMtrrType(self: *const CacheSnapshot, start: u64, bytes: u64) ?MemoryType {
        if (!self.rangeSupportsUncachedMapping(start, bytes)) return null;
        const physical_mask = self.physicalMask();
        var first: ?MemoryType = null;
        var address = start;
        while (address < start + bytes) : (address += 4096) {
            var matched_types: u8 = 0;
            for (self.ranges[0 .. self.capability & 0xff]) |range| {
                if (range.mask & mtrr_valid == 0) continue;
                const mask = range.mask & physical_mask;
                if ((address & mask) != (range.base & mask)) continue;
                const type_value: u8 = @truncate(range.base);
                matched_types |= @as(u8, 1) << @as(u3, @intCast(type_value));
            }
            const type_value: u8 = if (matched_types == 0)
                @intFromEnum(self.defaultTypeAt(address))
            else if ((matched_types & 1) != 0)
                @intFromEnum(MemoryType.uc)
            else if (@popCount(matched_types) == 1)
                @ctz(matched_types)
            else if (matched_types == ((1 << @intFromEnum(MemoryType.wt)) |
                (1 << @intFromEnum(MemoryType.wb))))
                @intFromEnum(MemoryType.wt)
            else
                return null;
            const kind: MemoryType = @enumFromInt(type_value);
            if (first) |previous| {
                if (previous != kind) return null;
            } else first = kind;
        }
        return first;
    }

    pub fn rangeIsUncached(self: *const CacheSnapshot, start: u64, bytes: u64) bool {
        return self.rangeIsUncachedForPat(start, bytes, false);
    }

    /// PAT UC_MINUS combined with MTRR UC or WB is effectively UC (Intel
    /// SDM Vol. 3A, effective-memory-type table). WC is deliberately refused.
    /// Callers must separately prove compatibility with any WB-encoded alias.
    pub fn rangeIsUcMinusUncached(self: *const CacheSnapshot, start: u64, bytes: u64) bool {
        return self.rangeIsUncachedForPat(start, bytes, true);
    }

    fn rangeIsUncachedForPat(self: *const CacheSnapshot, start: u64, bytes: u64, allow_wb: bool) bool {
        if (!self.rangeSupportsUncachedMapping(start, bytes)) return false;
        var address = start;
        while (address < start + bytes) : (address += 4096) {
            if (!self.pageIsUncached(address, allow_wb)) return false;
        }
        return true;
    }

    fn pageIsUncached(self: *const CacheSnapshot, address: u64, allow_wb: bool) bool {
        const physical_mask = self.physicalMask();
        var matched = false;
        var all_wb = true;
        for (self.ranges[0 .. self.capability & 0xff]) |range| {
            if (range.mask & mtrr_valid == 0) continue;
            const mask = range.mask & physical_mask;
            if ((address & mask) != (range.base & mask)) continue;
            matched = true;
            // UC dominates overlaps. Without UC, admit only uniformly WB
            // ranges for UC_MINUS; mixed/other types remain fail-closed.
            if (@as(u8, @truncate(range.base)) == memory_type_uc) return true;
            if (@as(u8, @truncate(range.base)) != 6) all_wb = false;
        }
        if (matched) return allow_wb and all_wb;
        const default: u8 = @intFromEnum(self.defaultTypeAt(address));
        return default == memory_type_uc or (allow_wb and default == 6);
    }
};

fn validType(value: u8, capability: u64) bool {
    return switch (value) {
        0, 4, 5, 6 => true,
        1 => capability & (1 << 10) != 0,
        else => false,
    };
}

test "MMIO WC snapshot accepts only actual PAT WC entries and uniform MTRRs" {
    const address: u64 = 0xd0000000;
    const physical_mask: u64 = ((@as(u64, 1) << 46) - 1) & ~@as(u64, 4095);
    var proof = CacheSnapshot{
        .pat = 6 | (@as(u64, 1) << 8) | (@as(u64, 7) << 16),
        .capability = 2 | (1 << 10),
        .default_type = mtrr_enabled,
        .physical_bits = 46,
    };
    try std.testing.expect(proof.valid());
    try std.testing.expect(proof.patSlotIsWc(1));
    try std.testing.expect(!proof.patSlotIsWc(0));
    try std.testing.expect(!proof.patSlotIsWc(4));
    try std.testing.expectEqual(@as(?MemoryType, .uc), proof.uniformMtrrType(address, 8192));

    proof.ranges[0] = .{
        .base = address | @intFromEnum(MemoryType.wc),
        .mask = (physical_mask & ~@as(u64, 8191)) | mtrr_valid,
    };
    try std.testing.expectEqual(@as(?MemoryType, .wc), proof.uniformMtrrType(address, 8192));
    try std.testing.expect(proof.uniformMtrrType(address, 12288) == null);
    proof.ranges[1] = proof.ranges[0];
    try std.testing.expectEqual(@as(?MemoryType, .wc), proof.uniformMtrrType(address, 4096));
    proof.ranges[1].base = address | @intFromEnum(MemoryType.uc);
    try std.testing.expectEqual(@as(?MemoryType, .uc), proof.uniformMtrrType(address, 4096));
    proof.ranges[1].base = address | @intFromEnum(MemoryType.wb);
    try std.testing.expect(proof.uniformMtrrType(address, 4096) == null);
    proof.ranges[1] = .{};
    try std.testing.expect(proof.uniformMtrrType(address + 1, 4096) == null);

    proof.pat &= ~(@as(u64, 0xff) << 8);
    try std.testing.expect(!proof.patSlotIsWc(1));
    proof.pat |= @as(u64, 1) << 32;
    try std.testing.expect(proof.patSlotIsWc(4));
    proof.capability &= ~@as(u64, 1 << 10);
    try std.testing.expect(!proof.valid()); // A WC MTRR needs the CPU's WC capability.
}

test "MMIO WC snapshot AMD TOM2 WB default applies only to unmatched pages inside its bounds" {
    const pa: u64 = 0x1_4000_0000;
    const mask: u64 = (((@as(u64, 1) << 46) - 1) & ~@as(u64, 4095)) |
        mtrr_valid;
    var snapshot = CacheSnapshot{
        .pat = 6 | (@as(u64, 7) << 16),
        .capability = 1,
        .default_type = mtrr_enabled, // Firmware's ordinary default is UC.
        .physical_bits = 46,
        .amd_pat_mtrr_semantics = true,
        .amd_syscfg = amd_tom2_enable | amd_tom2_force_wb,
        .amd_top_mem2 = 0x8_0000_0000,
    };
    try std.testing.expect(snapshot.valid());
    try std.testing.expectEqual(@as(?MemoryType, .uc), snapshot.uniformMtrrType(0xffff_f000, 4096));
    try std.testing.expectEqual(@as(?MemoryType, .wb), snapshot.uniformMtrrType(0x1_0000_0000, 4096));
    try std.testing.expectEqual(@as(?MemoryType, .wb), snapshot.uniformMtrrType(0x7_ffff_f000, 4096));
    try std.testing.expectEqual(@as(?MemoryType, .uc), snapshot.uniformMtrrType(0x8_0000_0000, 4096));
    try std.testing.expect(!snapshot.rangeIsUncached(pa, 4096));
    try std.testing.expect(snapshot.rangeIsUcMinusUncached(pa, 4096));

    // The TOM2 default cannot defeat a variable UC override.
    snapshot.ranges[0] = .{ .base = pa | @intFromEnum(MemoryType.uc), .mask = (mask & ~@as(u64, 0x1f_ffff)) | mtrr_valid };
    try std.testing.expectEqual(@as(?MemoryType, .uc), snapshot.uniformMtrrType(pa, 4096));
    try std.testing.expect(snapshot.rangeIsUncached(pa, 4096));
    snapshot.ranges[0] = .{};

    var other = snapshot;
    other.amd_syscfg |= @as(u64, 1) << 20; // Unrelated SYSCFG bit.
    try std.testing.expect(snapshot.matches(&other));
    other.amd_top_mem2 -= 0x80_0000;
    try std.testing.expect(!snapshot.matches(&other));
    other = snapshot;
    other.amd_syscfg &= ~amd_tom2_force_wb;
    other.amd_top_mem2 = 0;
    try std.testing.expect(other.valid());
    try std.testing.expect(!snapshot.matches(&other));
    try std.testing.expectEqual(@as(?MemoryType, .uc), other.uniformMtrrType(pa, 4096));

    other = snapshot;
    other.default_type &= ~mtrr_enabled;
    try std.testing.expect(!other.valid());
    other = snapshot;
    other.amd_pat_mtrr_semantics = false;
    try std.testing.expect(!other.valid());
    other = snapshot;
    other.amd_top_mem2 = 0x1_0000_0000;
    try std.testing.expect(!other.valid());
    other = snapshot;
    other.amd_top_mem2 = @as(u64, 1) << 46;
    try std.testing.expect(snapshot.matches(&snapshot));
    try std.testing.expect(other.valid());
    other.amd_top_mem2 = (@as(u64, 1) << 46) + 0x80_0000;
    try std.testing.expect(!other.valid());
}

pub fn captureCacheSnapshot(out: *CacheSnapshot) bool {
    out.* = .{};
    const vendor = cpuid(0);
    out.amd_pat_mtrr_semantics = vendor.ebx == 0x6874_7541 and
        vendor.edx == 0x6974_6e65 and vendor.ecx == 0x444d_4163; // AuthenticAMD
    const features = cpuid(1).edx;
    if (features & (1 << 12) == 0 or features & (1 << 16) == 0) return false;
    out.physical_bits = reportedPhysicalAddressBits();
    out.pat = rdmsr(0x277);
    out.capability = rdmsr(mtrr_cap_msr);
    out.default_type = rdmsr(mtrr_def_type_msr);
    // SYSCFG/TOP_MEM2 are architectural on physical AMD64 CPUs. Hypervisors
    // may advertise AMD while withholding these optional MSRs; in that case
    // retain the ordinary MTRR proof and never infer the TOM2 WB exception.
    const leaf1 = cpuid(1);
    const family: u32 = ((leaf1.eax >> 8) & 0xf) +
        (if (((leaf1.eax >> 8) & 0xf) == 0xf) (leaf1.eax >> 20) & 0xff else 0);
    if (out.amd_pat_mtrr_semantics and family >= 0xf and
        leaf1.ecx & (@as(u32, 1) << 31) == 0)
    {
        out.amd_syscfg = rdmsr(amd_syscfg_msr);
        if (out.amd_syscfg & (amd_tom2_enable | amd_tom2_force_wb) ==
            (amd_tom2_enable | amd_tom2_force_wb))
        {
            out.amd_top_mem2 = rdmsr(amd_top_mem2_msr) & amd_tom2_mask;
        }
    }
    for (out.ranges[0 .. out.capability & 0xff], 0..) |*range, index| {
        range.base = rdmsr(mtrr_physbase0_msr + @as(u32, @intCast(index * 2)));
        range.mask = rdmsr(mtrr_physbase0_msr + @as(u32, @intCast(index * 2 + 1)));
    }
    return out.valid();
}

fn patWithWcSlot4(value: u64) u64 {
    return (value & ~(@as(u64, 0xff) << 32)) |
        (@as(u64, @intFromEnum(MemoryType.wc)) << 32);
}

/// Our own page tables use PAT slots 0/2/3 before user MMIO exists. Slot 4
/// is unreferenced there, so install WC on each CPU before it joins the
/// online set. No live translation changes type; firmware/bootloader tables
/// are no longer active. Linux similarly initializes PAT for ioremap_wc.
pub fn ensureWcPatSlot4() bool {
    if ((cpuid(1).edx & (1 << 16)) == 0) return false;
    const current = rdmsr(0x277);
    if ((current & 0xff) != 6 or ((current >> 16) & 0xff) != 7 or
        ((current >> 24) & 0xff) != 0) return false;
    const desired = patWithWcSlot4(current);
    if (current != desired) {
        const low: u32 = @truncate(desired);
        const high: u32 = @truncate(desired >> 32);
        asm volatile ("wrmsr"
            :
            : [msr] "{ecx}" (@as(u32, 0x277)),
              [lo] "{eax}" (low),
              [hi] "{edx}" (high),
            : .{ .memory = true });
    }
    return rdmsr(0x277) == desired;
}

test "WC PAT setup changes only unused slot four" {
    const old: u64 = 0x0007_0406_0007_0406;
    const updated = patWithWcSlot4(old);
    try std.testing.expectEqual(@as(u64, 1), (updated >> 32) & 0xff);
    try std.testing.expectEqual(old & ~(@as(u64, 0xff) << 32), updated & ~(@as(u64, 0xff) << 32));
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

fn reportedPhysicalAddressBits() u8 {
    if (cpuid(0x8000_0000).eax < 0x8000_0008) return 36;
    return @truncate(cpuid(0x8000_0008).eax);
}

fn physicalAddressBits() u8 {
    return @min(reportedPhysicalAddressBits(), 52);
}

/// The kernel's low identity map covers this register page. Its paging entry
/// uses the normal WB encoding, so firmware MTRRs must make the MMIO address
/// UC before it is safe to dereference as a register block.
pub fn isUncached(paddr: u64) bool {
    var snapshot: CacheSnapshot = .{};
    return captureCacheSnapshot(&snapshot) and snapshot.rangeIsUncached(paddr, 4096);
}
