const std = @import("std");

const page: usize = 0x1000;
const app_size: usize = 0x1400;
const lib_size: usize = 0x1400;

const et_dyn: u16 = 3;
const em_x86_64: u16 = 0x3e;
const ev_current: u32 = 1;

const pt_load: u32 = 1;
const pt_dynamic: u32 = 2;
const pt_interp: u32 = 3;
const pt_phdr: u32 = 6;
const pt_tls: u32 = 7;
const pt_gnu_relro: u32 = 0x6474_e552;
const pf_x: u32 = 1;
const pf_w: u32 = 2;
const pf_r: u32 = 4;

const dt_null: u64 = 0;
const dt_needed: u64 = 1;
const dt_pltrelsz: u64 = 2;
const dt_hash: u64 = 4;
const dt_strtab: u64 = 5;
const dt_symtab: u64 = 6;
const dt_gnu_hash: u64 = 0x6fff_fef5;
const dt_rela: u64 = 7;
const dt_relasz: u64 = 8;
const dt_relaent: u64 = 9;
const dt_strsz: u64 = 10;
const dt_syment: u64 = 11;
const dt_init: u64 = 12;
const dt_fini: u64 = 13;
const dt_soname: u64 = 14;
const dt_init_array: u64 = 25;
const dt_fini_array: u64 = 26;
const dt_init_arraysz: u64 = 27;
const dt_fini_arraysz: u64 = 28;
const dt_runpath: u64 = 29;
const dt_preinit_array: u64 = 32;
const dt_preinit_arraysz: u64 = 33;
const dt_pltrel: u64 = 20;
const dt_jmprel: u64 = 23;
const dt_verdef: u64 = 0x6fff_fffc;
const dt_verdefnum: u64 = 0x6fff_fffd;
const dt_verneed: u64 = 0x6fff_fffe;
const dt_verneednum: u64 = 0x6fff_ffff;
const dt_versym: u64 = 0x6fff_fff0;

const r_x86_64_jump_slot: u64 = 7;
const r_x86_64_relative: u64 = 8;
const r_x86_64_dtpmod64: u64 = 16;
const r_x86_64_dtpoff64: u64 = 17;
const r_x86_64_tlsdesc: u64 = 36;

pub fn main() !void {
    var app = [_]u8{0} ** app_size;
    var lib = [_]u8{0} ** lib_size;
    buildApp(&app);
    buildLib(&lib);
    try std.fs.cwd().writeFile(.{ .sub_path = "userland/fixtures/smoke_app.elf", .data = &app });
    try std.fs.cwd().writeFile(.{ .sub_path = "userland/fixtures/libsmoke.so", .data = &lib });
}

fn buildApp(image: []u8) void {
    const phnum: u16 = 6;
    const interp = "/lib/ld.so\x00";
    writeEhdr(image, 0x1c0, phnum);
    writePhdr(image, 0, pt_phdr, pf_r, 0x40, 0x40, phnum * 56, phnum * 56, 8);
    writePhdr(image, 1, pt_interp, pf_r, 0x4c0, 0x4c0, interp.len, interp.len, 1);
    writePhdr(image, 2, pt_load, pf_r | pf_x, 0, 0, 0x500, 0x500, 0x1000);
    writePhdr(image, 3, pt_load, pf_r | pf_w, page, page, 0x300, 0x300, 0x1000);
    writePhdr(image, 4, pt_dynamic, pf_r | pf_w, page, page, 0x1a0, 0x1a0, 8);
    writePhdr(image, 5, pt_gnu_relro, pf_r, page, page, 0x300, 0x300, 8);
    copyBytes(image, 0x4c0, interp);

    const got_va: u64 = 0x1210;
    var pc: usize = 0x1c0;
    image[pc..][0..3].* = .{ 0x48, 0x8b, 0x05 };
    writeU32(image, pc + 3, rel32(@intCast(got_va), pc + 7));
    pc += 7;
    image[pc..][0..2].* = .{ 0xff, 0xd0 };
    pc += 2;
    const msg = "ExecLoader smoke child reached ET_DYN writable data+bss entry\n";
    _ = emitLogReturn(image, pc, 0x380, msg);
    const preinit_msg = "smoke_app: preinit_array\n";
    const main_init_msg = "smoke_app: dt_init\n";
    const main_init_array_msg = "smoke_app: init_array\n";
    const main_fini_msg = "smoke_app: dt_fini\n";
    const main_fini_array_msg = "smoke_app: fini_array\n";
    _ = emitLogReturn(image, 0x240, 0x3c0, preinit_msg);
    _ = emitLogReturn(image, 0x280, 0x3e0, main_init_msg);
    _ = emitLogReturn(image, 0x2c0, 0x400, main_init_array_msg);
    _ = emitLogReturn(image, 0x300, 0x430, main_fini_msg);
    _ = emitLogReturn(image, 0x330, 0x450, main_fini_array_msg);
    copyBytes(image, 0x380, msg);
    copyBytes(image, 0x3c0, preinit_msg);
    copyBytes(image, 0x3e0, main_init_msg);
    copyBytes(image, 0x400, main_init_array_msg);
    copyBytes(image, 0x430, main_fini_msg);
    copyBytes(image, 0x450, main_fini_array_msg);

    writeDyn(image, 0x1000, 0, dt_needed, 1);
    writeDyn(image, 0x1000, 1, dt_hash, 0x11d0);
    writeDyn(image, 0x1000, 2, dt_symtab, 0x11a0);
    writeDyn(image, 0x1000, 3, dt_syment, 24);
    writeDyn(image, 0x1000, 4, dt_strtab, 0x1220);
    writeDyn(image, 0x1000, 5, dt_strsz, 40);
    writeDyn(image, 0x1000, 6, dt_jmprel, 0x11f0);
    writeDyn(image, 0x1000, 7, dt_pltrelsz, 24);
    writeDyn(image, 0x1000, 8, dt_pltrel, dt_rela);
    writeDyn(image, 0x1000, 9, dt_relaent, 24);
    writeDyn(image, 0x1000, 10, dt_runpath, 25);
    writeDyn(image, 0x1000, 11, dt_versym, 0x1250);
    writeDyn(image, 0x1000, 12, dt_verneed, 0x1260);
    writeDyn(image, 0x1000, 13, dt_verneednum, 1);
    writeDyn(image, 0x1000, 14, dt_rela, 0x1280);
    writeDyn(image, 0x1000, 15, dt_relasz, 72);
    writeDyn(image, 0x1000, 16, dt_init, 0x280);
    writeDyn(image, 0x1000, 17, dt_fini, 0x300);
    writeDyn(image, 0x1000, 18, dt_preinit_arraysz, 8);
    writeDyn(image, 0x1000, 19, dt_preinit_array, 0x12d0);
    writeDyn(image, 0x1000, 20, dt_init_arraysz, 8);
    writeDyn(image, 0x1000, 21, dt_init_array, 0x12d8);
    writeDyn(image, 0x1000, 22, dt_fini_arraysz, 8);
    writeDyn(image, 0x1000, 23, dt_fini_array, 0x12e0);
    writeDyn(image, 0x1000, 24, dt_null, 0);

    writeSym(image, 0x11a0, 0, 0, 0, 0, 0, 0);
    writeSym(image, 0x11a0, 1, 13, 0x12, 0, 0, 0);
    writeHash(image, 0x11d0);
    writeRela(image, 0x11f0, got_va, (1 << 32) | r_x86_64_jump_slot, 0);
    copyBytes(image, 0x1220, "\x00libsmoke.so\x00smoke_hello\x00/lib\x00SMOKE_1.0\x00");
    writeU16(image, 0x1250, 0);
    writeU16(image, 0x1252, 2);
    writeVerneed(image, 0x1260, 1, 30, 2);
    writeRela(image, 0x1280, 0x12d0, r_x86_64_relative, 0x240);
    writeRela(image, 0x1298, 0x12d8, r_x86_64_relative, 0x2c0);
    writeRela(image, 0x12b0, 0x12e0, r_x86_64_relative, 0x330);
    writeU64(image, 0x12d0, 0);
    writeU64(image, 0x12d8, 0);
    writeU64(image, 0x12e0, 0);
}

fn buildLib(image: []u8) void {
    const phnum: u16 = 6;
    writeEhdr(image, 0x1a0, phnum);
    writePhdr(image, 0, pt_phdr, pf_r, 0x40, 0x40, phnum * 56, phnum * 56, 8);
    writePhdr(image, 1, pt_load, pf_r | pf_x, 0, 0, 0x500, 0x500, 0x1000);
    writePhdr(image, 2, pt_load, pf_r | pf_w, page, page, 0x300, 0x300, 0x1000);
    writePhdr(image, 3, pt_dynamic, pf_r | pf_w, page, page, 0x150, 0x150, 8);
    writePhdr(image, 4, pt_tls, pf_r, 0x12d0, 0x12d0, 8, 16, 8);
    writePhdr(image, 5, pt_gnu_relro, pf_r, page, page, 0x300, 0x300, 8);

    const msg = "libsmoke: smoke_hello\n";
    _ = emitLogReturn(image, 0x1a0, 0x300, msg);
    copyBytes(image, 0x300, msg);

    const init_msg = "libsmoke: dt_init\n";
    const tlsdesc_msg = "libsmoke: tlsdesc ok\n";
    const init_array_msg = "libsmoke: init_array\n";
    const fini_msg = "libsmoke: dt_fini\n";
    const fini_array_msg = "libsmoke: fini_array\n";
    _ = emitLogThenTlsdesc(image, 0x1c0, 0x320, init_msg, 0x12a0, 0x340, tlsdesc_msg);
    _ = emitLogReturn(image, 0x220, 0x360, init_array_msg);
    _ = emitLogThenWrite(image, 0x250, 0x380, fini_msg, 0x1000);
    _ = emitLogReturn(image, 0x280, 0x3a0, fini_array_msg);
    copyBytes(image, 0x320, init_msg);
    copyBytes(image, 0x340, tlsdesc_msg);
    copyBytes(image, 0x360, init_array_msg);
    copyBytes(image, 0x380, fini_msg);
    copyBytes(image, 0x3a0, fini_array_msg);
    copyBytes(image, 0x12d0, "\x11\x22\x33\x44\x55\x66\x77\x88");

    writeDyn(image, 0x1000, 0, dt_init, 0x1c0);
    writeDyn(image, 0x1000, 1, dt_init_array, 0x12c0);
    writeDyn(image, 0x1000, 2, dt_init_arraysz, 8);
    writeDyn(image, 0x1000, 3, dt_fini, 0x250);
    writeDyn(image, 0x1000, 4, dt_fini_array, 0x12c8);
    writeDyn(image, 0x1000, 5, dt_fini_arraysz, 8);
    writeDyn(image, 0x1000, 6, dt_hash, 0x11b0);
    writeDyn(image, 0x1000, 7, dt_gnu_hash, 0x11d0);
    writeDyn(image, 0x1000, 8, dt_symtab, 0x1150);
    writeDyn(image, 0x1000, 9, dt_syment, 24);
    writeDyn(image, 0x1000, 10, dt_strtab, 0x1180);
    writeDyn(image, 0x1000, 11, dt_strsz, 43);
    writeDyn(image, 0x1000, 12, dt_rela, 0x1220);
    writeDyn(image, 0x1000, 13, dt_relasz, 120);
    writeDyn(image, 0x1000, 14, dt_relaent, 24);
    writeDyn(image, 0x1000, 15, dt_soname, 13);
    writeDyn(image, 0x1000, 16, dt_runpath, 25);
    writeDyn(image, 0x1000, 17, dt_versym, 0x11f0);
    writeDyn(image, 0x1000, 18, dt_verdef, 0x1200);
    writeDyn(image, 0x1000, 19, dt_verdefnum, 1);
    writeDyn(image, 0x1000, 20, dt_null, 0);

    writeSym(image, 0x1150, 0, 0, 0, 0, 0, 0);
    writeSym(image, 0x1150, 1, 1, 0x12, 1, 0x1a0, 0x2a);
    copyBytes(image, 0x1180, "\x00smoke_hello\x00libsmoke.so\x00$ORIGIN\x00SMOKE_1.0\x00");
    writeHash(image, 0x11b0);
    writeGnuHash(image, 0x11d0, "smoke_hello");
    writeU16(image, 0x11f0, 0);
    writeU16(image, 0x11f2, 2);
    writeVerdef(image, 0x1200, 2, 33);
    writeRela(image, 0x1220, 0x12b0, r_x86_64_dtpmod64, 0);
    writeRela(image, 0x1238, 0x12b8, r_x86_64_dtpoff64, 8);
    writeRela(image, 0x1250, 0x12a0, r_x86_64_tlsdesc, 0);
    writeRela(image, 0x1268, 0x12c0, r_x86_64_relative, 0x220);
    writeRela(image, 0x1280, 0x12c8, r_x86_64_relative, 0x280);
    writeU64(image, 0x12b0, 0);
    writeU64(image, 0x12b8, 0);
    writeU64(image, 0x12c0, 0);
    writeU64(image, 0x12c8, 0);
}

fn emitLog(image: []u8, start: usize, msg_va: u64, msg: []const u8) usize {
    var pc = start;
    image[pc..][0..7].* = .{ 0x48, 0xc7, 0xc0, 0x09, 0x00, 0x00, 0x00 };
    pc += 7;
    image[pc..][0..3].* = .{ 0x48, 0x8d, 0x3d };
    writeU32(image, pc + 3, rel32(msg_va, pc + 7));
    pc += 7;
    image[pc..][0..7].* = .{ 0x48, 0xc7, 0xc6, @intCast(msg.len), 0x00, 0x00, 0x00 };
    pc += 7;
    image[pc..][0..2].* = .{ 0x31, 0xd2 };
    pc += 2;
    image[pc..][0..2].* = .{ 0xcd, 0x80 };
    pc += 2;
    image[pc..][0..4].* = .{ 0xf3, 0x90, 0xeb, 0xfc };
    return pc + 4;
}

fn emitLogReturn(image: []u8, start: usize, msg_va: u64, msg: []const u8) usize {
    var pc = start;
    image[pc..][0..7].* = .{ 0x48, 0xc7, 0xc0, 0x09, 0x00, 0x00, 0x00 };
    pc += 7;
    image[pc..][0..3].* = .{ 0x48, 0x8d, 0x3d };
    writeU32(image, pc + 3, rel32(msg_va, pc + 7));
    pc += 7;
    image[pc..][0..7].* = .{ 0x48, 0xc7, 0xc6, @intCast(msg.len), 0x00, 0x00, 0x00 };
    pc += 7;
    image[pc..][0..2].* = .{ 0x31, 0xd2 };
    pc += 2;
    image[pc..][0..2].* = .{ 0xcd, 0x80 };
    pc += 2;
    image[pc] = 0xc3;
    return pc + 1;
}

fn emitLogThenTlsdesc(image: []u8, start: usize, msg_va: u64, msg: []const u8, tlsdesc_va: u64, tlsdesc_msg_va: u64, tlsdesc_msg: []const u8) usize {
    var pc = emitLogReturn(image, start, msg_va, msg);
    pc -= 1;
    image[pc..][0..3].* = .{ 0x48, 0x8d, 0x05 };
    writeU32(image, pc + 3, rel32(tlsdesc_va, pc + 7));
    pc += 7;
    image[pc..][0..2].* = .{ 0xff, 0x10 };
    pc += 2;
    pc = emitLogReturn(image, pc, tlsdesc_msg_va, tlsdesc_msg);
    return pc;
}

fn emitLogThenWrite(image: []u8, start: usize, msg_va: u64, msg: []const u8, write_va: u64) usize {
    var pc = emitLogReturn(image, start, msg_va, msg);
    pc -= 1;
    image[pc..][0..2].* = .{ 0xc6, 0x05 };
    writeU32(image, pc + 2, rel32(write_va, pc + 7));
    image[pc + 6] = 0x7f;
    pc += 7;
    image[pc] = 0xc3;
    return pc + 1;
}

fn writeEhdr(image: []u8, entry: u64, phnum: u16) void {
    image[0..16].* = .{ 0x7f, 'E', 'L', 'F', 2, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    writeU16(image, 16, et_dyn);
    writeU16(image, 18, em_x86_64);
    writeU32(image, 20, ev_current);
    writeU64(image, 24, entry);
    writeU64(image, 32, 0x40);
    writeU16(image, 52, 64);
    writeU16(image, 54, 56);
    writeU16(image, 56, phnum);
}

fn writePhdr(image: []u8, index: usize, typ: u32, flags: u32, off: u64, vaddr: u64, filesz: u64, memsz: u64, alignment: u64) void {
    const o = 0x40 + index * 56;
    writeU32(image, o + 0, typ);
    writeU32(image, o + 4, flags);
    writeU64(image, o + 8, off);
    writeU64(image, o + 16, vaddr);
    writeU64(image, o + 24, vaddr);
    writeU64(image, o + 32, filesz);
    writeU64(image, o + 40, memsz);
    writeU64(image, o + 48, alignment);
}

fn writeDyn(image: []u8, base: usize, index: usize, tag: u64, value: u64) void {
    const o = base + index * 16;
    writeU64(image, o, tag);
    writeU64(image, o + 8, value);
}

fn writeSym(image: []u8, base: usize, index: usize, name: u32, info: u8, shndx: u16, value: u64, size: u64) void {
    const o = base + index * 24;
    writeU32(image, o, name);
    image[o + 4] = info;
    image[o + 5] = 0;
    writeU16(image, o + 6, shndx);
    writeU64(image, o + 8, value);
    writeU64(image, o + 16, size);
}

fn writeHash(image: []u8, off: usize) void {
    writeU32(image, off + 0, 1);
    writeU32(image, off + 4, 2);
    writeU32(image, off + 8, 1);
    writeU32(image, off + 12, 0);
    writeU32(image, off + 16, 0);
}

fn gnuHashName(name: []const u8) u32 {
    var hash: u32 = 5381;
    var index: usize = 0;
    while (index < name.len) : (index += 1) {
        hash = hash *% 33 +% name[index];
    }
    return hash;
}

fn writeGnuHash(image: []u8, off: usize, name: []const u8) void {
    const hash = gnuHashName(name);
    const bloom_shift: u32 = 6;
    const bloom = (@as(u64, 1) << @intCast(hash % 64)) |
        (@as(u64, 1) << @intCast((hash >> @intCast(bloom_shift)) % 64));

    writeU32(image, off + 0, 1);
    writeU32(image, off + 4, 1);
    writeU32(image, off + 8, 1);
    writeU32(image, off + 12, bloom_shift);
    writeU64(image, off + 16, bloom);
    writeU32(image, off + 24, 1);
    writeU32(image, off + 28, hash | 1);
}

fn writeVerneed(image: []u8, off: usize, file_name: u32, version_name: u32, version_index: u16) void {
    writeU16(image, off + 0, 1);
    writeU16(image, off + 2, 1);
    writeU32(image, off + 4, file_name);
    writeU32(image, off + 8, 16);
    writeU32(image, off + 12, 0);
    writeU32(image, off + 16, 0);
    writeU16(image, off + 20, 0);
    writeU16(image, off + 22, version_index);
    writeU32(image, off + 24, version_name);
    writeU32(image, off + 28, 0);
}

fn writeVerdef(image: []u8, off: usize, version_index: u16, version_name: u32) void {
    writeU16(image, off + 0, 1);
    writeU16(image, off + 2, 0);
    writeU16(image, off + 4, version_index);
    writeU16(image, off + 6, 1);
    writeU32(image, off + 8, 0);
    writeU32(image, off + 12, 20);
    writeU32(image, off + 16, 0);
    writeU32(image, off + 20, version_name);
    writeU32(image, off + 24, 0);
}

fn writeRela(image: []u8, off: usize, r_offset: u64, r_info: u64, addend: i64) void {
    writeU64(image, off + 0, r_offset);
    writeU64(image, off + 8, r_info);
    writeU64(image, off + 16, @bitCast(addend));
}

fn copyBytes(image: []u8, off: usize, bytes: []const u8) void {
    @memcpy(image[off..][0..bytes.len], bytes);
}

fn rel32(target: u64, next_pc: usize) u32 {
    const delta: i64 = @as(i64, @intCast(target)) - @as(i64, @intCast(next_pc));
    return @bitCast(@as(i32, @intCast(delta)));
}

fn writeU16(image: []u8, off: usize, value: u16) void {
    image[off + 0] = @intCast(value & 0xff);
    image[off + 1] = @intCast(value >> 8);
}

fn writeU32(image: []u8, off: usize, value: u32) void {
    image[off + 0] = @intCast(value & 0xff);
    image[off + 1] = @intCast((value >> 8) & 0xff);
    image[off + 2] = @intCast((value >> 16) & 0xff);
    image[off + 3] = @intCast((value >> 24) & 0xff);
}

fn writeU64(image: []u8, off: usize, value: u64) void {
    var index: usize = 0;
    while (index < 8) : (index += 1) {
        image[off + index] = @intCast((value >> @intCast(index * 8)) & 0xff);
    }
}
