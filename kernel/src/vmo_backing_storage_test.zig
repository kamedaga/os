// Keep the private live-header state test in the same src-rooted module.
comptime {
    _ = @import("memory/vmo_backing.zig");
    _ = @import("state/types.zig");
    // Same-module tests exercise the private final-partial-chunk allocator;
    // importing VMO through the separate kernel module does not collect it.
    _ = @import("state/vmo.zig");
}
