const support = @import("abi_root");
const bootfs_format = support.bootfs_format;
const dynamic_linker_bootstrap_abi = support.dynamic_linker_bootstrap_abi;
const process_abi = support.process_abi;
const process_builder_abi = support.process_builder_abi;

const syscall_log: u64 = 0x9;
const syscall_alloc_map_pages: u64 = 0xC;
const syscall_ok: u64 = 0;
const syscall_set_fs_base_self: u64 = process_abi.syscall_set_fs_base_self;
const elf_phdr_bytes: u64 = 56;
const elf_dyn_bytes: u64 = 16;
const page_bytes: u64 = 4096;
const max_phdr_count: u64 = 128;
const max_dynamic_entries: u64 = 256;
const pt_dynamic: u32 = 2;
const pt_tls: u32 = 7;
const pt_gnu_relro: u32 = 0x6474_e552;
const dt_null: i64 = 0;
const dt_needed: i64 = 1;
const dt_pltrelsz: i64 = 2;
const dt_hash: i64 = 4;
const dt_strtab: i64 = 5;
const dt_symtab: i64 = 6;
const dt_rela: i64 = 7;
const dt_relasz: i64 = 8;
const dt_relaent: i64 = 9;
const dt_strsz: i64 = 10;
const dt_syment: i64 = 11;
const dt_init: i64 = 12;
const dt_fini: i64 = 13;
const dt_soname: i64 = 14;
const dt_rpath: i64 = 15;
const dt_init_array: i64 = 25;
const dt_fini_array: i64 = 26;
const dt_init_arraysz: i64 = 27;
const dt_fini_arraysz: i64 = 28;
const dt_runpath: i64 = 29;
const dt_flags: i64 = 30;
const dt_preinit_array: i64 = 32;
const dt_preinit_arraysz: i64 = 33;
const dt_pltrel: i64 = 20;
const dt_jmprel: i64 = 23;
const dt_flags_1: i64 = 0x6fff_fffb;
const dt_gnu_hash: i64 = 0x6fff_fef5;
const dt_relacount: i64 = 0x6fff_fff9;
const dt_verdef: i64 = 0x6fff_fffc;
const dt_verdefnum: i64 = 0x6fff_fffd;
const dt_verneed: i64 = 0x6fff_fffe;
const dt_verneednum: i64 = 0x6fff_ffff;
const dt_versym: i64 = 0x6fff_fff0;
const df_bind_now: u64 = 0x8;
const df_1_now: u64 = 0x1;
const elf_rela_bytes: u64 = 24;
const elf_sym_bytes: u64 = 24;
const r_x86_64_none: u64 = 0;
const r_x86_64_64: u64 = 1;
const r_x86_64_copy: u64 = 5;
const r_x86_64_glob_dat: u64 = 6;
const r_x86_64_jump_slot: u64 = 7;
const r_x86_64_relative: u64 = 8;
const r_x86_64_dtpmod64: u64 = 16;
const r_x86_64_dtpoff64: u64 = 17;
const r_x86_64_tpoff64: u64 = 18;
const r_x86_64_tlsdesc: u64 = 36;
const r_x86_64_irelative: u64 = 37;
const st_bind_weak: u8 = 2;
const max_needed_name_bytes: usize = 192;
const max_needed_path_bytes: usize = 256;
const max_symbol_count_without_hash: u32 = 64;
const max_symbol_count_with_gnu_hash: u32 = 16384;
const static_tls_bytes: usize = 64 * 1024;
const static_tls_align: u64 = 16;
const static_tls_tcb_bytes: u64 = page_bytes;
const static_tls_target_va: u64 = 0x2F00_0000;
const max_tls_modules: usize = dynamic_linker_bootstrap_abi.max_loaded_libs + 1;

const ProgramHeader = struct {
    p_type: u32,
    offset: u64,
    vaddr: u64,
    filesz: u64,
    memsz: u64,
    align_bytes: u64,
};

const DynamicInfo = struct {
    load_bias: u64 = 0,
    hash: u64 = 0,
    gnu_hash: u64 = 0,
    strtab: u64 = 0,
    strsz: u64 = 0,
    symtab: u64 = 0,
    syment: u64 = 0,
    rela: u64 = 0,
    relasz: u64 = 0,
    relaent: u64 = 0,
    relacount: u64 = 0,
    jmprel: u64 = 0,
    pltrelsz: u64 = 0,
    pltrel: u64 = 0,
    init: u64 = 0,
    fini: u64 = 0,
    preinit_array: u64 = 0,
    preinit_arraysz: u64 = 0,
    init_array: u64 = 0,
    init_arraysz: u64 = 0,
    fini_array: u64 = 0,
    fini_arraysz: u64 = 0,
    soname: u64 = 0,
    rpath: u64 = 0,
    runpath: u64 = 0,
    flags: u64 = 0,
    flags_1: u64 = 0,
    verdef: u64 = 0,
    verdefnum: u64 = 0,
    verneed: u64 = 0,
    verneednum: u64 = 0,
    versym: u64 = 0,
    needed: [8]u64 = [_]u64{0} ** 8,
    needed_count: usize = 0,
};

const TlsModule = struct {
    load_bias: u64 = 0,
    image_va: u64 = 0,
    filesz: u64 = 0,
    memsz: u64 = 0,
    align_bytes: u64 = 1,
    static_base: u64 = 0,
    static_tp_offset: u64 = 0,
};

const TlsState = struct {
    modules: [max_tls_modules]TlsModule = [_]TlsModule{.{}} ** max_tls_modules,
    module_count: usize = 0,
    thread_pointer: u64 = 0,
};

fn syscall3(nr: u64, arg0: u64, arg1: u64, arg2: u64) u64 {
    return asm volatile (
        \\push %rdi
        \\push %rsi
        \\int $0x80
        \\pop %rsi
        \\pop %rdi
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (nr),
          [arg0] "{rdi}" (arg0),
          [arg1] "{rsi}" (arg1),
          [arg2] "{rdx}" (arg2),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn syscall4(nr: u64, arg0: u64, arg1: u64, arg2: u64, arg3: u64) u64 {
    return asm volatile (
        \\push %rdi
        \\push %rsi
        \\int $0x80
        \\pop %rsi
        \\pop %rdi
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (nr),
          [arg0] "{rdi}" (arg0),
          [arg1] "{rsi}" (arg1),
          [arg2] "{rdx}" (arg2),
          [arg3] "{rcx}" (arg3),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

pub export fn __cap_tlsdesc_static() callconv(.naked) noreturn {
    asm volatile (
        \\movq 8(%rax), %rax
        \\retq
    );
}

fn userLog(message: []const u8) void {
    _ = syscall3(syscall_log, @intFromPtr(message.ptr), message.len, 0);
}

fn readU32(addr: u64) u32 {
    const ptr: *const u32 = @ptrFromInt(addr);
    return ptr.*;
}

fn readU16(addr: u64) u16 {
    const ptr: *const u16 = @ptrFromInt(addr);
    return ptr.*;
}

fn readU64(addr: u64) u64 {
    const ptr: *const u64 = @ptrFromInt(addr);
    return ptr.*;
}

fn readI64(addr: u64) i64 {
    return @bitCast(readU64(addr));
}

fn parsePhdr(addr: u64) ProgramHeader {
    return .{
        .p_type = readU32(addr + 0),
        .offset = readU64(addr + 8),
        .vaddr = readU64(addr + 16),
        .filesz = readU64(addr + 32),
        .memsz = readU64(addr + 40),
        .align_bytes = readU64(addr + 48),
    };
}

fn runtimeVa(load_bias: u64, image_vaddr: u64) ?u64 {
    const value, const overflow = @addWithOverflow(load_bias, image_vaddr);
    return if (overflow == 0) value else null;
}

fn findDynamicPhdr(cfg: *const volatile dynamic_linker_bootstrap_abi.Config) ?ProgramHeader {
    if (cfg.main_phdr == 0 or cfg.main_phnum == 0 or cfg.main_phnum > max_phdr_count) return null;
    if (cfg.main_phent < elf_phdr_bytes) return null;

    var index: u64 = 0;
    while (index < cfg.main_phnum) : (index += 1) {
        const phdr_addr = cfg.main_phdr + index * cfg.main_phent;
        const phdr = parsePhdr(phdr_addr);
        if (phdr.p_type == pt_dynamic) return phdr;
    }
    return null;
}

fn findMainTlsPhdr(cfg: *const volatile dynamic_linker_bootstrap_abi.Config) ?ProgramHeader {
    if (cfg.main_phdr == 0 or cfg.main_phnum == 0 or cfg.main_phnum > max_phdr_count) return null;
    if (cfg.main_phent < elf_phdr_bytes) return null;

    var index: u64 = 0;
    while (index < cfg.main_phnum) : (index += 1) {
        const phdr_addr = cfg.main_phdr + index * cfg.main_phent;
        const phdr = parsePhdr(phdr_addr);
        if (phdr.p_type == pt_tls) return phdr;
    }
    return null;
}

fn findMainRelroPhdr(cfg: *const volatile dynamic_linker_bootstrap_abi.Config) ?ProgramHeader {
    if (cfg.main_phdr == 0 or cfg.main_phnum == 0 or cfg.main_phnum > max_phdr_count) return null;
    if (cfg.main_phent < elf_phdr_bytes) return null;

    var index: u64 = 0;
    while (index < cfg.main_phnum) : (index += 1) {
        const phdr_addr = cfg.main_phdr + index * cfg.main_phent;
        const phdr = parsePhdr(phdr_addr);
        if (phdr.p_type == pt_gnu_relro) return phdr;
    }
    return null;
}

fn parseDynamic(cfg: *const volatile dynamic_linker_bootstrap_abi.Config, dynamic_phdr: ProgramHeader) ?DynamicInfo {
    if (dynamic_phdr.memsz < dynamic_phdr.filesz or dynamic_phdr.filesz < elf_dyn_bytes) return null;
    const dynamic_addr = runtimeVa(cfg.main_load_bias, dynamic_phdr.vaddr) orelse return null;
    return parseDynamicAt(cfg.main_load_bias, dynamic_addr, dynamic_phdr.filesz);
}

fn parseDynamicAt(load_bias: u64, dynamic_addr: u64, dynamic_filesz: u64) ?DynamicInfo {
    if (dynamic_addr == 0 or dynamic_filesz < elf_dyn_bytes) return null;
    var info = DynamicInfo{ .load_bias = load_bias };
    var offset: u64 = 0;
    var entry_count: u64 = 0;
    while (offset + elf_dyn_bytes <= dynamic_filesz and entry_count < max_dynamic_entries) : ({
        offset += elf_dyn_bytes;
        entry_count += 1;
    }) {
        const tag = readI64(dynamic_addr + offset);
        const value = readU64(dynamic_addr + offset + 8);
        if (tag == dt_null) break;
        switch (tag) {
            dt_needed => {
                if (info.needed_count < info.needed.len) {
                    info.needed[info.needed_count] = value;
                    info.needed_count += 1;
                }
            },
            dt_hash => info.hash = runtimeVa(load_bias, value) orelse return null,
            dt_gnu_hash => info.gnu_hash = runtimeVa(load_bias, value) orelse return null,
            dt_strtab => info.strtab = runtimeVa(load_bias, value) orelse return null,
            dt_strsz => info.strsz = value,
            dt_symtab => info.symtab = runtimeVa(load_bias, value) orelse return null,
            dt_syment => info.syment = value,
            dt_soname => info.soname = value,
            dt_rpath => info.rpath = value,
            dt_runpath => info.runpath = value,
            dt_rela => info.rela = runtimeVa(load_bias, value) orelse return null,
            dt_relasz => info.relasz = value,
            dt_relaent => info.relaent = value,
            dt_relacount => info.relacount = value,
            dt_jmprel => info.jmprel = runtimeVa(load_bias, value) orelse return null,
            dt_pltrelsz => info.pltrelsz = value,
            dt_pltrel => info.pltrel = value,
            dt_flags => info.flags = value,
            dt_flags_1 => info.flags_1 = value,
            dt_verdef => info.verdef = runtimeVa(load_bias, value) orelse return null,
            dt_verdefnum => info.verdefnum = value,
            dt_verneed => info.verneed = runtimeVa(load_bias, value) orelse return null,
            dt_verneednum => info.verneednum = value,
            dt_versym => info.versym = runtimeVa(load_bias, value) orelse return null,
            dt_init => info.init = runtimeVa(load_bias, value) orelse return null,
            dt_fini => info.fini = runtimeVa(load_bias, value) orelse return null,
            dt_preinit_array => info.preinit_array = runtimeVa(load_bias, value) orelse return null,
            dt_preinit_arraysz => info.preinit_arraysz = value,
            dt_init_array => info.init_array = runtimeVa(load_bias, value) orelse return null,
            dt_init_arraysz => info.init_arraysz = value,
            dt_fini_array => info.fini_array = runtimeVa(load_bias, value) orelse return null,
            dt_fini_arraysz => info.fini_arraysz = value,
            else => {},
        }
    }
    return info;
}

fn gnuHashSymbolCount(gnu_hash: u64) ?u32 {
    const nbuckets = readU32(gnu_hash + 0);
    const symoffset = readU32(gnu_hash + 4);
    const bloom_size = readU32(gnu_hash + 8);
    if (nbuckets == 0) return symoffset;

    const buckets = gnu_hash + 16 + @as(u64, bloom_size) * 8;
    const chains = buckets + @as(u64, nbuckets) * 4;
    var max_symbol = symoffset;
    var bucket_index: u32 = 0;
    while (bucket_index < nbuckets) : (bucket_index += 1) {
        const bucket = readU32(buckets + @as(u64, bucket_index) * 4);
        if (bucket == 0 or bucket < symoffset) continue;

        var symbol_index = bucket;
        var chain_addr = chains + @as(u64, symbol_index - symoffset) * 4;
        while (symbol_index < max_symbol_count_with_gnu_hash) : ({
            symbol_index += 1;
            chain_addr += 4;
        }) {
            const hash = readU32(chain_addr);
            if (symbol_index > max_symbol) max_symbol = symbol_index;
            if ((hash & 1) != 0) break;
        }
    }
    return max_symbol + 1;
}

fn symbolCount(info: DynamicInfo) u32 {
    if (info.hash != 0) return readU32(info.hash + 4);
    if (info.gnu_hash != 0) return gnuHashSymbolCount(info.gnu_hash) orelse max_symbol_count_without_hash;
    return max_symbol_count_without_hash;
}

fn symbolName(info: DynamicInfo, symbol_index: u64) ?[]const u8 {
    if (info.symtab == 0 or info.strtab == 0 or info.syment < elf_sym_bytes) return null;
    if (symbol_index >= symbolCount(info)) return null;
    const sym_addr = info.symtab + symbol_index * info.syment;
    return dynString(info.strtab, info.strsz, readU32(sym_addr));
}

fn symbolBinding(info: DynamicInfo, symbol_index: u64) ?u8 {
    if (info.symtab == 0 or info.syment < elf_sym_bytes) return null;
    if (symbol_index >= symbolCount(info)) return null;
    const sym_addr = info.symtab + symbol_index * info.syment;
    const info_byte: *const u8 = @ptrFromInt(sym_addr + 4);
    return info_byte.* >> 4;
}

fn symbolIsUndefined(info: DynamicInfo, symbol_index: u64) bool {
    if (info.symtab == 0 or info.syment < elf_sym_bytes) return false;
    if (symbol_index >= symbolCount(info)) return false;
    const sym_addr = info.symtab + symbol_index * info.syment;
    return readU16(sym_addr + 6) == 0;
}

fn symbolIsUndefinedWeak(info: DynamicInfo, symbol_index: u64) bool {
    return symbolIsUndefined(info, symbol_index) and (symbolBinding(info, symbol_index) orelse 0) == st_bind_weak;
}

fn symbolRuntimeValue(info: DynamicInfo, symbol_index: u64) ?u64 {
    if (info.symtab == 0 or info.syment < elf_sym_bytes) return null;
    if (symbol_index >= symbolCount(info)) return null;
    const sym_addr = info.symtab + symbol_index * info.syment;
    const shndx = blk: {
        const ptr: *const u16 = @ptrFromInt(sym_addr + 6);
        break :blk ptr.*;
    };
    if (shndx == 0) return null;
    return runtimeVa(info.load_bias, readU64(sym_addr + 8));
}

fn symbolSize(info: DynamicInfo, symbol_index: u64) ?u64 {
    if (info.symtab == 0 or info.syment < elf_sym_bytes) return null;
    if (symbol_index >= symbolCount(info)) return null;
    const sym_addr = info.symtab + symbol_index * info.syment;
    return readU64(sym_addr + 16);
}

fn gnuHashName(name: []const u8) u32 {
    var hash: u32 = 5381;
    var index: usize = 0;
    while (index < name.len) : (index += 1) {
        hash = hash *% 33 +% name[index];
    }
    return hash;
}

fn symbolVersionIndex(info: DynamicInfo, symbol_index: u64) ?u16 {
    if (info.versym == 0) return null;
    if (symbol_index >= symbolCount(info)) return null;
    return readU16(info.versym + symbol_index * 2) & 0x7fff;
}

fn verneedNameForIndex(info: DynamicInfo, version_index: u16) ?[]const u8 {
    if (info.verneed == 0 or info.verneednum == 0 or info.strtab == 0 or info.strsz == 0) return null;
    var need_addr = info.verneed;
    var need_index: u64 = 0;
    while (need_index < info.verneednum) : (need_index += 1) {
        const aux_count = readU16(need_addr + 2);
        var aux_addr = need_addr + readU32(need_addr + 8);
        var aux_index: u16 = 0;
        while (aux_index < aux_count) : (aux_index += 1) {
            const other = readU16(aux_addr + 6) & 0x7fff;
            const name_off = readU32(aux_addr + 8);
            if (other == version_index) return dynString(info.strtab, info.strsz, name_off);
            const next = readU32(aux_addr + 12);
            if (next == 0) break;
            aux_addr += next;
        }
        const next = readU32(need_addr + 12);
        if (next == 0) break;
        need_addr += next;
    }
    return null;
}

fn verdefNameForIndex(info: DynamicInfo, version_index: u16) ?[]const u8 {
    if (info.verdef == 0 or info.verdefnum == 0 or info.strtab == 0 or info.strsz == 0) return null;
    var def_addr = info.verdef;
    var def_index: u64 = 0;
    while (def_index < info.verdefnum) : (def_index += 1) {
        const ndx = readU16(def_addr + 4) & 0x7fff;
        if (ndx == version_index) {
            const aux_addr = def_addr + readU32(def_addr + 12);
            const name_off = readU32(aux_addr + 0);
            return dynString(info.strtab, info.strsz, name_off);
        }
        const next = readU32(def_addr + 16);
        if (next == 0) break;
        def_addr += next;
    }
    return null;
}

fn requiredVersionName(info: DynamicInfo, symbol_index: u64) ?[]const u8 {
    const version_index = symbolVersionIndex(info, symbol_index) orelse return null;
    if (version_index <= 1) return null;
    return verneedNameForIndex(info, version_index) orelse verdefNameForIndex(info, version_index);
}

fn symbolMatchesRequiredVersion(info: DynamicInfo, symbol_index: u64, required_version: ?[]const u8) bool {
    const required = required_version orelse return true;
    const version_index = symbolVersionIndex(info, symbol_index) orelse return false;
    if (version_index <= 1) return false;
    const provided = verdefNameForIndex(info, version_index) orelse return false;
    return sameBytes(provided, required);
}

fn findSymbolIndexInGnuHash(info: DynamicInfo, name: []const u8, required_version: ?[]const u8) ?u64 {
    if (info.gnu_hash == 0 or info.symtab == 0 or info.strtab == 0 or info.syment < elf_sym_bytes) return null;

    const nbuckets = readU32(info.gnu_hash + 0);
    const symoffset = readU32(info.gnu_hash + 4);
    const bloom_size = readU32(info.gnu_hash + 8);
    const bloom_shift = readU32(info.gnu_hash + 12);
    if (nbuckets == 0 or bloom_size == 0) return null;

    const hash = gnuHashName(name);
    const bloom = info.gnu_hash + 16;
    const bloom_word = readU64(bloom + @as(u64, (hash / 64) % bloom_size) * 8);
    const mask = (@as(u64, 1) << @intCast(hash % 64)) | (@as(u64, 1) << @intCast((hash >> @intCast(bloom_shift)) % 64));
    if ((bloom_word & mask) != mask) return null;

    const buckets = bloom + @as(u64, bloom_size) * 8;
    const chains = buckets + @as(u64, nbuckets) * 4;
    var symbol_index = readU32(buckets + @as(u64, hash % nbuckets) * 4);
    if (symbol_index < symoffset) return null;

    var chain_addr = chains + @as(u64, symbol_index - symoffset) * 4;
    while (symbol_index < max_symbol_count_with_gnu_hash) : ({
        symbol_index += 1;
        chain_addr += 4;
    }) {
        const chain_hash = readU32(chain_addr);
        if ((chain_hash | 1) == (hash | 1)) {
            const candidate = symbolName(info, symbol_index) orelse return null;
            if (sameBytes(candidate, name) and symbolMatchesRequiredVersion(info, symbol_index, required_version)) return symbol_index;
        }
        if ((chain_hash & 1) != 0) break;
    }
    return null;
}

fn findSymbolInInfo(info: DynamicInfo, name: []const u8, required_version: ?[]const u8) ?u64 {
    if (info.gnu_hash != 0) {
        const index = findSymbolIndexInGnuHash(info, name, required_version) orelse return null;
        return symbolRuntimeValue(info, index);
    }
    const count = symbolCount(info);
    var index: u32 = 1;
    while (index < count) : (index += 1) {
        const candidate = symbolName(info, index) orelse continue;
        if (!sameBytes(candidate, name)) continue;
        if (!symbolMatchesRequiredVersion(info, index, required_version)) continue;
        return symbolRuntimeValue(info, index);
    }
    return null;
}

fn findSymbolSizeInInfo(info: DynamicInfo, name: []const u8, required_version: ?[]const u8) ?u64 {
    if (info.gnu_hash != 0) {
        const index = findSymbolIndexInGnuHash(info, name, required_version) orelse return null;
        return symbolSize(info, index);
    }
    const count = symbolCount(info);
    var index: u32 = 1;
    while (index < count) : (index += 1) {
        const candidate = symbolName(info, index) orelse continue;
        if (!sameBytes(candidate, name)) continue;
        if (!symbolMatchesRequiredVersion(info, index, required_version)) continue;
        return symbolSize(info, index);
    }
    return null;
}

fn findSymbolIndexLinear(info: DynamicInfo, name: []const u8, required_version: ?[]const u8) ?u64 {
    const count = symbolCount(info);
    var index: u32 = 1;
    while (index < count) : (index += 1) {
        const candidate = symbolName(info, index) orelse continue;
        if (!sameBytes(candidate, name)) continue;
        if (!symbolMatchesRequiredVersion(info, index, required_version)) continue;
        return index;
    }
    return null;
}

fn findSymbolIndexInInfo(info: DynamicInfo, name: []const u8, required_version: ?[]const u8) ?u64 {
    if (info.gnu_hash != 0) return findSymbolIndexInGnuHash(info, name, required_version);
    return findSymbolIndexLinear(info, name, required_version);
}

fn symbolRawValue(info: DynamicInfo, symbol_index: u64) ?u64 {
    if (info.symtab == 0 or info.syment < elf_sym_bytes) return null;
    if (symbol_index >= symbolCount(info)) return null;
    const sym_addr = info.symtab + symbol_index * info.syment;
    const shndx = readU16(sym_addr + 6);
    if (shndx == 0) return null;
    return readU64(sym_addr + 8);
}

fn moduleIdForLoadBias(cfg: *const volatile dynamic_linker_bootstrap_abi.Config, load_bias: u64) ?u64 {
    if (load_bias == cfg.main_load_bias) return 1;
    var index: usize = 0;
    while (index < cfg.loaded_lib_count and index < dynamic_linker_bootstrap_abi.max_loaded_libs) : (index += 1) {
        if (cfg.loaded_libs[index].load_bias == load_bias) return @intCast(index + 2);
    }
    return null;
}

const ResolvedTlsSymbol = struct {
    module_id: u64,
    load_bias: u64,
    offset: u64,
};

fn resolveTlsSymbolInInfo(
    cfg: *const volatile dynamic_linker_bootstrap_abi.Config,
    info: DynamicInfo,
    name: []const u8,
    required_version: ?[]const u8,
) ?ResolvedTlsSymbol {
    const index = findSymbolIndexInInfo(info, name, required_version) orelse return null;
    const module_id = moduleIdForLoadBias(cfg, info.load_bias) orelse return null;
    const offset = symbolRawValue(info, index) orelse return null;
    return .{ .module_id = module_id, .load_bias = info.load_bias, .offset = offset };
}

fn resolveTlsSymbol(
    cfg: *const volatile dynamic_linker_bootstrap_abi.Config,
    main_info: DynamicInfo,
    self_info: DynamicInfo,
    name: []const u8,
    required_version: ?[]const u8,
) ?ResolvedTlsSymbol {
    if (resolveTlsSymbolInInfo(cfg, self_info, name, required_version)) |symbol| return symbol;
    if (resolveTlsSymbolInInfo(cfg, main_info, name, required_version)) |symbol| return symbol;
    var index: usize = 0;
    while (index < cfg.loaded_lib_count and index < dynamic_linker_bootstrap_abi.max_loaded_libs) : (index += 1) {
        const lib = cfg.loaded_libs[index];
        if (lib.dynamic_va == 0 or lib.dynamic_filesz == 0) continue;
        const info = parseDynamicAt(lib.load_bias, lib.dynamic_va, lib.dynamic_filesz) orelse continue;
        if (resolveTlsSymbolInInfo(cfg, info, name, required_version)) |symbol| return symbol;
    }
    return null;
}

fn addSigned(base: u64, addend: i64) ?u64 {
    if (addend >= 0) {
        const value, const overflow = @addWithOverflow(base, @as(u64, @intCast(addend)));
        return if (overflow == 0) value else null;
    }
    if (addend == -9223372036854775808) return null;
    const magnitude: u64 = @intCast(-addend);
    if (base < magnitude) return null;
    return base - magnitude;
}

fn writeU64(addr: u64, value: u64) void {
    const ptr: *u64 = @ptrFromInt(addr);
    ptr.* = value;
}

const ResolvedSymbol = struct {
    value: u64,
    size: u64,
};

fn copyMemory(dest_va: u64, src_va: u64, byte_count: u64) bool {
    var index: u64 = 0;
    while (index < byte_count) : (index += 1) {
        const byte: *const u8 = @ptrFromInt(src_va + index);
        const dst: *u8 = @ptrFromInt(dest_va + index);
        dst.* = byte.*;
    }
    return true;
}

fn zeroMemory(dest_va: u64, byte_count: u64) void {
    var index: u64 = 0;
    while (index < byte_count) : (index += 1) {
        const dst: *u8 = @ptrFromInt(dest_va + index);
        dst.* = 0;
    }
}

fn alignUp(value: u64, align_bytes: u64) ?u64 {
    const alignment = if (align_bytes == 0) 1 else align_bytes;
    const remainder = value % alignment;
    if (remainder == 0) return value;
    const delta = alignment - remainder;
    const rounded, const overflow = @addWithOverflow(value, delta);
    return if (overflow == 0) rounded else null;
}

fn addSignedWrap(base: u64, addend: i64) u64 {
    if (addend >= 0) return base +% @as(u64, @intCast(addend));
    if (addend == -9223372036854775808) return base -% (@as(u64, 1) << 63);
    return base -% @as(u64, @intCast(-addend));
}

fn staticTlsBaseForLoadBias(tls: *const TlsState, load_bias: u64) ?u64 {
    var index: usize = 0;
    while (index < tls.module_count) : (index += 1) {
        if (tls.modules[index].load_bias == load_bias and tls.modules[index].static_base != 0) {
            return tls.modules[index].static_base;
        }
    }
    return null;
}

fn staticTlsTpOffset(tls: *const TlsState, load_bias: u64, symbol_offset: u64) ?u64 {
    if (tls.thread_pointer == 0) return null;
    const base = staticTlsBaseForLoadBias(tls, load_bias) orelse return null;
    const symbol_va, const overflow = @addWithOverflow(base, symbol_offset);
    if (overflow != 0) return null;
    return symbol_va -% tls.thread_pointer;
}

fn logStaticTlsLayout(tls: *const TlsState) void {
    var index: usize = 0;
    while (index < tls.module_count) : (index += 1) {
        userLog("ld: TLS module[");
        logHex(@intCast(index + 1));
        userLog("] base=");
        logHex(tls.modules[index].static_base);
        userLog(" tp_offset=");
        logHex(tls.modules[index].static_tp_offset);
        userLog(" memsz=");
        logHex(tls.modules[index].memsz);
        userLog(" align=");
        logHex(tls.modules[index].align_bytes);
        userLog("\n");
    }
}

fn resolveSymbolWithSize(
    cfg: *const volatile dynamic_linker_bootstrap_abi.Config,
    main_info: DynamicInfo,
    self_info: DynamicInfo,
    name: []const u8,
    required_version: ?[]const u8,
) ?ResolvedSymbol {
    if (self_info.symtab != 0) {
        if (findSymbolInInfo(self_info, name, required_version)) |value| {
            if (findSymbolSizeInInfo(self_info, name, required_version)) |size| return .{ .value = value, .size = size };
        }
    }
    if (main_info.symtab != 0) {
        if (findSymbolInInfo(main_info, name, required_version)) |value| {
            if (findSymbolSizeInInfo(main_info, name, required_version)) |size| return .{ .value = value, .size = size };
        }
    }
    var index: usize = 0;
    while (index < cfg.loaded_lib_count and index < dynamic_linker_bootstrap_abi.max_loaded_libs) : (index += 1) {
        const lib = cfg.loaded_libs[index];
        if (lib.dynamic_va == 0 or lib.dynamic_filesz == 0) continue;
        const info = parseDynamicAt(lib.load_bias, lib.dynamic_va, lib.dynamic_filesz) orelse continue;
        if (findSymbolInInfo(info, name, required_version)) |value| {
            if (findSymbolSizeInInfo(info, name, required_version)) |size| {
                return .{ .value = value, .size = size };
            }
        }
    }
    return null;
}

fn applyRelocations(
    cfg: *const volatile dynamic_linker_bootstrap_abi.Config,
    tls: *const TlsState,
    info: DynamicInfo,
    load_bias: u64,
    main_info: DynamicInfo,
) bool {
    if (info.rela != 0 and info.relasz != 0) {
        if ((info.relasz % elf_rela_bytes) != 0 or info.relaent != elf_rela_bytes) {
            userLog("ld: unsupported RELA format\n");
            return false;
        }
        const rela_count = info.relasz / elf_rela_bytes;
        const fast_relative_count = if (info.relacount > rela_count) rela_count else info.relacount;
        if (fast_relative_count != 0) {
            if (!applyRelaCountEntries(load_bias, info.rela, fast_relative_count)) return false;
        }
        const remaining_count = rela_count - fast_relative_count;
        if (remaining_count != 0) {
            const remaining_base = info.rela + fast_relative_count * elf_rela_bytes;
            const remaining_size = remaining_count * elf_rela_bytes;
            if (!applyRelaEntries(cfg, tls, info, load_bias, main_info, remaining_base, remaining_size, false, "DT_RELA")) return false;
        }
    }
    if (info.jmprel != 0 and info.pltrelsz != 0) {
        // Lazy binding is intentionally unsupported. CapabilityOS resolves every
        // PLT relocation before initializers so PT_GNU_RELRO can be sealed early.
        if (info.pltrel != dt_rela) {
            userLog("ld: unsupported PLT relocation format\n");
            return false;
        }
        if (!applyRelaEntries(cfg, tls, info, load_bias, main_info, info.jmprel, info.pltrelsz, false, "DT_JMPREL")) return false;
    }
    return true;
}

fn applyRelaCountEntries(
    load_bias: u64,
    rela_base: u64,
    count: u64,
) bool {
    var index: u64 = 0;
    while (index < count) : (index += 1) {
        const rela = rela_base + index * elf_rela_bytes;
        const r_offset = readU64(rela + 0);
        const r_info = readU64(rela + 8);
        const r_addend = readI64(rela + 16);
        const r_type = r_info & 0xffff_ffff;
        const r_sym = r_info >> 32;
        if (r_type != r_x86_64_relative or r_sym != 0) {
            userLog("ld: bad DT_RELACOUNT entry\n");
            return false;
        }
        const dest_va = runtimeVa(load_bias, r_offset) orelse return false;
        const relocated = addSigned(load_bias, r_addend) orelse return false;
        writeU64(dest_va, relocated);
    }
    userLog("ld: applied DT_RELACOUNT count=");
    logHex(count);
    userLog("\n");
    return true;
}

fn applyRelaEntries(
    cfg: *const volatile dynamic_linker_bootstrap_abi.Config,
    tls: *const TlsState,
    info: DynamicInfo,
    load_bias: u64,
    main_info: DynamicInfo,
    rela_base: u64,
    relasz: u64,
    require_relaent: bool,
    label: []const u8,
) bool {
    if ((require_relaent and info.relaent != elf_rela_bytes) or (relasz % elf_rela_bytes) != 0) {
        userLog("ld: unsupported RELA format\n");
        return false;
    }

    var offset: u64 = 0;
    var applied: u64 = 0;
    while (offset < relasz) : (offset += elf_rela_bytes) {
        const rela = rela_base + offset;
        const r_offset = readU64(rela + 0);
        const r_info = readU64(rela + 8);
        const r_addend = readI64(rela + 16);
        const r_type = r_info & 0xffff_ffff;
        const r_sym = r_info >> 32;
        const dest_va = runtimeVa(load_bias, r_offset) orelse return false;

        if (r_type == r_x86_64_none) {
            continue;
        }

        if (r_type == r_x86_64_relative) {
            if (r_sym != 0) {
                userLog("ld: relative relocation has symbol\n");
                return false;
            }
            const relocated = addSigned(load_bias, r_addend) orelse return false;
            writeU64(dest_va, relocated);
            applied += 1;
            continue;
        }

        if (r_type == r_x86_64_irelative) {
            if (r_sym != 0) {
                userLog("ld: IRELATIVE relocation has symbol\n");
                return false;
            }
            const resolver_va = addSigned(load_bias, r_addend) orelse return false;
            const resolver: *const fn () callconv(.c) u64 = @ptrFromInt(resolver_va);
            writeU64(dest_va, resolver());
            applied += 1;
            continue;
        }

        if (r_type == r_x86_64_glob_dat or r_type == r_x86_64_jump_slot or r_type == r_x86_64_64 or r_type == r_x86_64_copy) {
            const name = symbolName(info, r_sym) orelse {
                userLog("ld: relocation symbol invalid\n");
                return false;
            };
            const required_version = requiredVersionName(info, r_sym);
            const value = resolveSymbolWithSize(cfg, main_info, info, name, required_version) orelse {
                if (symbolIsUndefinedWeak(info, r_sym)) {
                    userLog("ld: unresolved weak symbol ");
                    userLog(name);
                    userLog(" -> 0\n");
                    if (r_type != r_x86_64_copy) {
                        const relocated = addSigned(0, r_addend) orelse return false;
                        writeU64(dest_va, relocated);
                    }
                    applied += 1;
                    continue;
                }
                userLog("ld: unresolved symbol ");
                userLog(name);
                if (required_version) |version| {
                    userLog("@");
                    userLog(version);
                }
                userLog("\n");
                return false;
            };
            if (r_type == r_x86_64_copy) {
                const source_va = addSigned(value.value, r_addend) orelse return false;
                if (value.size == 0) {
                    userLog("ld: R_X86_64_COPY size=0 for ");
                    userLog(name);
                    userLog("\n");
                    return false;
                }
                if (!copyMemory(dest_va, source_va, value.size)) return false;
            } else {
                const relocated = addSigned(value.value, r_addend) orelse return false;
                writeU64(dest_va, relocated);
            }
            applied += 1;
            continue;
        }

        if (r_type == r_x86_64_dtpmod64 or r_type == r_x86_64_dtpoff64) {
            var module_id = moduleIdForLoadBias(cfg, load_bias) orelse return false;
            var tls_offset: u64 = 0;
            if (r_sym != 0) {
                const name = symbolName(info, r_sym) orelse {
                    userLog("ld: TLS relocation symbol invalid\n");
                    return false;
                };
                const required_version = requiredVersionName(info, r_sym);
                const symbol = resolveTlsSymbol(cfg, main_info, info, name, required_version) orelse {
                    userLog("ld: unresolved TLS symbol ");
                    userLog(name);
                    if (required_version) |version| {
                        userLog("@");
                        userLog(version);
                    }
                    userLog("\n");
                    return false;
                };
                module_id = symbol.module_id;
                tls_offset = symbol.offset;
            }
            const value = if (r_type == r_x86_64_dtpmod64) module_id else addSigned(tls_offset, r_addend) orelse return false;
            writeU64(dest_va, value);
            applied += 1;
            continue;
        }

        if (r_type == r_x86_64_tpoff64) {
            var target_load_bias = load_bias;
            var tls_offset: u64 = 0;
            if (r_sym != 0) {
                const name = symbolName(info, r_sym) orelse {
                    userLog("ld: static TLS relocation symbol invalid\n");
                    return false;
                };
                const required_version = requiredVersionName(info, r_sym);
                const symbol = resolveTlsSymbol(cfg, main_info, info, name, required_version) orelse {
                    userLog("ld: unresolved static TLS symbol ");
                    userLog(name);
                    if (required_version) |version| {
                        userLog("@");
                        userLog(version);
                    }
                    userLog("\n");
                    return false;
                };
                target_load_bias = symbol.load_bias;
                tls_offset = symbol.offset;
            }
            const tp_offset = staticTlsTpOffset(tls, target_load_bias, tls_offset) orelse {
                userLog("ld: static TLS block unavailable\n");
                return false;
            };
            writeU64(dest_va, addSignedWrap(tp_offset, r_addend));
            applied += 1;
            continue;
        }

        if (r_type == r_x86_64_tlsdesc) {
            var target_load_bias = load_bias;
            var tls_offset: u64 = 0;
            if (r_sym != 0) {
                const name = symbolName(info, r_sym) orelse {
                    userLog("ld: TLSDESC relocation symbol invalid\n");
                    return false;
                };
                const required_version = requiredVersionName(info, r_sym);
                const symbol = resolveTlsSymbol(cfg, main_info, info, name, required_version) orelse {
                    userLog("ld: unresolved TLSDESC symbol ");
                    userLog(name);
                    if (required_version) |version| {
                        userLog("@");
                        userLog(version);
                    }
                    userLog("\n");
                    return false;
                };
                target_load_bias = symbol.load_bias;
                tls_offset = symbol.offset;
            }
            const tp_offset = staticTlsTpOffset(tls, target_load_bias, tls_offset) orelse {
                userLog("ld: TLSDESC static TLS block unavailable\n");
                return false;
            };
            writeU64(dest_va, @intFromPtr(&__cap_tlsdesc_static));
            writeU64(dest_va + 8, addSignedWrap(tp_offset, r_addend));
            applied += 1;
            continue;
        }

        userLog("ld: unsupported relocation type ");
        logHex(r_type);
        userLog("\n");
        return false;
    }
    userLog("ld: applied ");
    userLog(label);
    userLog(" count=");
    logHex(applied);
    userLog("\n");
    return true;
}

fn logDynamicSummary(label: []const u8, info: DynamicInfo) void {
    if (info.strtab != 0 and info.strsz != 0) {
        userLog("ld: ");
        userLog(label);
        userLog(" DT_STRTAB ready\n");
    }
    if (info.symtab != 0 and info.syment != 0) {
        userLog("ld: ");
        userLog(label);
        userLog(" DT_SYMTAB ready\n");
    }
    if (info.gnu_hash != 0) {
        userLog("ld: ");
        userLog(label);
        userLog(" DT_GNU_HASH ready\n");
    }
    if (info.rela != 0 and info.relaent != 0) {
        userLog("ld: ");
        userLog(label);
        userLog(" DT_RELA ready\n");
    }
    if (info.jmprel != 0 and info.pltrelsz != 0) {
        userLog("ld: ");
        userLog(label);
        userLog(" DT_JMPREL ready\n");
        userLog("ld: ");
        userLog(label);
        userLog(" lazy binding unsupported, always bind-now\n");
    }
    if ((info.flags & df_bind_now) != 0) {
        userLog("ld: ");
        userLog(label);
        userLog(" DF_BIND_NOW set\n");
    }
    if ((info.flags_1 & df_1_now) != 0) {
        userLog("ld: ");
        userLog(label);
        userLog(" DF_1_NOW set\n");
    }
    if (info.preinit_array != 0 and info.preinit_arraysz != 0) {
        userLog("ld: ");
        userLog(label);
        userLog(" preinit ready\n");
    }
    if (info.init != 0 or (info.init_array != 0 and info.init_arraysz != 0)) {
        userLog("ld: ");
        userLog(label);
        userLog(" init ready\n");
    }
    if (info.fini != 0 or (info.fini_array != 0 and info.fini_arraysz != 0)) {
        userLog("ld: ");
        userLog(label);
        userLog(" fini registered\n");
    }
    if (info.soname != 0) {
        userLog("ld: ");
        userLog(label);
        userLog(" DT_SONAME ");
        logDynString(info.strtab, info.strsz, info.soname);
        userLog("\n");
    }
    if (info.runpath != 0) {
        userLog("ld: ");
        userLog(label);
        userLog(" DT_RUNPATH ");
        logDynString(info.strtab, info.strsz, info.runpath);
        userLog("\n");
    } else if (info.rpath != 0) {
        userLog("ld: ");
        userLog(label);
        userLog(" DT_RPATH ");
        logDynString(info.strtab, info.strsz, info.rpath);
        userLog("\n");
    }
    if (info.versym != 0) {
        userLog("ld: ");
        userLog(label);
        userLog(" DT_VERSYM ready\n");
    }
    if (info.verdef != 0 and info.verdefnum != 0) {
        userLog("ld: ");
        userLog(label);
        userLog(" DT_VERDEF count=");
        logHex(info.verdefnum);
        userLog("\n");
    }
    if (info.verneed != 0 and info.verneednum != 0) {
        userLog("ld: ");
        userLog(label);
        userLog(" DT_VERNEED count=");
        logHex(info.verneednum);
        userLog("\n");
    }
}

fn logTlsMetadata(label: []const u8, vaddr: u64, filesz: u64, memsz: u64, align_bytes: u64) void {
    if (memsz == 0) return;
    userLog("ld: ");
    userLog(label);
    userLog(" PT_TLS vaddr=");
    logHex(vaddr);
    userLog(" filesz=");
    logHex(filesz);
    userLog(" memsz=");
    logHex(memsz);
    userLog(" align=");
    logHex(align_bytes);
    userLog("\n");
}

fn registerTlsModule(tls: *TlsState, label: []const u8, load_bias: u64, vaddr: u64, filesz: u64, memsz: u64, align_bytes: u64) void {
    logTlsMetadata(label, vaddr, filesz, memsz, align_bytes);
    if (memsz == 0) return;
    if (filesz > memsz) {
        userLog("ld: ");
        userLog(label);
        userLog(" TLS filesz > memsz\n");
        return;
    }
    if (tls.module_count >= tls.modules.len) {
        userLog("ld: ");
        userLog(label);
        userLog(" TLS module table full\n");
        return;
    }
    const image_va = runtimeVa(load_bias, vaddr) orelse {
        userLog("ld: ");
        userLog(label);
        userLog(" TLS image invalid\n");
        return;
    };
    tls.modules[tls.module_count] = .{
        .load_bias = load_bias,
        .image_va = image_va,
        .filesz = filesz,
        .memsz = memsz,
        .align_bytes = if (align_bytes == 0) 1 else align_bytes,
        .static_base = 0,
    };
    tls.module_count += 1;
    userLog("ld: ");
    userLog(label);
    userLog(" TLS metadata ready\n");
}

fn setupStaticTls(tls: *TlsState) bool {
    const page_count: u64 = static_tls_bytes / page_bytes;
    if (syscall4(syscall_alloc_map_pages, static_tls_target_va, page_count, 1, 0) != syscall_ok) {
        userLog("ld: static TLS map failed\n");
        return false;
    }
    const area_base: u64 = static_tls_target_va;
    const area_bytes: u64 = @intCast(static_tls_bytes);
    zeroMemory(area_base, area_bytes);

    var cursor: u64 = 0;
    var index: usize = 0;
    while (index < tls.module_count) : (index += 1) {
        cursor = alignUp(cursor, tls.modules[index].align_bytes) orelse return false;
        const next, const overflow = @addWithOverflow(cursor, tls.modules[index].memsz);
        if (overflow != 0 or next > area_bytes) {
            userLog("ld: static TLS block overflow\n");
            return false;
        }
        const dest = area_base + cursor;
        tls.modules[index].static_base = dest;
        if (tls.modules[index].filesz != 0 and !copyMemory(dest, tls.modules[index].image_va, tls.modules[index].filesz)) return false;
        if (tls.modules[index].memsz > tls.modules[index].filesz) {
            zeroMemory(dest + tls.modules[index].filesz, tls.modules[index].memsz - tls.modules[index].filesz);
        }
        cursor = next;
    }

    const total = alignUp(cursor, static_tls_align) orelse return false;
    const tcb_end, const tcb_overflow = @addWithOverflow(total, static_tls_tcb_bytes);
    if (tcb_overflow != 0 or tcb_end > area_bytes) {
        userLog("ld: static TLS TCB overflow\n");
        return false;
    }
    tls.thread_pointer = area_base + total;
    zeroMemory(tls.thread_pointer, static_tls_tcb_bytes);
    writeU64(tls.thread_pointer, tls.thread_pointer);
    index = 0;
    while (index < tls.module_count) : (index += 1) {
        tls.modules[index].static_tp_offset = tls.modules[index].static_base -% tls.thread_pointer;
    }
    const result = syscall3(syscall_set_fs_base_self, tls.thread_pointer, 0, 0);
    if (result != syscall_ok) {
        userLog("ld: set_fs_base_self failed ");
        logHex(result);
        userLog("\n");
        return false;
    }
    logStaticTlsLayout(tls);
    userLog("ld: static TLS ready fs_base=");
    logHex(tls.thread_pointer);
    userLog(" size=");
    logHex(total);
    userLog("\n");
    return true;
}

fn logRelroMetadata(label: []const u8, load_bias: u64, vaddr: u64, memsz: u64) void {
    if (memsz == 0) return;
    const start = runtimeVa(load_bias, vaddr) orelse return;
    const end = runtimeVa(start, memsz) orelse return;
    userLog("ld: ");
    userLog(label);
    userLog(" PT_GNU_RELRO start=");
    logHex(start);
    userLog(" end=");
    logHex(end);
    userLog(" size=");
    logHex(memsz);
    userLog("\n");
}

fn pageDown(value: u64) u64 {
    return value & ~(page_bytes - 1);
}

fn pageUp(value: u64) ?u64 {
    const rounded, const overflow = @addWithOverflow(value, page_bytes - 1);
    if (overflow != 0) return null;
    return pageDown(rounded);
}

fn protectRelro(label: []const u8, load_bias: u64, vaddr: u64, memsz: u64) void {
    if (memsz == 0) return;
    const start = runtimeVa(load_bias, vaddr) orelse return;
    const end = runtimeVa(start, memsz) orelse return;
    const page_start = pageDown(start);
    const page_end = pageUp(end) orelse return;
    if (page_end <= page_start) return;
    const prot_bits = process_builder_abi.mapProtToBits(.{ .read = true });
    const result = syscall3(process_builder_abi.syscall_mprotect_self, page_start, page_end - page_start, prot_bits);
    if (result == syscall_ok) {
        userLog("ld: ");
        userLog(label);
        userLog(" PT_GNU_RELRO protected\n");
    } else {
        userLog("ld: ");
        userLog(label);
        userLog(" PT_GNU_RELRO protect failed ");
        logHex(result);
        userLog("\n");
    }
}

fn logDynString(strtab: u64, strsz: u64, offset: u64) void {
    const text = dynString(strtab, strsz, offset) orelse {
        userLog("<bad-str>");
        return;
    };
    if (text.len == 0) {
        userLog("<empty>");
        return;
    }
    userLog(text);
}

fn dynString(strtab: u64, strsz: u64, offset: u64) ?[]const u8 {
    if (strtab == 0 or offset >= strsz) return null;
    const max_len_u64 = strsz - offset;
    const max_len: usize = @intCast(if (max_len_u64 < max_needed_name_bytes) max_len_u64 else max_needed_name_bytes);
    const ptr: [*]const u8 = @ptrFromInt(strtab + offset);
    var len: usize = 0;
    while (len < max_len) : (len += 1) {
        if (ptr[len] == 0) return ptr[0..len];
    }
    return null;
}

fn sameBytes(a: []const u8, b: []const u8) bool {
    if (a.len != b.len) return false;
    var index: usize = 0;
    while (index < a.len) : (index += 1) {
        if (a[index] != b[index]) return false;
    }
    return true;
}

fn neededPath(name: []const u8, out: *[max_needed_path_bytes]u8) ?[]const u8 {
    if (name.len == 0) return null;
    if (name[0] == '/') {
        if (name.len > out.len) return null;
        @memcpy(out[0..name.len], name);
        return out[0..name.len];
    }
    const prefix = "/lib/";
    if (prefix.len + name.len > out.len) return null;
    @memcpy(out[0..prefix.len], prefix);
    @memcpy(out[prefix.len .. prefix.len + name.len], name);
    return out[0 .. prefix.len + name.len];
}

fn bootfsPathForEntry(header: *const bootfs_format.BootFsHeader, entry: bootfs_format.BootFsEntry) ?[]const u8 {
    if (entry.kind != bootfs_format.kind_regular) return null;
    const path_end = @as(u64, entry.path_offset) + @as(u64, entry.path_bytes);
    if (path_end > header.string_table_bytes) return null;
    const ptr: [*]const u8 = @ptrFromInt(@intFromPtr(header) + header.string_table_offset + entry.path_offset);
    return ptr[0..entry.path_bytes];
}

fn findBootFsRegular(cfg: *const volatile dynamic_linker_bootstrap_abi.Config, path: []const u8) ?bootfs_format.BootFsEntry {
    if (cfg.bootfs_image_va == 0 or cfg.bootfs_image_bytes < @sizeOf(bootfs_format.BootFsHeader)) return null;
    const header: *const bootfs_format.BootFsHeader = @ptrFromInt(cfg.bootfs_image_va);
    if (header.magic != bootfs_format.magic or header.version != bootfs_format.version) return null;
    if (header.header_bytes != @sizeOf(bootfs_format.BootFsHeader)) return null;
    if (header.image_bytes == 0 or header.image_bytes > cfg.bootfs_image_bytes) return null;

    const entry_table_bytes = @as(u64, header.entry_count) * @as(u64, header.entry_bytes);
    if (header.entry_bytes < @sizeOf(bootfs_format.BootFsEntry)) return null;
    if (header.entry_table_offset + entry_table_bytes > header.image_bytes) return null;
    if (header.string_table_offset + header.string_table_bytes > header.image_bytes) return null;

    const entries: [*]const bootfs_format.BootFsEntry = @ptrFromInt(cfg.bootfs_image_va + header.entry_table_offset);
    var index: usize = 0;
    while (index < header.entry_count) : (index += 1) {
        const entry = entries[index];
        const entry_path = bootfsPathForEntry(header, entry) orelse continue;
        if (!sameBytes(entry_path, path)) continue;
        if (entry.data_offset + entry.data_bytes > header.image_bytes) return null;
        return entry;
    }
    return null;
}

fn resolveNeededFromBootFs(cfg: *const volatile dynamic_linker_bootstrap_abi.Config, name: []const u8) void {
    var path_buf: [max_needed_path_bytes]u8 = undefined;
    const path = neededPath(name, &path_buf) orelse {
        userLog("ld: needed path invalid\n");
        return;
    };
    if (findBootFsRegular(cfg, path)) |_| {
        userLog("ld: resolve ");
        userLog(path);
        userLog(" ok\n");
    } else {
        userLog("ld: resolve ");
        userLog(path);
        userLog(" missing\n");
    }
}

fn inspectMainDynamic(cfg: *const volatile dynamic_linker_bootstrap_abi.Config, tls: *TlsState) ?DynamicInfo {
    const dynamic_phdr = findDynamicPhdr(cfg) orelse {
        userLog("ld: PT_DYNAMIC absent\n");
        return null;
    };
    const info = parseDynamic(cfg, dynamic_phdr) orelse {
        userLog("ld: PT_DYNAMIC parse failed\n");
        return null;
    };

    userLog("ld: PT_DYNAMIC found\n");
    logDynamicSummary("main", info);
    if (findMainTlsPhdr(cfg)) |tls_phdr| {
        registerTlsModule(tls, "main", cfg.main_load_bias, tls_phdr.vaddr, tls_phdr.filesz, tls_phdr.memsz, tls_phdr.align_bytes);
    }
    if (findMainRelroPhdr(cfg)) |relro| {
        logRelroMetadata("main", cfg.main_load_bias, relro.vaddr, relro.memsz);
    }

    var index: usize = 0;
    while (index < info.needed_count) : (index += 1) {
        userLog("ld: DT_NEEDED ");
        logDynString(info.strtab, info.strsz, info.needed[index]);
        userLog("\n");
        if (dynString(info.strtab, info.strsz, info.needed[index])) |name| {
            resolveNeededFromBootFs(cfg, name);
        }
    }

    inspectLoadedLibs(cfg, tls);
    if (!setupStaticTls(tls)) {
        userLog("ld: static TLS setup failed\n");
    }
    if (!applyRelocations(cfg, tls, info, cfg.main_load_bias, info)) {
        userLog("ld: main relocation failed\n");
    }
    applyLoadedLibRelocations(cfg, tls, info);
    protectLoadedLibRelro(cfg);
    if (findMainRelroPhdr(cfg)) |relro| {
        protectRelro("main", cfg.main_load_bias, relro.vaddr, relro.memsz);
    }
    logFinalizerRegistrationPlan(cfg, info);
    runPreinitArray(info, "main");
    runLoadedLibInitializers(cfg);
    runInitializers(info, "main");
    return info;
}

fn applyLoadedLibRelocations(
    cfg: *const volatile dynamic_linker_bootstrap_abi.Config,
    tls: *const TlsState,
    main_info: DynamicInfo,
) void {
    const count = if (cfg.loaded_lib_count > dynamic_linker_bootstrap_abi.max_loaded_libs) dynamic_linker_bootstrap_abi.max_loaded_libs else cfg.loaded_lib_count;
    var index: usize = 0;
    while (index < count) : (index += 1) {
        const lib = cfg.loaded_libs[index];
        if (lib.dynamic_va == 0 or lib.dynamic_filesz == 0) {
            userLog("ld: lib dynamic absent\n");
            continue;
        }
        const info = parseDynamicAt(lib.load_bias, lib.dynamic_va, lib.dynamic_filesz) orelse {
            userLog("ld: lib dynamic parse failed\n");
            continue;
        };
        if (!applyRelocations(cfg, tls, info, lib.load_bias, main_info)) {
            userLog("ld: lib relocation failed\n");
        }
    }
}

fn runPreinitArray(info: DynamicInfo, label: []const u8) void {
    if (info.preinit_array == 0 or info.preinit_arraysz == 0) return;
    if ((info.preinit_arraysz % 8) != 0) {
        userLog("ld: bad DT_PREINIT_ARRAY size\n");
        return;
    }
    var offset: u64 = 0;
    while (offset < info.preinit_arraysz) : (offset += 8) {
        const fn_va = readU64(info.preinit_array + offset);
        if (fn_va == 0) continue;
        userLog("ld: run ");
        userLog(label);
        userLog(" DT_PREINIT_ARRAY\n");
        const preinit_fn: *const fn () callconv(.c) void = @ptrFromInt(fn_va);
        preinit_fn();
    }
}

fn runInitializers(info: DynamicInfo, label: []const u8) void {
    if (info.init != 0) {
        userLog("ld: run ");
        userLog(label);
        userLog(" DT_INIT\n");
        const init_fn: *const fn () callconv(.c) void = @ptrFromInt(info.init);
        init_fn();
    }
    if (info.init_array == 0 or info.init_arraysz == 0) return;
    if ((info.init_arraysz % 8) != 0) {
        userLog("ld: bad DT_INIT_ARRAY size\n");
        return;
    }
    var offset: u64 = 0;
    while (offset < info.init_arraysz) : (offset += 8) {
        const fn_va = readU64(info.init_array + offset);
        if (fn_va == 0) continue;
        userLog("ld: run ");
        userLog(label);
        userLog(" DT_INIT_ARRAY\n");
        const init_fn: *const fn () callconv(.c) void = @ptrFromInt(fn_va);
        init_fn();
    }
}

fn runLoadedLibInitializers(cfg: *const volatile dynamic_linker_bootstrap_abi.Config) void {
    const count = if (cfg.loaded_lib_count > dynamic_linker_bootstrap_abi.max_loaded_libs) dynamic_linker_bootstrap_abi.max_loaded_libs else cfg.loaded_lib_count;
    if (count != 0) {
        userLog("ld: init order dependency-first libs=");
        logHex(@intCast(count));
        userLog("\n");
    }
    var remaining = count;
    while (remaining != 0) {
        remaining -= 1;
        const index = remaining;
        const lib = cfg.loaded_libs[index];
        if (lib.dynamic_va == 0 or lib.dynamic_filesz == 0) continue;
        const info = parseDynamicAt(lib.load_bias, lib.dynamic_va, lib.dynamic_filesz) orelse continue;
        runInitializers(info, "lib");
    }
}

fn runFinalizers(cfg: *const volatile dynamic_linker_bootstrap_abi.Config, main_info: DynamicInfo) void {
    runFinalizerForInfo(main_info, "main");
    runLoadedLibFinalizers(cfg);
}

fn runFinalizerForInfo(info: DynamicInfo, label: []const u8) void {
    if (info.fini_array != 0 and info.fini_arraysz != 0) {
        if ((info.fini_arraysz % 8) != 0) {
            userLog("ld: bad DT_FINI_ARRAY size\n");
            return;
        }
        var remaining = info.fini_arraysz / 8;
        while (remaining != 0) {
            remaining -= 1;
            const fn_va = readU64(info.fini_array + remaining * 8);
            if (fn_va == 0) continue;
            userLog("ld: run ");
            userLog(label);
            userLog(" DT_FINI_ARRAY\n");
            const fini_fn: *const fn () callconv(.c) void = @ptrFromInt(fn_va);
            fini_fn();
        }
    }
    if (info.fini != 0) {
        userLog("ld: run ");
        userLog(label);
        userLog(" DT_FINI\n");
        const fini_fn: *const fn () callconv(.c) void = @ptrFromInt(info.fini);
        fini_fn();
    }
}

fn runLoadedLibFinalizers(cfg: *const volatile dynamic_linker_bootstrap_abi.Config) void {
    const count = if (cfg.loaded_lib_count > dynamic_linker_bootstrap_abi.max_loaded_libs) dynamic_linker_bootstrap_abi.max_loaded_libs else cfg.loaded_lib_count;
    if (count != 0) {
        userLog("ld: fini order main-first libs=");
        logHex(@intCast(count));
        userLog("\n");
    }
    var index: usize = 0;
    while (index < count) : (index += 1) {
        const lib = cfg.loaded_libs[index];
        if (lib.dynamic_va == 0 or lib.dynamic_filesz == 0) continue;
        const info = parseDynamicAt(lib.load_bias, lib.dynamic_va, lib.dynamic_filesz) orelse continue;
        runFinalizerForInfo(info, "lib");
    }
}

fn hasFinalizers(info: DynamicInfo) bool {
    return info.fini != 0 or (info.fini_array != 0 and info.fini_arraysz != 0);
}

fn logFinalizerRegistration(label: []const u8, info: DynamicInfo) void {
    if (!hasFinalizers(info)) return;
    userLog("ld: fini plan ");
    userLog(label);
    if (info.fini != 0) userLog(" DT_FINI");
    if (info.fini_array != 0 and info.fini_arraysz != 0) {
        userLog(" DT_FINI_ARRAY bytes=");
        logHex(info.fini_arraysz);
    }
    userLog("\n");
}

fn logFinalizerRegistrationPlan(cfg: *const volatile dynamic_linker_bootstrap_abi.Config, main_info: DynamicInfo) void {
    logFinalizerRegistration("main", main_info);
    const count = if (cfg.loaded_lib_count > dynamic_linker_bootstrap_abi.max_loaded_libs) dynamic_linker_bootstrap_abi.max_loaded_libs else cfg.loaded_lib_count;
    var index: usize = 0;
    while (index < count) : (index += 1) {
        const lib = cfg.loaded_libs[index];
        if (lib.dynamic_va == 0 or lib.dynamic_filesz == 0) continue;
        const info = parseDynamicAt(lib.load_bias, lib.dynamic_va, lib.dynamic_filesz) orelse continue;
        logFinalizerRegistration("lib", info);
    }
}

fn protectLoadedLibRelro(cfg: *const volatile dynamic_linker_bootstrap_abi.Config) void {
    const count = if (cfg.loaded_lib_count > dynamic_linker_bootstrap_abi.max_loaded_libs) dynamic_linker_bootstrap_abi.max_loaded_libs else cfg.loaded_lib_count;
    var index: usize = 0;
    while (index < count) : (index += 1) {
        const lib = cfg.loaded_libs[index];
        protectRelro("lib", lib.load_bias, lib.relro_vaddr, lib.relro_memsz);
    }
}

fn logHex(value: u64) void {
    const digits = "0123456789abcdef";
    var buf: [18]u8 = undefined;
    buf[0] = '0';
    buf[1] = 'x';
    var shift: u6 = 60;
    var out: usize = 2;
    while (true) {
        buf[out] = digits[(value >> shift) & 0xf];
        out += 1;
        if (shift == 0) break;
        shift -= 4;
    }
    userLog(buf[0..out]);
}

fn inspectLoadedLibs(cfg: *const volatile dynamic_linker_bootstrap_abi.Config, tls: *TlsState) void {
    const count = cfg.loaded_lib_count;
    if (count == 0) {
        userLog("ld: no loaded libs\n");
        return;
    }
    var index: usize = 0;
    while (index < count and index < dynamic_linker_bootstrap_abi.max_loaded_libs) : (index += 1) {
        const lib = cfg.loaded_libs[index];
        userLog("ld: lib[");
        logHex(@intCast(index));
        userLog("] bias=");
        logHex(lib.load_bias);
        userLog(" dyn_va=");
        logHex(lib.dynamic_va);
        userLog(" dyn_size=");
        logHex(lib.dynamic_filesz);
        userLog("\n");
        if (lib.dynamic_va == 0 or lib.dynamic_filesz == 0) {
            userLog("ld: lib dynamic absent\n");
            continue;
        }
        const info = parseDynamicAt(lib.load_bias, lib.dynamic_va, lib.dynamic_filesz) orelse {
            userLog("ld: lib dynamic parse failed\n");
            continue;
        };
        userLog("ld: lib PT_DYNAMIC found\n");
        logDynamicSummary("lib", info);
        registerTlsModule(tls, "lib", lib.load_bias, lib.tls_vaddr, lib.tls_filesz, lib.tls_memsz, lib.tls_align);
        logRelroMetadata("lib", lib.load_bias, lib.relro_vaddr, lib.relro_memsz);
    }
}

pub export fn _start() noreturn {
    const cfg: *const volatile dynamic_linker_bootstrap_abi.Config = @ptrFromInt(dynamic_linker_bootstrap_abi.target_va);
    if (cfg.magic != dynamic_linker_bootstrap_abi.magic or cfg.version != dynamic_linker_bootstrap_abi.version or cfg.main_entry == 0) {
        userLog("ld: missing bootstrap config\n");
        while (true) asm volatile ("pause");
    }

    userLog("ld: started\n");
    userLog("ld: main bias=");
    logHex(cfg.main_load_bias);
    userLog("\n");
    var tls = TlsState{};
    const main_info = inspectMainDynamic(cfg, &tls);
    const entry: *const fn () callconv(.c) void = @ptrFromInt(cfg.main_entry);
    entry();
    if (main_info) |info| {
        runFinalizers(cfg, info);
    }
    while (true) asm volatile ("pause");
}

