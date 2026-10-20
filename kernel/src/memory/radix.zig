//! Shared sparse directory shape. Allocation and reader lifetime belong to the
//! caller; walking this directory never allocates or retains a body loan.
pub fn Directory(comptime Block: type) type {
    return struct {
        pub const Node = struct {
            children: [64]?*Node = .{null} ** 64,
            block: ?*Block = null,
            // Optional bit-1 summary used by SlotTable bounded free-FD search.
            marked: u64 = 0,
        };

        pub inline fn get(root: ?*Node, ordinal: u32, comptime levels: usize) ?*Block {
            var node = root orelse return null;
            var level: usize = levels;
            while (level != 0) {
                level -= 1;
                const shift: u5 = @intCast(level * 6);
                node = node.children[(ordinal >> shift) & 63] orelse return null;
            }
            return node.block;
        }
    };
}
