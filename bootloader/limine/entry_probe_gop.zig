//! Selectable, non-production entry that tests only Limine -> CPU -> GOP.
const std = @import("std");
const limine = @import("protocol.zig");
const requests = @import("entry_probe_requests.zig");
const framebuffer = @import("entry_probe_framebuffer.zig");

pub fn panic(_: []const u8, _: ?*std.builtin.StackTrace, _: ?usize) noreturn {
    haltForever();
}

pub export fn _start() callconv(.c) noreturn {
    // Limine provides an initial >=64 KiB stack, so no production-kernel
    // stack, allocator, ACPI parser, or userland is needed for this probe.
    const response = requests.framebuffer_request.response orelse haltForever();
    if (response.framebuffer_count == 0) haltForever();
    const primary = response.framebuffers[0] orelse haltForever();
    _ = framebuffer.paint(primary.address, primary.width, primary.height,
        primary.pitch, primary.bpp, primary.memory_model == limine.framebuffer_rgb);
    haltForever();
}

fn haltForever() noreturn {
    while (true) asm volatile ("hlt");
}
