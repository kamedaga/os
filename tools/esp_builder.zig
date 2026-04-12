const std = @import("std");
const rootfs_host = @import("rootfs_host");

const bytes_per_sector: usize = rootfs_host.sector_bytes;
const sectors_per_cluster: u8 = 8;
const reserved_sector_count: u16 = 1;
const fat_count: u8 = 2;
const root_entry_count: u16 = 512;
const dir_attr: u8 = 0x10;
const archive_attr: u8 = 0x20;
const media_descriptor: u8 = 0xF8;
const fat16_eoc: u16 = 0xFFFF;
const boot_signature: u8 = 0x29;
const volume_id: u32 = 0x4341_504F;
const root_dir_sectors: u32 = (@as(u32, root_entry_count) * 32 + (bytes_per_sector - 1)) / bytes_per_sector;
const dir_entries_per_cluster: usize = (bytes_per_sector * sectors_per_cluster) / 32;
const fixed_date: u16 = ((2026 - 1980) << 9) | (1 << 5) | 1;
const fixed_time: u16 = 0;

const LoadedSpec = struct {
    image_path: []u8,
    source_path: []u8,
    data: []u8,
    parent_dir_index: usize,
    short_name: [11]u8,
    start_cluster: u16 = 0,
    cluster_count: u16 = 0,
};

const Directory = struct {
    path: []u8,
    parent_index: ?usize,
    short_name: [11]u8,
    cluster: u16 = 0,
};

const VolumeLayout = struct {
    partition_first_lba: u64,
    total_sectors: u32,
    sectors_per_fat: u16,
    total_clusters: u32,
    fat_start_sector: u32,
    root_dir_start_sector: u32,
    data_start_sector: u32,

    fn clusterToSector(self: VolumeLayout, cluster: u16) u32 {
        std.debug.assert(cluster >= 2);
        return self.data_start_sector + (@as(u32, cluster) - 2) * sectors_per_cluster;
    }
};

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

fn upperAscii(byte: u8) u8 {
    if (byte >= 'a' and byte <= 'z') return byte - 32;
    return byte;
}

fn isSupportedShortNameByte(byte: u8) bool {
    const upper = upperAscii(byte);
    return (upper >= 'A' and upper <= 'Z') or
        (upper >= '0' and upper <= '9') or
        upper == '_' or
        upper == '-';
}

fn encodeShortName(component: []const u8) ![11]u8 {
    if (component.len == 0 or std.mem.eql(u8, component, ".") or std.mem.eql(u8, component, "..")) {
        return error.InvalidImagePath;
    }
    var out = [_]u8{' '} ** 11;
    const dot_index = std.mem.lastIndexOfScalar(u8, component, '.');
    const name = if (dot_index) |index| component[0..index] else component;
    const ext = if (dot_index) |index| component[index + 1 ..] else "";
    if (name.len == 0 or name.len > 8 or ext.len > 3) return error.UnsupportedShortName;
    for (name, 0..) |byte, i| {
        if (!isSupportedShortNameByte(byte)) return error.UnsupportedShortName;
        out[i] = upperAscii(byte);
    }
    for (ext, 0..) |byte, i| {
        if (!isSupportedShortNameByte(byte)) return error.UnsupportedShortName;
        out[8 + i] = upperAscii(byte);
    }
    return out;
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

fn ensureDirectory(allocator: std.mem.Allocator, dirs: *std.ArrayList(Directory), path: []const u8) !usize {
    if (findDirectory(dirs.items, path)) |index| return index;
    const split = splitParentPath(path);
    const parent_index = try ensureDirectory(allocator, dirs, split.parent);
    const path_copy = try allocator.dupe(u8, path);
    errdefer allocator.free(path_copy);
    try dirs.append(allocator, .{
        .path = path_copy,
        .parent_index = parent_index,
        .short_name = try encodeShortName(split.leaf),
    });
    return dirs.items.len - 1;
}

fn loadManifest(
    allocator: std.mem.Allocator,
    manifest_path: []const u8,
    dirs: *std.ArrayList(Directory),
) !std.ArrayList(LoadedSpec) {
    var specs = std.ArrayList(LoadedSpec).empty;
    errdefer {
        for (specs.items) |spec| {
            allocator.free(spec.image_path);
            allocator.free(spec.source_path);
            allocator.free(spec.data);
        }
        specs.deinit(allocator);
    }

    try dirs.append(allocator, .{
        .path = try allocator.dupe(u8, "/"),
        .parent_index = null,
        .short_name = [_]u8{' '} ** 11,
    });

    const cwd = std.fs.cwd();
    const manifest_bytes = try cwd.readFileAlloc(allocator, manifest_path, std.math.maxInt(usize));
    defer allocator.free(manifest_bytes);

    var line_it = std.mem.splitScalar(u8, manifest_bytes, '\n');
    while (line_it.next()) |line| {
        const parsed = try parseManifestLine(line) orelse continue;
        if (parsed.image_path.len < 2 or parsed.image_path[0] != '/') return error.InvalidImagePath;
        const split = splitParentPath(parsed.image_path);
        const parent_dir_index = try ensureDirectory(allocator, dirs, split.parent);
        const image_path_copy = try allocator.dupe(u8, parsed.image_path);
        errdefer allocator.free(image_path_copy);
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
            .short_name = try encodeShortName(split.leaf),
        });
    }
    return specs;
}

fn computeFat16Layout(region: rootfs_host.PartitionRegion) !VolumeLayout {
    const total_sectors_u64 = region.last_lba - region.first_lba + 1;
    if (total_sectors_u64 > std.math.maxInt(u32)) return error.PartitionTooLarge;
    const total_sectors: u32 = @intCast(total_sectors_u64);
    var sectors_per_fat: u32 = 1;
    while (true) {
        const data_sectors = total_sectors - reserved_sector_count - root_dir_sectors - fat_count * sectors_per_fat;
        const cluster_count = data_sectors / sectors_per_cluster;
        const needed_fat_bytes = (cluster_count + 2) * 2;
        const needed_fat_sectors = (needed_fat_bytes + bytes_per_sector - 1) / bytes_per_sector;
        if (needed_fat_sectors == sectors_per_fat) {
            if (cluster_count < 4085 or cluster_count > 65524) return error.UnsupportedFatLayout;
            return .{
                .partition_first_lba = region.first_lba,
                .total_sectors = total_sectors,
                .sectors_per_fat = @intCast(sectors_per_fat),
                .total_clusters = cluster_count,
                .fat_start_sector = reserved_sector_count,
                .root_dir_start_sector = reserved_sector_count + fat_count * sectors_per_fat,
                .data_start_sector = reserved_sector_count + fat_count * sectors_per_fat + root_dir_sectors,
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
    sector[1] = 0x3C;
    sector[2] = 0x90;
    @memcpy(sector[3..11], "CAPOS   ");
    setU16Le(&sector, 11, bytes_per_sector);
    sector[13] = sectors_per_cluster;
    setU16Le(&sector, 14, reserved_sector_count);
    sector[16] = fat_count;
    setU16Le(&sector, 17, root_entry_count);
    setU16Le(&sector, 19, 0);
    sector[21] = media_descriptor;
    setU16Le(&sector, 22, layout.sectors_per_fat);
    setU16Le(&sector, 24, 63);
    setU16Le(&sector, 26, 255);
    setU32Le(&sector, 28, @intCast(layout.partition_first_lba));
    setU32Le(&sector, 32, layout.total_sectors);
    sector[36] = 0x80;
    sector[38] = boot_signature;
    setU32Le(&sector, 39, volume_id);
    @memcpy(sector[43..54], "CAPABILITY ");
    @memcpy(sector[54..62], "FAT16   ");
    sector[510] = 0x55;
    sector[511] = 0xAA;
    try writeAllAt(file, partitionOffset(layout, 0), &sector);
}

fn buildDirent(short_name: [11]u8, attr: u8, cluster: u16, file_size: u32) Dirent {
    return .{
        .name = short_name,
        .attr = attr,
        .nt_res = 0,
        .crt_time_tenth = 0,
        .crt_time = fixed_time,
        .crt_date = fixed_date,
        .last_access_date = fixed_date,
        .first_cluster_hi = 0,
        .write_time = fixed_time,
        .write_date = fixed_date,
        .first_cluster_lo = cluster,
        .file_size = file_size,
    };
}

fn writeDirent(bytes: []u8, index: usize, entry: Dirent) void {
    const start = index * @sizeOf(Dirent);
    const end = start + @sizeOf(Dirent);
    @memcpy(bytes[start..end], std.mem.asBytes(&entry));
}

fn setDotName(name: *[11]u8, parent: bool) void {
    name.* = [_]u8{' '} ** 11;
    name[0] = '.';
    if (parent) name[1] = '.';
}

fn writeFatTables(file: *std.fs.File, layout: VolumeLayout, fat: []const u16) !void {
    var sector = [_]u8{0} ** bytes_per_sector;
    var fat_index: u32 = 0;
    while (fat_index < fat_count) : (fat_index += 1) {
        var sector_index: u32 = 0;
        while (sector_index < layout.sectors_per_fat) : (sector_index += 1) {
            @memset(&sector, 0);
            var entry_index: usize = 0;
            while (entry_index < bytes_per_sector / 2) : (entry_index += 1) {
                const fat_entry_index = sector_index * (bytes_per_sector / 2) + entry_index;
                if (fat_entry_index >= fat.len) break;
                setU16Le(&sector, entry_index * 2, fat[fat_entry_index]);
            }
            const rel_sector = layout.fat_start_sector + fat_index * layout.sectors_per_fat + sector_index;
            try writeAllAt(file, partitionOffset(layout, rel_sector), &sector);
        }
    }
}

fn writeRootDirectory(file: *std.fs.File, layout: VolumeLayout, dirs: []const Directory, specs: []const LoadedSpec) !void {
    var root_bytes = [_]u8{0} ** (root_dir_sectors * bytes_per_sector);
    var entry_index: usize = 0;
    for (dirs[1..]) |dir| {
        if (dir.parent_index.? != 0) continue;
        writeDirent(root_bytes[0..], entry_index, buildDirent(dir.short_name, dir_attr, dir.cluster, 0));
        entry_index += 1;
    }
    for (specs) |spec| {
        if (spec.parent_dir_index != 0) continue;
        writeDirent(root_bytes[0..], entry_index, buildDirent(spec.short_name, archive_attr, spec.start_cluster, @intCast(spec.data.len)));
        entry_index += 1;
    }
    try writeAllAt(file, partitionOffset(layout, layout.root_dir_start_sector), root_bytes[0..]);
}

fn countChildEntries(parent_dir_index: usize, dirs: []const Directory, specs: []const LoadedSpec) usize {
    var count: usize = 2;
    for (dirs[1..]) |dir| {
        if (dir.parent_index.? == parent_dir_index) count += 1;
    }
    for (specs) |spec| {
        if (spec.parent_dir_index == parent_dir_index) count += 1;
    }
    return count;
}

fn writeSubdirectories(file: *std.fs.File, layout: VolumeLayout, dirs: []const Directory, specs: []const LoadedSpec) !void {
    for (dirs[1..], 1..) |dir, dir_index| {
        if (countChildEntries(dir_index, dirs, specs) > dir_entries_per_cluster) return error.DirectoryTooLarge;
        var bytes = [_]u8{0} ** (bytes_per_sector * sectors_per_cluster);
        var dot = [_]u8{' '} ** 11;
        var dotdot = [_]u8{' '} ** 11;
        setDotName(&dot, false);
        setDotName(&dotdot, true);
        writeDirent(bytes[0..], 0, buildDirent(dot, dir_attr, dir.cluster, 0));
        const parent_cluster = if (dir.parent_index.? == 0) 0 else dirs[dir.parent_index.?].cluster;
        writeDirent(bytes[0..], 1, buildDirent(dotdot, dir_attr, parent_cluster, 0));
        var entry_index: usize = 2;
        for (dirs[1..]) |child_dir| {
            if (child_dir.parent_index.? != dir_index) continue;
            writeDirent(bytes[0..], entry_index, buildDirent(child_dir.short_name, dir_attr, child_dir.cluster, 0));
            entry_index += 1;
        }
        for (specs) |spec| {
            if (spec.parent_dir_index != dir_index) continue;
            writeDirent(bytes[0..], entry_index, buildDirent(spec.short_name, archive_attr, spec.start_cluster, @intCast(spec.data.len)));
            entry_index += 1;
        }
        try writeAllAt(file, partitionOffset(layout, layout.clusterToSector(dir.cluster)), bytes[0..]);
    }
}

fn writeFileData(file: *std.fs.File, layout: VolumeLayout, specs: []const LoadedSpec) !void {
    for (specs) |spec| {
        if (spec.cluster_count == 0) continue;
        var offset: usize = 0;
        var cluster_offset: u16 = 0;
        while (cluster_offset < spec.cluster_count) : (cluster_offset += 1) {
            var cluster_bytes = [_]u8{0} ** (bytes_per_sector * sectors_per_cluster);
            const chunk = @min(spec.data.len - offset, cluster_bytes.len);
            @memcpy(cluster_bytes[0..chunk], spec.data[offset .. offset + chunk]);
            try writeAllAt(file, partitionOffset(layout, layout.clusterToSector(spec.start_cluster + cluster_offset)), cluster_bytes[0..]);
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
        for (dirs.items) |dir| allocator.free(dir.path);
        dirs.deinit(allocator);
    }
    var specs = try loadManifest(allocator, args[3], &dirs);
    defer {
        for (specs.items) |spec| {
            allocator.free(spec.image_path);
            allocator.free(spec.source_path);
            allocator.free(spec.data);
        }
        specs.deinit(allocator);
    }

    var disk = try std.fs.cwd().openFile(args[1], .{ .mode = .read_write });
    defer disk.close();

    const region = try rootfs_host.openPartitionRegion(&disk, partition_index);
    const layout = try computeFat16Layout(region);

    var next_cluster: u32 = 2;
    for (dirs.items[1..]) |*dir| {
        dir.cluster = @intCast(next_cluster);
        next_cluster += 1;
    }
    for (specs.items) |*spec| {
        const cluster_count_u64 = (spec.data.len + (bytes_per_sector * sectors_per_cluster) - 1) / (bytes_per_sector * sectors_per_cluster);
        if (cluster_count_u64 > std.math.maxInt(u16)) return error.FileTooLarge;
        spec.cluster_count = @intCast(cluster_count_u64);
        if (spec.cluster_count == 0) {
            spec.start_cluster = 0;
        } else {
            spec.start_cluster = @intCast(next_cluster);
            next_cluster += spec.cluster_count;
        }
    }
    if (next_cluster - 2 > layout.total_clusters) return error.NoSpaceLeft;

    const fat_len: usize = @intCast(layout.total_clusters + 2);
    var fat = try allocator.alloc(u16, fat_len);
    defer allocator.free(fat);
    @memset(fat, 0);
    fat[0] = 0xFFF8;
    fat[1] = fat16_eoc;
    for (dirs.items[1..]) |dir| fat[dir.cluster] = fat16_eoc;
    for (specs.items) |spec| {
        if (spec.cluster_count == 0) continue;
        var i: u16 = 0;
        while (i < spec.cluster_count) : (i += 1) {
            const cluster = spec.start_cluster + i;
            fat[cluster] = if (i + 1 == spec.cluster_count) fat16_eoc else cluster + 1;
        }
    }

    try writeBootSector(&disk, layout);
    try writeFatTables(&disk, layout, fat);
    try writeRootDirectory(&disk, layout, dirs.items, specs.items);
    try writeSubdirectories(&disk, layout, dirs.items, specs.items);
    try writeFileData(&disk, layout, specs.items);
    try disk.sync();

    std.debug.print("esp_builder: wrote {d} file(s) into partition {d} from {s}\n", .{ specs.items.len, partition_index, args[3] });
}
