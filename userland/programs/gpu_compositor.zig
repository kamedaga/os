const compositor = @import("support_root").compositor_core;

pub export fn _start() noreturn {
    compositor.run(true);
}
