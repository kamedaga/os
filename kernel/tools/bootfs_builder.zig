const std = @import("std");

const bootfs_magic: u32 = 0x5346_5442; // "BTFS"
const bootfs_version: u16 = 1;
const bootfs_kind_regular: u8 = 1;
const bootfs_flag_executable_hint: u8 = 1 << 0;
const bootfs_data_align: u64 = 16;

const BootFsHeader = extern struct {
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

const BootFsEntry = extern struct {
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

const FileSpec = struct {
    image_path: []const u8,
    source_path: []const u8,
    data: []u8,
    path_offset: u32 = 0,
    data_offset: u64 = 0,
};

fn lessThan(lhs: []const u8, rhs: []const u8) bool {
    return std.mem.order(u8, lhs, rhs) == .lt;
}

fn insertionSortByPath(items: []FileSpec) void {
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

fn validateImagePath(path: []const u8) !void {
    if (path.len < 2 or path[0] != '/') return error.InvalidImagePath;
    if (path[path.len - 1] == '/') return error.InvalidImagePath;
    if (std.mem.indexOf(u8, path, "//") != null) return error.InvalidImagePath;
    if (std.mem.indexOf(u8, path, "/./") != null) return error.InvalidImagePath;
    if (std.mem.endsWith(u8, path, "/.")) return error.InvalidImagePath;
    if (std.mem.indexOf(u8, path, "/../") != null) return error.InvalidImagePath;
    if (std.mem.endsWith(u8, path, "/..")) return error.InvalidImagePath;
}

fn modeBitsForPath(path: []const u8) u32 {
    return if (std.mem.endsWith(u8, path, ".elf")) 0o555 else 0o444;
}

fn flagsForPath(path: []const u8) u8 {
    return if (std.mem.endsWith(u8, path, ".elf")) bootfs_flag_executable_hint else 0;
}

pub fn main() !void {
    var gpa = std.heap.GeneralPurposeAllocator(.{}){};
    defer _ = gpa.deinit();
    const allocator = gpa.allocator();

    const args = try std.process.argsAlloc(allocator);
    defer std.process.argsFree(allocator, args);

    if (args.len < 4 or ((args.len - 2) % 2) != 0) {
        std.debug.print("usage: {s} <out> <image_path> <source_path> [...]\n", .{args[0]});
        return error.InvalidArgument;
    }

    var specs = std.ArrayList(FileSpec).empty;
    defer {
        for (specs.items) |spec| allocator.free(spec.data);
        specs.deinit(allocator);
    }

    const cwd = std.fs.cwd();
    var arg_i: usize = 2;
    while (arg_i < args.len) : (arg_i += 2) {
        const image_path = args[arg_i];
        const source_path = args[arg_i + 1];
        try validateImagePath(image_path);
        const data = try cwd.readFileAlloc(allocator, source_path, std.math.maxInt(usize));
        try specs.append(allocator, .{
            .image_path = image_path,
            .source_path = source_path,
            .data = data,
        });
    }

    insertionSortByPath(specs.items);

    var string_table_bytes: usize = 0;
    for (specs.items) |*spec| {
        spec.path_offset = @intCast(string_table_bytes);
        string_table_bytes += spec.image_path.len;
    }

    const header_bytes = @sizeOf(BootFsHeader);
    const entry_bytes_total = @sizeOf(BootFsEntry) * specs.items.len;
    const entry_table_offset: u64 = header_bytes;
    const string_table_offset: u64 = header_bytes + entry_bytes_total;
    const data_offset: u64 = std.mem.alignForward(u64, string_table_offset + string_table_bytes, bootfs_data_align);

    var cursor = data_offset;
    for (specs.items) |*spec| {
        cursor = std.mem.alignForward(u64, cursor, bootfs_data_align);
        spec.data_offset = cursor;
        cursor += spec.data.len;
    }
    const image_bytes = cursor;

    const image = try allocator.alloc(u8, @intCast(image_bytes));
    defer allocator.free(image);
    @memset(image, 0);

    const header = BootFsHeader{
        .magic = bootfs_magic,
        .version = bootfs_version,
        .header_bytes = @sizeOf(BootFsHeader),
        .image_bytes = image_bytes,
        .entry_count = @intCast(specs.items.len),
        .entry_bytes = @intCast(entry_bytes_total),
        .entry_table_offset = entry_table_offset,
        .string_table_offset = string_table_offset,
        .string_table_bytes = string_table_bytes,
        .data_offset = data_offset,
        .data_bytes = image_bytes - data_offset,
        .flags = 0,
    };
    @memcpy(image[0..@sizeOf(BootFsHeader)], std.mem.asBytes(&header));

    for (specs.items, 0..) |spec, idx| {
        const entry = BootFsEntry{
            .path_offset = spec.path_offset,
            .path_bytes = @intCast(spec.image_path.len),
            .kind = bootfs_kind_regular,
            .flags = flagsForPath(spec.image_path),
            .data_offset = spec.data_offset,
            .data_bytes = spec.data.len,
            .mode_bits = modeBitsForPath(spec.image_path),
        };
        const entry_off: usize = @intCast(entry_table_offset + idx * @sizeOf(BootFsEntry));
        @memcpy(image[entry_off .. entry_off + @sizeOf(BootFsEntry)], std.mem.asBytes(&entry));
    }

    for (specs.items) |spec| {
        const string_off: usize = @intCast(string_table_offset + spec.path_offset);
        @memcpy(image[string_off .. string_off + spec.image_path.len], spec.image_path);
        const data_off: usize = @intCast(spec.data_offset);
        @memcpy(image[data_off .. data_off + spec.data.len], spec.data);
    }

    const out_path = args[1];
    if (std.fs.path.dirname(out_path)) |dir| try cwd.makePath(dir);
    const out_file = try cwd.createFile(out_path, .{ .truncate = true });
    defer out_file.close();
    try out_file.writeAll(image);
}
