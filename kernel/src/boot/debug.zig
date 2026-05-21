pub const Hooks = struct {
    write: *const fn ([]const u8) void,
};

pub fn logReadyTitle(hooks: Hooks, title: []const u8) void {
    hooks.write(title);
    hooks.write("\n");
}
