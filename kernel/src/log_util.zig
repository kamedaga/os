const std = @import("std");
const serial = @import("serial.zig");
const kernel_log = @import("kernel_log.zig");

const serialWrite = kernel_log.write;

pub fn printNumber(value: anytype) void {
    serial.printNumber(value);
    var buf: [32]u8 = undefined;
    const s = std.fmt.bufPrint(buf[0..], "{}", .{value}) catch return;
    kernel_log.appendText(s);
}

pub fn printNumberU64(value: u64) void {
    printNumber(value);
}

pub fn printHex(value: u64) void {
    const hex = "0123456789abcdef";
    serial.write("0x");
    kernel_log.appendText("0x");
    var started = false;
    var shift: u6 = 60;
    while (true) {
        const nibble: u4 = @intCast((value >> shift) & 0xF);
        if (started or nibble != 0 or shift == 0) {
            serial.writeByte(hex[nibble]);
            kernel_log.appendByte(hex[nibble]);
            started = true;
        }
        if (shift == 0) break;
        shift -= 4;
    }
}

pub fn writeErrorName(err: anyerror) void {
    serialWrite(@errorName(err));
}

pub fn logMessage(message: []const u8) void {
    serialWrite(message);
    serialWrite("\n");
}

pub fn logError(prefix: []const u8, err: anyerror) void {
    serialWrite(prefix);
    serialWrite(@errorName(err));
    serialWrite("\n");
}

pub fn logLabelStepError(prefix: []const u8, label: []const u8, step: []const u8, err: anyerror) void {
    serialWrite(prefix);
    serialWrite(label);
    serialWrite(step);
    serialWrite(": ");
    serialWrite(@errorName(err));
    serialWrite("\n");
}

pub fn logLabelMessage(label: []const u8, suffix: []const u8) void {
    serialWrite(label);
    serialWrite(suffix);
    serialWrite("\n");
}

pub fn logPrefixedLabelMessage(prefix: []const u8, label: []const u8, suffix: []const u8) void {
    serialWrite(prefix);
    serialWrite(label);
    serialWrite(suffix);
    serialWrite("\n");
}

pub fn logRequiredMax(prefix: []const u8, required: usize, max: usize) void {
    serialWrite(prefix);
    printNumber(required);
    serialWrite(" max=");
    printNumber(max);
    serialWrite("\n");
}

pub fn logRequiredBytes(prefix: []const u8, required: usize) void {
    serialWrite(prefix);
    printNumber(required);
    serialWrite("\n");
}

pub fn logIndexedError(prefix: []const u8, index: usize, err: anyerror) void {
    serialWrite(prefix);
    printNumber(index);
    serialWrite(" err=");
    serialWrite(@errorName(err));
    serialWrite("\n");
}

pub fn logIndexedMapFailure(prefix: []const u8, index: usize, va: u64, paddr: u64) void {
    serialWrite(prefix);
    printNumber(index);
    serialWrite(" va=");
    printHex(va);
    serialWrite(" paddr=");
    printHex(paddr);
    serialWrite("\n");
}
