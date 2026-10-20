//! Serial pre-AP boot only. Limine promises usable RAM in the HHDM, but
//! 12.9.0 has handed us an ELF-layout-dependent missing high PDP entry.
//! Validate actual translations before exposing a writable boot resource view.
const std = @import("std");
const builtin = @import("builtin");
const mask: u64 = 0x000f_ffff_ffff_f000;

pub const Result = struct { mapped: bool, next: u64 };
pub const View = struct {
    root: u64,
    base: u64,
    read_word: *const fn (u64) ?u64,

    pub fn inspect(self: View, physical: u64) Result {
        return self.inspectAccess(physical, true);
    }

    fn inspectAccess(self: View, physical: u64, write: bool) Result {
        const fallback: Result = .{ .mapped = false, .next = physical +| 4096 };
        const va = std.math.add(u64, self.base, physical) catch return fallback;
        if (self.base & 4095 != 0 or va < 0xffff_8000_0000_0000 or self.root == 0) return fallback;
        var table = self.root & mask;
        var writable = true;
        for ([_]u6{ 39, 30, 21, 12 }) |shift| {
            const span = @as(u64, 1) << shift;
            const next = physical +| (span - (va & (span - 1)));
            const missing: Result = .{ .mapped = false, .next = next };
            const address = std.math.add(u64, self.base, table) catch return missing;
            // This is a fault-contained read, not an unchecked dereference.
            // The CPU walks the ACTIVE root to validate the table's HHDM page
            // before supplying its entry. Software recursively checking that
            // same page would have no trusted root and could recurse forever.
            const slot = std.math.add(u64, address, ((va >> shift) & 511) * 8) catch return missing;
            const entry = self.read_word(slot) orelse return missing;
            if (entry & 1 == 0) return missing;
            writable = writable and entry & 2 != 0;
            if (shift == 12 or (shift != 39 and entry & 128 != 0)) {
                if (shift != 12 and entry & mask & (span - 1) & ~@as(u64, 4096) != 0) return missing;
                const mapped = (entry & mask & ~(span - 1)) | (va & (span - 1));
                return .{ .mapped = (!write or writable) and mapped == physical, .next = next };
            }
            if (shift == 39 and entry & 128 != 0) return missing;
            table = entry & mask;
        }
        unreachable;
    }

    pub fn contains(self: View, physical: u64, bytes: usize) bool {
        return self.containsAccess(physical, bytes, true);
    }

    pub fn containsReadable(self: View, physical: u64, bytes: usize) bool {
        return self.containsAccess(physical, bytes, false);
    }

    fn containsAccess(self: View, physical: u64, bytes: usize, write: bool) bool {
        if (bytes == 0) return false;
        const end = std.math.add(u64, physical, bytes) catch return false;
        var cursor = physical;
        while (cursor < end) {
            const result = self.inspectAccess(cursor, write);
            if (!result.mapped or result.next <= cursor) return false;
            cursor = result.next;
        }
        return true;
    }
};

const Descriptor = packed struct { limit: u16, base: u64 };
const Gate = packed struct {
    low: u16,
    selector: u16,
    ist: u8 = 0,
    attributes: u8 = 0x8e,
    middle: u16,
    high: u32,
    reserved: u32 = 0,
};
// Static, image-mapped storage avoids a 4 KiB boot stack frame. No AP, IRQ,
// callback, or logging may use this transient IDT. Unexpected faults fail shut.
var gates: [256]Gate = undefined;
var active = false;

fn readInstruction() callconv(.naked) void {
    asm volatile (
        \\.global boot_hhdm_read_instruction
        \\boot_hhdm_read_instruction:
        \\mov (%%rdi), %%rax
        \\mov %%rax, (%%rsi)
        \\mov $1, %%eax
        \\ret
        \\.global boot_hhdm_read_failed
        \\boot_hhdm_read_failed:
        \\xor %%eax, %%eax
        \\ret
    );
}
// Serial pre-AP probes share the same temporary-IDT containment as HHDM.
// Only these exact instruction sites (or the selected execution target) recover.
export var boot_permission_target: u64 = 0;
export var boot_permission_error: u64 = 0;
fn writeInstruction() callconv(.naked) void {
    asm volatile (
        \\mov (%%rdi), %%al
        \\.global boot_permission_write
        \\boot_permission_write:
        \\mov %%al, (%%rdi)
        \\mov $1, %%eax
        \\ret
    );
}
fn executeInstruction() callconv(.naked) void {
    asm volatile (
        \\call *%%rdi
        \\mov $1, %%eax
        \\ret
        \\.global boot_permission_exec_failed
        \\boot_permission_exec_failed:
        \\add $8, %%rsp
        \\xor %%eax, %%eax
        \\ret
    );
}
fn faultHandler() callconv(.naked) void {
    asm volatile (
        \\.global boot_permission_fault_entry
        \\boot_permission_fault_entry:
        \\push %%rax
        \\cmpq $0, boot_permission_user(%%rip)
        \\je 4f
        \\cmpq $0x23, 24(%%rsp)
        \\jne 1f
        \\cmpq $0x1000a, 16(%%rsp)
        \\je 5f
        \\cmpq $0x1000f, 16(%%rsp)
        \\jne 1f
        \\5:
        \\movabs $boot_permission_user_return, %%rax
        \\mov %%rax, 16(%%rsp)
        \\movq $8, 24(%%rsp)
        \\movq $2, 32(%%rsp)
        \\mov boot_permission_user_rsp(%%rip), %%rax
        \\mov %%rax, 40(%%rsp)
        \\movq $16, 48(%%rsp)
        \\jmp 6f
        \\4:
        \\movabs $boot_hhdm_read_instruction, %%rax
        \\cmp %%rax, 16(%%rsp)
        \\je 2f
        \\movabs $boot_permission_write, %%rax
        \\cmp %%rax, 16(%%rsp)
        \\je 2f
        \\mov boot_permission_target(%%rip), %%rax
        \\test %%rax, %%rax
        \\jz 1f
        \\cmp %%rax, 16(%%rsp)
        \\jne 1f
        \\movabs $boot_permission_exec_failed, %%rax
        \\jmp 3f
        \\2:
        \\movabs $boot_hhdm_read_failed, %%rax
        \\3:
        \\mov %%rax, 16(%%rsp)
        \\6:
        \\mov 8(%%rsp), %%rax
        \\mov %%rax, boot_permission_error(%%rip)
        \\pop %%rax
        \\add $8, %%rsp
        \\iretq
        \\1: cli
        \\hlt
        \\jmp 1b
    );
}

pub export var boot_permission_user: u64 = 0;
pub export var boot_permission_user_rsp: u64 = 0;
pub export var boot_permission_user_cr3: u64 = 0;

// This stub and its saved state live in shared slot 511. It restores CR3
// before returning to Zig, so no direct-map borrow can escape the boundary.
fn userReadInstruction() callconv(.naked) void {
    asm volatile (
        \\mov %%rsp, boot_permission_user_rsp(%%rip)
        \\mov %%cr3, %%rax
        \\mov %%rax, boot_permission_user_cr3(%%rip)
        \\mov %%rdi, %%cr3
        \\pushq $0x1b
        \\pushq $0x12000
        \\pushq $2
        \\pushq $0x23
        \\pushq $0x10000
        \\iretq
        \\.global boot_permission_user_return
        \\boot_permission_user_return:
        \\mov boot_permission_user_cr3(%%rip), %%rax
        \\mov %%rax, %%cr3
        \\xor %%eax, %%eax
        \\ret
    );
}

// If the forbidden read unexpectedly succeeds, the fixture executes INT80.
// Normalize its frame so the same boundary returns a failed check safely.
fn userReadSucceeded() callconv(.naked) void {
    asm volatile (
        \\pushq $0
        \\jmp boot_permission_fault_entry
    );
}
fn unexpectedHandler() callconv(.naked) void {
    asm volatile (
        \\1: cli
        \\hlt
        \\jmp 1b
    );
}

const Operation = enum { read, write, execute, user_read };
fn checkedRead(address: u64) ?u64 {
    return checkedOperation(address, .read);
}
pub fn permissionFault(address: u64, execute: bool) bool {
    const result = checkedOperation(address, if (execute) .execute else .write);
    // Present, supervisor protection violation; no reserved-bit violation.
    return result == null and (@as(*volatile u64, &boot_permission_error).* & 31) == @as(u64, if (execute) 17 else 3);
}
pub fn unmappedReadFault(address: u64) bool {
    const result = checkedOperation(address, .read);
    return result == null and (@as(*volatile u64, &boot_permission_error).* & 31) == 0;
}
pub fn userReadFault(root: u64) bool {
    _ = checkedOperation(root, .user_read);
    return (@as(*volatile u64, &boot_permission_error).* & 31) == 5;
}
fn checkedOperation(address: u64, operation: Operation) ?u64 {
    if (builtin.is_test) unreachable;
    var flags: u64 = 0;
    asm volatile ("pushfq; pop %[out]; cli"
        : [out] "=r" (flags),
        :
        : .{ .memory = true });
    if (active) while (true) asm volatile ("cli; hlt");
    active = true;
    var old: Descriptor = undefined;
    asm volatile ("sidt (%[ptr])"
        :
        : [ptr] "r" (&old),
        : .{ .memory = true });
    var cs: u16 = 0;
    asm volatile ("mov %%cs, %[out]"
        : [out] "=r" (cs),
    );
    const stop = @intFromPtr(&unexpectedHandler);
    const recover = @intFromPtr(&faultHandler);
    for (&gates, 0..) |*gate, vector| {
        const handler = if (vector == 14 or vector == 13) recover else stop;
        // CPL3 recovery must use the PF IST, not RSP0: RSP0 would overwrite
        // the suspended BSP call frames that this diagnostic resumes.
        gate.* = .{ .low = @truncate(handler), .selector = cs, .ist = if (operation == .user_read) 1 else 0, .middle = @truncate(handler >> 16), .high = @truncate(handler >> 32) };
    }
    if (operation == .user_read) {
        const handler = @intFromPtr(&userReadSucceeded);
        gates[128] = .{ .low = @truncate(handler), .selector = cs, .ist = 1, .attributes = 0xee, .middle = @truncate(handler >> 16), .high = @truncate(handler >> 32) };
    }
    const temporary: Descriptor = .{ .limit = @sizeOf(@TypeOf(gates)) - 1, .base = @intFromPtr(&gates) };
    asm volatile ("lidt (%[ptr])"
        :
        : [ptr] "r" (&temporary),
        : .{ .memory = true });
    var value: u64 = 0;
    const read: *const fn (u64, *u64) callconv(.c) bool = @ptrCast(&readInstruction);
    @as(*volatile u64, &boot_permission_target).* = if (operation == .execute) address else 0;
    @as(*volatile u64, &boot_permission_error).* = 0;
    @as(*volatile u64, &boot_permission_user).* = @intFromBool(operation == .user_read);
    const write: *const fn (u64) callconv(.c) bool = @ptrCast(&writeInstruction);
    const execute: *const fn (u64) callconv(.c) bool = @ptrCast(&executeInstruction);
    const user_read: *const fn (u64) callconv(.c) bool = @ptrCast(&userReadInstruction);
    const ok = switch (operation) {
        .read => read(address, &value),
        .write => write(address),
        .execute => execute(address),
        .user_read => user_read(address),
    };
    @as(*volatile u64, &boot_permission_user).* = 0;
    @as(*volatile u64, &boot_permission_target).* = 0;
    asm volatile ("lidt (%[ptr])"
        :
        : [ptr] "r" (&old),
        : .{ .memory = true });
    active = false;
    if (flags & (1 << 9) != 0) asm volatile ("sti" ::: .{ .memory = true });
    return if (ok) value else null;
}

pub fn current(base: u64) ?View {
    if (builtin.is_test) unreachable;
    var cr3: u64 = 0;
    var cr4: u64 = 0;
    asm volatile ("mov %%cr3, %[out]"
        : [out] "=r" (cr3),
    );
    asm volatile ("mov %%cr4, %[out]"
        : [out] "=r" (cr4),
    );
    // This boot path requests the default four-level x86 paging mode.
    if (cr4 & (1 << 12) != 0) return null;
    return .{ .root = cr3 & mask, .base = base, .read_word = checkedRead };
}

pub const Fake = if (builtin.is_test) struct {
    pub var pdp: [512]u64 = undefined;
    pub var table_missing = false;
    pub fn reset() View {
        for (&pdp, 0..) |*entry, i| entry.* = (@as(u64, i) << 30) | 0x83;
        table_missing = false;
        return .{ .base = 0xffff_8000_0000_0000, .root = 0x1000, .read_word = read };
    }
    fn read(address: u64) ?u64 {
        const pa = address - 0xffff_8000_0000_0000;
        if (pa >= 0x1000 and pa < 0x2000) return 0x2003;
        if (pa >= 0x2000 and pa < 0x3000 and !table_missing) return pdp[(pa - 0x2000) / 8];
        return null;
    }
} else struct {};

test "staged page HHDM checks every target span and fault-contained table access" {
    const view = Fake.reset();
    try std.testing.expect(view.contains(4096, 8192));
    Fake.pdp[4] = 0;
    try std.testing.expect(!view.contains(1 << 32, 4096));
    try std.testing.expect(!view.contains((1 << 32) - 4096, 8192));
    try std.testing.expectEqual(@as(u64, 5) << 30, view.inspect(1 << 32).next);
    Fake.table_missing = true;
    try std.testing.expect(!view.contains(4096, 4096));
    _ = Fake.reset();
    Fake.pdp[0] &= ~@as(u64, 2);
    try std.testing.expect(!view.contains(4096, 4096));
    try std.testing.expect(view.containsReadable(4096, 4096));
    Fake.pdp[0] = (1 << 30) | 0x83;
    try std.testing.expect(!view.contains(4096, 4096));
}

test "staged page HHDM walks 4KiB leaves and rejects every missing level" {
    const Tables = struct {
        var entries = [_]u64{ 0x2003, 0x3003, 0x4003, 0x5003 };
        var unreadable: u64 = 0;
        fn read(address: u64) ?u64 {
            const physical = address - 0xffff_8000_0000_0000;
            const table = physical & ~@as(u64, 4095);
            if (table == unreadable or table < 4096 or table > 16384) return null;
            return entries[table / 4096 - 1];
        }
    };
    const view: View = .{ .root = 0x1000, .base = 0xffff_8000_0000_0000, .read_word = Tables.read };
    try std.testing.expect(view.contains(0x5000, 4096));
    for (&Tables.entries, 0..) |*entry, index| {
        const saved = entry.*;
        entry.* = 0;
        try std.testing.expect(!view.contains(0x5000, 4096));
        entry.* = saved;
        Tables.unreadable = (index + 1) * 4096;
        try std.testing.expect(!view.contains(0x5000, 4096));
        Tables.unreadable = 0;
    }
    try std.testing.expect(!view.contains(0x5000, 4097));
}
