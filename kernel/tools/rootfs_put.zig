const std = @import("std");
const layout = @import("persistent_fs_layout");

const sector_bytes: usize = 512;

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

const PartitionRegion = struct {
    first_lba: u64,
    last_lba: u64,
};

const FileSpec = struct {
    image_path: []const u8,
    root_name: []const u8,
    source_path: []const u8,
    data: []u8,
};

const Extent = struct {
    start: u64,
    end: u64,
};

const VolumeState = struct {
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

fn openPartitionRegion(file: *std.fs.File, partition_index: u32) !PartitionRegion {
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

fn diskCapacityBlocks(file: *std.fs.File) !u64 {
    const bytes = try file.getEndPos();
    return bytes / @as(u64, sector_bytes);
}

fn validateRootImagePath(path: []const u8) ![]const u8 {
    if (path.len < 2 or path[0] != '/') return error.InvalidImagePath;
    if (path[path.len - 1] == '/') return error.InvalidImagePath;
    const root_name = path[1..];
    if (std.mem.indexOfScalar(u8, root_name, '/')) |_| return error.DirectoriesUnsupported;
    if (root_name.len > layout.max_name_bytes) return error.ImagePathTooLong;
    return root_name;
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

fn persistSuperblock(file: *std.fs.File, state: *const VolumeState) !void {
    var block: [sector_bytes]u8 = [_]u8{0} ** sector_bytes;
    @memcpy(block[0..@sizeOf(layout.VolumeSuperblock)], std.mem.asBytes(&state.superblock));
    try writeBlock(file, state.superblock.fs_start_block, block[0..]);
}

fn loadDirectory(file: *std.fs.File, state: *VolumeState) !void {
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

fn persistDirectory(file: *std.fs.File, state: *const VolumeState) !void {
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

fn formatVolume(file: *std.fs.File, region: PartitionRegion, capacity_blocks: u64) !VolumeState {
    if (!layout.canFormatVolume(region.first_lba, @as(u64, sector_bytes), capacity_blocks)) return error.PartitionTooSmall;
    var state = VolumeState{
        .superblock = layout.initSuperblock(region.first_lba, @as(u64, sector_bytes)),
    };
    layout.clearDirectory(&state.dir_entries);
    try persistSuperblock(file, &state);
    try persistDirectory(file, &state);
    return state;
}

fn loadOrInitializeVolume(file: *std.fs.File, region: PartitionRegion, capacity_blocks: u64) !VolumeState {
    if (try loadSuperblock(file, region, capacity_blocks)) |sb| {
        var state = VolumeState{ .superblock = sb };
        try loadDirectory(file, &state);
        return state;
    }
    return try formatVolume(file, region, capacity_blocks);
}

fn findDirEntryByName(entries: []const layout.VolumeDirEntry, name: []const u8) ?usize {
    for (entries, 0..) |*entry, index| {
        if (!layout.dirEntryUsed(entry)) continue;
        if (std.mem.eql(u8, layout.dirEntryName(entry), name)) return index;
    }
    return null;
}

fn allocDirEntry(entries: *[layout.max_dir_entries]layout.VolumeDirEntry, name: []const u8) !usize {
    for (entries, 0..) |*entry, index| {
        if (layout.dirEntryUsed(entry)) continue;
        entry.* = .{};
        entry.flags = layout.dir_entry_flag_used;
        layout.setDirEntryName(entry, name);
        return index;
    }
    return error.DirectoryFull;
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

fn recomputeNextFreeBlock(state: *VolumeState) void {
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

fn upsertFile(file: *std.fs.File, state: *VolumeState, capacity_blocks: u64, spec: *const FileSpec) !void {
    const file_index = findDirEntryByName(state.dir_entries[0..], spec.root_name) orelse try allocDirEntry(&state.dir_entries, spec.root_name);
    const entry = &state.dir_entries[file_index];
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

pub fn main() !void {
    var gpa = std.heap.GeneralPurposeAllocator(.{}){};
    defer _ = gpa.deinit();
    const allocator = gpa.allocator();

    const args = try std.process.argsAlloc(allocator);
    defer std.process.argsFree(allocator, args);

    if (args.len < 5 or ((args.len - 3) % 2) != 0) {
        std.debug.print("usage: {s} <disk_img> <partition_index> <image_path> <source_path> [...]\n", .{args[0]});
        return error.InvalidArgument;
    }

    const partition_index = try std.fmt.parseUnsigned(u32, args[2], 10);
    var specs = std.ArrayList(FileSpec).empty;
    defer {
        for (specs.items) |spec| allocator.free(spec.data);
        specs.deinit(allocator);
    }

    const cwd = std.fs.cwd();
    var arg_i: usize = 3;
    while (arg_i < args.len) : (arg_i += 2) {
        const image_path = args[arg_i];
        const source_path = args[arg_i + 1];
        const root_name = try validateRootImagePath(image_path);
        const data = try cwd.readFileAlloc(allocator, source_path, std.math.maxInt(usize));
        try specs.append(allocator, .{
            .image_path = image_path,
            .root_name = root_name,
            .source_path = source_path,
            .data = data,
        });
    }

    var disk = try cwd.openFile(args[1], .{ .mode = .read_write });
    defer disk.close();

    const region = try openPartitionRegion(&disk, partition_index);
    const disk_blocks = try diskCapacityBlocks(&disk);
    if (region.last_lba >= disk_blocks) return error.PartitionPastEndOfDisk;
    const capacity_blocks = region.last_lba + 1;
    var state = try loadOrInitializeVolume(&disk, region, capacity_blocks);
    for (specs.items) |*spec| try upsertFile(&disk, &state, capacity_blocks, spec);
    try disk.sync();

    for (specs.items) |spec| {
        std.debug.print("rootfs_put: wrote {s} from {s} into partition {d}\n", .{ spec.image_path, spec.source_path, partition_index });
    }
}
