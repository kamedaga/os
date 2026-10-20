// Root at src keeps shared state/architecture imports inside one Zig module.
comptime {
    _ = @import("boot/resource_pages.zig");
}
