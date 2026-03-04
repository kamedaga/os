const std = @import("std");

pub const max_load_segments = 16;

pub const LoadSegment = struct {
    vaddr: u64,
    file_offset: u64,
    file_size: u64,
    mem_size: u64,
    flags: u32,
    align_bytes: u64,
};

pub const DynamicSegment = struct {
    vaddr: u64,
    mem_size: u64,
};

pub const Image = struct {
    entry: u64,
    load_segments: [max_load_segments]LoadSegment = undefined,
    load_segment_len: usize = 0,
    dynamic_segment: ?DynamicSegment = null,
};

pub const Error = error{
    ImageTooSmall,
    BadMagic,
    UnsupportedClass,
    UnsupportedEndian,
    UnsupportedMachine,
    UnsupportedType,
    BadVersion,
    ProgramHeaderOutOfRange,
    SegmentOutOfRange,
    SegmentSizeInvalid,
    NoLoadSegment,
    TooManyLoadSegments,
    TooManyDynamicSegments,
};

pub const LoadToPageError = Error || error{
    DestinationTooSmall,
    SegmentOutsideMappedRange,
    SegmentTooLarge,
    EntryOutsideMappedRange,
    AddressOverflow,
    DynamicSegmentOutsideMappedRange,
    InvalidDynamicTable,
    UnsupportedRelaFormat,
    UnsupportedRelocationType,
    RelocationOutOfRange,
    RelocationOverflow,
};

const elf_magic = [_]u8{ 0x7F, 'E', 'L', 'F' };
const elf_class_64 = 2;
const elf_data_lsb = 1;
const elf_version_current = 1;
const elf_type_exec: u16 = 2;
const elf_type_dyn: u16 = 3;
const elf_machine_x86_64: u16 = 0x3E;
const elf_phdr_size: u16 = 56;
const pt_load: u32 = 1;
const pt_dynamic: u32 = 2;
const dt_null: u64 = 0;
const dt_rela: u64 = 7;
const dt_relasz: u64 = 8;
const dt_relaent: u64 = 9;
const dt_relacount: u64 = 0x6FFF_FFF9;
const rela_entry_bytes: u64 = 24;
const r_x86_64_relative: u32 = 8;

pub const demo_idle_base_va: u64 = 0x2000_0000;
pub const demo_idle_entry_offset: u64 = 0x100;
pub const demo_idle_entry_va: u64 = demo_idle_base_va + demo_idle_entry_offset;
pub const demo_idle_reloc_target_offset: u64 = 0x1F8;

fn readU16Le(bytes: []const u8, off: usize) Error!u16 {
    if (off + 2 > bytes.len) return Error.ImageTooSmall;
    return @as(u16, bytes[off]) |
        (@as(u16, bytes[off + 1]) << 8);
}

fn readU32Le(bytes: []const u8, off: usize) Error!u32 {
    if (off + 4 > bytes.len) return Error.ImageTooSmall;
    return @as(u32, bytes[off]) |
        (@as(u32, bytes[off + 1]) << 8) |
        (@as(u32, bytes[off + 2]) << 16) |
        (@as(u32, bytes[off + 3]) << 24);
}

fn readU64Le(bytes: []const u8, off: usize) Error!u64 {
    if (off + 8 > bytes.len) return Error.ImageTooSmall;
    var value: u64 = 0;
    var i: usize = 0;
    while (i < 8) : (i += 1) {
        value |= @as(u64, bytes[off + i]) << @intCast(i * 8);
    }
    return value;
}

fn readI64Le(bytes: []const u8, off: usize) Error!i64 {
    const value = try readU64Le(bytes, off);
    return @bitCast(value);
}

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

pub fn parse(image: []const u8) Error!Image {
    if (image.len < 64) return Error.ImageTooSmall;
    if (!std.mem.eql(u8, image[0..4], &elf_magic)) return Error.BadMagic;
    if (image[4] != elf_class_64) return Error.UnsupportedClass;
    if (image[5] != elf_data_lsb) return Error.UnsupportedEndian;
    if (image[6] != elf_version_current) return Error.BadVersion;

    const e_type = try readU16Le(image, 16);
    if (e_type != elf_type_exec and e_type != elf_type_dyn) return Error.UnsupportedType;

    const e_machine = try readU16Le(image, 18);
    if (e_machine != elf_machine_x86_64) return Error.UnsupportedMachine;

    const e_version = try readU32Le(image, 20);
    if (e_version != elf_version_current) return Error.BadVersion;

    const entry = try readU64Le(image, 24);
    const phoff = try readU64Le(image, 32);
    const phentsize = try readU16Le(image, 54);
    const phnum = try readU16Le(image, 56);
    if (phnum == 0) return Error.NoLoadSegment;
    if (phentsize < elf_phdr_size) return Error.ProgramHeaderOutOfRange;

    var parsed = Image{ .entry = entry };

    var i: u16 = 0;
    while (i < phnum) : (i += 1) {
        const ph_off_u64 = phoff + (@as(u64, i) * @as(u64, phentsize));
        if (ph_off_u64 > std.math.maxInt(usize)) return Error.ProgramHeaderOutOfRange;
        const ph_off: usize = @intCast(ph_off_u64);
        if (ph_off + elf_phdr_size > image.len) return Error.ProgramHeaderOutOfRange;

        const p_type = try readU32Le(image, ph_off + 0);
        const p_flags = try readU32Le(image, ph_off + 4);
        const p_offset = try readU64Le(image, ph_off + 8);
        const p_vaddr = try readU64Le(image, ph_off + 16);
        const p_filesz = try readU64Le(image, ph_off + 32);
        const p_memsz = try readU64Le(image, ph_off + 40);
        const p_align = try readU64Le(image, ph_off + 48);

        if (p_memsz < p_filesz) return Error.SegmentSizeInvalid;
        if (p_offset > std.math.maxInt(usize)) return Error.SegmentOutOfRange;
        if (p_filesz > std.math.maxInt(usize)) return Error.SegmentOutOfRange;

        const seg_off: usize = @intCast(p_offset);
        const seg_filesz: usize = @intCast(p_filesz);
        if (seg_off + seg_filesz > image.len) return Error.SegmentOutOfRange;

        if (p_type == pt_load) {
            if (parsed.load_segment_len >= parsed.load_segments.len) return Error.TooManyLoadSegments;
            parsed.load_segments[parsed.load_segment_len] = .{
                .vaddr = p_vaddr,
                .file_offset = p_offset,
                .file_size = p_filesz,
                .mem_size = p_memsz,
                .flags = p_flags,
                .align_bytes = p_align,
            };
            parsed.load_segment_len += 1;
        } else if (p_type == pt_dynamic) {
            if (parsed.dynamic_segment != null) return Error.TooManyDynamicSegments;
            parsed.dynamic_segment = .{
                .vaddr = p_vaddr,
                .mem_size = p_memsz,
            };
        }
    }

    if (parsed.load_segment_len == 0) return Error.NoLoadSegment;
    return parsed;
}

const probe_pie_elf = blk: {
    var image = [_]u8{0} ** 0x200;

    image[0] = 0x7F;
    image[1] = 0x45;
    image[2] = 0x4C;
    image[3] = 0x46;
    image[4] = elf_class_64;
    image[5] = elf_data_lsb;
    image[6] = elf_version_current;

    writeU16Le(image[0..], 16, elf_type_dyn);
    writeU16Le(image[0..], 18, elf_machine_x86_64);
    writeU32Le(image[0..], 20, elf_version_current);
    writeU64Le(image[0..], 24, demo_idle_entry_offset);
    writeU64Le(image[0..], 32, 64);
    writeU16Le(image[0..], 52, 64);
    writeU16Le(image[0..], 54, elf_phdr_size);
    writeU16Le(image[0..], 56, 2);

    // PT_LOAD: single 4KiB page window (headers + text + dynamic + rela).
    writeU32Le(image[0..], 64 + 0, pt_load);
    writeU32Le(image[0..], 64 + 4, 0x7); // R|W|X
    writeU64Le(image[0..], 64 + 8, 0x0);
    writeU64Le(image[0..], 64 + 16, 0x0);
    writeU64Le(image[0..], 64 + 32, 0x200);
    writeU64Le(image[0..], 64 + 40, 0x1000);
    writeU64Le(image[0..], 64 + 48, 0x1000);

    // PT_DYNAMIC: points inside the mapped PT_LOAD.
    writeU32Le(image[0..], 64 + 56 + 0, pt_dynamic);
    writeU32Le(image[0..], 64 + 56 + 4, 0x6); // R|W
    writeU64Le(image[0..], 64 + 56 + 8, 0x180);
    writeU64Le(image[0..], 64 + 56 + 16, 0x180);
    writeU64Le(image[0..], 64 + 56 + 32, 0x40);
    writeU64Le(image[0..], 64 + 56 + 40, 0x40);
    writeU64Le(image[0..], 64 + 56 + 48, 0x8);

    // Dynamic table entries.
    writeU64Le(image[0..], 0x180 + 0, dt_rela);
    writeU64Le(image[0..], 0x180 + 8, 0x1C0);
    writeU64Le(image[0..], 0x190 + 0, dt_relasz);
    writeU64Le(image[0..], 0x190 + 8, rela_entry_bytes);
    writeU64Le(image[0..], 0x1A0 + 0, dt_relaent);
    writeU64Le(image[0..], 0x1A0 + 8, rela_entry_bytes);
    writeU64Le(image[0..], 0x1B0 + 0, dt_null);
    writeU64Le(image[0..], 0x1B0 + 8, 0);

    // Rela entry: *(base + 0x1F8) = base + 0x100.
    writeU64Le(image[0..], 0x1C0 + 0, demo_idle_reloc_target_offset);
    writeU64Le(image[0..], 0x1C0 + 8, r_x86_64_relative);
    writeU64Le(image[0..], 0x1C0 + 16, demo_idle_entry_offset);

    // pause; jmp -4
    image[0x100] = 0xF3;
    image[0x101] = 0x90;
    image[0x102] = 0xEB;
    image[0x103] = 0xFC;

    break :blk image;
};

pub fn embeddedIdleElf() []const u8 {
    return probe_pie_elf[0..];
}

fn checkedAddU64(a: u64, b: u64) LoadToPageError!u64 {
    const sum, const overflow = @addWithOverflow(a, b);
    if (overflow != 0) return error.AddressOverflow;
    return sum;
}

fn checkedMulU64(a: u64, b: u64) LoadToPageError!u64 {
    const product, const overflow = @mulWithOverflow(a, b);
    if (overflow != 0) return error.AddressOverflow;
    return product;
}

fn checkedAddSigned(base: u64, addend: i64) LoadToPageError!u64 {
    const sum = @as(i128, @intCast(base)) + @as(i128, addend);
    if (sum < 0 or sum > std.math.maxInt(u64)) return error.RelocationOverflow;
    return @intCast(sum);
}

fn checkedOffAndLen(offset_u64: u64, len_u64: u64, max_len: usize) LoadToPageError!struct { off: usize, len: usize } {
    if (offset_u64 > std.math.maxInt(usize)) return error.SegmentTooLarge;
    if (len_u64 > std.math.maxInt(usize)) return error.SegmentTooLarge;
    const off: usize = @intCast(offset_u64);
    const len: usize = @intCast(len_u64);
    if (off > max_len) return error.SegmentOutsideMappedRange;
    if (len > max_len - off) return error.SegmentOutsideMappedRange;
    return .{ .off = off, .len = len };
}

fn applyRelativeRelocations(parsed: Image, load_base_va: u64, mapped_bytes: []u8) LoadToPageError!void {
    const dynamic = parsed.dynamic_segment orelse return;
    const dyn_range = try checkedOffAndLen(dynamic.vaddr, dynamic.mem_size, mapped_bytes.len);
    if ((dynamic.mem_size % 16) != 0) return error.InvalidDynamicTable;

    var rela_vaddr: ?u64 = null;
    var rela_size: u64 = 0;
    var rela_ent: u64 = 0;
    var rela_count: ?u64 = null;

    var dyn_cursor: usize = 0;
    while (dyn_cursor + 16 <= dyn_range.len) : (dyn_cursor += 16) {
        const tag = try readU64Le(mapped_bytes, dyn_range.off + dyn_cursor);
        const value = try readU64Le(mapped_bytes, dyn_range.off + dyn_cursor + 8);
        if (tag == dt_null) break;
        switch (tag) {
            dt_rela => rela_vaddr = value,
            dt_relasz => rela_size = value,
            dt_relaent => rela_ent = value,
            dt_relacount => rela_count = value,
            else => {},
        }
    }

    if (rela_size == 0) return;
    const rela_base_va = rela_vaddr orelse return error.InvalidDynamicTable;
    if (rela_ent == 0) rela_ent = rela_entry_bytes;
    if (rela_ent != rela_entry_bytes) return error.UnsupportedRelaFormat;
    if ((rela_size % rela_ent) != 0) return error.InvalidDynamicTable;

    var total_entries = rela_size / rela_ent;
    if (rela_count) |count| {
        if (count > total_entries) return error.InvalidDynamicTable;
        total_entries = count;
    }

    var idx: u64 = 0;
    while (idx < total_entries) : (idx += 1) {
        const rela_stride = try checkedMulU64(idx, rela_ent);
        const rela_va = try checkedAddU64(rela_base_va, rela_stride);
        const rela_range = try checkedOffAndLen(rela_va, rela_entry_bytes, mapped_bytes.len);

        const r_offset = try readU64Le(mapped_bytes, rela_range.off + 0);
        const r_info = try readU64Le(mapped_bytes, rela_range.off + 8);
        const r_addend = try readI64Le(mapped_bytes, rela_range.off + 16);

        const reloc_type: u32 = @intCast(r_info & 0xFFFF_FFFF);
        const reloc_sym = r_info >> 32;
        if (reloc_type != r_x86_64_relative or reloc_sym != 0) return error.UnsupportedRelocationType;

        if (r_offset > std.math.maxInt(usize)) return error.RelocationOutOfRange;
        const target_off: usize = @intCast(r_offset);
        if (target_off + 8 > mapped_bytes.len) return error.RelocationOutOfRange;

        const relocated = try checkedAddSigned(load_base_va, r_addend);
        writeU64Le(mapped_bytes, target_off, relocated);
    }
}

pub fn loadToSinglePage(image_bytes: []const u8, load_base_va: u64, dest_page: []u8) LoadToPageError!Image {
    if (dest_page.len < 4096) return error.DestinationTooSmall;

    const parsed = try parse(image_bytes);
    var loaded = parsed;
    loaded.entry = try checkedAddU64(load_base_va, parsed.entry);
    @memset(dest_page, 0);

    var entry_in_segment = false;
    var i: usize = 0;
    while (i < parsed.load_segment_len) : (i += 1) {
        const seg = parsed.load_segments[i];

        const runtime_start = try checkedAddU64(load_base_va, seg.vaddr);
        const runtime_end = try checkedAddU64(runtime_start, seg.mem_size);
        const seg_off_u64 = runtime_start - load_base_va;
        if (seg_off_u64 > std.math.maxInt(usize)) return error.SegmentTooLarge;
        if (seg.file_offset > std.math.maxInt(usize)) return error.SegmentTooLarge;
        if (seg.file_size > std.math.maxInt(usize)) return error.SegmentTooLarge;
        if (seg.mem_size > std.math.maxInt(usize)) return error.SegmentTooLarge;

        const seg_off: usize = @intCast(seg_off_u64);
        const file_off: usize = @intCast(seg.file_offset);
        const file_size: usize = @intCast(seg.file_size);
        const mem_size: usize = @intCast(seg.mem_size);
        if (seg_off + mem_size > dest_page.len) return error.SegmentOutsideMappedRange;
        if (file_off + file_size > image_bytes.len) return error.SegmentOutOfRange;

        @memcpy(dest_page[seg_off .. seg_off + file_size], image_bytes[file_off .. file_off + file_size]);

        if (loaded.entry >= runtime_start and loaded.entry < runtime_end) {
            entry_in_segment = true;
        }
    }

    try applyRelativeRelocations(parsed, load_base_va, dest_page);

    if (!entry_in_segment) return error.EntryOutsideMappedRange;
    return loaded;
}

pub fn probe() bool {
    const parsed = parse(probe_pie_elf[0..]) catch return false;
    if (!(parsed.entry == demo_idle_entry_offset and parsed.load_segment_len == 1)) return false;

    var page = [_]u8{0xCC} ** 4096;
    const loaded = loadToSinglePage(probe_pie_elf[0..], demo_idle_base_va, page[0..]) catch return false;
    const relocated_ptr = readU64Le(page[0..], @intCast(demo_idle_reloc_target_offset)) catch return false;
    return loaded.entry == demo_idle_entry_va and
        page[@intCast(demo_idle_entry_offset)] == 0xF3 and
        page[@intCast(demo_idle_entry_offset + 1)] == 0x90 and
        relocated_ptr == demo_idle_entry_va;
}
