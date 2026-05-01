const std = @import("std");
const rootfs_host = @import("rootfs_host");

const bytes_per_sector: usize = rootfs_host.sector_bytes;
const sectors_per_cluster: u8 = 8;
const reserved_sector_count: u16 = 32;
const fat_count: u8 = 2;
const media_descriptor: u8 = 0xF8;
const fat32_eoc: u32 = 0x0FFF_FFFF;
const boot_signature: u8 = 0x29;
const volume_id: u32 = 0x5246_5331;
const fixed_date: u16 = ((2026 - 1980) << 9) | (1 << 5) | 1;
const fixed_time: u16 = 0;
const dir_attr: u8 = 0x10;
const archive_attr: u8 = 0x20;
const lfn_attr: u8 = 0x0F;
const dir_entries_per_cluster: usize = (bytes_per_sector * sectors_per_cluster) / 32;

const Dirent = extern struct {
    name: [11]u8,
    attr: u8,
    nt_res: u8,
    crt_time_tenth: u8,
    crt_time: u16,
    crt_date: u16,
    last_access_date: u16,
    first_cluster_hi: u16,
    write_time: u16,
    write_date: u16,
    first_cluster_lo: u16,
    file_size: u32,
};

const Directory = struct {
    path: []u8,
    parent_index: ?usize,
    leaf: []u8,
    short_name: [11]u8,
    cluster: u32 = 0,
};

const LoadedSpec = struct {
    image_path: []u8,
    source_path: []u8,
    data: []u8,
    parent_dir_index: usize,
    leaf: []u8,
    short_name: [11]u8,
    start_cluster: u32 = 0,
    cluster_count: u32 = 0,
};

const VolumeLayout = struct {
    partition_first_lba: u64,
    total_sectors: u32,
    sectors_per_fat: u32,
    total_clusters: u32,
    fat_start_sector: u32,
    data_start_sector: u32,
    root_cluster: u32,

    fn clusterToSector(self: VolumeLayout, cluster: u32) u32 {
        std.debug.assert(cluster >= 2);
        return self.data_start_sector + (cluster - 2) * sectors_per_cluster;
    }
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

fn setU16Le(bytes: []u8, offset: usize, value: u16) void {
    std.mem.writeInt(u16, bytes[offset..][0..2], value, .little);
}

fn setU32Le(bytes: []u8, offset: usize, value: u32) void {
    std.mem.writeInt(u32, bytes[offset..][0..4], value, .little);
}

fn parseManifestLine(line: []const u8) !?struct { image_path: []const u8, source_path: []const u8 } {
    const trimmed = std.mem.trim(u8, line, " \t\r");
    if (trimmed.len == 0 or trimmed[0] == '#') return null;
    const eq_index = std.mem.indexOfScalar(u8, trimmed, '=') orelse return error.InvalidManifestLine;
    const image_path = std.mem.trim(u8, trimmed[0..eq_index], " \t");
    const source_path = std.mem.trim(u8, trimmed[eq_index + 1 ..], " \t");
    if (image_path.len == 0 or source_path.len == 0) return error.InvalidManifestLine;
    return .{ .image_path = image_path, .source_path = source_path };
}

fn resolveManifestSourcePath(allocator: std.mem.Allocator, manifest_path: []const u8, source_path: []const u8) ![]u8 {
    if (std.fs.path.isAbsolute(source_path)) return allocator.dupe(u8, source_path);
    const manifest_dir = std.fs.path.dirname(manifest_path) orelse ".";
    return std.fs.path.join(allocator, &.{ manifest_dir, source_path });
}

fn splitParentPath(path: []const u8) struct { parent: []const u8, leaf: []const u8 } {
    const last_sep = std.mem.lastIndexOfScalar(u8, path, '/') orelse 0;
    const leaf = path[last_sep + 1 ..];
    const parent = if (last_sep == 0) "/" else path[0..last_sep];
    return .{ .parent = parent, .leaf = leaf };
}

fn findDirectory(dirs: []const Directory, path: []const u8) ?usize {
    for (dirs, 0..) |dir, index| {
        if (std.mem.eql(u8, dir.path, path)) return index;
    }
    return null;
}

fn uppercaseAscii(byte: u8) u8 {
    if (byte >= 'a' and byte <= 'z') return byte - 32;
    return byte;
}

fn makeShortName(serial: usize, is_dir: bool, ext_hint: []const u8) [11]u8 {
    var out = [_]u8{' '} ** 11;
    const prefix = if (is_dir) "D" else "F";
    var buf: [8]u8 = [_]u8{' '} ** 8;
    const text = std.fmt.bufPrint(&buf, "{s}{d:0>7}", .{ prefix, serial }) catch unreachable;
    @memcpy(out[0..text.len], text);
    const ext_len: usize = @min(ext_hint.len, 3);
    var i: usize = 0;
    while (i < ext_len) : (i += 1) out[8 + i] = uppercaseAscii(ext_hint[i]);
    return out;
}

fn extensionOf(name: []const u8) []const u8 {
    const dot = std.mem.lastIndexOfScalar(u8, name, '.') orelse return "";
    if (dot + 1 >= name.len) return "";
    return name[dot + 1 ..];
}

fn ensureDirectory(allocator: std.mem.Allocator, dirs: *std.ArrayList(Directory), path: []const u8) !usize {
    if (findDirectory(dirs.items, path)) |index| return index;
    const split = splitParentPath(path);
    const parent_index = try ensureDirectory(allocator, dirs, split.parent);
    const path_copy = try allocator.dupe(u8, path);
    errdefer allocator.free(path_copy);
    const leaf_copy = try allocator.dupe(u8, split.leaf);
    errdefer allocator.free(leaf_copy);
    try dirs.append(allocator, .{
        .path = path_copy,
        .parent_index = parent_index,
        .leaf = leaf_copy,
        .short_name = makeShortName(dirs.items.len, true, ""),
    });
    return dirs.items.len - 1;
}

fn loadManifest(allocator: std.mem.Allocator, manifest_path: []const u8, dirs: *std.ArrayList(Directory)) !std.ArrayList(LoadedSpec) {
    var specs = std.ArrayList(LoadedSpec).empty;
    errdefer {
        for (specs.items) |spec| {
            allocator.free(spec.image_path);
            allocator.free(spec.source_path);
            allocator.free(spec.data);
            allocator.free(spec.leaf);
        }
        specs.deinit(allocator);
    }

    try dirs.append(allocator, .{
        .path = try allocator.dupe(u8, "/"),
        .parent_index = null,
        .leaf = try allocator.dupe(u8, ""),
        .short_name = [_]u8{' '} ** 11,
        .cluster = 2,
    });

    const cwd = std.fs.cwd();
    const manifest_bytes = try cwd.readFileAlloc(allocator, manifest_path, std.math.maxInt(usize));
    defer allocator.free(manifest_bytes);

    var serial: usize = 1;
    var line_it = std.mem.splitScalar(u8, manifest_bytes, '\n');
    while (line_it.next()) |line| {
        const parsed = try parseManifestLine(line) orelse continue;
        try rootfs_host.validateImagePath(parsed.image_path);
        const split = splitParentPath(parsed.image_path);
        const parent_dir_index = try ensureDirectory(allocator, dirs, split.parent);
        if (std.mem.eql(u8, parsed.source_path, rootfs_host.directory_source_token)) continue;

        const image_path_copy = try allocator.dupe(u8, parsed.image_path);
        errdefer allocator.free(image_path_copy);
        const leaf_copy = try allocator.dupe(u8, split.leaf);
        errdefer allocator.free(leaf_copy);
        const resolved_source_path = try resolveManifestSourcePath(allocator, manifest_path, parsed.source_path);
        defer allocator.free(resolved_source_path);
        const source_path_copy = try allocator.dupe(u8, resolved_source_path);
        errdefer allocator.free(source_path_copy);
        const data = try cwd.readFileAlloc(allocator, resolved_source_path, std.math.maxInt(usize));
        errdefer allocator.free(data);
        try specs.append(allocator, .{
            .image_path = image_path_copy,
            .source_path = source_path_copy,
            .data = data,
            .parent_dir_index = parent_dir_index,
            .leaf = leaf_copy,
            .short_name = makeShortName(serial, false, extensionOf(split.leaf)),
        });
        serial += 1;
    }
    return specs;
}

fn computeFat32Layout(region: rootfs_host.PartitionRegion) !VolumeLayout {
    const total_sectors_u64 = region.last_lba - region.first_lba + 1;
    if (total_sectors_u64 > std.math.maxInt(u32)) return error.PartitionTooLarge;
    const total_sectors: u32 = @intCast(total_sectors_u64);
    var sectors_per_fat: u32 = 1;
    while (true) {
        const data_sectors = total_sectors - reserved_sector_count - @as(u32, fat_count) * sectors_per_fat;
        const cluster_count = data_sectors / sectors_per_cluster;
        const needed_fat_bytes = (cluster_count + 2) * 4;
        const needed_fat_sectors = (needed_fat_bytes + bytes_per_sector - 1) / bytes_per_sector;
        if (needed_fat_sectors == sectors_per_fat) {
            if (cluster_count < 65525) return error.UnsupportedFat32Layout;
            return .{
                .partition_first_lba = region.first_lba,
                .total_sectors = total_sectors,
                .sectors_per_fat = sectors_per_fat,
                .total_clusters = cluster_count,
                .fat_start_sector = reserved_sector_count,
                .data_start_sector = reserved_sector_count + @as(u32, fat_count) * sectors_per_fat,
                .root_cluster = 2,
            };
        }
        sectors_per_fat = @intCast(needed_fat_sectors);
    }
}

fn partitionOffset(layout: VolumeLayout, sector_index: u32) u64 {
    return (layout.partition_first_lba + sector_index) * bytes_per_sector;
}

fn writeBootSector(file: *std.fs.File, layout: VolumeLayout) !void {
    var sector = [_]u8{0} ** bytes_per_sector;
    sector[0] = 0xEB;
    sector[1] = 0x58;
    sector[2] = 0x90;
    @memcpy(sector[3..11], "CAPOS   ");
    setU16Le(&sector, 11, bytes_per_sector);
    sector[13] = sectors_per_cluster;
    setU16Le(&sector, 14, reserved_sector_count);
    sector[16] = fat_count;
    setU16Le(&sector, 17, 0);
    setU16Le(&sector, 19, 0);
    sector[21] = media_descriptor;
    setU16Le(&sector, 22, 0);
    setU16Le(&sector, 24, 63);
    setU16Le(&sector, 26, 255);
    setU32Le(&sector, 28, @intCast(layout.partition_first_lba));
    setU32Le(&sector, 32, layout.total_sectors);
    setU32Le(&sector, 36, layout.sectors_per_fat);
    setU16Le(&sector, 40, 0);
    setU16Le(&sector, 42, 0);
    setU32Le(&sector, 44, layout.root_cluster);
    setU16Le(&sector, 48, 1);
    setU16Le(&sector, 50, 6);
    sector[64] = 0x80;
    sector[66] = boot_signature;
    setU32Le(&sector, 67, volume_id);
    @memcpy(sector[71..82], "CAPROOTFS  ");
    @memcpy(sector[82..90], "FAT32   ");
    sector[510] = 0x55;
    sector[511] = 0xAA;
    try writeAllAt(file, partitionOffset(layout, 0), &sector);

    var fsinfo = [_]u8{0} ** bytes_per_sector;
    setU32Le(&fsinfo, 0, 0x41615252);
    setU32Le(&fsinfo, 484, 0x61417272);
    setU32Le(&fsinfo, 488, 0xFFFF_FFFF);
    setU32Le(&fsinfo, 492, 0xFFFF_FFFF);
    setU32Le(&fsinfo, 508, 0xAA55_0000);
    try writeAllAt(file, partitionOffset(layout, 1), &fsinfo);
    try writeAllAt(file, partitionOffset(layout, 6), &sector);
}

fn lfnChecksum(short_name: [11]u8) u8 {
    var sum: u8 = 0;
    for (short_name) |byte| sum = ((sum & 1) << 7) + (sum >> 1) +% byte;
    return sum;
}

fn putLfnChar(entry: *[32]u8, slot: usize, ch: u16) void {
    const offsets = [_]usize{ 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
    std.mem.writeInt(u16, entry[offsets[slot]..][0..2], ch, .little);
}

fn appendLfnEntries(entries: *std.ArrayList([32]u8), allocator: std.mem.Allocator, name: []const u8, short_name: [11]u8) !void {
    const lfn_count = (name.len + 12) / 13;
    const checksum = lfnChecksum(short_name);
    var remaining = lfn_count;
    while (remaining > 0) {
        remaining -= 1;
        var entry = [_]u8{0xFF} ** 32;
        const seq: u8 = @intCast(remaining + 1);
        entry[0] = seq | if (remaining + 1 == lfn_count) @as(u8, 0x40) else 0;
        entry[11] = lfn_attr;
        entry[12] = 0;
        entry[13] = checksum;
        entry[26] = 0;
        entry[27] = 0;
        const start = remaining * 13;
        var i: usize = 0;
        while (i < 13) : (i += 1) {
            const index = start + i;
            const ch: u16 = if (index < name.len) name[index] else if (index == name.len) 0 else 0xFFFF;
            putLfnChar(&entry, i, ch);
        }
        try entries.append(allocator, entry);
    }
}

fn makeDirent(short_name: [11]u8, attr: u8, cluster: u32, file_size: u32) [32]u8 {
    var entry = [_]u8{0} ** 32;
    @memcpy(entry[0..11], &short_name);
    entry[11] = attr;
    setU16Le(&entry, 14, fixed_time);
    setU16Le(&entry, 16, fixed_date);
    setU16Le(&entry, 18, fixed_date);
    setU16Le(&entry, 20, @intCast(cluster >> 16));
    setU16Le(&entry, 22, fixed_time);
    setU16Le(&entry, 24, fixed_date);
    setU16Le(&entry, 26, @intCast(cluster & 0xFFFF));
    setU32Le(&entry, 28, file_size);
    return entry;
}

fn addNamedDirent(entries: *std.ArrayList([32]u8), allocator: std.mem.Allocator, name: []const u8, short_name: [11]u8, attr: u8, cluster: u32, file_size: u32) !void {
    try appendLfnEntries(entries, allocator, name, short_name);
    try entries.append(allocator, makeDirent(short_name, attr, cluster, file_size));
}

fn writeDirectoryCluster(file: *std.fs.File, layout: VolumeLayout, allocator: std.mem.Allocator, dir_index: usize, dirs: []const Directory, specs: []const LoadedSpec) !void {
    var entries = std.ArrayList([32]u8).empty;
    defer entries.deinit(allocator);

    if (dir_index != 0) {
        try entries.append(allocator, makeDirent([_]u8{'.'} ++ [_]u8{' '} ** 10, dir_attr, dirs[dir_index].cluster, 0));
        const parent_cluster = if (dirs[dir_index].parent_index) |parent| dirs[parent].cluster else 0;
        var dotdot = [_]u8{' '} ** 11;
        dotdot[0] = '.';
        dotdot[1] = '.';
        try entries.append(allocator, makeDirent(dotdot, dir_attr, parent_cluster, 0));
    }

    for (dirs, 0..) |dir, index| {
        if (index == 0 or dir.parent_index == null or dir.parent_index.? != dir_index) continue;
        try addNamedDirent(&entries, allocator, dir.leaf, dir.short_name, dir_attr, dir.cluster, 0);
    }
    for (specs) |spec| {
        if (spec.parent_dir_index != dir_index) continue;
        if (spec.data.len > std.math.maxInt(u32)) return error.FileTooLarge;
        try addNamedDirent(&entries, allocator, spec.leaf, spec.short_name, archive_attr, spec.start_cluster, @intCast(spec.data.len));
    }
    if (entries.items.len > dir_entries_per_cluster) return error.DirectoryTooLarge;

    var cluster_bytes = [_]u8{0} ** (bytes_per_sector * sectors_per_cluster);
    for (entries.items, 0..) |entry, i| {
        const start = i * 32;
        @memcpy(cluster_bytes[start .. start + 32], &entry);
    }
    try writeAllAt(file, partitionOffset(layout, layout.clusterToSector(dirs[dir_index].cluster)), &cluster_bytes);
}

fn writeFatTables(file: *std.fs.File, layout: VolumeLayout, fat: []const u32) !void {
    var sector = [_]u8{0} ** bytes_per_sector;
    var fat_index: u32 = 0;
    while (fat_index < fat_count) : (fat_index += 1) {
        var sector_index: u32 = 0;
        while (sector_index < layout.sectors_per_fat) : (sector_index += 1) {
            @memset(&sector, 0);
            var entry_index: usize = 0;
            while (entry_index < bytes_per_sector / 4) : (entry_index += 1) {
                const fat_entry_index = sector_index * (bytes_per_sector / 4) + entry_index;
                if (fat_entry_index >= fat.len) break;
                setU32Le(&sector, entry_index * 4, fat[fat_entry_index]);
            }
            try writeAllAt(file, partitionOffset(layout, layout.fat_start_sector + fat_index * layout.sectors_per_fat + sector_index), &sector);
        }
    }
}

fn writeFileData(file: *std.fs.File, layout: VolumeLayout, specs: []const LoadedSpec) !void {
    for (specs) |spec| {
        if (spec.cluster_count == 0) continue;
        var offset: usize = 0;
        var cluster_offset: u32 = 0;
        while (cluster_offset < spec.cluster_count) : (cluster_offset += 1) {
            var cluster_bytes = [_]u8{0} ** (bytes_per_sector * sectors_per_cluster);
            const remaining = spec.data.len - offset;
            const chunk = @min(remaining, cluster_bytes.len);
            if (chunk > 0) @memcpy(cluster_bytes[0..chunk], spec.data[offset .. offset + chunk]);
            try writeAllAt(file, partitionOffset(layout, layout.clusterToSector(spec.start_cluster + cluster_offset)), &cluster_bytes);
            offset += chunk;
        }
    }
}

pub fn main() !void {
    var gpa = std.heap.GeneralPurposeAllocator(.{}){};
    defer _ = gpa.deinit();
    const allocator = gpa.allocator();
    const args = try std.process.argsAlloc(allocator);
    defer std.process.argsFree(allocator, args);
    if (args.len != 4) {
        std.debug.print("usage: {s} <disk_img> <partition_index> <manifest_path>\n", .{args[0]});
        return error.InvalidArgument;
    }

    const partition_index = try std.fmt.parseUnsigned(u32, args[2], 10);
    var dirs = std.ArrayList(Directory).empty;
    defer {
        for (dirs.items) |dir| {
            allocator.free(dir.path);
            allocator.free(dir.leaf);
        }
        dirs.deinit(allocator);
    }
    var specs = try loadManifest(allocator, args[3], &dirs);
    defer {
        for (specs.items) |spec| {
            allocator.free(spec.image_path);
            allocator.free(spec.source_path);
            allocator.free(spec.data);
            allocator.free(spec.leaf);
        }
        specs.deinit(allocator);
    }

    var disk = try std.fs.cwd().openFile(args[1], .{ .mode = .read_write });
    defer disk.close();
    const region = try rootfs_host.openPartitionRegion(&disk, partition_index);
    const layout = try computeFat32Layout(region);

    var next_cluster: u32 = 2;
    for (dirs.items) |*dir| {
        dir.cluster = next_cluster;
        next_cluster += 1;
    }
    dirs.items[0].cluster = layout.root_cluster;
    next_cluster = @max(next_cluster, layout.root_cluster + 1);
    for (specs.items) |*spec| {
        const cluster_bytes = bytes_per_sector * sectors_per_cluster;
        const cluster_count = (spec.data.len + cluster_bytes - 1) / cluster_bytes;
        if (cluster_count > std.math.maxInt(u32)) return error.FileTooLarge;
        spec.cluster_count = @intCast(cluster_count);
        if (spec.cluster_count == 0) {
            spec.start_cluster = 0;
        } else {
            spec.start_cluster = next_cluster;
            next_cluster += spec.cluster_count;
        }
    }
    if (next_cluster - 2 > layout.total_clusters) return error.NoSpaceLeft;

    const fat_len: usize = @intCast(layout.total_clusters + 2);
    var fat = try allocator.alloc(u32, fat_len);
    defer allocator.free(fat);
    @memset(fat, 0);
    fat[0] = @as(u32, 0x0FFF_FF00) | @as(u32, media_descriptor);
    fat[1] = fat32_eoc;
    for (dirs.items) |dir| fat[dir.cluster] = fat32_eoc;
    for (specs.items) |spec| {
        if (spec.cluster_count == 0) continue;
        var i: u32 = 0;
        while (i < spec.cluster_count) : (i += 1) {
            const cluster = spec.start_cluster + i;
            fat[cluster] = if (i + 1 == spec.cluster_count) fat32_eoc else cluster + 1;
        }
    }

    try writeBootSector(&disk, layout);
    try writeFatTables(&disk, layout, fat);
    for (dirs.items, 0..) |_, index| try writeDirectoryCluster(&disk, layout, allocator, index, dirs.items, specs.items);
    try writeFileData(&disk, layout, specs.items);
    try disk.sync();
    std.debug.print("rootfs_builder: wrote FAT32 {d} file(s) and {d} dir(s) into partition {d} from {s}\n", .{ specs.items.len, dirs.items.len, partition_index, args[3] });
}
