const std = @import("std");

pub const max_dir_entries: usize = 64;
pub const max_name_bytes: usize = 32;
pub const minimum_data_blocks: u64 = 16;
pub const volume_magic: u64 = 0x3153_4650; // "PFS1"
pub const volume_version: u64 = 1;
pub const dir_entry_flag_used: u32 = 1;
pub const dir_entry_flag_directory: u32 = 1 << 1;
pub const dir_mode_bits: u32 = 0x4000;
pub const file_mode_bits: u32 = 0x8000;

pub const VolumeSuperblock = extern struct {
    magic: u64 = volume_magic,
    version: u64 = volume_version,
    block_size: u64 = 0,
    fs_start_block: u64 = 0,
    dir_start_block: u64 = 0,
    dir_block_count: u64 = 0,
    data_start_block: u64 = 0,
    next_free_block: u64 = 0,
};

pub const VolumeDirEntry = extern struct {
    flags: u32 = 0,
    name_bytes: u16 = 0,
    reserved0: u16 = 0,
    file_size: u64 = 0,
    start_block: u64 = 0,
    block_count: u32 = 0,
    reserved1: u32 = 0,
    name: [max_name_bytes]u8 = [_]u8{0} ** max_name_bytes,
};

comptime {
    std.debug.assert(@sizeOf(VolumeSuperblock) == 64);
    std.debug.assert(@sizeOf(VolumeDirEntry) == 64);
}

pub fn requiredDirBlockCount(block_size: u64) u64 {
    const bytes: u64 = max_dir_entries * @sizeOf(VolumeDirEntry);
    return (bytes + block_size - 1) / block_size;
}

pub fn expectedDirStartBlock(fs_start_block: u64) u64 {
    return fs_start_block + 1;
}

pub fn expectedDataStartBlock(fs_start_block: u64, block_size: u64) u64 {
    return expectedDirStartBlock(fs_start_block) + requiredDirBlockCount(block_size);
}

pub fn initSuperblock(fs_start_block: u64, block_size: u64) VolumeSuperblock {
    const data_start_block = expectedDataStartBlock(fs_start_block, block_size);
    return .{
        .magic = volume_magic,
        .version = volume_version,
        .block_size = block_size,
        .fs_start_block = fs_start_block,
        .dir_start_block = expectedDirStartBlock(fs_start_block),
        .dir_block_count = requiredDirBlockCount(block_size),
        .data_start_block = data_start_block,
        .next_free_block = data_start_block,
    };
}

pub fn canFormatVolume(fs_start_block: u64, block_size: u64, capacity_blocks: u64) bool {
    return capacity_blocks > expectedDataStartBlock(fs_start_block, block_size) + minimum_data_blocks;
}

pub fn validateSuperblock(sb: *const VolumeSuperblock, fs_start_block: u64, block_size: u64, capacity_blocks: u64) bool {
    if (sb.magic != volume_magic or sb.version != volume_version) return false;
    if (sb.block_size != block_size) return false;
    if (sb.fs_start_block != fs_start_block) return false;
    if (sb.dir_start_block != expectedDirStartBlock(fs_start_block)) return false;
    if (sb.dir_block_count != requiredDirBlockCount(block_size)) return false;
    if (sb.data_start_block != expectedDataStartBlock(fs_start_block, block_size)) return false;
    if (sb.next_free_block < sb.data_start_block or sb.next_free_block > capacity_blocks) return false;
    return true;
}

pub fn dirEntryUsed(entry: *const VolumeDirEntry) bool {
    return (entry.flags & dir_entry_flag_used) != 0;
}

pub fn dirEntryName(entry: *const VolumeDirEntry) []const u8 {
    return entry.name[0..entry.name_bytes];
}

pub fn dirEntryIsDirectory(entry: *const VolumeDirEntry) bool {
    return (entry.flags & dir_entry_flag_directory) != 0;
}

pub fn dirEntryParentIndex(entry: *const VolumeDirEntry) ?usize {
    if (entry.reserved0 == 0) return null;
    return entry.reserved0 - 1;
}

pub fn clearDirectory(entries: *[max_dir_entries]VolumeDirEntry) void {
    entries.* = [_]VolumeDirEntry{.{}} ** max_dir_entries;
}

pub fn setDirEntryName(entry: *VolumeDirEntry, name: []const u8) void {
    std.debug.assert(name.len <= max_name_bytes);
    @memset(entry.name[0..], 0);
    @memcpy(entry.name[0..name.len], name);
    entry.name_bytes = @intCast(name.len);
}

pub fn setDirEntryParentIndex(entry: *VolumeDirEntry, parent_index: ?usize) void {
    entry.reserved0 = if (parent_index) |index| @intCast(index + 1) else 0;
}

pub fn setDirEntryDirectory(entry: *VolumeDirEntry, is_directory: bool) void {
    if (is_directory)
        entry.flags |= dir_entry_flag_directory
    else
        entry.flags &= ~dir_entry_flag_directory;
}

pub fn blocksForSize(block_size: u64, size_bytes: u64) u64 {
    if (size_bytes == 0) return 0;
    return (size_bytes + block_size - 1) / block_size;
}
