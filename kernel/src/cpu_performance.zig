//! Boot-only, local Intel HWP initialization; no user ABI or runtime governor.
const std = @import("std");
pub const pm_enable: u32 = 0x770;
pub const capabilities: u32 = 0x771;
pub const request: u32 = 0x774;
const misc_enable: u32 = 0x1a0;
pub const Registers = struct { eax: u32 = 0, ebx: u32 = 0, ecx: u32 = 0, edx: u32 = 0 };
pub const Features = struct {
    intel: bool = false,
    max_basic: u32 = 0,
    signature: u32 = 0,
    msr: bool = false,
    hypervisor: bool = false,
    thermal: u32 = 0,
};
pub const Reason = enum { none, non_intel, leaf1_missing, hypervisor, msr_missing, leaf6_missing, hwp_missing };
pub const Status = enum { not_run, skipped, enabled, enable_mismatch, invalid_capabilities, request_mismatch };
pub const Result = struct {
    status: Status = .not_run,
    reason: Reason = .none,
    signature: u32 = 0,
    highest: u8 = 0,
    guaranteed: u8 = 0,
    efficient: u8 = 0,
    lowest: u8 = 0,
    epp_supported: bool = false,
    turbo_disabled: ?bool = null,
    pm_before: u64 = 0,
    pm_after: u64 = 0,
    expected: u64 = 0,
    actual: u64 = 0,
    mask: u64 = 0,
    failed_msr: u32 = 0,
    pub fn failed(self: Result) bool {
        return switch (self.status) { .enable_mismatch, .invalid_capabilities, .request_mismatch => true, else => false };
    }
};
pub fn exclusion(f: Features) Reason {
    if (!f.intel) return .non_intel;
    if (f.max_basic < 1) return .leaf1_missing;
    if (f.hypervisor) return .hypervisor;
    if (!f.msr) return .msr_missing;
    if (f.max_basic < 6) return .leaf6_missing;
    if (f.thermal & (1 << 7) == 0) return .hwp_missing;
    return .none;
}
// Energy-performance preference written to IA32_HWP_REQUEST[31:24] when the
// CPU advertises EPP (CPUID.06H:EAX[10]). 0x00 is the strongest performance
// hint. Why not the balanced 0x80: PachaOS services (LPR -> gpud -> driver
// sandbox) hand work across cores through short wake-run-block hops of ~20 us.
// Measured on i7-1195G7 with 0x80, a core woken after ~2 ms idle stayed near
// 1.2 GHz for the whole >130 us observation window, so every IPC hop paid
// +70..90 us (mc-16 wakework/drmlat). The hint lets HWP ramp on short bursts.
// Cost: higher package power/heat on battery; idle still uses HLT and the
// firmware power/thermal limits are untouched. This is a hint, not a frequency
// floor: min stays at lowest, so it does not pin the clock.
pub const epp_performance: u8 = 0x00;
pub fn requestValue(lowest: u8, highest: u8, epp: bool) ?u64 {
    if (lowest > highest) return null;
    // Intel SDM Vol.4 335592-085US Table 2-2 (774H), and Vol.3B
    // HWP Managing HWP: autonomous desired=0; optional EPP (see
    // epp_performance). Construct from zero, never carry firmware package/window
    // or unsupported/reserved bits into this CPU's independent request. With
    // epp_performance=0 the supported and unsupported values coincide, but the
    // EPP field stays in requestMask() so a stale firmware/previous EPP readback
    // is still detected as request_mismatch.
    return @as(u64, lowest) | (@as(u64, highest) << 8) | (if (epp) @as(u64, epp_performance) << 24 else 0);
}
pub fn requestMask(thermal: u32) u64 {
    return 0xffffff | (if (thermal & (1 << 10) != 0) @as(u64, 0xff) << 24 else 0) |
        (if (thermal & (1 << 9) != 0) @as(u64, 0x3ff) << 32 else 0) |
        (if (thermal & (1 << 11) != 0) @as(u64, 1) << 42 else 0);
}
pub fn turboMsrKnown(signature: u32) bool {
    const family = (signature >> 8) & 15;
    const model = ((signature >> 4) & 15) | ((signature >> 12) & 0xf0);
    // HWP discovery alone does NOT enumerate IA32_MISC_ENABLE[38]. Only the
    // SDM Vol.4 335592-085US p2-354 includes 06_8C/8D in Table2-39
    // and inherits Table2-20, p2-225..226 (1A0H bit38). Only these
    // reviewed Tiger Lake models are admitted; other HWP CPUs report
    // unknown rather than speculatively reading a model-specific register.
    return family == 6 and (model == 0x8c or model == 0x8d);
}
pub fn initialize(backend: anytype) Result {
    const zero = backend.cpuid(0);
    var f: Features = .{ .max_basic = zero.eax, .intel = zero.ebx == 0x756e6547 and zero.edx == 0x49656e69 and zero.ecx == 0x6c65746e };
    if (f.max_basic >= 1) {
        const one = backend.cpuid(1);
        f.signature = one.eax;
        f.msr = one.edx & (1 << 5) != 0;
        f.hypervisor = one.ecx & (1 << 31) != 0;
    }
    if (f.max_basic >= 6) f.thermal = backend.cpuid(6).eax;
    var result: Result = .{ .signature = f.signature, .reason = exclusion(f) };
    if (result.reason != .none) { result.status = .skipped; return result; }
    // RDMSR/WRMSR belong here because CPL0 and execution on the owning CPU
    // are required. CPUID excludes unsupported accesses; it is not #GP
    // recovery. Unexpected faults use the already installed exception path.
    result.pm_before = backend.readMsr(pm_enable);
    if (result.pm_before & 1 == 0) backend.writeMsr(pm_enable, result.pm_before | 1);
    result.pm_after = backend.readMsr(pm_enable);
    if (result.pm_after & 1 == 0) {
        result.status = .enable_mismatch;
        result.failed_msr = pm_enable;
        result.expected = result.pm_before | 1;
        result.actual = result.pm_after;
        result.mask = 1;
        return result;
    }
    // HWP CAPABILITIES/REQUEST are accessed only AFTER enable readback. HWP
    // enable is one-way until RESET. Failure below leaves it enabled; never
    // attempt an illegal disable rollback or continue with unexplained state.
    const cap = backend.readMsr(capabilities);
    result.highest = @truncate(cap);
    result.guaranteed = @truncate(cap >> 8);
    result.efficient = @truncate(cap >> 16);
    result.lowest = @truncate(cap >> 24);
    result.epp_supported = f.thermal & (1 << 10) != 0;
    if (turboMsrKnown(f.signature)) result.turbo_disabled = backend.readMsr(misc_enable) & (@as(u64, 1) << 38) != 0;
    result.expected = requestValue(result.lowest, result.highest, result.epp_supported) orelse {
        result.status = .invalid_capabilities;
        result.failed_msr = capabilities;
        result.actual = cap;
        return result;
    };
    result.mask = requestMask(f.thermal);
    backend.writeMsr(request, result.expected);
    result.actual = backend.readMsr(request);
    if ((result.actual & result.mask) != (result.expected & result.mask)) {
        result.status = .request_mismatch;
        result.failed_msr = request;
        return result;
    }
    result.status = .enabled;
    return result;
}
const Hardware = struct {
    fn cpuid(_: *@This(), leaf: u32) Registers {
        var a: u32 = 0; var b: u32 = 0; var c: u32 = 0; var d: u32 = 0;
        asm volatile ("cpuid" : [a] "={eax}" (a), [b] "={ebx}" (b), [c] "={ecx}" (c), [d] "={edx}" (d) : [leaf] "{eax}" (leaf), [sub] "{ecx}" (@as(u32, 0)));
        return .{ .eax = a, .ebx = b, .ecx = c, .edx = d };
    }
    fn readMsr(_: *@This(), msr: u32) u64 {
        var lo: u32 = 0; var hi: u32 = 0;
        asm volatile ("rdmsr" : [lo] "={eax}" (lo), [hi] "={edx}" (hi) : [msr] "{ecx}" (msr));
        return (@as(u64, hi) << 32) | lo;
    }
    fn writeMsr(_: *@This(), msr: u32, value: u64) void {
        asm volatile ("wrmsr" :: [msr] "{ecx}" (msr), [lo] "{eax}" (@as(u32, @truncate(value))), [hi] "{edx}" (@as(u32, @truncate(value >> 32))));
    }
};
pub fn initializeCurrentCpu() Result { var hw: Hardware = .{}; return initialize(&hw); }
pub fn formatResult(buf: []u8, slot: usize, r: Result) ![]const u8 {
    return std.fmt.bufPrint(buf, "cpu-perf: hwp cpu-slot={} status={s} reason={s} sig=0x{x} highest={} guaranteed={} efficient={} lowest={} epp={s} turbo_disabled={s} pm-before=0x{x} pm=0x{x} msr=0x{x} expected=0x{x} actual=0x{x} mask=0x{x}\n", .{
        slot, @tagName(r.status), @tagName(r.reason), r.signature, r.highest, r.guaranteed, r.efficient, r.lowest,
        if (r.status == .skipped or r.status == .not_run or r.status == .enable_mismatch) "unread" else if (!r.epp_supported) "unsupported" else if (r.status == .enabled) std.fmt.comptimePrint("0x{x}", .{epp_performance}) else "unconfirmed",
        if (r.turbo_disabled) |disabled| (if (disabled) "1" else "0") else "unknown",
        r.pm_before, r.pm_after, r.failed_msr, r.expected, r.actual, r.mask,
    });
}

const Mock = struct {
    f: Features = .{ .intel = true, .max_basic = 6, .signature = 0x806c1, .msr = true, .thermal = (1 << 7) | (1 << 9) | (1 << 10) | (1 << 11) },
    pm: u64 = 0x40,
    cap: u64 = 0x10305080,
    req: u64 = 0,
    enable_stuck: bool = false,
    request_stuck: bool = false,
    reads: usize = 0,
    writes: usize = 0,
    enables: usize = 0,
    requests: usize = 0,
    misc_reads: usize = 0,
    readback_or: u64 = 0,
    fn cpuid(self: *@This(), leaf: u32) Registers {
        std.debug.assert(leaf <= self.f.max_basic);
        return switch (leaf) {
            0 => .{ .eax = self.f.max_basic, .ebx = if (self.f.intel) 0x756e6547 else 0, .edx = 0x49656e69, .ecx = 0x6c65746e },
            1 => .{ .eax = self.f.signature, .edx = if (self.f.msr) 1 << 5 else 0, .ecx = if (self.f.hypervisor) 1 << 31 else 0 },
            6 => .{ .eax = self.f.thermal },
            else => unreachable,
        };
    }
    fn readMsr(self: *@This(), msr: u32) u64 {
        self.reads += 1;
        if (msr != pm_enable) std.debug.assert(self.pm & 1 != 0);
        return switch (msr) { pm_enable => self.pm, capabilities => self.cap, request => self.req | self.readback_or, misc_enable => blk: { self.misc_reads += 1; break :blk @as(u64, 1) << 38; }, else => unreachable };
    }
    fn writeMsr(self: *@This(), msr: u32, value: u64) void {
        self.writes += 1;
        switch (msr) {
            pm_enable => { self.enables += 1; std.debug.assert(value == self.pm | 1); if (!self.enable_stuck) self.pm = value; },
            request => { self.requests += 1; std.debug.assert(self.pm & 1 != 0); if (!self.request_stuck) self.req = value; },
            else => unreachable, // No turbo/power/thermal/interrupt writes.
        }
    }
};
test "HWP excluded CPUs perform zero MSR accesses" {
    for (0..6) |scenario| {
        var m: Mock = .{};
        const expected: Reason = switch (scenario) {
            0 => blk: { m.f.intel = false; break :blk .non_intel; },
            1 => blk: { m.f.max_basic = 0; break :blk .leaf1_missing; },
            2 => blk: { m.f.hypervisor = true; break :blk .hypervisor; },
            3 => blk: { m.f.msr = false; break :blk .msr_missing; },
            4 => blk: { m.f.max_basic = 5; break :blk .leaf6_missing; },
            5 => blk: { m.f.thermal = 0; break :blk .hwp_missing; },
            else => unreachable,
        };
        const r = initialize(&m);
        try std.testing.expectEqual(.skipped, r.status);
        try std.testing.expectEqual(expected, r.reason);
        try std.testing.expectEqual(@as(usize, 0), m.reads + m.writes);
        try std.testing.expect(!r.failed());
    }
}
test "HWP autonomous request preserves enable and writes only supported fields" {
    for ([_]bool{ false, true }) |epp| for ([_]bool{ false, true }) |already| {
        var m: Mock = .{};
        if (!epp) m.f.thermal &= ~@as(u32, 1 << 10);
        if (already) m.pm |= 1;
        const r = initialize(&m);
        try std.testing.expectEqual(.enabled, r.status);
        try std.testing.expectEqual(@as(u64, 0x41), r.pm_after);
        try std.testing.expectEqual(@as(usize, if (already) 0 else 1), m.enables);
        try std.testing.expectEqual(@as(usize, 1), m.requests);
        try std.testing.expectEqual(@as(u64, 0x8010), m.req);
        // The EPP field is verified only where supported, even though the
        // written value is zero in both cases.
        try std.testing.expectEqual(epp, r.mask & (@as(u64, 0xff) << 24) != 0);
        try std.testing.expectEqual(@as(u64, 0), m.req & ~@as(u64, 0xff00ffff));
        try std.testing.expectEqual(@as(?bool, true), r.turbo_disabled);
    };
    var other: Mock = .{}; other.f.signature = 0x906e1;
    try std.testing.expectEqual(@as(?bool, null), initialize(&other).turbo_disabled);
    try std.testing.expectEqual(@as(usize, 0), other.misc_reads);
    try std.testing.expect(turboMsrKnown(0x806d1));
    try std.testing.expect(!turboMsrKnown(0xf08c1));
}
test "HWP failures never publish success or disable an enabled CPU" {
    for (0..3) |scenario| {
        var m: Mock = .{};
        if (scenario == 0) m.enable_stuck = true;
        if (scenario == 1) m.cap = 0x90305080;
        if (scenario == 2) m.request_stuck = true;
        const r = initialize(&m);
        try std.testing.expect(r.failed());
        try std.testing.expectEqual(@as(usize, if (scenario == 2) 1 else 0), m.requests);
        try std.testing.expectEqual(scenario != 0, m.pm & 1 != 0);
        try std.testing.expectEqual(([_]Status{ .enable_mismatch, .invalid_capabilities, .request_mismatch })[scenario], r.status);
    }
    try std.testing.expectEqual(@as(?u64, 0xff00), requestValue(0, 255, false));
    try std.testing.expectEqual(@as(?u64, null), requestValue(2, 1, true));
    try std.testing.expectEqual(@as(u64, 0xffffff), requestMask(0));
    var buf: [512]u8 = undefined;
    var m: Mock = .{};
    const line = try formatResult(&buf, std.math.maxInt(u32), initialize(&m));
    try std.testing.expect(std.mem.indexOf(u8, line, "epp=0x0 ") != null);
    var unsupported: Mock = .{};
    unsupported.f.thermal &= ~@as(u32, 1 << 10);
    const uline = try formatResult(&buf, 0, initialize(&unsupported));
    try std.testing.expect(std.mem.indexOf(u8, uline, "epp=unsupported") != null);
    try std.testing.expect(std.mem.endsWith(u8, line, "\n"));
}

test "HWP verifies supported fields without trusting reserved readback bits" {
    var m: Mock = .{};
    m.readback_or = @as(u64, 1) << 63;
    try std.testing.expectEqual(.enabled, initialize(&m).status);
    for ([_]u64{ @as(u64, 1) << 32, @as(u64, 1) << 42 }) |bit| {
        var bad: Mock = .{ .readback_or = bit };
        try std.testing.expectEqual(.request_mismatch, initialize(&bad).status);
    }
    var diagnostic_only: Mock = .{ .cap = 0x10ffff80 };
    try std.testing.expectEqual(.enabled, initialize(&diagnostic_only).status);
}

test "HWP detects a stale balanced EPP readback when EPP is supported" {
    // Firmware or a previous kernel may leave EPP=0x80; a write that does not
    // stick must not be reported as enabled.
    var m: Mock = .{ .readback_or = @as(u64, 0x80) << 24 };
    const r = initialize(&m);
    try std.testing.expectEqual(.request_mismatch, r.status);
    try std.testing.expect(r.failed());
    var buf: [512]u8 = undefined;
    const line = try formatResult(&buf, 0, r);
    try std.testing.expect(std.mem.indexOf(u8, line, "epp=unconfirmed") != null);
    // Without EPP support the field is outside the verified mask.
    var u: Mock = .{ .readback_or = @as(u64, 0x80) << 24 };
    u.f.thermal &= ~@as(u32, 1 << 10);
    try std.testing.expectEqual(.enabled, initialize(&u).status);
}
