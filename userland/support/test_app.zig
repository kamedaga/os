const elf_magic = [_]u8{ 0x7F, 'E', 'L', 'F' };
const elf_class_64: u8 = 2;
const elf_data_lsb: u8 = 1;
const elf_version_current: u8 = 1;
const elf_type_exec: u16 = 2;
const elf_machine_x86_64: u16 = 0x3E;
const elf_phdr_size: u16 = 56;
const pt_load: u32 = 1;

pub const entry_va: u64 = 0x2000_0000;

fn writeU16Le(bytes: []u8, off: usize, value: u16) void {
    bytes[off] = @intCast(value & 0xFF);
    bytes[off + 1] = @intCast((value >> 8) & 0xFF);
}

fn writeU32Le(bytes: []u8, off: usize, value: u32) void {
    bytes[off] = @intCast(value & 0xFF);
    bytes[off + 1] = @intCast((value >> 8) & 0xFF);
    bytes[off + 2] = @intCast((value >> 16) & 0xFF);
    bytes[off + 3] = @intCast((value >> 24) & 0xFF);
}

fn writeU64Le(bytes: []u8, off: usize, value: u64) void {
    var i: usize = 0;
    while (i < 8) : (i += 1) {
        bytes[off + i] = @intCast((value >> @intCast(i * 8)) & 0xFF);
    }
}

const image = blk: {
    var elf = [_]u8{0} ** 0x104;

    elf[0] = elf_magic[0];
    elf[1] = elf_magic[1];
    elf[2] = elf_magic[2];
    elf[3] = elf_magic[3];
    elf[4] = elf_class_64;
    elf[5] = elf_data_lsb;
    elf[6] = elf_version_current;

    writeU16Le(elf[0..], 16, elf_type_exec);
    writeU16Le(elf[0..], 18, elf_machine_x86_64);
    writeU32Le(elf[0..], 20, @as(u32, elf_version_current));
    writeU64Le(elf[0..], 24, entry_va);
    writeU64Le(elf[0..], 32, 64);
    writeU16Le(elf[0..], 52, 64);
    writeU16Le(elf[0..], 54, elf_phdr_size);
    writeU16Le(elf[0..], 56, 1);

    writeU32Le(elf[0..], 64 + 0, pt_load);
    writeU32Le(elf[0..], 64 + 4, 0x5); // R|X
    writeU64Le(elf[0..], 64 + 8, 0x100);
    writeU64Le(elf[0..], 64 + 16, entry_va);
    writeU64Le(elf[0..], 64 + 32, 4);
    writeU64Le(elf[0..], 64 + 40, 0x1000);
    writeU64Le(elf[0..], 64 + 48, 0x1000);

    // pause; jmp -4
    elf[0x100] = 0xF3;
    elf[0x101] = 0x90;
    elf[0x102] = 0xEB;
    elf[0x103] = 0xFC;

    break :blk elf;
};

pub fn elfBytes() []const u8 {
    return image[0..];
}
