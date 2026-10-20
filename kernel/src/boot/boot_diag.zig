//! One-boot GOP diagnostics for the separate DIAG ELF. This is not a console:
//! the normal image has this entire path compiled out and userland owns TTY.
const lock_diagnostics = @import("../sync/lock_diagnostics.zig");
const std = @import("std");
const root = @import("root");
const x86 = @import("../arch/x86_64/platform.zig");
const mtrr = @import("../arch/x86_64/mtrr.zig");
const kernel_log = @import("../kernel_log.zig");
const FramebufferInfo = @import("boot_resources.zig").FramebufferInfo;

pub const enabled = if (@hasDecl(root, "boot_diagnostic_enabled")) root.boot_diagnostic_enabled else false;
const font = if (enabled) @import("boot_diag_font").bytes else "";
// Runtime per-CPU copy windows live in a separate dynamically allocated
// island. The diagnostic ELF retains the final legacy scratch PTE so early
// GOP output remains usable before MADT storage is allocated.
const slot: usize = x86.page_entries - 1;
const slot_va: u64 = x86.phys_copy_window_va + slot * 4096;

var fb: FramebufferInfo = undefined;
var mapped_page: u64 = 0;
var armed: bool = false;
var active: bool = false;
var drawing: u8 = 0;
var drawing_record: lock_diagnostics.Record = .{};
var next_row: usize = 0;
var snapshot: mtrr.CacheSnapshot = .{};
var trace_lock: u8 = 0;
var trace_record: lock_diagnostics.Record = .{};
var trace_line: [96]u8 = undefined;
var trace_len: usize = 0;
var trace_row: usize = 0;
var trace_first_row: usize = 0;
var trace_max_row: usize = 0;
var trace_wrapped: bool = false;

/// Called while Limine still owns CR3. Validate the complete physical span
/// before enabling any post-switch write. The seventh early marker reports
/// this admission decision even if the later CR3 transition itself stalls.
pub fn arm(info: FramebufferInfo) bool {
    if (comptime !enabled) return false;
    armed = false;
    active = false;
    if (info.pixel_format != 1 or info.width == 0 or info.height == 0 or
        info.width > info.pixels_per_scan_line or info.size_bytes == 0 or
        font.len < 4 + 256 * 16 or font[0] != 0x36 or font[1] != 0x04 or font[3] != 16) return false;
    const pitch = @as(u64, info.pixels_per_scan_line) * 4;
    if (pitch * @as(u64, info.height) != info.size_bytes) return false;
    const first = info.paddr & ~@as(u64, 4095);
    const end_raw = std.math.add(u64, info.paddr, info.size_bytes) catch return false;
    const end = std.mem.alignForward(u64, end_raw, 4096);
    if (end < end_raw or end <= first) return false;
    if (!mtrr.captureCacheSnapshot(&snapshot) or
        !snapshot.rangeSupportsUncachedMapping(first, end - first)) return false;
    fb = info;
    armed = true;
    return true;
}

/// The current CR3 has the copy-window page table. Low framebuffer pages
/// otherwise have a competing WB identity alias, so change that alias to UC
/// *before* mapping the diagnostic slot. High GOP addresses need no new VA.
pub fn afterCr3() void {
    if (comptime !enabled) return;
    if (!armed) return;
    active = true;
    mark('C');
}

fn begin() bool {
    const diagnostic_if = lock_diagnostics.maskInterrupts();
    defer lock_diagnostics.restoreInterrupts(diagnostic_if);
    if (!active or @atomicRmw(u8, &drawing, .Xchg, 1, .acq_rel) != 0) return false;
    lock_diagnostics.acquiredTry(&drawing_record, .{ .class = .boot_drawing });
    return true;
}

fn finish() void {
    const diagnostic_if = lock_diagnostics.maskInterrupts();
    defer lock_diagnostics.restoreInterrupts(diagnostic_if);
    lock_diagnostics.assertOwned(&drawing_record);
    if (!@import("builtin").is_test) {
        x86.phys_copy_window_pt[slot] = 0;
        x86.invlpg(slot_va);
    }
    mapped_page = 0;
    lock_diagnostics.releasing(&drawing_record);
    @atomicStore(u8, &drawing, 0, .release);
}

fn pixel(x: usize, y: usize, color: u32) void {
    if (x >= fb.width or y >= fb.height) return;
    const offset = @as(u64, @intCast(y)) * @as(u64, fb.pixels_per_scan_line) * 4 +
        @as(u64, @intCast(x)) * 4;
    if (offset > fb.size_bytes or fb.size_bytes - offset < 4) return;
    const physical = fb.paddr + offset;
    const page = physical & ~@as(u64, 4095);
    if (page != mapped_page) {
        // PAT entry 3 is verified UC by CacheSnapshot.valid(); NX prevents
        // the display aperture from ever becoming an executable mapping.
        x86.phys_copy_window_pt[slot] = page | x86.page_present | x86.page_rw |
            x86.page_nx | (1 << 3) | (1 << 4);
        x86.invlpg(slot_va);
        mapped_page = page;
    }
    const p: [*]volatile u8 = @ptrFromInt(slot_va + (physical & 4095));
    p[0] = @truncate(color);
    p[1] = @truncate(color >> 8);
    p[2] = @truncate(color >> 16);
    p[3] = 0;
}

fn writeLine(row: usize, message: []const u8, color: u32) void {
    const scale: usize = if (fb.width >= 800 and fb.height >= 480) 2 else 1;
    const y0 = 64 + row * 16 * scale;
    if (y0 + 16 * scale > fb.height) return;
    const max_columns = @min(message.len, (fb.width -| 16) / (8 * scale));
    // Scanline order keeps a single 4-KiB UC PTE hot while drawing text.
    for (0..16) |gy| {
        for (0..scale) |sy| {
            for (message[0..max_columns], 0..) |char, column| {
                const glyph: usize = if (char >= 32 and char <= 126) char else '?';
                const bits = font[4 + glyph * 16 + gy];
                for (0..8) |gx| {
                    if ((bits & (@as(u8, 0x80) >> @intCast(gx))) == 0) continue;
                    for (0..scale) |sx| {
                        pixel(8 + (column * 8 + gx) * scale + sx, y0 + gy * scale + sy, color);
                    }
                }
            }
        }
    }
}

pub fn mark(checkpoint: u8) void {
    if (comptime !enabled) return;
    if (!begin()) return;
    defer finish();
    const description: []const u8 = switch (checkpoint) {
        'C' => "CR3 INSTALLED / GOP MAPPED",
        'D' => "D RUNTIME / SMP READY",
        'E' => "E KERNEL SUBSYSTEMS READY",
        'I' => "I AMD IOMMU INITIALIZATION",
        'J' => "J IVRS PARSED",
        'K' => "K PCI / IVMD CHECKED",
        'L' => "L IOMMU MMIO ENABLE",
        'F' => "F IOMMU READY",
        'G' => "G BOOT PROCESSES READY",
        'H' => "H ENTERING USERLAND",
        else => return,
    };
    writeLine(next_row, description, 0x00ffffff);
    next_row += 1;
}

fn clearTraceRow(row: usize) void {
    const scale: usize = if (fb.width >= 800 and fb.height >= 480) 2 else 1;
    const y0 = 64 + row * 16 * scale;
    for (y0..y0 + 16 * scale) |y| {
        for (0..fb.width) |x| pixel(x, y, 0);
    }
}

fn showTraceLine(line: []const u8) void {
    if (!begin()) return;
    defer finish();
    if (trace_wrapped) clearTraceRow(trace_row);
    writeLine(trace_row, line, 0x0000ffff);
    trace_row += 1;
    if (trace_row == trace_max_row) {
        trace_row = trace_first_row;
        trace_wrapped = true;
    }
}

fn traceText(text: []const u8) void {
    // User logging can be interrupted by another logger, including an IRQ on
    // this CPU. Drop that fragment instead of spinning with interrupts off or
    // recursively using the single temporary GOP PTE.
    // Keep the unchecked early-return and deferred-store control flow intact:
    // even empty diagnostic calls in those blocks can change LLVM block layout.
    const diagnostic_if = lock_diagnostics.maskInterrupts();
    if (comptime lock_diagnostics.enabled) {
        if (@atomicRmw(u8, &trace_lock, .Xchg, 1, .acq_rel) != 0) {
            lock_diagnostics.restoreInterrupts(diagnostic_if);
            return;
        }
    } else {
        if (@atomicRmw(u8, &trace_lock, .Xchg, 1, .acq_rel) != 0) return;
    }
    lock_diagnostics.acquiredTry(&trace_record, .{ .class = .boot_trace });
    lock_diagnostics.restoreInterrupts(diagnostic_if);
    defer {
        if (comptime lock_diagnostics.enabled) {
            const release_if = lock_diagnostics.maskInterrupts();
            lock_diagnostics.releasing(&trace_record);
            @atomicStore(u8, &trace_lock, 0, .release);
            lock_diagnostics.restoreInterrupts(release_if);
        } else @atomicStore(u8, &trace_lock, 0, .release);
    }
    for (text) |char| {
        if (char == '\r') continue;
        if (char == '\n') {
            if (trace_len != 0) {
                const line = trace_line[0..trace_len];
                const console_ready = std.mem.startsWith(u8, line, "[live_console] framebuffer ");
                showTraceLine(line);
                trace_len = 0;
                // The userland framebuffer console now owns the GOP. Do not
                // race its renderer or paint over ash after this milestone.
                if (console_ready) kernel_log.setDiagnosticObserver(null);
            }
            continue;
        }
        if (trace_len < trace_line.len) {
            trace_line[trace_len] = if (char >= 32 and char <= 126) char else '?';
            trace_len += 1;
        }
    }
}

pub fn startPostUserLogMirror() void {
    if (comptime !enabled) return;
    if (!active) return;
    const scale: usize = if (fb.width >= 800 and fb.height >= 480) 2 else 1;
    if (fb.height <= 64) return;
    trace_max_row = (fb.height - 64) / (16 * scale);
    trace_first_row = next_row + 2;
    if (trace_first_row >= trace_max_row) return;
    trace_row = trace_first_row;
    trace_wrapped = false;
    trace_len = 0;
    writeLine(next_row + 1, "POST-H SERIAL LOG (DIAGNOSTIC):", 0x00ffffff);
    kernel_log.setDiagnosticObserver(traceText);
}

pub fn showHalt() void {
    if (comptime !enabled) return;
    if (!begin()) return;
    defer finish();
    writeLine(next_row + 1, "HALT - LAST KERNEL LOG LINES:", 0x00ffffff);
    var row = next_row + 2;
    const log = kernel_log.boot_log_buffer[0..@min(kernel_log.boot_log_len, kernel_log.boot_log_buffer.len)];
    var start = log.len;
    var lines: usize = 0;
    while (start > 0 and lines < 9) {
        start -= 1;
        if (log[start] == '\n') lines += 1;
    }
    if (start != 0) start += 1;
    var cursor = start;
    while (cursor < log.len and row < 27) {
        const end = if (std.mem.indexOfScalarPos(u8, log, cursor, '\n')) |i| i else log.len;
        if (end > cursor) {
            writeLine(row, log[cursor..end], 0x0000ffff);
            row += 1;
        }
        cursor = end + 1;
    }
}

pub fn showPanic(message: []const u8) void {
    if (comptime !enabled) return;
    if (!begin()) return;
    defer finish();
    writeLine(next_row + 1, "KERNEL PANIC:", 0x00ffffff);
    writeLine(next_row + 2, message, 0x0000ffff);
}

test "diagnostic slot cannot collide with per-CPU copy slots" {
    try std.testing.expect(slot_va < @import("../arch/x86_64/cpu_runtime.zig").base or slot_va >= @import("../boot/image_range.zig").virtual_base);
    try std.testing.expectEqual(x86.phys_copy_window_va + slot * 4096, slot_va);
}

test "lock admission boot trace and drawing actual single exchange paths" {
    if (!lock_diagnostics.enabled) return;
    active = true;
    defer active = false;
    try std.testing.expect(begin());
    try std.testing.expect(!begin());
    try std.testing.expectEqual(lock_diagnostics.order.Mode.try_once, drawing_record.mode);
    finish();
    trace_len = 0;
    traceText("abc");
    try std.testing.expectEqual(@as(usize, 3), trace_len);
    try std.testing.expectEqual(lock_diagnostics.order.Mode.try_once, trace_record.mode);
    try std.testing.expect(!trace_record.linked);
    trace_len = 0;
    lock_diagnostics.assertRootEmpty();
}
