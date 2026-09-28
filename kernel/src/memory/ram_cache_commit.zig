const std = @import("std");
const policy = @import("ram_cache_policy.zig");
const paging = @import("ram_cache_paging.zig");
const vm = @import("user_vm.zig");
const copy = @import("../user_copy.zig");
const Mode = @import("ram_cache_transition.zig").Mode;

fn validateSpans(spans: []const policy.Span, pending: bool) !void {
    var last: u64 = 0;
    for (spans) |span| {
        if (span.owner == 0 or span.base == 0 or span.bytes == 0 or
            (span.base | span.bytes) & 4095 != 0 or span.base < last or
            (!pending and span.mode == .wb)) return error.Invalid;
        last = try std.math.add(u64, span.base, span.bytes);
    }
}

fn sameType(a: ?policy.Span, b: ?policy.Span) bool {
    if (a) |left| {
        const right = b orelse return false;
        return left.owner == right.owner and left.mode == right.mode and left.pat_slot == right.pat_slot;
    }
    return b == null;
}

pub const Change = struct {
    pages: []const u64,
    owner: u64,
    mode: Mode,
    pat: u3,
    old: *const policy.Snapshot,
    pending: *const policy.Snapshot,
    final: *const policy.Snapshot,

    fn validatePoint(self: Change, pa: u64) !void {
        const before = policy.lookup(self.old.policy, pa);
        const after = policy.lookup(self.final.policy, pa);
        if (!paging.pagesIntersect(self.pages, pa & ~@as(u64, 4095), 4096)) {
            if (!sameType(before, after)) return error.UnrelatedPolicyChange;
            return;
        }
        if (before) |span| if (span.owner != self.owner) return error.OwnerConflict;
        if (self.mode == .wb) {
            if (after != null) return error.Invalid;
        } else {
            const span = after orelse return error.Invalid;
            if (span.owner != self.owner or span.mode != self.mode or span.pat_slot != self.pat) return error.Invalid;
        }
    }

    pub fn validate(self: Change) !void {
        try paging.validatePages(self.pages, @as(u64, 1) << 52);
        if (self.owner == 0 or self.old.pending.len != 0 or self.final.pending.len != 0 or
            (self.mode == .wb and self.pat != 0)) return error.Invalid;
        try validateSpans(self.old.policy, false);
        try validateSpans(self.final.policy, false);
        try validateSpans(self.pending.pending, true);
        // Pending is the unchanged old policy plus exactly the withdrawn
        // PFNs, never a premature new policy or a block on unrelated RAM.
        if (!std.meta.eql(self.old.policy, self.pending.policy)) {
            if (self.old.policy.len != self.pending.policy.len) return error.Invalid;
            for (self.old.policy, self.pending.policy) |a, b|
                if (!std.meta.eql(a, b)) return error.Invalid;
        }
        var index: usize = 0;
        for (self.pending.pending) |span| {
            if (span.owner != self.owner) return error.OwnerConflict;
            var pa = span.base;
            while (pa < span.base + span.bytes) : (pa += 4096) {
                if (index == self.pages.len or self.pages[index] != pa) return error.Invalid;
                index += 1;
            }
        }
        if (index != self.pages.len) return error.Invalid;
        // A piecewise-constant policy can differ only at a span or target
        // boundary. Check both tables' boundaries as well as each target.
        for ([_][]const policy.Span{ self.old.policy, self.final.policy }) |spans| {
            for (spans) |span| {
                try self.validatePoint(span.base);
                try self.validatePoint(span.base + span.bytes);
            }
        }
        for (self.pages) |pa| {
            try self.validatePoint(pa);
            try self.validatePoint(pa + 4096);
        }
    }
};

const NativeArch = struct {
    kernel_cr3: u64,
    fn publish(_: *@This(), snapshot: *const policy.Snapshot) void {
        copy.publishRamCacheSnapshot(snapshot);
    }
    fn retire(self: *@This(), base: u64, bytes: usize) void {
        copy.retirePhysCopyWindowsForRange(self.kernel_cr3, base, bytes);
    }
    fn users(_: *@This()) void {
        vm.flushAllUserIdentityContexts(0, 4096);
    }
    fn kernel(self: *@This(), drain: bool) void {
        if (drain) copy.flushTlbAndCachesForCr3Range(self.kernel_cr3, 0, 4096) else copy.flushTlbForCr3Range(self.kernel_cr3, 0, 4096);
    }
};

/// Internal bridge, not an authority-granting API. Caller holds state/shared
/// VM/all-AS locks and has proved ownership, DMA exclusion, PAT/MTRR and all
/// aliases; snapshot and plan storage remain mapped across the transition.
/// On return old/pending readers have retired. VMO lifetime state and storage
/// reclamation remain the caller's responsibility.
pub fn commit(plan: *@import("ram_cache_vm_plan.zig").Plan, change: Change, kernel_cr3: u64) !void {
    if (kernel_cr3 == 0 or kernel_cr3 & 4095 != 0) return error.Invalid;
    var arch = NativeArch{ .kernel_cr3 = kernel_cr3 };
    try commitWithArch(plan, change, &arch);
}

fn commitWithArch(plan: anytype, change: Change, arch: anytype) !void {
    const trace = change.pages.len >= 128 and @TypeOf(arch.*) == NativeArch;
    try change.validate();
    if (trace) @import("../kernel_log.zig").write("ram-cache: commit validated\n");
    try plan.validatePolicy(change.old);
    // Check current leaf identity, allowing only asynchronous hardware A/D.
    for (plan.leaves) |leaf| {
        if ((@atomicLoad(u64, leaf.entry, .acquire) ^ leaf.previous) & ~@as(u64, 0x60) != 0)
            return error.StaleAlias;
    }
    arch.publish(change.pending);
    plan.withdraw();
    if (trace) @import("../kernel_log.zig").write("ram-cache: aliases withdrawn\n");
    // The rendezvous clears only each CPU's cached physical-copy PTE. A
    // sparse bounding interval can evict an unrelated cached window in a
    // hole, but its next guarded use remaps it; no policy/PAT is changed for
    // those holes. One acknowledged IPI replaces up to one per scattered PFN.
    arch.retire(change.pages[0], @intCast(change.pages[change.pages.len - 1] + 4096 - change.pages[0]));
    arch.users();
    if (trace) @import("../kernel_log.zig").write("ram-cache: first user shootdown complete\n");
    arch.kernel(true);
    if (trace) @import("../kernel_log.zig").write("ram-cache: cache drain complete\n");
    plan.publish(change.pat);
    arch.users();
    if (trace) @import("../kernel_log.zig").write("ram-cache: second user shootdown complete\n");
    arch.kernel(false);
    arch.publish(change.final);
    // Includes CPU-local IRQ-disabled copy readers of the replaced pending
    // snapshot. Storage cannot be recycled at the preceding publication.
    arch.kernel(false);
    if (trace) @import("../kernel_log.zig").write("ram-cache: final reader barrier complete\n");
}

test "MMIO overlay native RAM commit orders copy retirement drain and final readers" {
    const Trace = struct {
        bytes: [32]u8 = undefined,
        len: usize = 0,
        fn add(self: *@This(), c: u8) void {
            self.bytes[self.len] = c;
            self.len += 1;
        }
    };
    const FakePlan = struct {
        trace: *Trace,
        leaves: []paging.Leaf = &.{},
        fn validatePolicy(_: *@This(), _: *const policy.Snapshot) !void {}
        fn withdraw(self: *@This()) void {
            self.trace.add('w');
        }
        fn publish(self: *@This(), _: u3) void {
            self.trace.add('p');
        }
    };
    const Arch = struct {
        trace: *Trace,
        publications: usize = 0,
        ranges: [2]struct { base: u64, bytes: usize } = undefined,
        count: usize = 0,
        fn publish(self: *@This(), _: *const policy.Snapshot) void {
            self.trace.add(if (self.publications == 0) 'b' else 'f');
            self.publications += 1;
        }
        fn retire(self: *@This(), base: u64, bytes: usize) void {
            self.ranges[self.count] = .{ .base = base, .bytes = bytes };
            self.count += 1;
            self.trace.add('r');
        }
        fn users(self: *@This()) void {
            self.trace.add('u');
        }
        fn kernel(self: *@This(), drain: bool) void {
            self.trace.add(if (drain) 'd' else 'k');
        }
    };
    const spans = [_]policy.Span{
        .{ .base = 0x1000, .bytes = 8192, .owner = 7, .mode = .wc, .pat_slot = 4 },
        .{ .base = 0x5000, .bytes = 4096, .owner = 7, .mode = .wc, .pat_slot = 4 },
    };
    const old: policy.Snapshot = .{};
    const pending: policy.Snapshot = .{ .pending = &spans };
    const final: policy.Snapshot = .{ .policy = &spans };
    const change = Change{ .pages = &.{ 0x1000, 0x2000, 0x5000 }, .owner = 7, .mode = .wc, .pat = 4, .old = &old, .pending = &pending, .final = &final };
    var trace = Trace{};
    var plan = FakePlan{ .trace = &trace };
    var arch = Arch{ .trace = &trace };
    try commitWithArch(&plan, change, &arch);
    try std.testing.expectEqualStrings("bwrudpukfk", trace.bytes[0..trace.len]);
    try std.testing.expectEqual(@as(usize, 1), arch.count);
    try std.testing.expectEqual(@as(u64, 0x1000), arch.ranges[0].base);
    try std.testing.expectEqual(@as(usize, 0x5000), arch.ranges[0].bytes);
    // Malformed publication fails before touching any architectural state.
    const extra = [_]policy.Span{.{ .base = 0x9000, .bytes = 4096, .owner = 7, .mode = .wc, .pat_slot = 4 }};
    const bad_final: policy.Snapshot = .{ .policy = &extra };
    var invalid = change;
    invalid.final = &bad_final;
    trace.len = 0;
    try std.testing.expectError(error.UnrelatedPolicyChange, commitWithArch(&plan, invalid, &arch));
    try std.testing.expectEqual(@as(usize, 0), trace.len);
    const incomplete: policy.Snapshot = .{ .pending = spans[0..1] };
    invalid = change;
    invalid.pending = &incomplete;
    try std.testing.expectError(error.Invalid, commitWithArch(&plan, invalid, &arch));
    try std.testing.expectEqual(@as(usize, 0), trace.len);
    // Restoring WB removes only these spans, using the same retirement order.
    const restore_pending: policy.Snapshot = .{ .policy = &spans, .pending = &spans };
    invalid = change;
    invalid.old = &final;
    invalid.pending = &restore_pending;
    invalid.final = &old;
    invalid.mode = .wb;
    invalid.pat = 0;
    try invalid.validate();
}
