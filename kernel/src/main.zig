const std = @import("std");
const kernel = @import("kernel.zig");
const uefi = std.os.uefi;

const serial_port: u16 = 0x3F8;
const page_entries: usize = 512;
const four_gib: u64 = 4 * 1024 * 1024 * 1024;
const two_mib: u64 = 2 * 1024 * 1024;
const pd_table_count: usize = 4; // 4 * 1GiB = 4GiB
const user_va: u64 = 0x400000;
const user_stack_top: u64 = 0x402000;
const user_stack_page_va: u64 = user_stack_top - 0x1000;
const user_unmapped_test_va: u64 = 0x500000;
const reserved_low_mem_end: u64 = 16 * 1024 * 1024;
const page_addr_mask: u64 = 0x000F_FFFF_FFFF_F000;
const gdt_kernel_code_selector: u16 = 0x08;
const gdt_kernel_data_selector: u16 = 0x10;
const gdt_user_code_selector: u16 = 0x18;
const gdt_user_data_selector: u16 = 0x20;
const gdt_tss_selector: u16 = 0x28;

const page_present: u64 = 1 << 0;
const page_rw: u64 = 1 << 1;
const page_user: u64 = 1 << 2;
const page_ps: u64 = 1 << 7;

const debug_skip_exit_boot_services = false;
const debug_skip_cr3_switch = false;
const debug_trigger_page_fault_test = false;

var pml4_table: [page_entries]u64 align(4096) = [_]u64{0} ** page_entries;
var pdp_table: [page_entries]u64 align(4096) = [_]u64{0} ** page_entries;
var pd_tables: [pd_table_count][page_entries]u64 align(4096) = [_][page_entries]u64{[_]u64{0} ** page_entries} ** pd_table_count;
var user_pml4_table: [page_entries]u64 align(4096) = [_]u64{0} ** page_entries;
var user_pdp_table: [page_entries]u64 align(4096) = [_]u64{0} ** page_entries;
var user_pd_table: [page_entries]u64 align(4096) = [_]u64{0} ** page_entries;
var user_pt_table: [page_entries]u64 align(4096) = [_]u64{0} ** page_entries;
var global_free_list: kernel.FreePageList = .{};
var idt: [256]IdtEntry align(16) = [_]IdtEntry{zeroIdtEntry()} ** 256;
var gdt: [7]u64 align(16) = .{
    0x0000000000000000, // 0x00 null
    0x00AF9A000000FFFF, // 0x08 kernel code (DPL=0)
    0x00AF92000000FFFF, // 0x10 kernel data (DPL=0)
    0x00AFFA000000FFFF, // 0x18 user code (DPL=3)
    0x00AFF2000000FFFF, // 0x20 user data (DPL=3)
    0x0000000000000000, // 0x28 TSS low
    0x0000000000000000, // 0x30 TSS high
};
var ring0_stack: [64 * 1024]u8 align(16) = [_]u8{0} ** (64 * 1024);
var tss: Tss = std.mem.zeroes(Tss);
var kernel_state_global: kernel.KernelState = undefined;
var kernel_state_ready = false;

const syscall_alloc_page: u64 = 0x1;
const syscall_map_page: u64 = 0x2;
const syscall_move_cap: u64 = 0x3;

const syscall_ok: u64 = 0;
const syscall_err_invalid = 1;
const syscall_err_not_ready = 2;
const syscall_err_alloc = 4;
const syscall_err_map = 5;
const syscall_err_move = 6;

const MemoryStats = struct {
    detected_regions: usize,
    total_usable_bytes: u64,
};

const ReservedRange = struct {
    start: u64,
    end: u64, // exclusive
};

const IdtEntry = packed struct {
    offset_low: u16,
    selector: u16,
    ist: u8,
    type_attr: u8,
    offset_mid: u16,
    offset_high: u32,
    zero: u32,
};

const IdtPtr = packed struct {
    limit: u16,
    base: u64,
};

const GdtPtr = packed struct {
    limit: u16,
    base: u64,
};

const Tss = packed struct {
    _rsv0: u32 = 0,
    rsp0: u64 = 0,
    rsp1: u64 = 0,
    rsp2: u64 = 0,
    _rsv1: u64 = 0,
    ist1: u64 = 0,
    ist2: u64 = 0,
    ist3: u64 = 0,
    ist4: u64 = 0,
    ist5: u64 = 0,
    ist6: u64 = 0,
    ist7: u64 = 0,
    _rsv2: u64 = 0,
    _rsv3: u16 = 0,
    iomap_base: u16 = 0,
};

const SyscallRegs = extern struct {
    r15: u64,
    r14: u64,
    r13: u64,
    r12: u64,
    r11: u64,
    r10: u64,
    r9: u64,
    r8: u64,
    rbp: u64,
    rdi: u64,
    rsi: u64,
    rdx: u64,
    rcx: u64,
    rbx: u64,
    rax: u64,
};

fn zeroIdtEntry() IdtEntry {
    return .{
        .offset_low = 0,
        .selector = 0,
        .ist = 0,
        .type_attr = 0,
        .offset_mid = 0,
        .offset_high = 0,
        .zero = 0,
    };
}

fn outb(port: u16, value: u8) void {
    asm volatile ("outb %[value], %[port]"
        :
        : [value] "{al}" (value),
          [port] "{dx}" (port),
    );
}

fn serialInit() void {
    outb(serial_port + 1, 0x00);
    outb(serial_port + 3, 0x80);
    outb(serial_port + 0, 0x03);
    outb(serial_port + 1, 0x00);
    outb(serial_port + 3, 0x03);
    outb(serial_port + 2, 0xC7);
    outb(serial_port + 4, 0x0B);
}

fn serialWriteByte(b: u8) void {
    outb(serial_port, b);
}

fn serialWrite(text: []const u8) void {
    for (text) |ch| {
        if (ch == '\n') serialWriteByte('\r');
        serialWriteByte(ch);
    }
}

fn serialWriteHexRaw(value: u64) void {
    const hex = "0123456789abcdef";
    serialWrite("0x");
    var shift: u6 = 60;
    while (true) {
        const nibble: u4 = @intCast((value >> shift) & 0xF);
        serialWriteByte(hex[nibble]);
        if (shift == 0) break;
        shift -= 4;
    }
}

pub export fn pageFaultHandlerCommon(cr2: u64, error_code: u64) callconv(.c) noreturn {
    asm volatile ("cli");
    serialWrite("PAGE FAULT\n");
    serialWrite("  CR2=");
    serialWriteHexRaw(cr2);
    serialWrite("\n");
    serialWrite("  EC=");
    serialWriteHexRaw(error_code);
    serialWrite("\n");
    while (true) {
        asm volatile ("hlt");
    }
}

pub export fn doubleFaultHandlerCommon(error_code: u64) callconv(.c) noreturn {
    asm volatile ("cli");
    serialWrite("DOUBLE FAULT\n");
    serialWrite("  EC=");
    serialWriteHexRaw(error_code);
    serialWrite("\n");
    while (true) {
        asm volatile ("hlt");
    }
}

fn haltLoop() noreturn {
    while (true) asm volatile ("hlt");
}

pub export fn generalProtectionHandlerCommon(error_code: u64) callconv(.c) noreturn {
    asm volatile ("cli");
    serialWrite("GENERAL PROTECTION\n");
    serialWrite("  EC=");
    serialWriteHexRaw(error_code);
    serialWrite("\n");
    haltLoop();
}

pub export fn invalidTssHandlerCommon(error_code: u64) callconv(.c) noreturn {
    asm volatile ("cli");
    serialWrite("INVALID TSS\n");
    serialWrite("  EC=");
    serialWriteHexRaw(error_code);
    serialWrite("\n");
    haltLoop();
}

pub export fn segmentNotPresentHandlerCommon(error_code: u64) callconv(.c) noreturn {
    asm volatile ("cli");
    serialWrite("SEGMENT NOT PRESENT\n");
    serialWrite("  EC=");
    serialWriteHexRaw(error_code);
    serialWrite("\n");
    haltLoop();
}

pub export fn stackSegmentFaultHandlerCommon(error_code: u64) callconv(.c) noreturn {
    asm volatile ("cli");
    serialWrite("STACK SEGMENT FAULT\n");
    serialWrite("  EC=");
    serialWriteHexRaw(error_code);
    serialWrite("\n");
    haltLoop();
}

pub export fn invalidOpcodeHandlerCommon() callconv(.c) noreturn {
    asm volatile ("cli");
    serialWrite("INVALID OPCODE\n");
    haltLoop();
}

fn parseRights(bits: u64) kernel.Rights {
    return .{
        .cpu_read = (bits & 0x1) != 0,
        .cpu_write = (bits & 0x2) != 0,
        .dma = (bits & 0x4) != 0,
    };
}

fn mapUserPageFromCapability(
    state: *const kernel.KernelState,
    principal: kernel.PrincipalId,
    va: u64,
    paddr: u64,
    writable: bool,
) bool {
    if ((va & 0xFFF) != 0) return false;
    if ((paddr & 0xFFF) != 0) return false;
    if (paddr >= four_gib) return false;

    const pml4_index: usize = @intCast((va >> 39) & 0x1FF);
    const pdp_index: usize = @intCast((va >> 30) & 0x1FF);
    const pd_index: usize = @intCast((va >> 21) & 0x1FF);
    const pt_index: usize = @intCast((va >> 12) & 0x1FF);
    const user_pdp_index: usize = @intCast((user_va >> 30) & 0x1FF);
    const user_pd_index: usize = @intCast((user_va >> 21) & 0x1FF);

    // 現段階は user_pt_table (1本) の範囲に限定する。
    if (pml4_index != 0 or pdp_index != user_pdp_index or pd_index != user_pd_index) return false;

    const cap = state.getTableConst(principal).find(paddr) orelse return false;
    if (!cap.rights.cpu_read) return false;
    if (writable and !cap.rights.cpu_write) return false;

    user_pt_table[pt_index] = paddr | page_present | page_user | (if (writable) page_rw else 0);
    invlpg(va);
    return true;
}

pub export fn syscallDispatch(regs: *SyscallRegs) callconv(.c) u64 {
    if (!kernel_state_ready) return syscall_err_not_ready;
    const state = &kernel_state_global;

    switch (regs.rax) {
        syscall_alloc_page => {
            const cap = state.allocPageTo(.Process0, &global_free_list) catch return syscall_err_alloc;
            return cap.paddr;
        },
        syscall_map_page => {
            const writable = (regs.rdx & 0x1) != 0;
            if (mapUserPageFromCapability(state, .Process0, regs.rdi, regs.rsi, writable)) {
                return syscall_ok;
            }
            return syscall_err_map;
        },
        syscall_move_cap => {
            const to = switch (regs.rsi) {
                0 => kernel.PrincipalId.Process0,
                1 => kernel.PrincipalId.Device0,
                else => return syscall_err_invalid,
            };
            const from = if (to == .Process0) kernel.PrincipalId.Device0 else kernel.PrincipalId.Process0;
            const rights = parseRights(regs.rdx);
            state.moveCap(from, to, regs.rdi, rights) catch return syscall_err_move;
            return syscall_ok;
        },
        else => return syscall_err_invalid,
    }
}

pub export fn pageFaultHandlerStub() callconv(.naked) noreturn {
    asm volatile (
        \\mov %cr2, %rdi
        \\mov (%rsp), %rsi
        \\mov %rdi, %rcx
        \\mov %rsi, %rdx
        \\and $-16, %rsp
        \\sub $8, %rsp
        \\jmp pageFaultHandlerCommon
    );
}

pub export fn doubleFaultHandlerStub() callconv(.naked) noreturn {
    asm volatile (
        \\mov (%rsp), %rdi
        \\mov %rdi, %rcx
        \\and $-16, %rsp
        \\sub $8, %rsp
        \\jmp doubleFaultHandlerCommon
    );
}

pub export fn syscallHandlerStub() callconv(.naked) noreturn {
    asm volatile (
        \\push %rax
        \\push %rbx
        \\push %rcx
        \\push %rdx
        \\push %rsi
        \\push %rdi
        \\push %rbp
        \\push %r8
        \\push %r9
        \\push %r10
        \\push %r11
        \\push %r12
        \\push %r13
        \\push %r14
        \\push %r15
        // Win64: 32-byte shadow space を確保しつつ call 前 16-byte alignment を維持する。
        \\sub $32, %rsp
        \\lea 32(%rsp), %rdi
        \\mov %rdi, %rcx
        \\call syscallDispatch
        \\add $32, %rsp
        \\mov %rax, 112(%rsp)
        \\pop %r15
        \\pop %r14
        \\pop %r13
        \\pop %r12
        \\pop %r11
        \\pop %r10
        \\pop %r9
        \\pop %r8
        \\pop %rbp
        \\pop %rdi
        \\pop %rsi
        \\pop %rdx
        \\pop %rcx
        \\pop %rbx
        \\pop %rax
        \\iretq
    );
}

pub export fn generalProtectionHandlerStub() callconv(.naked) noreturn {
    asm volatile (
        \\mov (%rsp), %rdi
        \\mov %rdi, %rcx
        \\and $-16, %rsp
        \\sub $8, %rsp
        \\jmp generalProtectionHandlerCommon
    );
}

pub export fn invalidTssHandlerStub() callconv(.naked) noreturn {
    asm volatile (
        \\mov (%rsp), %rdi
        \\mov %rdi, %rcx
        \\and $-16, %rsp
        \\sub $8, %rsp
        \\jmp invalidTssHandlerCommon
    );
}

pub export fn segmentNotPresentHandlerStub() callconv(.naked) noreturn {
    asm volatile (
        \\mov (%rsp), %rdi
        \\mov %rdi, %rcx
        \\and $-16, %rsp
        \\sub $8, %rsp
        \\jmp segmentNotPresentHandlerCommon
    );
}

pub export fn stackSegmentFaultHandlerStub() callconv(.naked) noreturn {
    asm volatile (
        \\mov (%rsp), %rdi
        \\mov %rdi, %rcx
        \\and $-16, %rsp
        \\sub $8, %rsp
        \\jmp stackSegmentFaultHandlerCommon
    );
}

pub export fn invalidOpcodeHandlerStub() callconv(.naked) noreturn {
    asm volatile (
        \\and $-16, %rsp
        \\sub $8, %rsp
        \\jmp invalidOpcodeHandlerCommon
    );
}

fn loadGdtAndReloadSegments() void {
    const tss_base = @intFromPtr(&tss);
    const tss_limit: u64 = @sizeOf(Tss) - 1;
    tss.rsp0 = @intFromPtr(&ring0_stack) + ring0_stack.len;
    tss.iomap_base = @sizeOf(Tss);
    gdt[5] =
        (tss_limit & 0xFFFF) |
        ((tss_base & 0x00FF_FFFF) << 16) |
        (@as(u64, 0x89) << 40) |
        (((tss_limit >> 16) & 0xF) << 48) |
        (((tss_base >> 24) & 0xFF) << 56);
    gdt[6] = (tss_base >> 32) & 0xFFFF_FFFF;

    const gdt_ptr = GdtPtr{
        .limit = @as(u16, @intCast(@sizeOf(@TypeOf(gdt)) - 1)),
        .base = @intFromPtr(&gdt),
    };
    asm volatile ("lgdt (%[ptr])"
        :
        : [ptr] "r" (&gdt_ptr),
        : .{ .memory = true });

    // CS/SS/DS を新しい GDT の selector へ揃える。
    asm volatile (
        \\pushq %[kcs]
        \\pushq $1f
        \\lretq
        \\1:
        \\mov %[kds], %%ax
        \\mov %%ax, %%ds
        \\mov %%ax, %%es
        \\mov %%ax, %%ss
        :
        : [kcs] "i" (@as(u64, gdt_kernel_code_selector)),
          [kds] "i" (gdt_kernel_data_selector),
        : .{ .memory = true });
    asm volatile (
        \\mov %[tss_sel], %%ax
        \\ltr %%ax
        :
        : [tss_sel] "i" (gdt_tss_selector),
        : .{ .memory = true });
}

fn setIdtEntry(vec: usize, handler: usize) void {
    setIdtEntryWithAttr(vec, handler, 0x8E);
}

fn setIdtEntryWithAttr(vec: usize, handler: usize, type_attr: u8) void {
    idt[vec] = .{
        .offset_low = @as(u16, @truncate(handler & 0xFFFF)),
        .selector = gdt_kernel_code_selector,
        .ist = 0,
        .type_attr = type_attr,
        .offset_mid = @as(u16, @truncate((handler >> 16) & 0xFFFF)),
        .offset_high = @as(u32, @truncate(handler >> 32)),
        .zero = 0,
    };
}

fn loadIdt() void {
    const idt_ptr = IdtPtr{
        .limit = @as(u16, @intCast(@sizeOf(@TypeOf(idt)) - 1)),
        .base = @intFromPtr(&idt),
    };
    asm volatile ("lidt (%[ptr])"
        :
        : [ptr] "r" (&idt_ptr),
        : .{ .memory = true });
}

fn initIdtPageFaultOnly() void {
    @memset(idt[0..], zeroIdtEntry());
    setIdtEntry(6, @intFromPtr(&invalidOpcodeHandlerStub)); // #UD
    setIdtEntry(10, @intFromPtr(&invalidTssHandlerStub)); // #TS
    setIdtEntry(11, @intFromPtr(&segmentNotPresentHandlerStub)); // #NP
    setIdtEntry(12, @intFromPtr(&stackSegmentFaultHandlerStub)); // #SS
    setIdtEntry(13, @intFromPtr(&generalProtectionHandlerStub)); // #GP
    setIdtEntry(14, @intFromPtr(&pageFaultHandlerStub)); // #PF
    setIdtEntry(8, @intFromPtr(&doubleFaultHandlerStub)); // #DF
    // DPL=3 trap gate to allow ring3 software syscall entry.
    setIdtEntryWithAttr(0x80, @intFromPtr(&syscallHandlerStub), 0xEF);
    loadIdt();
}

fn triggerPageFaultTest() noreturn {
    serialWrite("triggering page fault test...\n");
    const bad_ptr: *volatile u64 = @ptrFromInt(0xFFFF_8000_0000_0000);
    bad_ptr.* = 0xDEADBEEF;
    while (true) {
        asm volatile ("hlt");
    }
}

fn printNumber(value: anytype) void {
    var num_buf: [32]u8 = undefined;
    const s = std.fmt.bufPrint(&num_buf, "{d}", .{value}) catch "err";
    serialWrite(s);
}

fn printHex(value: u64) void {
    var num_buf: [32]u8 = undefined;
    const s = std.fmt.bufPrint(&num_buf, "0x{x}", .{value}) catch "err";
    serialWrite(s);
}

fn dumpPrincipalCaps(state: *const kernel.KernelState, principal: kernel.PrincipalId, label: []const u8) void {
    serialWrite(label);
    serialWrite(" caps:\n");

    const table = state.getTableConst(principal);
    if (table.len == 0) {
        serialWrite("  none\n");
        return;
    }

    var i: usize = 0;
    while (i < table.len) : (i += 1) {
        const cap = table.caps[i];
        serialWrite("  ");
        printHex(cap.paddr);
        if (!cap.rights.cpu_read and !cap.rights.cpu_write and cap.rights.dma) {
            serialWrite(" (dma)");
        }
        serialWrite("\n");
    }
}

fn dumpCapabilityView(state: *const kernel.KernelState) void {
    dumpPrincipalCaps(state, .Process0, "Process0");
    dumpPrincipalCaps(state, .Device0, "Device0");
}

const ExitBootResult = enum {
    success,
    failed,
};

fn exitBootServicesWithRetry() ExitBootResult {
    var mmap_buffer: [64 * 1024]u8 align(@alignOf(uefi.tables.MemoryDescriptor)) = undefined;

    const st = uefi.system_table;
    const bs = st.boot_services orelse return .failed;

    var attempt: usize = 0;
    while (attempt < 8) : (attempt += 1) {
        const mmap = bs.getMemoryMap(mmap_buffer[0..]) catch return .failed;
        bs.exitBootServices(uefi.handle, mmap.info.key) catch |err| switch (err) {
            // map key 競合は再取得で回復可能。
            error.InvalidParameter => continue,
            else => return .failed,
        };

        // UEFI仕様: ExitBootServices 後に該当ポインタを null 化し、CRC を再計算する。
        st.console_in_handle = null;
        st.con_in = null;
        st.console_out_handle = null;
        st.con_out = null;
        st.standard_error_handle = null;
        st.std_err = null;
        st.boot_services = null;
        st.hdr.crc32 = 0;
        const st_bytes = @as([*]u8, @ptrCast(st))[0..@as(usize, st.hdr.header_size)];
        st.hdr.crc32 = std.hash.Crc32.hash(st_bytes);

        return .success;
    }

    return .failed;
}

fn writeCr3(value: u64) void {
    asm volatile ("mov %[value], %%cr3"
        :
        : [value] "r" (value),
        : .{ .memory = true });
}

fn invlpg(addr: u64) void {
    asm volatile ("invlpg (%[addr])"
        :
        : [addr] "r" (addr),
        : .{ .memory = true });
}

fn installIdentityPageTables0To1GiB() bool {
    @memset(pml4_table[0..], 0);
    @memset(pdp_table[0..], 0);
    var pd_idx: usize = 0;
    while (pd_idx < pd_table_count) : (pd_idx += 1) {
        @memset(pd_tables[pd_idx][0..], 0);
    }

    const pml4_pa: u64 = @intFromPtr(&pml4_table);
    const pdp_pa: u64 = @intFromPtr(&pdp_table);
    const pd0_pa: u64 = @intFromPtr(&pd_tables[0]);

    // この段階では 0..4GiB を identity map する。テーブル実体も 4GiB 未満前提。
    if (pml4_pa >= four_gib or pdp_pa >= four_gib or pd0_pa >= four_gib) return false;

    const kernel_table_flags = page_present | page_rw;
    const kernel_large_page_flags = page_present | page_rw | page_ps;

    pml4_table[0] = pdp_pa | kernel_table_flags;
    pd_idx = 0;
    while (pd_idx < pd_table_count) : (pd_idx += 1) {
        const pd_pa: u64 = @intFromPtr(&pd_tables[pd_idx]);
        pdp_table[pd_idx] = pd_pa | kernel_table_flags;

        var i: usize = 0;
        while (i < page_entries) : (i += 1) {
            const absolute_entry = (pd_idx * page_entries) + i;
            const base = @as(u64, absolute_entry) * two_mib;
            pd_tables[pd_idx][i] = base | kernel_large_page_flags;
        }
    }

    writeCr3(pml4_pa);
    return true;
}

fn hardenKernelMappingsSupervisorOnly() void {
    // kernel の既存 map は ring3 から見えないよう User ビットを強制的に落とす。
    pml4_table[0] &= ~page_user;

    var pdp_idx: usize = 0;
    while (pdp_idx < pd_table_count) : (pdp_idx += 1) {
        pdp_table[pdp_idx] &= ~page_user;

        var pd_idx: usize = 0;
        while (pd_idx < page_entries) : (pd_idx += 1) {
            pd_tables[pdp_idx][pd_idx] &= ~page_user;
        }
    }
}

fn buildUserAddressSpace(user_page_paddr: u64, user_stack_paddr: u64) bool {
    @memset(user_pml4_table[0..], 0);
    @memset(user_pdp_table[0..], 0);
    @memset(user_pd_table[0..], 0);
    @memset(user_pt_table[0..], 0);

    const user_pml4_pa: u64 = @intFromPtr(&user_pml4_table);
    const user_pdp_pa: u64 = @intFromPtr(&user_pdp_table);
    const user_pd_pa: u64 = @intFromPtr(&user_pd_table);
    const user_pt_pa: u64 = @intFromPtr(&user_pt_table);
    if (user_pml4_pa >= four_gib or user_pdp_pa >= four_gib or user_pd_pa >= four_gib or user_pt_pa >= four_gib) return false;
    if (user_page_paddr >= four_gib or user_stack_paddr >= four_gib) return false;

    var i: usize = 256;
    while (i < page_entries) : (i += 1) {
        user_pml4_table[i] = pml4_table[i] & ~page_user;
    }

    const pdp_index: usize = @intCast((user_va >> 30) & 0x1FF);
    const pd_index: usize = @intCast((user_va >> 21) & 0x1FF);
    const user_pt_index: usize = @intCast((user_va >> 12) & 0x1FF);
    const stack_pt_index: usize = @intCast((user_stack_page_va >> 12) & 0x1FF);
    const stack_pd_index: usize = @intCast((user_stack_page_va >> 21) & 0x1FF);
    if (stack_pd_index != pd_index) return false;

    // low-half は kernel identity map を土台にし、対象2ページだけ user 可視にする。
    user_pml4_table[0] = user_pdp_pa | page_present | page_rw | page_user;
    i = 0;
    while (i < pd_table_count) : (i += 1) {
        const pd_pa: u64 = @intFromPtr(&pd_tables[i]);
        user_pdp_table[i] = pd_pa | page_present | page_rw;
    }

    i = 0;
    while (i < page_entries) : (i += 1) {
        user_pd_table[i] = pd_tables[0][i] & ~page_user;
    }
    user_pdp_table[pdp_index] = user_pd_pa | page_present | page_rw | page_user;
    user_pd_table[pd_index] = user_pt_pa | page_present | page_rw | page_user;
    user_pt_table[user_pt_index] = user_page_paddr | page_present | page_rw | page_user;
    user_pt_table[stack_pt_index] = user_stack_paddr | page_present | page_rw | page_user;
    return true;
}

fn buildUserAddressSpaceFromCapabilities(
    state: *const kernel.KernelState,
    principal: kernel.PrincipalId,
    user_page: kernel.PageCapability,
    user_stack_page: kernel.PageCapability,
) bool {
    const table = state.getTableConst(principal);
    const user_cap = table.find(user_page.paddr) orelse return false;
    const stack_cap = table.find(user_stack_page.paddr) orelse return false;
    if (!user_cap.rights.cpu_read or !user_cap.rights.cpu_write) return false;
    if (!stack_cap.rights.cpu_read or !stack_cap.rights.cpu_write) return false;
    return buildUserAddressSpace(user_cap.paddr, stack_cap.paddr);
}

fn writeU64LE(ptr: [*]volatile u8, offset: usize, value: u64) void {
    var i: usize = 0;
    while (i < 8) : (i += 1) {
        ptr[offset + i] = @intCast((value >> @intCast(i * 8)) & 0xFF);
    }
}

fn installUserMemoryWritePfTestCode(user_page_paddr: u64) void {
    // ring3 test code:
    // 1) sys_alloc_page -> RAX (new paddr)
    // 2) sys_map_page(user_unmapped_test_va, paddr, writable=1)
    // 3) mapped write to user_unmapped_test_va (should succeed)
    // 4) sys_move_cap(paddr, to=Device0, rights=dma-only)
    // 5) same write again (Present dropped by sync hook -> #PF expected)
    const code: [*]volatile u8 = @ptrFromInt(user_page_paddr);
    var off: usize = 0;

    // sys_alloc_page
    code[off] = 0x48; // mov rax, imm64
    code[off + 1] = 0xB8;
    writeU64LE(code, off + 2, syscall_alloc_page);
    off += 10;
    code[off] = 0xCD; // int 0x80
    code[off + 1] = 0x80;
    off += 2;
    code[off] = 0x48; // mov rbx, rax (keep new paddr)
    code[off + 1] = 0x89;
    code[off + 2] = 0xC3;
    off += 3;

    // sys_map_page(va=user_unmapped_test_va, paddr=rbx, flags=1 writable)
    code[off] = 0x48; // mov rax, imm64
    code[off + 1] = 0xB8;
    writeU64LE(code, off + 2, syscall_map_page);
    off += 10;
    code[off] = 0x48; // mov rdi, imm64
    code[off + 1] = 0xBF;
    writeU64LE(code, off + 2, user_unmapped_test_va);
    off += 10;
    code[off] = 0x48; // mov rsi, rbx
    code[off + 1] = 0x89;
    code[off + 2] = 0xDE;
    off += 3;
    code[off] = 0x48; // mov rdx, imm64
    code[off + 1] = 0xBA;
    writeU64LE(code, off + 2, 0x1);
    off += 10;
    code[off] = 0xCD; // int 0x80
    code[off + 1] = 0x80;
    off += 2;

    // first mapped write should succeed
    code[off] = 0x48; // mov rax, imm64
    code[off + 1] = 0xB8;
    writeU64LE(code, off + 2, user_unmapped_test_va);
    off += 10;
    code[off] = 0xC7; // mov dword ptr [rax], imm32
    code[off + 1] = 0x00;
    code[off + 2] = 0xDD;
    code[off + 3] = 0xCC;
    code[off + 4] = 0xBB;
    code[off + 5] = 0xAA;
    off += 6;

    // sys_move_cap(paddr=rbx, to=Device0, rights=dma-only=0x4)
    code[off] = 0x48; // mov rax, imm64
    code[off + 1] = 0xB8;
    writeU64LE(code, off + 2, syscall_move_cap);
    off += 10;
    code[off] = 0x48; // mov rdi, rbx
    code[off + 1] = 0x89;
    code[off + 2] = 0xDF;
    off += 3;
    code[off] = 0x48; // mov rsi, imm64 (to Device0)
    code[off + 1] = 0xBE;
    writeU64LE(code, off + 2, 0x1);
    off += 10;
    code[off] = 0x48; // mov rdx, imm64 (dma-only rights bits)
    code[off + 1] = 0xBA;
    writeU64LE(code, off + 2, 0x4);
    off += 10;
    code[off] = 0xCD; // int 0x80
    code[off + 1] = 0x80;
    off += 2;

    // second write should #PF after Present is dropped by pte_sync_hook
    code[off] = 0x48; // mov rax, imm64
    code[off + 1] = 0xB8;
    writeU64LE(code, off + 2, user_unmapped_test_va);
    off += 10;
    code[off] = 0xC7;
    code[off + 1] = 0x00;
    code[off + 2] = 0x78;
    code[off + 3] = 0x56;
    code[off + 4] = 0x34;
    code[off + 5] = 0x12;
    off += 6;

    // fallback: jmp $
    code[off] = 0xEB;
    code[off + 1] = 0xFE;
}

fn enterUserModeIretq(user_entry_va: u64, user_rsp: u64) noreturn {
    const user_cs: u64 = gdt_user_code_selector | 0x3;
    const user_ss: u64 = gdt_user_data_selector | 0x3;
    const user_rflags: u64 = 0x2; // IF=0 で外部割り込みを抑止

    asm volatile (
        \\pushq %[ss]
        \\pushq %[rsp]
        \\pushq %[rflags]
        \\pushq %[cs]
        \\pushq %[rip]
        \\iretq
        :
        : [ss] "r" (user_ss),
          [rsp] "r" (user_rsp),
          [rflags] "r" (user_rflags),
          [cs] "r" (user_cs),
          [rip] "r" (user_entry_va),
        : .{ .memory = true });
    unreachable;
}

fn syncPageTableRightsForPaddr(paddr: u64, rights: kernel.Rights) void {
    const user_pt_base_va = user_va & ~@as(u64, 0x1F_FFFF);
    var i: usize = 0;
    while (i < page_entries) : (i += 1) {
        var entry = user_pt_table[i];
        if ((entry & page_present) == 0) continue;
        const mapped_paddr = entry & page_addr_mask;
        if (mapped_paddr != paddr) continue;

        if (!rights.cpu_read and !rights.cpu_write) {
            entry &= ~page_present;
        } else {
            entry |= page_present;
            if (rights.cpu_write) {
                entry |= page_rw;
            } else {
                entry &= ~page_rw;
            }
        }

        user_pt_table[i] = entry;
        const va = user_pt_base_va + (@as(u64, i) * 4096);
        invlpg(va);
    }
}

fn pageAlignDown(addr: u64) u64 {
    return addr & ~@as(u64, 4095);
}

fn pageAlignUp(addr: u64) u64 {
    return (addr + 4095) & ~@as(u64, 4095);
}

fn isReserved(paddr: u64, reserved: []const ReservedRange) bool {
    for (reserved) |r| {
        if (paddr >= r.start and paddr < r.end) return true;
    }
    return false;
}

fn collectMemoryStatsAndFreePages(
    bs: *uefi.tables.BootServices,
    free_list: *kernel.FreePageList,
) ?MemoryStats {
    var mmap_buffer: [64 * 1024]u8 align(@alignOf(uefi.tables.MemoryDescriptor)) = undefined;
    const mmap = bs.getMemoryMap(mmap_buffer[0..]) catch return null;

    free_list.* = .{};
    var detected_regions: usize = 0;
    var total_usable_bytes: u64 = 0;
    const pml4_start = pageAlignDown(@intFromPtr(&pml4_table));
    const pml4_end = pageAlignUp(@intFromPtr(&pml4_table) + @sizeOf(@TypeOf(pml4_table)));
    const pdp_start = pageAlignDown(@intFromPtr(&pdp_table));
    const pdp_end = pageAlignUp(@intFromPtr(&pdp_table) + @sizeOf(@TypeOf(pdp_table)));
    const pd_start = pageAlignDown(@intFromPtr(&pd_tables));
    const pd_end = pageAlignUp(@intFromPtr(&pd_tables) + @sizeOf(@TypeOf(pd_tables)));
    const user_pml4_start = pageAlignDown(@intFromPtr(&user_pml4_table));
    const user_pml4_end = pageAlignUp(@intFromPtr(&user_pml4_table) + @sizeOf(@TypeOf(user_pml4_table)));
    const user_pdp_start = pageAlignDown(@intFromPtr(&user_pdp_table));
    const user_pdp_end = pageAlignUp(@intFromPtr(&user_pdp_table) + @sizeOf(@TypeOf(user_pdp_table)));
    const user_pd_start = pageAlignDown(@intFromPtr(&user_pd_table));
    const user_pd_end = pageAlignUp(@intFromPtr(&user_pd_table) + @sizeOf(@TypeOf(user_pd_table)));
    const user_pt_start = pageAlignDown(@intFromPtr(&user_pt_table));
    const user_pt_end = pageAlignUp(@intFromPtr(&user_pt_table) + @sizeOf(@TypeOf(user_pt_table)));
    const mmap_start = pageAlignDown(@intFromPtr(&mmap_buffer));
    const mmap_end = pageAlignUp(@intFromPtr(&mmap_buffer) + mmap_buffer.len);
    // カーネル自身が使う最低限の領域は free list から除外する。
    const reserved = [_]ReservedRange{
        .{ .start = 0, .end = reserved_low_mem_end },
        .{ .start = pml4_start, .end = pml4_end },
        .{ .start = pdp_start, .end = pdp_end },
        .{ .start = pd_start, .end = pd_end },
        .{ .start = user_pml4_start, .end = user_pml4_end },
        .{ .start = user_pdp_start, .end = user_pdp_end },
        .{ .start = user_pd_start, .end = user_pd_end },
        .{ .start = user_pt_start, .end = user_pt_end },
        .{ .start = mmap_start, .end = mmap_end },
    };

    var it = mmap.iterator();
    while (it.next()) |desc| {
        if (desc.type == .conventional_memory) {
            const region_id = detected_regions;
            var i: u64 = 0;
            while (i < desc.number_of_pages) : (i += 1) {
                const paddr = desc.physical_start + (i * 4096);
                if (isReserved(paddr, reserved[0..])) continue;
                free_list.appendPage(region_id, paddr) catch return null;
            }
            detected_regions += 1;
            total_usable_bytes += desc.number_of_pages * 4096;
        }
    }

    return .{
        .detected_regions = detected_regions,
        .total_usable_bytes = total_usable_bytes,
    };
}

pub fn main() void {
    serialInit();
    serialWrite("[stage] boot entry\n");
    serialWrite("SakuraMicroKernel Phase1 boot\n");
    _ = gdt_user_code_selector;
    _ = gdt_user_data_selector;

    const bs = uefi.system_table.boot_services orelse {
        serialWrite("boot services missing\n");
        while (true) asm volatile ("hlt");
    };
    const memory_stats = collectMemoryStatsAndFreePages(bs, &global_free_list) orelse {
        serialWrite("memory map parse failed\n");
        while (true) asm volatile ("hlt");
    };
    serialWrite("[stage] memory stats collected\n");
    serialWrite("Detected ");
    printNumber(memory_stats.detected_regions);
    serialWrite(" regions\n");

    const total_usable_mb = memory_stats.total_usable_bytes / (1024 * 1024);
    serialWrite("Total usable memory: ");
    printNumber(total_usable_mb);
    serialWrite("MB\n");
    serialWrite("free pages: ");
    printNumber(global_free_list.len);
    serialWrite("\n");

    if (debug_skip_exit_boot_services) {
        serialWrite("[debug] skip ExitBootServices\n");
    } else {
        serialWrite("try ExitBootServices...\n");
        switch (exitBootServicesWithRetry()) {
            .success => serialWrite("ExitBootServices success\n"),
            .failed => {
                serialWrite("ExitBootServices failed\n");
                while (true) asm volatile ("hlt");
            },
        }
        serialWrite("UEFI services terminated\n");
    }
    loadGdtAndReloadSegments();
    serialWrite("GDT loaded (kernel/user segments)\n");

    if (debug_skip_cr3_switch) {
        serialWrite("[debug] skip CR3 switch\n");
    } else {
        serialWrite("build page tables (identity 0..4GiB)\n");
        if (!installIdentityPageTables0To1GiB()) {
            serialWrite("page table install failed\n");
            while (true) asm volatile ("hlt");
        }
        hardenKernelMappingsSupervisorOnly();
        serialWrite("CR3 switched to custom PML4\n");
    }
    initIdtPageFaultOnly();
    serialWrite("IDT loaded (#PF/#DF/#INT80)\n");
    if (debug_trigger_page_fault_test) {
        triggerPageFaultTest();
    }
    serialWrite("enter bare-metal capability demo\n");

    var state = kernel.KernelState.initFromDetectedRegions(memory_stats.detected_regions) catch |err| {
        serialWrite("region init failed: ");
        serialWrite(@errorName(err));
        serialWrite("\n");
        while (true) asm volatile ("hlt");
    };
    state.pte_sync_hook = syncPageTableRightsForPaddr;
    const user_page = state.allocPageTo(.Process0, &global_free_list) catch |err| {
        serialWrite("allocPageTo for user map failed: ");
        serialWrite(@errorName(err));
        serialWrite("\n");
        while (true) asm volatile ("hlt");
    };
    const user_stack_page = state.allocPageTo(.Process0, &global_free_list) catch |err| {
        serialWrite("allocPageTo for user stack failed: ");
        serialWrite(@errorName(err));
        serialWrite("\n");
        while (true) asm volatile ("hlt");
    };
    if (!buildUserAddressSpaceFromCapabilities(&state, .Process0, user_page, user_stack_page)) {
        serialWrite("user page table build failed\n");
        while (true) asm volatile ("hlt");
    }
    serialWrite("user page table ready\n");
    serialWrite("  user_va=");
    printHex(user_va);
    serialWrite("\n");
    serialWrite("  user_pa=");
    printHex(user_page.paddr);
    serialWrite("\n");
    serialWrite("  user_stack_top=");
    printHex(user_stack_top);
    serialWrite("\n");
    serialWrite("  user_stack_pa=");
    printHex(user_stack_page.paddr);
    serialWrite("\n");
    installUserMemoryWritePfTestCode(user_page.paddr);

    var allocated: [3]u64 = undefined;
    var i: usize = 0;
    while (i < 3) : (i += 1) {
        // free list から Process0 へ 3ページ配布する。
        const cap = state.allocPageTo(.Process0, &global_free_list) catch |err| {
            serialWrite("allocPageTo failed: ");
            serialWrite(@errorName(err));
            serialWrite("\n");
            while (true) asm volatile ("hlt");
        };
        allocated[i] = cap.paddr;
    }

    dumpCapabilityView(&state);
    serialWrite("\nstart_dma ");
    printHex(allocated[1]);
    serialWrite("\n\n");

    state.startDma(allocated[1]) catch |err| {
        serialWrite("DMA start failed: ");
        serialWrite(@errorName(err));
        serialWrite("\n");
        return;
    };
    dumpCapabilityView(&state);
    kernel_state_global = state;
    kernel_state_ready = true;
    serialWrite("\nenter ring3 with iretq (sys_alloc/map/move + expected #PF)\n");
    writeCr3(@intFromPtr(&user_pml4_table));
    enterUserModeIretq(user_va, user_stack_top);
}
