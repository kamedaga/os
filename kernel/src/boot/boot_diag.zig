//! One-boot GOP diagnostics for the separate DIAG ELF. This is not a console:
//! the normal image has this entire path compiled out and userland owns TTY.
const std = @import("std");
const root = @import("root");
const x86 = @import("../arch/x86_64/platform.zig");
const layout = @import("../arch/x86_64/physical_layout.zig");
const mtrr = @import("../arch/x86_64/mtrr.zig");
const kernel_log = @import("../kernel_log.zig");
const FramebufferInfo = @import("boot_resources.zig").FramebufferInfo;

pub const enabled = if (@hasDecl(root, "boot_diagnostic_enabled")) root.boot_diagnostic_enabled else false;
const font = if (enabled) @import("boot_diag_font").bytes else "";
// user_copy owns 0..max_cpus-1. The diagnostic ELF exclusively borrows the
// final PTE in the existing physical-copy window, one GOP page at a time.
const slot: usize = x86.page_entries - 1;
const slot_va: u64 = x86.phys_copy_window_va + slot * 4096;
const high_identity_start: u64 = x86.phys_copy_window_va + (1 << 21);
const high_identity_end: u64 = x86.phys_copy_window_va +
    (@as(u64, x86.high_mmio_pdp_table_count) << 30);

var fb: FramebufferInfo = undefined;
var mapped_page: u64 = 0;
var armed: bool = false;
var active: bool = false;
var drawing: u8 = 0;
var next_row: usize = 0;
var snapshot: mtrr.CacheSnapshot = .{};
var trace_lock: u8 = 0;
var trace_line: [96]u8 = undefined;
var trace_len: usize = 0;
var trace_row: usize = 0;
var trace_first_row: usize = 0;
var trace_max_row: usize = 0;
var trace_wrapped: bool = false;

comptime {
    if (slot < x86.max_cpus) @compileError("diagnostic GOP slot overlaps user_copy");
}

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
    // The high MMIO identity map uses WB PTEs. It may alias the GOP only if
    // the MTRR makes that overlapping portion effectively uncached already.
    const overlap_first = @max(first, high_identity_start);
    const overlap_end = @min(end, high_identity_end);
    if (overlap_first < overlap_end and
        !snapshot.rangeIsUncached(overlap_first, overlap_end - overlap_first)) return false;
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
    const first = fb.paddr & ~@as(u64, 4095);
    const end_raw = fb.paddr + fb.size_bytes;
    const end = std.mem.alignForward(u64, end_raw, 4096);
    const low_end = @min(end, layout.identity_limit);
    if (first < low_end and
        !x86.mapBootMmioIdentityRange(first, low_end - first)) return;
    active = true;
    mark('C');
}

fn begin() bool {
    if (!active or @atomicRmw(u8, &drawing, .Xchg, 1, .acq_rel) != 0) return false;
    return true;
}

fn finish() void {
    x86.phys_copy_window_pt[slot] = 0;
    x86.invlpg(slot_va);
    mapped_page = 0;
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
    if (@atomicRmw(u8, &trace_lock, .Xchg, 1, .acq_rel) != 0) return;
    defer @atomicStore(u8, &trace_lock, 0, .release);
    for (text) |char| {
        if (char == '\r') continue;
        if (char == '\n') {
            if (trace_len != 0) {
                const line = trace_line[0..trace_len];
                const console_ready = std.mem.startsWith(u8, line,
                    "[live_console] framebuffer ");
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
    try std.testing.expect(slot >= x86.max_cpus);
    try std.testing.expectEqual(x86.phys_copy_window_va + slot * 4096, slot_va);
}
