// Host fixture enables the same bounded ring-only path used by timer IRQs.
pub const kernel_log_ring_enabled = true;
test {
    _ = @import("kernel_log.zig");
}
