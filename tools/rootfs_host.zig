const std = @import("std");
const layout = @import("persistent_fs_layout");

pub const sector_bytes: usize = 512;
pub const directory_source_token = "@dir";

const GptHeader = struct {
    partition_entry_lba: u64,
    partition_entry_count: u32,
    partition_entry_size: u32,
};

const GptEntryPrefix = struct {
    type_guid: [16]u8,
    first_lba: u64,
    last_lba: u64,
};

pub const PartitionRegion = struct {
    first_lba: u64,
    last_lba: u64,
};

pub const EntryKind = enum {
    file,
    directory,
};

pub const FileSpec = struct {
    image_path: []const u8,
    source_path: []const u8,
    data: []const u8,
    kind: EntryKind = .file,
};

const Extent = struct {
    start: u64,
    end: u64,
};

const PathTarget = struct {
    parent_path: ?[]const u8,
    name: []const u8,
};

pub const VolumeState = struct {
    superblock: layout.VolumeSuperblock = .{},
    dir_entries: [layout.max_dir_entries]layout.VolumeDirEntry = [_]layout.VolumeDirEntry{.{}} ** layout.max_dir_entries,
};

fn readAllAt(file: *std.fs.File, offset: u64, out: []u8) !void {
    try file.seekTo(offset);
    const bytes_read = try file.readAll(out);
    if (bytes_read != out.len) return error.EndOfStream;
}

fn writeAllAt(file: *std.fs.File, offset: u64, bytes: []const u8) !void {
    try file.seekTo(offset);
    try file.writeAll(bytes);
}

fn readU32Le(bytes: []const u8, offset: usize) u32 {
    return std.mem.readInt(u32, bytes[offset..][0..4], .little);
}

fn readU64Le(bytes: []const u8, offset: usize) u64 {
    return std.mem.readInt(u64, bytes[offset..][0..8], .little);
}

fn parseGptHeader(bytes: []const u8) !GptHeader {
    if (bytes.len < sector_bytes) return error.InvalidGptHeader;
    if (!std.mem.eql(u8, bytes[0..8], "EFI PART")) return error.InvalidGptHeader;
    return .{
        .partition_entry_lba = readU64Le(bytes, 72),
        .partition_entry_count = readU32Le(bytes, 80),
        .partition_entry_size = readU32Le(bytes, 84),
    };
}

fn parseGptEntryPrefix(bytes: []const u8) !GptEntryPrefix {
    if (bytes.len < 48) return error.InvalidPartitionEntry;
    var type_guid: [16]u8 = undefined;
    @memcpy(type_guid[0..], bytes[0..16]);
    return .{
        .type_guid = type_guid,
        .first_lba = readU64Le(bytes, 32),
        .last_lba = readU64Le(bytes, 40),
    };
}

fn partitionEntryIsUnused(entry: *const GptEntryPrefix) bool {
    for (entry.type_guid) |byte| {
        if (byte != 0) return false;
    }
    return true;
}

pub fn openPartitionRegion(file: *std.fs.File, partition_index: u32) !PartitionRegion {
    if (partition_index == 0) return error.InvalidPartitionIndex;
    var header_sector: [sector_bytes]u8 = undefined;
    try readAllAt(file, @as(u64, sector_bytes), header_sector[0..]);
    const header = try parseGptHeader(header_sector[0..]);
    if (partition_index > header.partition_entry_count) return error.InvalidPartitionIndex;
    if (header.partition_entry_size < 48) return error.InvalidGptHeader;
    const entry_offset = header.partition_entry_lba * @as(u64, sector_bytes) + (@as(u64, partition_index) - 1) * header.partition_entry_size;
    var entry_bytes: [48]u8 = undefined;
    try readAllAt(file, entry_offset, entry_bytes[0..]);
    const entry = try parseGptEntryPrefix(entry_bytes[0..]);
    if (partitionEntryIsUnused(&entry)) return error.PartitionNotFound;
    if (entry.last_lba < entry.first_lba) return error.InvalidPartitionEntry;
    return .{
        .first_lba = entry.first_lba,
        .last_lba = entry.last_lba,
    };
}

pub fn diskCapacityBlocks(file: *std.fs.File) !u64 {
    const bytes = try file.getEndPos();
    return bytes / @as(u64, sector_bytes);
}

pub fn validateImagePath(path: []const u8) !void {
    if (path.len < 2 or path[0] != '/') return error.InvalidImagePath;
    if (path[path.len - 1] == '/') return error.InvalidImagePath;

    var pos: usize = 1;
    while (pos < path.len) {
        const start = pos;
        while (pos < path.len and path[pos] != '/') : (pos += 1) {}
        const component = path[start..pos];
        if (component.len == 0) return error.InvalidImagePath;
        if (std.mem.eql(u8, component, ".") or std.mem.eql(u8, component, "..")) return error.InvalidImagePath;
        if (component.len > layout.max_name_bytes) return error.ImagePathTooLong;
        if (pos < path.len) pos += 1;
    }
}

fn splitPathTarget(abs_path: []const u8) PathTarget {
    const last_slash = std.mem.lastIndexOfScalar(u8, abs_path, '/') orelse unreachable;
    return .{
        .parent_path = if (last_slash == 0) null else abs_path[0..last_slash],
        .name = abs_path[last_slash + 1 ..],
    };
}

fn entryParentMatches(entry: *const layout.VolumeDirEntry, parent_index: ?usize) bool {
    return layout.dirEntryParentIndex(entry) == parent_index;
}

fn readBlock(file: *std.fs.File, block_index: u64, out: []u8) !void {
    std.debug.assert(out.len == sector_bytes);
    try readAllAt(file, block_index * @as(u64, sector_bytes), out);
}

fn writeBlock(file: *std.fs.File, block_index: u64, bytes: []const u8) !void {
    std.debug.assert(bytes.len == sector_bytes);
    try writeAllAt(file, block_index * @as(u64, sector_bytes), bytes);
}

fn loadSuperblock(file: *std.fs.File, region: PartitionRegion, capacity_blocks: u64) !?layout.VolumeSuperblock {
    var block: [sector_bytes]u8 = undefined;
    try readBlock(file, region.first_lba, block[0..]);
    var sb: layout.VolumeSuperblock = undefined;
    @memcpy(std.mem.asBytes(&sb), block[0..@sizeOf(layout.VolumeSuperblock)]);
    if (!layout.validateSuperblock(&sb, region.first_lba, @as(u64, sector_bytes), capacity_blocks)) return null;
    return sb;
}

pub fn persistSuperblock(file: *std.fs.File, state: *const VolumeState) !void {
    var block: [sector_bytes]u8 = [_]u8{0} ** sector_bytes;
    @memcpy(block[0..@sizeOf(layout.VolumeSuperblock)], std.mem.asBytes(&state.superblock));
    try writeBlock(file, state.superblock.fs_start_block, block[0..]);
}

pub fn loadDirectory(file: *std.fs.File, state: *VolumeState) !void {
    layout.clearDirectory(&state.dir_entries);
    const entries_per_block: usize = sector_bytes / @sizeOf(layout.VolumeDirEntry);
    var entry_index: usize = 0;
    var block_offset: u64 = 0;
    while (block_offset < state.superblock.dir_block_count) : (block_offset += 1) {
        var block: [sector_bytes]u8 = undefined;
        try readBlock(file, state.superblock.dir_start_block + block_offset, block[0..]);
        var slot: usize = 0;
        while (slot < entries_per_block and entry_index < layout.max_dir_entries) : ({
            slot += 1;
            entry_index += 1;
        }) {
            const start = slot * @sizeOf(layout.VolumeDirEntry);
            const end = start + @sizeOf(layout.VolumeDirEntry);
            @memcpy(std.mem.asBytes(&state.dir_entries[entry_index]), block[start..end]);
        }
    }
}

pub fn persistDirectory(file: *std.fs.File, state: *const VolumeState) !void {
    const entries_per_block: usize = sector_bytes / @sizeOf(layout.VolumeDirEntry);
    var entry_index: usize = 0;
    var block_offset: u64 = 0;
    while (block_offset < state.superblock.dir_block_count) : (block_offset += 1) {
        var block: [sector_bytes]u8 = [_]u8{0} ** sector_bytes;
        var slot: usize = 0;
        while (slot < entries_per_block and entry_index < layout.max_dir_entries) : ({
            slot += 1;
            entry_index += 1;
        }) {
            const start = slot * @sizeOf(layout.VolumeDirEntry);
            const end = start + @sizeOf(layout.VolumeDirEntry);
            @memcpy(block[start..end], std.mem.asBytes(&state.dir_entries[entry_index]));
        }
        try writeBlock(file, state.superblock.dir_start_block + block_offset, block[0..]);
    }
}

pub fn formatVolume(file: *std.fs.File, region: PartitionRegion, capacity_blocks: u64) !VolumeState {
    if (!layout.canFormatVolume(region.first_lba, @as(u64, sector_bytes), capacity_blocks)) return error.PartitionTooSmall;
    var state = VolumeState{
        .superblock = layout.initSuperblock(region.first_lba, @as(u64, sector_bytes)),
    };
    layout.clearDirectory(&state.dir_entries);
    try persistSuperblock(file, &state);
    try persistDirectory(file, &state);
    return state;
}

pub fn loadOrInitializeVolume(file: *std.fs.File, region: PartitionRegion, capacity_blocks: u64) !VolumeState {
    if (try loadSuperblock(file, region, capacity_blocks)) |sb| {
        var state = VolumeState{ .superblock = sb };
        try loadDirectory(file, &state);
        return state;
    }
    return try formatVolume(file, region, capacity_blocks);
}

fn findChildEntryByName(entries: []const layout.VolumeDirEntry, parent_index: ?usize, name: []const u8) ?usize {
    for (entries, 0..) |*entry, index| {
        if (!layout.dirEntryUsed(entry)) continue;
        if (!entryParentMatches(entry, parent_index)) continue;
        if (std.mem.eql(u8, layout.dirEntryName(entry), name)) return index;
    }
    return null;
}

fn allocDirEntry(
    entries: *[layout.max_dir_entries]layout.VolumeDirEntry,
    parent_index: ?usize,
    name: []const u8,
    is_directory: bool,
) !usize {
    for (entries, 0..) |*entry, index| {
        if (layout.dirEntryUsed(entry)) continue;
        entry.* = .{};
        entry.flags = layout.dir_entry_flag_used;
        layout.setDirEntryName(entry, name);
        layout.setDirEntryParentIndex(entry, parent_index);
        layout.setDirEntryDirectory(entry, is_directory);
        return index;
    }
    return error.DirectoryFull;
}

fn ensureDirectoryPath(entries: *[layout.max_dir_entries]layout.VolumeDirEntry, abs_path: []const u8) !?usize {
    if (abs_path.len == 0) return null;
    try validateImagePath(abs_path);

    var current_parent: ?usize = null;
    var pos: usize = 1;
    while (pos < abs_path.len) {
        const start = pos;
        while (pos < abs_path.len and abs_path[pos] != '/') : (pos += 1) {}
        const component = abs_path[start..pos];
        const child_index = findChildEntryByName(entries[0..], current_parent, component) orelse blk: {
            break :blk try allocDirEntry(entries, current_parent, component, true);
        };
        const child = &entries[child_index];
        if (!layout.dirEntryIsDirectory(child)) return error.NotDirectory;
        current_parent = child_index;
        if (pos < abs_path.len) pos += 1;
    }
    return current_parent;
}

fn ensureParentDirectories(entries: *[layout.max_dir_entries]layout.VolumeDirEntry, abs_path: []const u8) !?usize {
    const target = splitPathTarget(abs_path);
    if (target.parent_path) |parent_path| return try ensureDirectoryPath(entries, parent_path);
    return null;
}

fn buildUsedExtents(entries: []const layout.VolumeDirEntry, exclude_file_index: ?usize, out: *[layout.max_dir_entries]Extent) usize {
    var count: usize = 0;
    for (entries, 0..) |*entry, index| {
        if (!layout.dirEntryUsed(entry)) continue;
        if (exclude_file_index != null and exclude_file_index.? == index) continue;
        if (entry.block_count == 0 or entry.start_block == 0) continue;
        out[count] = .{
            .start = entry.start_block,
            .end = entry.start_block + entry.block_count,
        };
        count += 1;
    }
    var i: usize = 0;
    while (i < count) : (i += 1) {
        var min_index = i;
        var j: usize = i + 1;
        while (j < count) : (j += 1) {
            if (out[j].start < out[min_index].start) min_index = j;
        }
        if (min_index != i) {
            const tmp = out[i];
            out[i] = out[min_index];
            out[min_index] = tmp;
        }
    }
    return count;
}

pub fn recomputeNextFreeBlock(state: *VolumeState) void {
    var next = state.superblock.data_start_block;
    for (&state.dir_entries) |*entry| {
        if (!layout.dirEntryUsed(entry)) continue;
        if (entry.block_count == 0 or entry.start_block == 0) continue;
        const end = entry.start_block + entry.block_count;
        if (end > next) next = end;
    }
    state.superblock.next_free_block = next;
}

fn findFreeExtent(state: *const VolumeState, capacity_blocks: u64, required_blocks: u64, exclude_file_index: ?usize) ?u64 {
    if (required_blocks == 0) return 0;
    var extents: [layout.max_dir_entries]Extent = undefined;
    const count = buildUsedExtents(state.dir_entries[0..], exclude_file_index, &extents);
    var cursor = state.superblock.data_start_block;
    var i: usize = 0;
    while (i < count) : (i += 1) {
        const extent = extents[i];
        if (cursor + required_blocks <= extent.start) return cursor;
        if (extent.end > cursor) cursor = extent.end;
    }
    if (cursor + required_blocks <= capacity_blocks) return cursor;
    return null;
}

fn writeFileData(file: *std.fs.File, start_block: u64, data: []const u8) !void {
    const block_count = layout.blocksForSize(@as(u64, sector_bytes), @intCast(data.len));
    var block_index: u64 = 0;
    var offset: usize = 0;
    while (block_index < block_count) : (block_index += 1) {
        var block: [sector_bytes]u8 = [_]u8{0} ** sector_bytes;
        const chunk = @min(data.len - offset, sector_bytes);
        if (chunk > 0) @memcpy(block[0..chunk], data[offset .. offset + chunk]);
        try writeBlock(file, start_block + block_index, block[0..]);
        offset += chunk;
    }
}

fn upsertDirectory(file: *std.fs.File, state: *VolumeState) !void {
    recomputeNextFreeBlock(state);
    try persistDirectory(file, state);
    try persistSuperblock(file, state);
}

pub fn upsertFile(file: *std.fs.File, state: *VolumeState, capacity_blocks: u64, spec: *const FileSpec) !void {
    try validateImagePath(spec.image_path);

    if (spec.kind == .directory) {
        _ = try ensureDirectoryPath(&state.dir_entries, spec.image_path);
        try upsertDirectory(file, state);
        return;
    }

    const target = splitPathTarget(spec.image_path);
    const parent_index = try ensureParentDirectories(&state.dir_entries, spec.image_path);
    const file_index = findChildEntryByName(state.dir_entries[0..], parent_index, target.name) orelse
        try allocDirEntry(&state.dir_entries, parent_index, target.name, false);
    const entry = &state.dir_entries[file_index];
    if (layout.dirEntryIsDirectory(entry)) return error.IsDirectory;

    layout.setDirEntryName(entry, target.name);
    layout.setDirEntryParentIndex(entry, parent_index);
    layout.setDirEntryDirectory(entry, false);

    const needed_blocks = layout.blocksForSize(@as(u64, sector_bytes), @intCast(spec.data.len));
    if (needed_blocks == 0) {
        entry.start_block = 0;
        entry.block_count = 0;
        entry.file_size = 0;
    } else {
        const start_block = findFreeExtent(state, capacity_blocks, needed_blocks, file_index) orelse return error.NoSpaceLeft;
        try writeFileData(file, start_block, spec.data);
        entry.start_block = start_block;
        entry.block_count = @intCast(needed_blocks);
        entry.file_size = @intCast(spec.data.len);
    }
    recomputeNextFreeBlock(state);
    try persistDirectory(file, state);
    try persistSuperblock(file, state);
}

pub fn preparePartition(file: *std.fs.File, partition_index: u32) !struct { region: PartitionRegion, capacity_blocks: u64 } {
    const region = try openPartitionRegion(file, partition_index);
    const disk_blocks = try diskCapacityBlocks(file);
    if (region.last_lba >= disk_blocks) return error.PartitionPastEndOfDisk;
    return .{
        .region = region,
        .capacity_blocks = region.last_lba + 1,
    };
}
