const std = @import("std");

const serial_port: u16 = 0x3F8;

fn outb(port: u16, value: u8) void {
    asm volatile ("outb %[value], %[port]"
        :
        : [value] "{al}" (value),
          [port] "{dx}" (port),
    );
}

fn writeByteInternal(b: u8) void {
    outb(serial_port, b);
}

pub fn writeRaw(text: []const u8) void {
    for (text) |ch| {
        if (ch == '\n') writeByteInternal('\r');
        writeByteInternal(ch);
    }
}

pub fn init() void {
    outb(serial_port + 1, 0x00);
    outb(serial_port + 3, 0x80);
    outb(serial_port + 0, 0x03);
    outb(serial_port + 1, 0x00);
    outb(serial_port + 3, 0x03);
    outb(serial_port + 2, 0xC7);
    outb(serial_port + 4, 0x0B);
}

pub fn write(text: []const u8) void {
    writeRaw(text);
}

pub fn writeHexRaw(value: u64) void {
    const hex = "0123456789abcdef";
    write("0x");
    var shift: u6 = 60;
    while (true) {
        const nibble: u4 = @intCast((value >> shift) & 0xF);
        writeByteInternal(hex[nibble]);
        if (shift == 0) break;
        shift -= 4;
    }
}

pub fn writeBool01(value: bool) void {
    writeByteInternal(if (value) '1' else '0');
}

pub fn printNumber(value: anytype) void {
    var num_buf: [32]u8 = undefined;
    const s = std.fmt.bufPrint(&num_buf, "{d}", .{value}) catch "err";
    write(s);
}

pub fn printHex(value: u64) void {
    var num_buf: [32]u8 = undefined;
    const s = std.fmt.bufPrint(&num_buf, "0x{x}", .{value}) catch "err";
    write(s);
}
