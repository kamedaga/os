const std = @import("std");
const serial = @import("serial.zig");

const boot_log_max_bytes: usize = 32 * 1024;

pub var boot_log_buffer: [boot_log_max_bytes]u8 = [_]u8{0} ** boot_log_max_bytes;
pub var boot_log_len: usize = 0;

pub fn reset() void {
    boot_log_len = 0;
}

pub fn appendText(text: []const u8) void {
    if (boot_log_len >= boot_log_buffer.len) return;
    const remaining = boot_log_buffer.len - boot_log_len;
    const copy_len: usize = if (text.len < remaining) text.len else remaining;
    @memcpy(boot_log_buffer[boot_log_len .. boot_log_len + copy_len], text[0..copy_len]);
    boot_log_len += copy_len;
}

pub fn appendByte(value: u8) void {
    if (boot_log_len >= boot_log_buffer.len) return;
    boot_log_buffer[boot_log_len] = value;
    boot_log_len += 1;
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
}

pub fn writeFmt(comptime fmt: []const u8, args: anytype) void {
    var buf: [256]u8 = undefined;
    const s = std.fmt.bufPrint(buf[0..], fmt, args) catch return;
    write(s);
}

pub fn writeHexRaw(value: u64) void {
    serial.writeHexRaw(value);
    appendHexText(value, true);
}

pub fn writeBool01(value: bool) void {
    serial.writeBool01(value);
    appendText(if (value) "1" else "0");
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
