const compositor = @import("abi_root").compositor_core;

pub export fn _start() noreturn {
    compositor.earlyStartLog();
    compositor.run(true);
}

