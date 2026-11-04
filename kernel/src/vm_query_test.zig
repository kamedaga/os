const std = @import("std");
const kernel = @import("kernel.zig");
const vm = @import("memory/user_vm.zig");
const sc = @import("syscall/numbers.zig");
const abi = @import("kernel_abi_root").vm_abi;
const owner = kernel.processPrincipalFromIndex(0).?;
var spaces: [1]vm.UserAddressSpace align(4096) = .{.{}};
var directory: vm.UserAddressSpaceTable = undefined;
var output: [40]u8 = undefined;
var copies: usize = 0;
var fail_copy = false;
var mutate: ?*kernel.VmaEntry = null;

fn copy(proc: kernel.PrincipalId, address: u64, bytes: []const u8) bool {
    std.debug.assert(proc == owner);
    // A usercopy may reenter VM fault handling; it must observe an unlocked AS.
    std.debug.assert(spaces[0].lock_state.value == 0);
    copies += 1;
    if (address != 0x8000 or fail_copy) return false;
    if (mutate) |entry| entry.start_va = 0xdead000;
    @memcpy(&output, bytes);
    return true;
}
fn words() [5]u64 {
    var result: [5]u64 = undefined;
    for (&result, 0..) |*value, i| value.* = std.mem.readInt(u64, output[i * 8 ..][0..8], .little);
    return result;
}

pub fn run(query: anytype) !void {
    comptime std.debug.assert(@import("builtin").is_test);
    const state = try std.testing.allocator.create(kernel.KernelState);
    defer std.testing.allocator.destroy(state);
    state.* = .{};
    spaces[0] = .{};
    directory.initializeFreshStorage(&spaces);
    // Query needs only AS/shared-VM locks, never page-table hooks or backing.
    var hooks: vm.Hooks = undefined;
    hooks.user_spaces = &directory;
    vm.init(hooks);
    copies = 0;
    fail_copy = false;
    mutate = null;
    const h = .{ .copy_bytes_to_user_va = copy };
    try std.testing.expectEqual(sc.syscall_err_empty, query(h, state, owner, 0, 0x8000, 40));
    try std.testing.expectEqual(@as(usize, 0), copies);

    // Real VMA/index records without resident pages or usable VMO references:
    // metadata enumeration must never resolve or allocate lazy backing.
    var refs: [3]@import("state/types.zig").VmaReferences = .{ .{}, .{}, .{} };
    var entries = [_]kernel.VmaEntry{
        .{ .active = true, .start_va = 0, .size_bytes = 4096, .prot = .{ .read = true, .exec = true }, .flags = .{ .private = true }, .references = &refs[0] },
        .{ .active = true, .start_va = 0x4000, .size_bytes = 8192, .prot = .{ .read = true, .write = true, .pkey = 3 }, .flags = .{ .private = true, .anonymous = true, .noreserve = true }, .references = &refs[1] },
        .{ .active = true, .start_va = 0x9000, .size_bytes = 4096, .prot = .{}, .flags = .{ .shared = true }, .vmo_offset = 8192, .references = &refs[2] },
    };
    refs[1].address_node.child = .{ &entries[0], &entries[2] };
    state.vma_tables[0].address_index.root = &entries[1];
    for ([_]u64{ 0, 1, 4095 }) |cursor| {
        try std.testing.expectEqual(sc.syscall_ok, query(h, state, owner, cursor, 0x8000, 40));
        try std.testing.expectEqual([5]u64{ 0, 4096, abi.prot_read | abi.prot_exec, abi.mmap_private, 0 }, words());
    }
    for ([_]u64{ 4096, 0x3000, 0x4000, 0x5fff }) |cursor| {
        try std.testing.expectEqual(sc.syscall_ok, query(h, state, owner, cursor, 0x8000, 40));
        try std.testing.expectEqual([5]u64{ 0x4000, 8192, abi.prot_read | abi.prot_write, abi.mmap_private | abi.mmap_anonymous | abi.mmap_noreserve | (3 << abi.mmap_pkey_shift), 0 }, words());
    }
    for ([_]u64{ 0x6000, 0x8000, 0x9000, 0x9fff }) |cursor| {
        try std.testing.expectEqual(sc.syscall_ok, query(h, state, owner, cursor, 0x8000, 40));
        try std.testing.expectEqual([5]u64{ 0x9000, 4096, 0, abi.mmap_shared, 8192 }, words());
    }
    const before = output;
    const previous_copies = copies;
    for ([_]u64{ 0xa000, std.math.maxInt(u64) }) |cursor|
        try std.testing.expectEqual(sc.syscall_err_empty, query(h, state, owner, cursor, 0x8000, 40));
    for ([_]u64{ 0, 39, 41, std.math.maxInt(u64) }) |size|
        try std.testing.expectEqual(sc.syscall_err_invalid, query(h, state, owner, 0, 0x8000, size));
    for ([_]u64{ 0, std.math.maxInt(u64) - 39, std.math.maxInt(u64) }) |address|
        try std.testing.expectEqual(sc.syscall_err_invalid, query(h, state, owner, 0, address, 40));
    try std.testing.expectEqual(previous_copies, copies);
    try std.testing.expectEqual(before, output);
    // The normal checked-copy hook rejects unmapped or nonwritable buffers.
    try std.testing.expectEqual(sc.syscall_err_invalid, query(h, state, owner, 0, 0x9000, 40));
    fail_copy = true;
    try std.testing.expectEqual(sc.syscall_err_invalid, query(h, state, owner, 0, 0x8000, 40));
    fail_copy = false;
    mutate = &entries[0];
    try std.testing.expectEqual(sc.syscall_ok, query(h, state, owner, 0, 0x8000, 40));
    try std.testing.expectEqual(@as(u64, 0), words()[0]);
    try std.testing.expectEqual(@as(u64, 0xdead000), entries[0].start_va);
    mutate = null;
}
