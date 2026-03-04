const std = @import("std");

pub const Location = struct {
    bus: u8,
    device: u8,
    function: u8,
};

const config_address_port: u16 = 0xCF8;
const config_data_port: u16 = 0xCFC;

fn outl(port: u16, value: u32) void {
    asm volatile ("outl %[value], %[port]"
        :
        : [value] "{eax}" (value),
          [port] "{dx}" (port),
    );
}

fn inl(port: u16) u32 {
    var value: u32 = 0;
    asm volatile ("inl %[port], %[value]"
        : [value] "={eax}" (value),
        : [port] "{dx}" (port),
    );
    return value;
}

fn configAddress(loc: Location, offset: u8) u32 {
    return 0x8000_0000 |
        (@as(u32, loc.bus) << 16) |
        (@as(u32, loc.device) << 11) |
        (@as(u32, loc.function) << 8) |
        (@as(u32, offset) & 0xFC);
}

pub fn readConfigU32(loc: Location, offset: u8) u32 {
    outl(config_address_port, configAddress(loc, offset));
    return inl(config_data_port);
}

pub fn writeConfigU32(loc: Location, offset: u8, value: u32) void {
    outl(config_address_port, configAddress(loc, offset));
    outl(config_data_port, value);
}

pub fn readConfigU16(loc: Location, offset: u8) u16 {
    const value = readConfigU32(loc, offset);
    const shift: u5 = @intCast((offset & 0x2) * 8);
    return @intCast((value >> shift) & 0xFFFF);
}

pub fn writeConfigU16(loc: Location, offset: u8, value: u16) void {
    const aligned = offset & 0xFC;
    const shift: u5 = @intCast((offset & 0x2) * 8);
    const mask: u32 = ~(@as(u32, 0xFFFF) << shift);
    const cur = readConfigU32(loc, aligned);
    const next = (cur & mask) | (@as(u32, value) << shift);
    writeConfigU32(loc, aligned, next);
}

pub fn readConfigU8(loc: Location, offset: u8) u8 {
    const value = readConfigU32(loc, offset);
    const shift: u5 = @intCast((offset & 0x3) * 8);
    return @intCast((value >> shift) & 0xFF);
}

pub fn readVendorId(loc: Location) u16 {
    return readConfigU16(loc, 0x00);
}

pub fn readDeviceId(loc: Location) u16 {
    return readConfigU16(loc, 0x02);
}

pub fn readSubsystemId(loc: Location) u16 {
    return readConfigU16(loc, 0x2E);
}

pub fn readHeaderType(loc: Location) u8 {
    return readConfigU8(loc, 0x0E);
}

test "config address encoding" {
    const loc = Location{
        .bus = 0x12,
        .device = 0x03,
        .function = 0x04,
    };
    try std.testing.expectEqual(@as(u32, 0x8012_1C20), configAddress(loc, 0x20));
}
