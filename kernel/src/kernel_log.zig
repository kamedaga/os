const std = @import("std");
const serial = @import("serial.zig");
const root = @import("root");
const log_abi = @import("kernel_abi_root").kernel_log_abi;
pub const ring_enabled = if (@hasDecl(root, "kernel_log_ring_enabled"))
    root.kernel_log_ring_enabled
else
    false;
const diagnostic_enabled = if (@hasDecl(root, "boot_diagnostic_enabled"))
    root.boot_diagnostic_enabled
else
    false;

// The separate diagnostic ELF can observe existing serial log fragments
// after H. The ordinary kernel has no observer calls or GOP dependency.
var diagnostic_observer: ?*const fn ([]const u8) void = null;

pub fn setDiagnosticObserver(observer: ?*const fn ([]const u8) void) void {
    if (comptime diagnostic_enabled) diagnostic_observer = observer;
}

const boot_log_max_bytes: usize = 32 * 1024;

// Debug builds retain recent kernel-originated bytes after the one-time boot
// snapshot. A contended/reentrant writer drops a fragment instead of waiting
// inside an interrupt or fault path. Readers snapshot under the short lock and
// copy to user memory only after releasing it.
pub fn Ring(comptime capacity: usize) type {
    return struct {
        const Self = @This();
        bytes: [capacity]u8 = [_]u8{0} ** capacity,
        next: u64 = 0,
        dropped: u64 = 0,
        lock_word: u8 = 0,

        fn tryLock(self: *Self) bool {
            return @cmpxchgStrong(u8, &self.lock_word, 0, 1, .acquire, .monotonic) == null;
        }

        fn unlock(self: *Self) void {
            @atomicStore(u8, &self.lock_word, 0, .release);
        }

        pub fn append(self: *Self, text: []const u8) void {
            if (!self.tryLock()) {
                _ = @atomicRmw(u64, &self.dropped, .Add, 1, .monotonic);
                return;
            }
            defer self.unlock();
            for (text) |byte| {
                self.bytes[@intCast(self.next % capacity)] = byte;
                self.next +%= 1;
            }
        }

        pub fn snapshot(self: *Self, cursor: u64, out: []u8, result: *log_abi.Read) bool {
            var attempts: usize = 0;
            while (!self.tryLock()) : (attempts += 1) {
                if (attempts == 4096) return false;
                std.atomic.spinLoopHint();
            }
            defer self.unlock();
            const oldest = self.next - @min(self.next, capacity);
            const start = if (cursor < oldest) oldest else @min(cursor, self.next);
            const count: usize = @intCast(@min(self.next - start, out.len));
            for (out[0..count], 0..) |*byte, index| {
                byte.* = self.bytes[@intCast((start + index) % capacity)];
            }
            result.next_cursor = start + count;
            result.oldest_cursor = oldest;
            result.written = count;
            result.dropped = @atomicLoad(u64, &self.dropped, .monotonic);
            result.overrun = @intFromBool(cursor < oldest);
            return true;
        }
    };
}

var debug_ring: if (ring_enabled) Ring(64 * 1024) else void =
    if (ring_enabled) .{} else {};

pub fn readDebug(cursor: u64, out: []u8, result: *log_abi.Read) bool {
    return if (comptime ring_enabled) debug_ring.snapshot(cursor, out, result) else false;
}

pub var boot_log_buffer: [boot_log_max_bytes]u8 = [_]u8{0} ** boot_log_max_bytes;
pub var boot_log_len: usize = 0;

pub fn reset() void {
    boot_log_len = 0;
    if (comptime ring_enabled) debug_ring = .{};
    if (comptime diagnostic_enabled) diagnostic_observer = null;
}

pub fn appendText(text: []const u8) void {
    if (boot_log_len < boot_log_buffer.len) {
        const remaining = boot_log_buffer.len - boot_log_len;
        const copy_len: usize = if (text.len < remaining) text.len else remaining;
        @memcpy(boot_log_buffer[boot_log_len .. boot_log_len + copy_len], text[0..copy_len]);
        boot_log_len += copy_len;
    }
    if (comptime diagnostic_enabled) {
        if (diagnostic_observer) |observer| observer(text);
    }
}

pub fn appendByte(value: u8) void {
    if (boot_log_len < boot_log_buffer.len) {
        boot_log_buffer[boot_log_len] = value;
        boot_log_len += 1;
    }
    if (comptime diagnostic_enabled) {
        if (diagnostic_observer) |observer| {
            const one = [_]u8{value};
            observer(&one);
        }
    }
}

pub fn appendHexText(value: u64, fixed_width: bool) void {
    const hex = "0123456789abcdef";
    appendText("0x");
    var started = fixed_width;
    var shift: u6 = 60;
    while (true) {
        const nibble: u4 = @intCast((value >> shift) & 0xF);
        if (started or nibble != 0 or shift == 0) {
            appendByte(hex[nibble]);
            started = true;
        }
        if (shift == 0) break;
        shift -= 4;
    }
}

pub fn write(text: []const u8) void {
    serial.write(text);
    appendText(text);
    if (comptime ring_enabled) debug_ring.append(text);
}

pub fn writeUser(text: []const u8) void {
    // syscall_log keeps its historical serial and boot-snapshot behavior, but
    // user-originated messages must not impersonate kernel ring records.
    serial.write(text);
    appendText(text);
}

pub fn writeFmt(comptime fmt: []const u8, args: anytype) void {
    var buf: [256]u8 = undefined;
    const s = std.fmt.bufPrint(buf[0..], fmt, args) catch return;
    write(s);
}

pub fn writeHexRaw(value: u64) void {
    serial.writeHexRaw(value);
    appendHexText(value, true);
    if (comptime ring_enabled) {
        var buf: [18]u8 = undefined;
        const formatted = std.fmt.bufPrint(&buf, "0x{x:0>16}", .{value}) catch unreachable;
        debug_ring.append(formatted);
    }
}

pub fn writeBool01(value: bool) void {
    serial.writeBool01(value);
    appendText(if (value) "1" else "0");
    if (comptime ring_enabled) debug_ring.append(if (value) "1" else "0");
}

test "debug ring cursor, wrap and overrun" {
    var ring = Ring(5){};
    ring.append("abc");
    var result: log_abi.Read = undefined;
    var out: [5]u8 = undefined;
    try std.testing.expect(ring.snapshot(0, &out, &result));
    try std.testing.expectEqualStrings("abc", out[0..result.written]);
    ring.append("defg");
    try std.testing.expect(ring.snapshot(0, &out, &result));
    try std.testing.expectEqualStrings("cdefg", out[0..result.written]);
    try std.testing.expectEqual(@as(u64, 2), result.oldest_cursor);
    try std.testing.expectEqual(@as(u64, 7), result.next_cursor);
    try std.testing.expectEqual(@as(u64, 1), result.overrun);
}

test "debug ring reports a dropped contended fragment" {
    var ring = Ring(8){};
    ring.lock_word = 1;
    ring.append("lost");
    ring.lock_word = 0;
    var result: log_abi.Read = undefined;
    var out: [8]u8 = undefined;
    try std.testing.expect(ring.snapshot(0, &out, &result));
    try std.testing.expectEqual(@as(u64, 0), result.written);
    try std.testing.expectEqual(@as(u64, 1), result.dropped);
}

test "default kernel does not expose the ring" {
    if (comptime !ring_enabled) {
        var result: log_abi.Read = undefined;
        var out: [1]u8 = undefined;
        try std.testing.expect(!readDebug(0, &out, &result));
    }
}

pub fn writeThreadIndexLabel(thread_index: usize) void {
    switch (thread_index) {
        0 => write("0"),
        1 => write("1"),
        2 => write("2"),
        3 => write("3"),
        4 => write("4"),
        5 => write("5"),
        6 => write("6"),
        7 => write("7"),
        8 => write("8"),
        9 => write("9"),
        else => write("?"),
    }
}

pub fn writeOnly(text: []const u8) void {
    serial.write(text);
}

pub fn writeOnlyBool01(value: bool) void {
    serial.writeBool01(value);
}

pub fn writeOnlyThreadIndexLabel(thread_index: usize) void {
    switch (thread_index) {
        0 => writeOnly("0"),
        1 => writeOnly("1"),
        2 => writeOnly("2"),
        3 => writeOnly("3"),
        4 => writeOnly("4"),
        5 => writeOnly("5"),
        6 => writeOnly("6"),
        7 => writeOnly("7"),
        8 => writeOnly("8"),
        9 => writeOnly("9"),
        else => writeOnly("?"),
    }
}
