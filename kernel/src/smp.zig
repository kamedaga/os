const std = @import("std");
const x86_platform = @import("arch/x86_64/platform.zig");
const lapic = @import("lapic.zig");
const interrupts = @import("interrupts.zig");
const scheduler_observer = @import("scheduler_observer.zig");

pub const max_cpus: usize = x86_platform.max_cpus;
const trampoline_page_bytes: usize = 4096;
const cpu_state_absent: u32 = 0;
const cpu_state_booting: u32 = 1;
const cpu_state_idle: u32 = 2;
const cpu_state_user: u32 = 3;

pub const CpuState = enum(u32) {
    absent = cpu_state_absent,
    booting = cpu_state_booting,
    idle = cpu_state_idle,
    user = cpu_state_user,
};

pub const CpuInfo = struct {
    slot: usize,
    lapic_id: ?u8,
    state: CpuState,
    started: bool,
    is_bootstrap: bool,
};

pub const BootInfo = struct {
    trampoline_base: u64 = 0,
    lapic_ids: [max_cpus]u8 = [_]u8{0} ** max_cpus,
    lapic_count: u8 = 0,
};

var observed_cpu_count: u32 = 1;
var ap_started: [max_cpus]u32 = [_]u32{0} ** max_cpus;
var cpu_states: [max_cpus]u32 = [_]u32{cpu_state_absent} ** max_cpus;
var runtime_lapic_ids: [max_cpus]u8 = [_]u8{0xFF} ** max_cpus;
var ap_user_timer_vector: u8 = 0;
var ap_user_timer_initial_count: u32 = 0;
var ap_syscall_entry: usize = 0;
var wake_ipi_vector: u8 = 0;

extern fn stageUserReturnFromFramePointerForCurrentCpu(frame_addr: usize, iret_offset: usize) callconv(.winapi) void;

fn staticStorageEnd(comptime T: type, ptr: *T) usize {
    return @intFromPtr(ptr) + @sizeOf(T);
}

fn maxStaticEnd(a: usize, b: usize) usize {
    return if (a > b) a else b;
}

pub fn kernelStaticStorageEndAddr() usize {
    var end: usize = 0;
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(observed_cpu_count), &observed_cpu_count));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(ap_started), &ap_started));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(cpu_states), &cpu_states));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(runtime_lapic_ids), &runtime_lapic_ids));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(ap_user_timer_vector), &ap_user_timer_vector));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(ap_user_timer_initial_count), &ap_user_timer_initial_count));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(wake_ipi_vector), &wake_ipi_vector));
    return end;
}

pub fn configureApUserTimer(timer_vector: u8, initial_count: u32) void {
    ap_user_timer_vector = timer_vector;
    ap_user_timer_initial_count = initial_count;
}

pub fn configureApSyscallEntry(entry: usize) void {
    ap_syscall_entry = entry;
}

pub fn configureWakeIpiVector(vector: u8) void {
    wake_ipi_vector = vector;
}

fn stateFromRaw(raw: u32) CpuState {
    return switch (raw) {
        cpu_state_booting => .booting,
        cpu_state_idle => .idle,
        cpu_state_user => .user,
        else => .absent,
    };
}

pub fn cpuCount() usize {
    return @intCast((@as(*volatile u32, @ptrCast(&observed_cpu_count))).*);
}

pub fn cpuState(cpu_slot: usize) CpuState {
    if (cpu_slot >= max_cpus) return .absent;
    return stateFromRaw(cpuStatePtr(cpu_slot).*);
}

pub fn cpuInfo(cpu_slot: usize) ?CpuInfo {
    if (cpu_slot >= max_cpus) return null;
    const apic_id = runtimeLapicIdPtr(cpu_slot).*;
    return .{
        .slot = cpu_slot,
        .lapic_id = if (apic_id == 0xFF) null else apic_id,
        .state = cpuState(cpu_slot),
        .started = isStarted(cpu_slot),
        .is_bootstrap = cpu_slot == 0,
    };
}

fn readStackPointer() u64 {
    var rsp: u64 = 0;
    asm volatile ("mov %%rsp, %[out]"
        : [out] "=r" (rsp),
    );
    return rsp;
}

pub fn currentCpuSlot() usize {
    if (x86_platform.cpuSlotForStackPointer(readStackPointer())) |slot| return slot;
    return 0;
}

pub fn isBootstrapCpu() bool {
    return currentCpuSlot() == 0;
}

pub fn isCpuIdle(cpu_slot: usize) bool {
    return cpuState(cpu_slot) == .idle;
}

fn appendLapicId(info: *BootInfo, id: u8) void {
    var i: usize = 0;
    while (i < info.lapic_count) : (i += 1) {
        if (info.lapic_ids[i] == id) return;
    }
    if (info.lapic_count >= max_cpus) return;
    info.lapic_ids[info.lapic_count] = id;
    info.lapic_count += 1;
}

fn emit8(page: [*]u8, off: *usize, value: u8) void {
    page[off.*] = value;
    off.* += 1;
}

fn emit16(page: [*]u8, off: *usize, value: u16) void {
    emit8(page, off, @truncate(value));
    emit8(page, off, @truncate(value >> 8));
}

fn emit32(page: [*]u8, off: *usize, value: u32) void {
    emit16(page, off, @truncate(value));
    emit16(page, off, @truncate(value >> 16));
}

fn emit64(page: [*]u8, off: *usize, value: u64) void {
    emit32(page, off, @truncate(value));
    emit32(page, off, @truncate(value >> 32));
}

fn write64(page: [*]u8, off: usize, value: u64) void {
    var cursor = off;
    emit64(page, &cursor, value);
}

fn buildTrampoline(base: u64, cr3: u64, stack_top: u64, cpu_slot: usize, entry: u64) ?u8 {
    if ((base & 0xFFF) != 0 or base >= 0x100000) return null;
    if ((cr3 >> 32) != 0) return null;
    const page: [*]u8 = @ptrFromInt(base);
    @memset(page[0..trampoline_page_bytes], 0);

    var off: usize = 0;
    const gdt_off: usize = 0x100;
    const gdtr_off: usize = 0x120;
    const protected_off: usize = 0x140;
    const long64_off: usize = 0x180;

    emit8(page, &off, 0xFA); // cli
    emit8(page, &off, 0x0E); // push cs
    emit8(page, &off, 0x1F); // pop ds
    emit8(page, &off, 0x31);
    emit8(page, &off, 0xC0); // xor ax, ax
    emit8(page, &off, 0x8E);
    emit8(page, &off, 0xC0); // mov es, ax
    emit8(page, &off, 0x8E);
    emit8(page, &off, 0xD0); // mov ss, ax
    emit8(page, &off, 0x66);
    emit8(page, &off, 0x0F);
    emit8(page, &off, 0x01);
    emit8(page, &off, 0x16);
    emit16(page, &off, @intCast(gdtr_off)); // lgdt
    emit8(page, &off, 0x66);
    emit8(page, &off, 0x0F);
    emit8(page, &off, 0x20);
    emit8(page, &off, 0xC0); // mov eax, cr0
    emit8(page, &off, 0x66);
    emit8(page, &off, 0x83);
    emit8(page, &off, 0xC8);
    emit8(page, &off, 0x01); // or eax, PE
    emit8(page, &off, 0x66);
    emit8(page, &off, 0x0F);
    emit8(page, &off, 0x22);
    emit8(page, &off, 0xC0); // mov cr0, eax
    emit8(page, &off, 0x66);
    emit8(page, &off, 0xEA);
    emit32(page, &off, @intCast(base + protected_off));
    emit16(page, &off, 0x18); // ljmp protected32

    write64(page, gdt_off + 0, 0);
    write64(page, gdt_off + 8, 0x00AF9A000000FFFF);
    write64(page, gdt_off + 16, 0x00CF92000000FFFF);
    write64(page, gdt_off + 24, 0x00CF9A000000FFFF);
    var gdtr_cursor = gdtr_off;
    emit16(page, &gdtr_cursor, 31);
    emit32(page, &gdtr_cursor, @intCast(base + gdt_off));

    off = protected_off;
    emit8(page, &off, 0x66);
    emit8(page, &off, 0xB8);
    emit16(page, &off, 0x10); // mov ax, 0x10
    emit8(page, &off, 0x8E);
    emit8(page, &off, 0xD8); // mov ds, ax
    emit8(page, &off, 0x8E);
    emit8(page, &off, 0xC0); // mov es, ax
    emit8(page, &off, 0x8E);
    emit8(page, &off, 0xD0); // mov ss, ax
    emit8(page, &off, 0x0F);
    emit8(page, &off, 0x20);
    emit8(page, &off, 0xE0); // mov eax, cr4
    emit8(page, &off, 0x83);
    emit8(page, &off, 0xC8);
    emit8(page, &off, 0x20); // or eax, PAE
    emit8(page, &off, 0x0F);
    emit8(page, &off, 0x22);
    emit8(page, &off, 0xE0); // mov cr4, eax
    emit8(page, &off, 0xB8);
    emit32(page, &off, @truncate(cr3)); // mov eax, cr3
    emit8(page, &off, 0x0F);
    emit8(page, &off, 0x22);
    emit8(page, &off, 0xD8); // mov cr3, eax
    emit8(page, &off, 0xB9);
    emit32(page, &off, 0xC0000080); // mov ecx, EFER
    emit8(page, &off, 0x0F);
    emit8(page, &off, 0x32); // rdmsr
    emit8(page, &off, 0x0D);
    emit32(page, &off, 0x00000100); // or eax, LME
    emit8(page, &off, 0x0F);
    emit8(page, &off, 0x30); // wrmsr
    emit8(page, &off, 0x0F);
    emit8(page, &off, 0x20);
    emit8(page, &off, 0xC0); // mov eax, cr0
    emit8(page, &off, 0x0D);
    emit32(page, &off, 0x80000000); // or eax, PG
    emit8(page, &off, 0x0F);
    emit8(page, &off, 0x22);
    emit8(page, &off, 0xC0); // mov cr0, eax
    emit8(page, &off, 0xEA);
    emit32(page, &off, @intCast(base + long64_off));
    emit16(page, &off, 0x08); // ljmp long64

    off = long64_off;
    emit8(page, &off, 0x66);
    emit8(page, &off, 0xB8);
    emit16(page, &off, 0x10); // mov ax, 0x10
    emit8(page, &off, 0x8E);
    emit8(page, &off, 0xD8); // mov ds, ax
    emit8(page, &off, 0x8E);
    emit8(page, &off, 0xC0); // mov es, ax
    emit8(page, &off, 0x8E);
    emit8(page, &off, 0xD0); // mov ss, ax
    emit8(page, &off, 0x0F);
    emit8(page, &off, 0x20);
    emit8(page, &off, 0xC0); // mov rax, cr0
    emit8(page, &off, 0x48);
    emit8(page, &off, 0x83);
    emit8(page, &off, 0xE0);
    emit8(page, &off, 0xFB); // and rax, ~EM
    emit8(page, &off, 0x48);
    emit8(page, &off, 0x83);
    emit8(page, &off, 0xC8);
    emit8(page, &off, 0x02); // or rax, MP
    emit8(page, &off, 0x0F);
    emit8(page, &off, 0x22);
    emit8(page, &off, 0xC0); // mov cr0, rax
    emit8(page, &off, 0x0F);
    emit8(page, &off, 0x20);
    emit8(page, &off, 0xE0); // mov rax, cr4
    emit8(page, &off, 0x48);
    emit8(page, &off, 0x0D);
    emit32(page, &off, 0x00000600); // or rax, OSFXSR | OSXMMEXCPT
    emit8(page, &off, 0x0F);
    emit8(page, &off, 0x22);
    emit8(page, &off, 0xE0); // mov cr4, rax
    emit8(page, &off, 0x0F);
    emit8(page, &off, 0x06); // clts
    emit8(page, &off, 0xDB);
    emit8(page, &off, 0xE3); // fninit
    emit8(page, &off, 0x48);
    emit8(page, &off, 0xBC);
    emit64(page, &off, stack_top); // mov rsp, imm64
    emit8(page, &off, 0x48);
    emit8(page, &off, 0xB9);
    emit64(page, &off, @intCast(cpu_slot)); // mov rcx, imm64
    emit8(page, &off, 0x48);
    emit8(page, &off, 0xB8);
    emit64(page, &off, entry); // mov rax, imm64
    emit8(page, &off, 0x48);
    emit8(page, &off, 0x83);
    emit8(page, &off, 0xEC);
    emit8(page, &off, 0x20); // sub rsp, 32
    emit8(page, &off, 0xFF);
    emit8(page, &off, 0xD0); // call rax
    emit8(page, &off, 0xFA);
    emit8(page, &off, 0xF4);
    emit8(page, &off, 0xEB);
    emit8(page, &off, 0xFD);

    return @intCast(base >> 12);
}

fn apStartedPtr(cpu_slot: usize) *volatile u32 {
    return @ptrCast(&ap_started[cpu_slot]);
}

fn cpuStatePtr(cpu_slot: usize) *volatile u32 {
    return @ptrCast(&cpu_states[cpu_slot]);
}

fn runtimeLapicIdPtr(cpu_slot: usize) *volatile u8 {
    return @ptrCast(&runtime_lapic_ids[cpu_slot]);
}

fn markStarted(cpu_slot: usize) void {
    apStartedPtr(cpu_slot).* = 1;
}

fn isStarted(cpu_slot: usize) bool {
    return apStartedPtr(cpu_slot).* != 0;
}

pub fn markBootstrapCpuReady() void {
    observed_cpu_count = 1;
    runtimeLapicIdPtr(0).* = lapic.localApicId();
    cpuStatePtr(0).* = cpu_state_idle;
    apStartedPtr(0).* = 1;
}

pub fn startIdleAps(info: *BootInfo, kernel_cr3: u64) void {
    if (info.trampoline_base == 0) {
        markBootstrapCpuReady();
        return;
    }
    if (info.lapic_count == 0) {
        appendLapicId(info, lapic.localApicId());
    }
    const bsp_id = lapic.localApicId();
    observed_cpu_count = if (info.lapic_count == 0) 1 else info.lapic_count;
    runtimeLapicIdPtr(0).* = bsp_id;
    cpuStatePtr(0).* = cpu_state_idle;
    apStartedPtr(0).* = 1;

    var cpu_slot: usize = 1;
    var i: usize = 0;
    while (i < info.lapic_count and cpu_slot < max_cpus) : (i += 1) {
        const apic_id = info.lapic_ids[i];
        if (apic_id == bsp_id) continue;
        apStartedPtr(cpu_slot).* = 0;
        runtimeLapicIdPtr(cpu_slot).* = 0xFF;
        cpuStatePtr(cpu_slot).* = cpu_state_booting;
        if (!x86_platform.mapCpuRuntimeStacks(cpu_slot)) {
            cpu_slot += 1;
            continue;
        }
        const stack_top = x86_platform.cpuKernelStackTop(cpu_slot) orelse {
            cpu_slot += 1;
            continue;
        };
        const trampoline_base = info.trampoline_base + (@as(u64, @intCast(cpu_slot)) * trampoline_page_bytes);
        const vector = buildTrampoline(
            trampoline_base,
            kernel_cr3,
            stack_top,
            cpu_slot,
            @intFromPtr(&apIdleEntry),
        ) orelse {
            cpu_slot += 1;
            continue;
        };
        if (!lapic.sendInitSipi(apic_id, vector)) {
            cpu_slot += 1;
            continue;
        }
        var spins: u32 = 0;
        while (!isStarted(cpu_slot) and spins < 10000000) : (spins += 1) {
            asm volatile ("pause");
        }
        cpu_slot += 1;
    }
}

fn apIdleEntry(cpu_slot: usize) callconv(.winapi) noreturn {
    asm volatile ("cli");
    if (!x86_platform.loadGdtAndReloadSegmentsForCpu(cpu_slot)) {
        cpuStatePtr(cpu_slot).* = cpu_state_absent;
        markStarted(cpu_slot);
        while (true) asm volatile ("hlt");
    }
    x86_platform.loadInterruptTableForCurrentCpu();
    if (ap_syscall_entry != 0) {
        x86_platform.enableSyscallEntry(ap_syscall_entry);
    }
    _ = x86_platform.enablePcidIfSupported();
    _ = x86_platform.enablePkuIfSupported();
    _ = lapic.enableLocalApic();
    runtimeLapicIdPtr(cpu_slot).* = lapic.localApicId();
    cpuStatePtr(cpu_slot).* = cpu_state_idle;
    markStarted(cpu_slot);
    apIdleLoop(cpu_slot);
}

pub fn returnCurrentApToIdleFromInterrupt() noreturn {
    const cpu_slot = currentCpuSlot();
    x86_platform.writeCr3(x86_platform.kernel_cr3_value);
    cpuStatePtr(cpu_slot).* = cpu_state_idle;
    apIdleLoop(cpu_slot);
}

pub fn wakeCpu(cpu_slot: usize) bool {
    if (cpu_slot == 0 or cpu_slot >= runtime_lapic_ids.len) return false;
    const apic_id = runtime_lapic_ids[cpu_slot];
    if (apic_id == 0xFF) return false;
    if (cpuState(cpu_slot) == .absent) return false;
    if (wake_ipi_vector == 0) return true;
    return lapic.sendFixedIpi(apic_id, wake_ipi_vector);
}

fn apIdleLoop(cpu_slot: usize) noreturn {
    while (true) {
        scheduler_observer.observeIdle(cpu_slot);
        scheduler_observer.pollIdleScheduler(cpu_slot);
        var user_entry: scheduler_observer.UserEntry = undefined;
        if (scheduler_observer.claimIdleUserEntry(cpu_slot, &user_entry)) {
            cpuStatePtr(cpu_slot).* = cpu_state_user;
            enterUserModeFromIdle(&user_entry);
        }
        asm volatile ("sti; hlt; cli" ::: .{ .memory = true });
    }
}

fn apIretToUser(entry: *const scheduler_observer.UserEntry) callconv(.winapi) void {
    asm volatile (std.fmt.comptimePrint(
            \\mov %[entry], %%rbx
            \\mov {d}(%%rbx), %%r11
            \\push %%r11
            \\lea {d}(%%rbx), %%rbx
            \\mov {d}(%%rbx), %%r10
            \\push %%r10
            \\mov {d}(%%rbx), %%r10
            \\push %%r10
            \\mov {d}(%%rbx), %%r10
            \\push %%r10
            \\mov {d}(%%rbx), %%r10
            \\push %%r10
            \\mov {d}(%%rbx), %%r10
            \\push %%r10
            \\mov {d}(%%rbx), %%r10
            \\push %%r10
            \\mov {d}(%%rbx), %%r10
            \\push %%r10
            \\mov {d}(%%rbx), %%r10
            \\push %%r10
            \\mov {d}(%%rbx), %%r10
            \\push %%r10
            \\mov {d}(%%rbx), %%r15
            \\mov {d}(%%rbx), %%r14
            \\mov {d}(%%rbx), %%r13
            \\mov {d}(%%rbx), %%r12
            \\mov {d}(%%rbx), %%r9
            \\mov {d}(%%rbx), %%r8
            \\mov {d}(%%rbx), %%rbp
            \\mov {d}(%%rbx), %%rdi
            \\mov {d}(%%rbx), %%rsi
            \\mov {d}(%%rbx), %%rdx
            \\mov {d}(%%rbx), %%rcx
            \\mov 72(%%rsp), %%r11
            \\mov %%r11, %%cr3
            \\pop %%rax
            \\pop %%r10
            \\pop %%rbx
            \\pop %%r11
            \\iretq
        , .{
            @offsetOf(scheduler_observer.UserEntry, "cr3"),
            @offsetOf(scheduler_observer.UserEntry, "frame"),
            @offsetOf(interrupts.TrapFrame, "ss"),
            @offsetOf(interrupts.TrapFrame, "rsp"),
            @offsetOf(interrupts.TrapFrame, "rflags"),
            @offsetOf(interrupts.TrapFrame, "cs"),
            @offsetOf(interrupts.TrapFrame, "rip"),
            @offsetOf(interrupts.TrapFrame, "r11"),
            @offsetOf(interrupts.TrapFrame, "rbx"),
            @offsetOf(interrupts.TrapFrame, "r10"),
            @offsetOf(interrupts.TrapFrame, "rax"),
            @offsetOf(interrupts.TrapFrame, "r15"),
            @offsetOf(interrupts.TrapFrame, "r14"),
            @offsetOf(interrupts.TrapFrame, "r13"),
            @offsetOf(interrupts.TrapFrame, "r12"),
            @offsetOf(interrupts.TrapFrame, "r9"),
            @offsetOf(interrupts.TrapFrame, "r8"),
            @offsetOf(interrupts.TrapFrame, "rbp"),
            @offsetOf(interrupts.TrapFrame, "rdi"),
            @offsetOf(interrupts.TrapFrame, "rsi"),
            @offsetOf(interrupts.TrapFrame, "rdx"),
            @offsetOf(interrupts.TrapFrame, "rcx"),
        })
        :
        : [entry] "r" (entry),
        : .{ .memory = true });
}

fn jumpToUserReturn() callconv(.winapi) void {
    asm volatile ("jmp userReturnToSavedFrame" ::: .{ .memory = true });
}

fn enterUserModeFromIdle(entry: *const scheduler_observer.UserEntry) noreturn {
    asm volatile ("cli");
    if (ap_user_timer_vector != 0) {
        _ = lapic.initTimer(ap_user_timer_vector, ap_user_timer_initial_count);
    }
    const fx_state: *const [512]u8 align(16) = @ptrFromInt(entry.fx_state_addr);
    asm volatile ("fxrstor64 (%[ptr])"
        :
        : [ptr] "r" (fx_state),
        : .{ .memory = true });
    x86_platform.writeFsBase(entry.fs_base);
    x86_platform.writeGsBase(entry.gs_base);
    x86_platform.writePkru(entry.pkru);
    stageUserReturnFromFramePointerForCurrentCpu(
        @intFromPtr(&entry.frame),
        @offsetOf(interrupts.TrapFrame, "rip"),
    );
    jumpToUserReturn();
    while (true) asm volatile ("hlt");
}
