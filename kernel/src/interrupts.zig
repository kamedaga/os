pub const IdtEntry = packed struct {
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

pub const TrapFrame = extern struct {
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
    rip: u64,
    cs: u64,
    rflags: u64,
    rsp: u64,
    ss: u64,
};

comptime {
    if (@offsetOf(TrapFrame, "rax") != 112) @compileError("TrapFrame layout mismatch: rax offset");
    if (@offsetOf(TrapFrame, "rip") != 120) @compileError("TrapFrame layout mismatch: rip offset");
    if (@offsetOf(TrapFrame, "ss") != 152) @compileError("TrapFrame layout mismatch: ss offset");
}

pub const ExceptionTrapFrame = extern struct {
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
    error_code: u64,
    rip: u64,
    cs: u64,
    rflags: u64,
    rsp: u64,
    ss: u64,
};

comptime {
    if (@offsetOf(ExceptionTrapFrame, "rax") != 112) @compileError("ExceptionTrapFrame layout mismatch: rax offset");
    if (@offsetOf(ExceptionTrapFrame, "error_code") != 120) @compileError("ExceptionTrapFrame layout mismatch: error_code offset");
    if (@offsetOf(ExceptionTrapFrame, "rip") != 128) @compileError("ExceptionTrapFrame layout mismatch: rip offset");
    if (@offsetOf(ExceptionTrapFrame, "ss") != 160) @compileError("ExceptionTrapFrame layout mismatch: ss offset");
}

pub fn zeroIdtEntry() IdtEntry {
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

pub fn clearIdt(idt: *[256]IdtEntry) void {
    @memset(idt[0..], zeroIdtEntry());
}

pub fn setIdtEntry(idt: *[256]IdtEntry, vec: usize, selector: u16, handler: usize, type_attr: u8) void {
    setIdtEntryWithIst(idt, vec, selector, handler, 0, type_attr);
}

pub fn setIdtEntryWithIst(idt: *[256]IdtEntry, vec: usize, selector: u16, handler: usize, ist: u8, type_attr: u8) void {
    idt[vec] = .{
        .offset_low = @as(u16, @truncate(handler & 0xFFFF)),
        .selector = selector,
        .ist = ist & 0x7,
        .type_attr = type_attr,
        .offset_mid = @as(u16, @truncate((handler >> 16) & 0xFFFF)),
        .offset_high = @as(u32, @truncate(handler >> 32)),
        .zero = 0,
    };
}

pub fn loadIdt(idt: *const [256]IdtEntry) void {
    const idt_ptr = IdtPtr{
        .limit = @as(u16, @intCast(@sizeOf(@TypeOf(idt.*)) - 1)),
        .base = @intFromPtr(idt),
    };
    asm volatile ("lidt (%[ptr])"
        :
        : [ptr] "r" (&idt_ptr),
        : .{ .memory = true }
    );
}
