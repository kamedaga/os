/// UEFI entry point for CapabilityOS kernel.
/// All boot logic lives in boot/entry.zig.  This file is intentionally minimal:
/// panic handler, exported asm symbols, and the UEFI main() stack-switch stub.
const std = @import("std");
const x86_platform = @import("arch/x86_64/platform.zig");
const uefi_services = @import("boot/uefi_services.zig");

// Pull in boot/entry.zig so that its `pub export` FX-state functions
// (saveCurrentThreadFxState, restoreCurrentThreadFxState, etc.) are emitted.
const entry = @import("boot/entry.zig");

// ---------------------------------------------------------------------------
// Exported symbols accessed by assembly stubs in traps.zig / syscalls.zig
// ---------------------------------------------------------------------------

export var user_return_saved_r10: u64 = 0;
export var user_return_saved_gprs: [15]u64 align(16) = [_]u64{0} ** 15;
export var user_return_iret_frame: [5]u64 align(16) = [_]u64{0} ** 5;

// ---------------------------------------------------------------------------
// Panic handler
// ---------------------------------------------------------------------------

pub fn panic(msg: []const u8, trace: ?*std.builtin.StackTrace, ret_addr: ?usize) noreturn {
    _ = trace;
    _ = ret_addr;
    uefi_services.earlyUefiWrite(&[_:0]u16{ 'P', 'A', 'N', 'I', 'C', ':', ' ', 0 });
    uefi_services.earlyUefiWriteAscii(msg);
    uefi_services.earlyUefiWrite(&[_:0]u16{ '\r', '\n', 0 });
    while (true) asm volatile ("hlt");
}

// ---------------------------------------------------------------------------
// UEFI entry point — switch to ring-0 stack, then call kernelMain()
// ---------------------------------------------------------------------------

pub fn main() void {
    const boot_stack_top = @as(usize, @intCast(x86_platform.ring0StackTop())) & ~@as(usize, 0xF);
    asm volatile (
        \\mov %[stack_top], %%rsp
        \\mov %[target], %%rax
        \\call *%%rax
        :
        : [stack_top] "r" (boot_stack_top),
          [target] "r" (&entry.kernelMain),
        : .{ .memory = true });
    unreachable;
}
