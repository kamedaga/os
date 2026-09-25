//! Selectable, non-production entry that proves control reached kernel code
//! without reading or writing the GOP framebuffer.
const std = @import("std");
const requests = @import("entry_probe_requests.zig");

pub fn panic(_: []const u8, _: ?*std.builtin.StackTrace, _: ?usize) noreturn {
    haltForever();
}

pub export fn _start() callconv(.c) noreturn {
    // Keep the same request-bearing object linked into this image, but do not
    // dereference any response: the reset observation is GOP-independent.
    asm volatile (""
        :
        : [request] "r" (&requests.framebuffer_request),
        : .{ .memory = true });
    const start = readTsc();
    // The 5900X invariant TSC and QEMU's virtual TSC make this visibly later
    // than the Limine handoff. Exact wall-clock duration is not a contract.
    while (readTsc() -% start < 15_000_000_000) asm volatile ("pause");
    asm volatile ("cli");
    outb(0xcf9, 0x02);
    for (0..100_000) |_| asm volatile ("pause");
    outb(0xcf9, 0x06);
    // If CF9 is unsupported on a particular board, stop. Never introduce a
    // triple-fault fallback that could turn an unproven reboot into a loop.
    haltForever();
}

fn readTsc() u64 {
    var lo: u32 = 0;
    var hi: u32 = 0;
    asm volatile ("rdtsc"
        : [lo] "={eax}" (lo),
          [hi] "={edx}" (hi),
    );
    return (@as(u64, hi) << 32) | lo;
}

fn outb(port: u16, value: u8) void {
    asm volatile ("outb %[value], %[port]"
        :
        : [value] "{al}" (value),
          [port] "{dx}" (port),
    );
}

fn haltForever() noreturn {
    while (true) asm volatile ("hlt");
}
