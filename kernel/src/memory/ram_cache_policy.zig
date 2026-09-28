const std = @import("std");
const Mode = @import("ram_cache_transition.zig").Mode;

/// Sparse physical policy shared by parent VMOs and all exported views.
/// Absence means WB. The owner is a canonical VMO generation token, never an
/// FD or virtual address. Storage and synchronization belong to the caller.
pub const Span = struct {
    base: u64,
    bytes: u64,
    owner: u64,
    mode: Mode,
    pat_slot: u3,
};

pub const Error = error{ Invalid, Conflict, NoSpace };

/// Published snapshots are immutable. A pending physical span is not a new
/// cache mode: readers must leave their copy guard before attempting a retry.
/// Writers retain replaced snapshots until a subsequent all-CPU rendezvous.
pub const Snapshot = struct {
    policy: []const Span = &.{},
    pending: []const Span = &.{},

    pub const Decision = union(enum) { ordinary, blocked, cached: Span };

    pub fn query(self: *const Snapshot, paddr: u64) Decision {
        if (lookup(self.pending, paddr) != null) return .blocked;
        if (lookup(self.policy, paddr)) |span| return .{ .cached = span };
        return .ordinary;
    }
};

fn end(span: Span) Error!u64 {
    if (span.base == 0 or span.bytes == 0 or span.owner == 0 or
        (span.base | span.bytes) & 4095 != 0)
        return error.Invalid;
    return std.math.add(u64, span.base, span.bytes) catch error.Invalid;
}

/// The published table is sorted, disjoint, and contains only non-WB spans.
/// Callers must hold a read exclusion compatible with transition commit.
pub fn lookup(spans: []const Span, paddr: u64) ?Span {
    var low: usize = 0;
    var high = spans.len;
    while (low < high) {
        const middle = low + (high - low) / 2;
        const span = spans[middle];
        if (paddr < span.base) {
            high = middle;
        } else if (paddr - span.base >= span.bytes) {
            low = middle + 1;
        } else return span;
    }
    return null;
}

fn append(out: []Span, count: *usize, span: Span) Error!void {
    if (span.bytes == 0 or span.mode == .wb) return;
    if (count.* != 0) {
        const previous = &out[count.* - 1];
        if (previous.base + previous.bytes == span.base and
            previous.owner == span.owner and previous.mode == span.mode and
            previous.pat_slot == span.pat_slot)
        {
            previous.bytes += span.bytes;
            return;
        }
    }
    if (count.* == out.len) return error.NoSpace;
    out[count.*] = span;
    count.* += 1;
}

/// Build a replacement in separate reserved storage before withdrawing any
/// aliases. Failure can modify scratch but never the published table. The
/// caller must separately prove RAM ownership and architectural PAT/MTRR
/// validity; a matching token here alone does not confer that authority.
pub fn prepare(spans: []const Span, change: Span, scratch: []Span) Error!usize {
    const finish = try end(change);
    if (change.mode == .wb and change.pat_slot != 0) return error.Invalid;
    // Reject accidental in-place preparation, which would destroy rollback.
    if (spans.len != 0 and scratch.len != 0) {
        const a = @intFromPtr(spans.ptr);
        const b = @intFromPtr(scratch.ptr);
        if (a < b + scratch.len * @sizeOf(Span) and
            b < a + spans.len * @sizeOf(Span)) return error.Invalid;
    }
    var previous_end: u64 = 0;
    for (spans) |span| {
        const span_end = try end(span);
        if (span.mode == .wb or span.base < previous_end) return error.Invalid;
        previous_end = span_end;
        if (span.base < finish and change.base < span_end and
            span.owner != change.owner) return error.Conflict;
    }

    var count: usize = 0;
    var inserted = false;
    for (spans) |span| {
        const span_end = span.base + span.bytes;
        if (span_end <= change.base) {
            try append(scratch, &count, span);
            continue;
        }
        if (!inserted) {
            if (span.base < change.base) {
                var prefix = span;
                prefix.bytes = change.base - span.base;
                try append(scratch, &count, prefix);
            }
            try append(scratch, &count, change);
            inserted = true;
        }
        if (span.base >= finish) {
            try append(scratch, &count, span);
        } else if (span_end > finish) {
            var suffix = span;
            suffix.base = finish;
            suffix.bytes = span_end - finish;
            try append(scratch, &count, suffix);
        }
    }
    if (!inserted) try append(scratch, &count, change);
    return count;
}

pub fn storageOverlaps(a: []const Span, b: []const Span) bool {
    if (a.len == 0 or b.len == 0) return false;
    const x = @intFromPtr(a.ptr);
    const y = @intFromPtr(b.ptr);
    return if (x <= y) y - x < a.len * @sizeOf(Span) else x - y < b.len * @sizeOf(Span);
}

fn validateOrdered(spans: []const Span, changes: bool) Error!void {
    var previous: u64 = 0;
    for (spans) |span| {
        const finish = try end(span);
        if (span.base < previous or (!changes and span.mode == .wb) or
            (span.mode == .wb and span.pat_slot != 0)) return error.Invalid;
        previous = finish;
    }
}

/// Merge disjoint sorted changes in one pass. Inputs stay immutable, including
/// on late conflict/capacity failure. This avoids copying a growing policy once
/// per scattered PFN; output always lives in the one designated scratch.
pub fn prepareMany(spans: []const Span, changes: []const Span, scratch: []Span) Error!usize {
    if (storageOverlaps(spans, scratch) or storageOverlaps(changes, scratch)) return error.Invalid;
    try validateOrdered(spans, false);
    try validateOrdered(changes, true);
    var count: usize = 0;
    var index: usize = 0;
    var start: u64 = if (spans.len != 0) spans[0].base else 0;
    for (changes) |change| {
        const finish = change.base + change.bytes;
        while (index < spans.len and spans[index].base + spans[index].bytes <= change.base) {
            var old = spans[index];
            old.bytes = old.base + old.bytes - start;
            old.base = start;
            try append(scratch, &count, old);
            index += 1;
            if (index < spans.len) start = spans[index].base;
        }
        if (index < spans.len and start < change.base) {
            var prefix = spans[index];
            prefix.base = start;
            prefix.bytes = change.base - start;
            try append(scratch, &count, prefix);
            start = change.base;
        }
        try append(scratch, &count, change);
        while (index < spans.len and start < finish) {
            if (spans[index].owner != change.owner) return error.Conflict;
            const old_end = spans[index].base + spans[index].bytes;
            if (old_end > finish) {
                start = finish;
                break;
            }
            index += 1;
            if (index < spans.len) start = spans[index].base;
        }
    }
    while (index < spans.len) {
        var suffix = spans[index];
        suffix.bytes = suffix.base + suffix.bytes - start;
        suffix.base = start;
        try append(scratch, &count, suffix);
        index += 1;
        if (index < spans.len) start = spans[index].base;
    }
    return count;
}

test "scatter policy merge retains holes and WB restoration removes only targets" {
    const old = [_]Span{.{ .base = 0x1000, .bytes = 0x7000, .owner = 9, .mode = .wc, .pat_slot = 4 }};
    const changes = [_]Span{
        .{ .base = 0x2000, .bytes = 0x1000, .owner = 9, .mode = .wb, .pat_slot = 0 },
        .{ .base = 0x5000, .bytes = 0x2000, .owner = 9, .mode = .uc, .pat_slot = 3 },
        .{ .base = 0xa000, .bytes = 0x1000, .owner = 9, .mode = .wc, .pat_slot = 4 },
    };
    var output: [8]Span = undefined;
    const n = try prepareMany(&old, &changes, &output);
    try std.testing.expectEqual(@as(usize, 5), n);
    try std.testing.expect(lookup(output[0..n], 0x2000) == null);
    try std.testing.expectEqual(Mode.wc, lookup(output[0..n], 0x3000).?.mode);
    try std.testing.expectEqual(Mode.uc, lookup(output[0..n], 0x6000).?.mode);
    try std.testing.expect(lookup(output[0..n], 0x9000) == null);
    try std.testing.expectError(error.NoSpace, prepareMany(&old, &changes, output[0..1]));
    var conflict = changes;
    conflict[1].owner = 10;
    try std.testing.expectError(error.Conflict, prepareMany(&old, &conflict, &output));
    try std.testing.expectEqual(@as(u64, 0x7000), old[0].bytes);
    try std.testing.expectError(error.Invalid, prepareMany(output[0..1], &changes, &output));
}

test "batch policy merge agrees with individual replacement across scatter subsets" {
    const old = [_]Span{
        .{ .base = 0x2000, .bytes = 0x3000, .owner = 7, .mode = .wc, .pat_slot = 4 },
        .{ .base = 0x6000, .bytes = 0x1000, .owner = 7, .mode = .uc, .pat_slot = 3 },
    };
    for (0..256) |mask| {
        var changes: [8]Span = undefined;
        var count: usize = 0;
        for (0..8) |page| {
            if (mask & (@as(usize, 1) << @as(u3, @intCast(page))) == 0) continue;
            const wb = page % 2 == 0;
            changes[count] = .{ .base = (page + 1) * 4096, .bytes = 4096, .owner = 7, .mode = if (wb) .wb else .uc_minus, .pat_slot = if (wb) 0 else 2 };
            count += 1;
        }
        var output: [32]Span = undefined;
        const n = try prepareMany(&old, changes[0..count], &output);
        var a: [32]Span = undefined;
        var b: [32]Span = undefined;
        var reference: []const Span = &old;
        for (changes[0..count], 0..) |change, index| {
            const scratch = if (index % 2 == 0) &a else &b;
            const used = try prepare(reference, change, scratch);
            reference = scratch[0..used];
        }
        try std.testing.expectEqualSlices(Span, reference, output[0..n]);
    }
}

test "physical policy splits and merges without per-view attributes" {
    const original = [_]Span{.{ .base = 0x1000, .bytes = 0x4000, .owner = 7, .mode = .wc, .pat_slot = 4 }};
    var scratch: [4]Span = undefined;
    const changed: Span = .{ .base = 0x2000, .bytes = 0x1000, .owner = 7, .mode = .uc, .pat_slot = 3 };
    const count = try prepare(&original, changed, &scratch);
    try std.testing.expectEqual(@as(usize, 3), count);
    try std.testing.expectEqual(Mode.wc, lookup(scratch[0..count], 0x1fff).?.mode);
    try std.testing.expectEqual(Mode.uc, lookup(scratch[0..count], 0x2000).?.mode);
    try std.testing.expectEqual(Mode.wc, lookup(scratch[0..count], 0x3000).?.mode);
    try std.testing.expect(lookup(scratch[0..count], 0x5000) == null);
    var merged: [4]Span = undefined;
    var restore = changed;
    restore.mode = .wc;
    restore.pat_slot = 4;
    const merged_count = try prepare(scratch[0..count], restore, &merged);
    try std.testing.expectEqual(@as(usize, 1), merged_count);
    try std.testing.expectEqual(original[0], merged[0]);
}

test "WB restoration removes policy only for the requested owned range" {
    const original = [_]Span{.{ .base = 0x1000, .bytes = 0x3000, .owner = 9, .mode = .wc, .pat_slot = 4 }};
    var scratch: [3]Span = undefined;
    const change: Span = .{ .base = 0x2000, .bytes = 0x1000, .owner = 9, .mode = .wb, .pat_slot = 0 };
    const count = try prepare(&original, change, &scratch);
    try std.testing.expectEqual(@as(usize, 2), count);
    try std.testing.expect(lookup(scratch[0..count], 0x2000) == null);
    try std.testing.expectEqual(Mode.wc, original[0].mode);
    var alien = change;
    alien.owner = 10;
    try std.testing.expectError(error.Conflict, prepare(&original, alien, &scratch));
}

test "preflight rejects capacity failure, overlap and invalid ranges" {
    var original = [_]Span{.{ .base = 0x1000, .bytes = 0x3000, .owner = 9, .mode = .wc, .pat_slot = 4 }};
    var scratch: [1]Span = undefined;
    var change: Span = .{ .base = 0x2000, .bytes = 0x1000, .owner = 9, .mode = .uc, .pat_slot = 3 };
    try std.testing.expectError(error.NoSpace, prepare(&original, change, &scratch));
    try std.testing.expectEqual(@as(u64, 0x3000), original[0].bytes);
    try std.testing.expectError(error.Invalid, prepare(&original, change, &original));
    change.base = 0xfffffffffffff000;
    try std.testing.expectError(error.Invalid, prepare(&original, change, &scratch));
    change.base = 0x2001;
    try std.testing.expectError(error.Invalid, prepare(&original, change, &scratch));
}

test "successive sparse transitions agree with a per-page reference" {
    var first: [64]Span = undefined;
    var second: [64]Span = undefined;
    var active: []Span = first[0..0];
    var scratch: []Span = &second;
    var expected: [16]Mode = .{.wb} ** 16;
    const modes = [_]Mode{ .wb, .wc, .uc, .uc_minus };
    const pats = [_]u3{ 0, 4, 3, 2 };
    // Include gaps, whole-table replacement, nested splits and coalescing.
    for (0..16) |begin| {
        for (begin + 1..17) |finish| {
            for (modes, pats) |mode, pat| {
                const change: Span = .{
                    .base = (begin + 1) * 4096,
                    .bytes = (finish - begin) * 4096,
                    .owner = 19,
                    .mode = mode,
                    .pat_slot = pat,
                };
                const count = try prepare(active, change, scratch);
                const old = active.ptr;
                active = scratch[0..count];
                scratch = old[0..64];
                @memset(expected[begin..finish], mode);
                for (expected, 0..) |wanted, page| {
                    const found = lookup(active, (page + 1) * 4096);
                    try std.testing.expectEqual(wanted, if (found) |span| span.mode else .wb);
                }
            }
        }
    }
}
