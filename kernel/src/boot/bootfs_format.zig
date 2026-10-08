pub const magic: u32 = 0x5346_5442; // "BTFS"
pub const kind_regular: u8 = 1;

pub const BootFsHeader = extern struct {
    magic: u32,
    header_bytes: u32,
    image_bytes: u64,
    entry_count: u32,
    entry_bytes: u32,
    entry_table_offset: u64,
    string_table_offset: u64,
    string_table_bytes: u64,
    data_offset: u64,
    data_bytes: u64,
    flags: u32,
    reserved0: u32 = 0,
    reserved1: [4]u64 = [_]u64{0} ** 4,
};

pub const BootFsEntry = extern struct {
    path_offset: u32,
    path_bytes: u16,
    kind: u8,
    flags: u8,
    data_offset: u64,
    data_bytes: u64,
    mode_bits: u32,
    reserved0: u32 = 0,
    reserved1: [2]u64 = [_]u64{0} ** 2,
};

comptime {
    if (@sizeOf(BootFsHeader) != 104 or @offsetOf(BootFsHeader, "header_bytes") != 4 or
        @offsetOf(BootFsHeader, "image_bytes") != 8)
        @compileError("bootfs header layout mismatch");
}
