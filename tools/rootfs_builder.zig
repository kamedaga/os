const std = @import("std");
const rootfs_host = @import("rootfs_host");

fn lessThan(lhs: []const u8, rhs: []const u8) bool {
    return std.mem.order(u8, lhs, rhs) == .lt;
}

fn insertionSortByPath(items: []rootfs_host.FileSpec) void {
    var i: usize = 1;
    while (i < items.len) : (i += 1) {
        var j = i;
        while (j > 0 and lessThan(items[j].image_path, items[j - 1].image_path)) : (j -= 1) {
            const tmp = items[j - 1];
            items[j - 1] = items[j];
            items[j] = tmp;
        }
    }
}

fn parseManifestLine(line: []const u8) !?struct { image_path: []const u8, source_path: []const u8 } {
    const trimmed = std.mem.trim(u8, line, " \t\r");
    if (trimmed.len == 0 or trimmed[0] == '#') return null;
    const eq_index = std.mem.indexOfScalar(u8, trimmed, '=') orelse return error.InvalidManifestLine;
    const image_path = std.mem.trim(u8, trimmed[0..eq_index], " \t");
    const source_path = std.mem.trim(u8, trimmed[eq_index + 1 ..], " \t");
    if (image_path.len == 0 or source_path.len == 0) return error.InvalidManifestLine;
    return .{
        .image_path = image_path,
        .source_path = source_path,
    };
}

fn resolveManifestSourcePath(allocator: std.mem.Allocator, manifest_path: []const u8, source_path: []const u8) ![]u8 {
    if (std.fs.path.isAbsolute(source_path)) return allocator.dupe(u8, source_path);
    const manifest_dir = std.fs.path.dirname(manifest_path) orelse ".";
    return std.fs.path.join(allocator, &.{ manifest_dir, source_path });
}

fn loadManifestSpecs(allocator: std.mem.Allocator, manifest_path: []const u8) !std.ArrayList(rootfs_host.FileSpec) {
    var specs = std.ArrayList(rootfs_host.FileSpec).empty;
    errdefer {
        for (specs.items) |spec| {
            allocator.free(spec.image_path);
            allocator.free(spec.root_name);
            allocator.free(spec.source_path);
            allocator.free(spec.data);
        }
        specs.deinit(allocator);
    }

    const cwd = std.fs.cwd();
    const manifest_bytes = try cwd.readFileAlloc(allocator, manifest_path, std.math.maxInt(usize));
    defer allocator.free(manifest_bytes);

    var line_it = std.mem.splitScalar(u8, manifest_bytes, '\n');
    var line_number: usize = 0;
    while (line_it.next()) |line| {
        line_number += 1;
        const parsed = parseManifestLine(line) catch {
            std.debug.print("{s}:{d}: invalid rootfs manifest line\n", .{ manifest_path, line_number });
            return error.InvalidManifestLine;
        };
        if (parsed == null) continue;
        const image_path = parsed.?.image_path;
        const source_path = parsed.?.source_path;
        const root_name = try rootfs_host.validateRootImagePath(image_path);
        const resolved_source_path = try resolveManifestSourcePath(allocator, manifest_path, source_path);
        defer allocator.free(resolved_source_path);
        const data = try cwd.readFileAlloc(allocator, resolved_source_path, std.math.maxInt(usize));
        const image_path_copy = try allocator.dupe(u8, image_path);
        errdefer allocator.free(image_path_copy);
        const root_name_copy = try allocator.dupe(u8, root_name);
        errdefer allocator.free(root_name_copy);
        const source_path_copy = try allocator.dupe(u8, resolved_source_path);
        errdefer allocator.free(source_path_copy);
        try specs.append(allocator, .{
            .image_path = image_path_copy,
            .root_name = root_name_copy,
            .source_path = source_path_copy,
            .data = data,
        });
    }

    insertionSortByPath(specs.items);
    var i: usize = 1;
    while (i < specs.items.len) : (i += 1) {
        if (std.mem.eql(u8, specs.items[i - 1].image_path, specs.items[i].image_path)) {
            std.debug.print("duplicate rootfs path in manifest: {s}\n", .{specs.items[i].image_path});
            return error.DuplicateImagePath;
        }
    }

    return specs;
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
    var specs = try loadManifestSpecs(allocator, args[3]);
    defer {
        for (specs.items) |spec| {
            allocator.free(spec.image_path);
            allocator.free(spec.root_name);
            allocator.free(spec.source_path);
            allocator.free(spec.data);
        }
        specs.deinit(allocator);
    }

    var disk = try std.fs.cwd().openFile(args[1], .{ .mode = .read_write });
    defer disk.close();

    const target = try rootfs_host.preparePartition(&disk, partition_index);
    var state = try rootfs_host.formatVolume(&disk, target.region, target.capacity_blocks);
    for (specs.items) |*spec| try rootfs_host.upsertFile(&disk, &state, target.capacity_blocks, spec);
    try disk.sync();

    std.debug.print("rootfs_builder: wrote {d} file(s) into partition {d} from {s}\n", .{ specs.items.len, partition_index, args[3] });
}
