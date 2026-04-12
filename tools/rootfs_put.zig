const std = @import("std");
const rootfs_host = @import("rootfs_host");

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
    var specs = std.ArrayList(rootfs_host.FileSpec).empty;
    defer {
        for (specs.items) |spec| allocator.free(spec.data);
        specs.deinit(allocator);
    }

    const cwd = std.fs.cwd();
    var arg_i: usize = 3;
    while (arg_i < args.len) : (arg_i += 2) {
        const image_path = args[arg_i];
        const source_path = args[arg_i + 1];
        const root_name = try rootfs_host.validateRootImagePath(image_path);
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

    const target = try rootfs_host.preparePartition(&disk, partition_index);
    var state = try rootfs_host.loadOrInitializeVolume(&disk, target.region, target.capacity_blocks);
    for (specs.items) |*spec| try rootfs_host.upsertFile(&disk, &state, target.capacity_blocks, spec);
    try disk.sync();

    for (specs.items) |spec| {
        std.debug.print("rootfs_put: wrote {s} from {s} into partition {d}\n", .{ spec.image_path, spec.source_path, partition_index });
    }
}
