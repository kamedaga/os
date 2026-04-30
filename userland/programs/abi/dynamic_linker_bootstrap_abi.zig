const process_abi = @import("process_abi.zig");

pub const magic: u64 = 0x4C44_4C44_5230_3031; // "LDLDR001"
pub const version: u64 = 1;
pub const target_va: u64 = process_abi.auxPageVa(37);
pub const bootfs_image_target_va: u64 = 0x3A80_0000;

pub const max_loaded_libs: usize = 8;

pub const LoadedLibInfo = extern struct {
    load_bias: u64 = 0,
    dynamic_va: u64 = 0,
    dynamic_filesz: u64 = 0,
    tls_vaddr: u64 = 0,
    tls_filesz: u64 = 0,
    tls_memsz: u64 = 0,
    tls_align: u64 = 0,
    relro_vaddr: u64 = 0,
    relro_memsz: u64 = 0,
};

pub const Config = extern struct {
    magic: u64 = magic,
    version: u64 = version,
    main_entry: u64 = 0,
    main_load_bias: u64 = 0,
    main_phdr: u64 = 0,
    main_phnum: u64 = 0,
    main_phent: u64 = 0,
    flags: u64 = 0,
    bootfs_image_va: u64 = 0,
    bootfs_image_bytes: u64 = 0,
    loaded_lib_count: u64 = 0,
    loaded_libs: [max_loaded_libs]LoadedLibInfo = [_]LoadedLibInfo{.{}} ** max_loaded_libs,
};
