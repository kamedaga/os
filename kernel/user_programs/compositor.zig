const compositor = @import("compositor_core.zig");

pub export fn _start() noreturn {
    compositor.earlyStartLog();
    compositor.run(true);
}
