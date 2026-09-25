const mtrr_cap_msr: u32 = 0x0000_00fe;
const mtrr_def_type_msr: u32 = 0x0000_02ff;
const mtrr_physbase0_msr: u32 = 0x0000_0200;
const mtrr_enabled: u64 = 1 << 11;
const mtrr_valid: u64 = 1 << 11;
const memory_type_uc: u8 = 0;
const std = @import("std");

/// Immutable boot-time proof for UC_MINUS BAR mappings. Fixed MTRRs are not
/// needed: admission excludes every address below 1 MiB. No MSR is written.
pub const CacheSnapshot = struct {
    pub const Range = struct { base: u64 = 0, mask: u64 = 0 };
    pat: u64 = 0,
    capability: u64 = 0,
    default_type: u64 = 0,
    physical_bits: u8 = 0,
    ranges: [256]Range = [_]Range{.{}} ** 256,

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
            self.physical_bits != other.physical_bits) return false;
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
        const default: u8 = @truncate(self.default_type);
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

pub fn captureCacheSnapshot(out: *CacheSnapshot) bool {
    out.* = .{};
    const features = cpuid(1).edx;
    if (features & (1 << 12) == 0 or features & (1 << 16) == 0) return false;
    out.physical_bits = reportedPhysicalAddressBits();
    out.pat = rdmsr(0x277);
    out.capability = rdmsr(mtrr_cap_msr);
    out.default_type = rdmsr(mtrr_def_type_msr);
    for (out.ranges[0 .. out.capability & 0xff], 0..) |*range, index| {
        range.base = rdmsr(mtrr_physbase0_msr + @as(u32, @intCast(index * 2)));
        range.mask = rdmsr(mtrr_physbase0_msr + @as(u32, @intCast(index * 2 + 1)));
    }
    return out.valid();
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
    // Fixed MTRRs below 1 MiB are outside this variable-range check.
    if (paddr < 0x100000) return false;
    if ((cpuid(1).edx & (@as(u32, 1) << 12)) == 0) return false;
    const mtrr_cap = rdmsr(mtrr_cap_msr);
    const def_type = rdmsr(mtrr_def_type_msr);
    if ((def_type & mtrr_enabled) == 0) return false;

    const address_bits = physicalAddressBits();
    const physical_mask = ((@as(u64, 1) << @intCast(address_bits)) - 1) & 0x000f_ffff_ffff_f000;
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
