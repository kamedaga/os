pub const magic: u32 = 0x5346_5442; // "BTFS"
pub const version: u16 = 1;
pub const kind_regular: u8 = 1;

pub const BootFsHeader = extern struct {
    magic: u32,
    version: u16,
    header_bytes: u16,
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
