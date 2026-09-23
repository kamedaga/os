const std = @import("std");
const x86_platform = @import("arch/x86_64/platform.zig");
const lapic = @import("lapic.zig");
const interrupts = @import("interrupts.zig");
const scheduler_observer = @import("scheduler_observer.zig");
const mtrr = @import("arch/x86_64/mtrr.zig");

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
    // Fail closed for shorthand broadcast unless MADT described every CPU.
    broadcast_topology_complete: bool = false,
};

var observed_cpu_count: u32 = 1;
var broadcast_online_mask: u64 = 0;
var ap_started: [max_cpus]u32 = [_]u32{0} ** max_cpus;
var cpu_states: [max_cpus]u32 = [_]u32{cpu_state_absent} ** max_cpus;
pub export var runtime_lapic_ids: [max_cpus]u8 = [_]u8{0xFF} ** max_cpus;
var ap_user_timer_vector: u8 = 0;
var ap_user_timer_initial_count: u32 = 0;
var ap_syscall_entry: usize = 0;
var wake_ipi_vector: u8 = 0;

// Serialized by the kernel-state lock, like native IRQ lease changes.
var irq_drain_first: u8 = 0;
var irq_drain_count: u8 = 0;
var irq_drain_generation: u64 = 0;
var irq_drain_ack: [max_cpus]u64 = [_]u64{0} ** max_cpus;

// Captured once during serial CPU startup, immutable once userspace runs.
// No partially booted or late/unverified topology may admit UC_MINUS aliases.
// Keep each CPU's evidence available for diagnosis without rereading MSRs or
// changing the boot-time admission decision. Slot zero belongs to the BSP.
var mmio_cache_snapshots: [max_cpus]mtrr.CacheSnapshot = [_]mtrr.CacheSnapshot{.{}} ** max_cpus;
var mmio_cache_matches: [max_cpus]u32 = [_]u32{0} ** max_cpus;
var mmio_cache_online_mask: u64 = 0;

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
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(broadcast_online_mask), &broadcast_online_mask));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(ap_started), &ap_started));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(cpu_states), &cpu_states));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(runtime_lapic_ids), &runtime_lapic_ids));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(ap_user_timer_vector), &ap_user_timer_vector));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(ap_user_timer_initial_count), &ap_user_timer_initial_count));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(wake_ipi_vector), &wake_ipi_vector));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(irq_drain_first), &irq_drain_first));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(irq_drain_count), &irq_drain_count));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(irq_drain_generation), &irq_drain_generation));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(irq_drain_ack), &irq_drain_ack));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(mmio_cache_snapshots), &mmio_cache_snapshots));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(mmio_cache_matches), &mmio_cache_matches));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(mmio_cache_online_mask), &mmio_cache_online_mask));
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
    return @intCast(@atomicLoad(u32, &observed_cpu_count, .acquire));
}

pub fn cpuState(cpu_slot: usize) CpuState {
    if (cpu_slot >= max_cpus) return .absent;
    return stateFromRaw(@atomicLoad(u32, &cpu_states[cpu_slot], .acquire));
}

pub fn onlineCpuMask() u64 {
    var mask: u64 = 0;
    var cpu_slot: usize = 0;
    while (cpu_slot < max_cpus and cpu_slot < 64) : (cpu_slot += 1) {
        if (!isStarted(cpu_slot)) continue;
        if (runtimeLapicIdPtr(cpu_slot).* == 0xFF) continue;
        switch (cpuState(cpu_slot)) {
            .idle, .user => mask |= @as(u64, 1) << @intCast(cpu_slot),
            .absent, .booting => {},
        }
    }
    return mask;
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
    return currentCpuSlotIfKnown() orelse 0;
}

pub fn currentCpuSlotIfKnown() ?usize {
    return x86_platform.cpuSlotForStackPointer(readStackPointer());
}

pub fn isBootstrapCpu() bool {
    return currentCpuSlot() == 0;
}

pub fn isCpuIdle(cpu_slot: usize) bool {
    return cpuState(cpu_slot) == .idle;
}

fn readU32(addr: u64) u32 {
    const p: [*]const u8 = @ptrFromInt(addr);
    return @as(u32, p[0]) |
        (@as(u32, p[1]) << 8) |
        (@as(u32, p[2]) << 16) |
        (@as(u32, p[3]) << 24);
}

fn readU64(addr: u64) u64 {
    return @as(u64, readU32(addr)) | (@as(u64, readU32(addr + 4)) << 32);
}

fn bytesEqual(addr: u64, expected: []const u8) bool {
    const p: [*]const u8 = @ptrFromInt(addr);
    var i: usize = 0;
    while (i < expected.len) : (i += 1) {
        if (p[i] != expected[i]) return false;
    }
    return true;
}

fn checksumOk(addr: u64, len: usize) bool {
    const p: [*]const u8 = @ptrFromInt(addr);
    var sum: u8 = 0;
    var i: usize = 0;
    while (i < len) : (i += 1) sum +%= p[i];
    return sum == 0;
}

fn tableLength(addr: u64) usize {
    return @intCast(readU32(addr + 4));
}

fn findMadtFromRoot(root_addr: u64, xsdt: bool) ?u64 {
    if (!bytesEqual(root_addr, if (xsdt) "XSDT" else "RSDT")) return null;
    const len = tableLength(root_addr);
    if (len < 36 or !checksumOk(root_addr, len)) return null;
    const entry_size: usize = if (xsdt) 8 else 4;
    var off: usize = 36;
    while (off + entry_size <= len) : (off += entry_size) {
        const table_addr = if (xsdt)
            readU64(root_addr + off)
        else
            @as(u64, readU32(root_addr + off));
        if (table_addr != 0 and bytesEqual(table_addr, "APIC")) return table_addr;
    }
    return null;
}

fn findMadt(rsdp: u64) ?u64 {
    if (rsdp == 0 or !bytesEqual(rsdp, "RSD PTR ") or !checksumOk(rsdp, 20)) return null;
    const revision = (@as([*]const u8, @ptrFromInt(rsdp)))[15];
    if (revision >= 2) {
        const len = readU32(rsdp + 20);
        const xsdt_addr = readU64(rsdp + 24);
        if (len >= 36 and xsdt_addr != 0 and checksumOk(rsdp, @intCast(len))) {
            if (findMadtFromRoot(xsdt_addr, true)) |madt| return madt;
        }
    }
    const rsdt_addr = readU32(rsdp + 16);
    if (rsdt_addr == 0) return null;
    return findMadtFromRoot(rsdt_addr, false);
}

fn appendLapicId(info: *BootInfo, id: u8) void {
    var i: usize = 0;
    while (i < info.lapic_count) : (i += 1) {
        if (info.lapic_ids[i] == id) return;
    }
    if (info.lapic_count >= max_cpus or id == 0xFF) {
        info.broadcast_topology_complete = false;
        if (info.lapic_count >= max_cpus) return;
    }
    info.lapic_ids[info.lapic_count] = id;
    info.lapic_count += 1;
}

fn collectMadtLapicIds(info: *BootInfo, rsdp: u64) bool {
    const madt = findMadt(rsdp) orelse return false;
    const len = tableLength(madt);
    if (len < 44 or !checksumOk(madt, len)) return false;
    info.broadcast_topology_complete = true;
    var off: usize = 44;
    while (off + 2 <= len) {
        const entry_addr = madt + off;
        const entry_type = (@as([*]const u8, @ptrFromInt(entry_addr)))[0];
        const entry_len = (@as([*]const u8, @ptrFromInt(entry_addr)))[1];
        if (entry_len < 2 or off + entry_len > len) return false;
        switch (entry_type) {
            0 => if (entry_len >= 8 and (readU32(entry_addr + 4) & 0x1) != 0) {
                appendLapicId(info, (@as([*]const u8, @ptrFromInt(entry_addr)))[3]);
            } else {
                info.broadcast_topology_complete = false;
            },
            9 => {
                // x2APIC enumeration is not sufficient to prove this xAPIC
                // shorthand's complete destination set. Keep unicast here.
                info.broadcast_topology_complete = false;
                if (entry_len >= 16 and (readU32(entry_addr + 4) & 0x1) != 0) {
                    const x2apic_id = readU32(entry_addr + 8);
                    if (x2apic_id <= std.math.maxInt(u8)) appendLapicId(info, @intCast(x2apic_id));
                }
            },
            else => {},
        }
        off += entry_len;
    }
    if (off != len) info.broadcast_topology_complete = false;
    return info.lapic_count != 0;
}

pub fn prepareBootInfo(rsdp: u64, trampoline_base: u64) ?BootInfo {
    const trampoline_bytes = trampoline_page_bytes;
    if (trampoline_base == 0 or (trampoline_base & 0xFFF) != 0 or
        trampoline_base +| trampoline_bytes > 0x100000)
    {
        return null;
    }
    var info = BootInfo{ .trampoline_base = trampoline_base };
    if (!collectMadtLapicIds(&info, rsdp)) return null;
    return info;
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
    emit32(page, &off, 0x00000900); // or eax, LME | NXE
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
    emit8(page, &off, 0x0D);
    emit32(page, &off, 0x00010002); // or rax, WP | MP
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

fn runtimeLapicIdPtr(cpu_slot: usize) *volatile u8 {
    return @ptrCast(&runtime_lapic_ids[cpu_slot]);
}

fn markStarted(cpu_slot: usize) void {
    @atomicStore(u32, &ap_started[cpu_slot], 1, .release);
}

fn isStarted(cpu_slot: usize) bool {
    return @atomicLoad(u32, &ap_started[cpu_slot], .acquire) != 0;
}

fn setCpuState(cpu_slot: usize, state: CpuState) void {
    @atomicStore(u32, &cpu_states[cpu_slot], @intFromEnum(state), .release);
}

pub fn startIdleAps(info: *BootInfo, kernel_cr3: u64) bool {
    @import("realtime_clock.zig").registerCpu(0, x86_platform.kernelPointerPaddr);
    @atomicStore(u64, &broadcast_online_mask, 0, .release);
    @atomicStore(u64, &mmio_cache_online_mask, 0, .release);
    @atomicStore(u32, &mmio_cache_matches[0], @intFromBool(mtrr.captureCacheSnapshot(&mmio_cache_snapshots[0])), .release);
    if (info.trampoline_base == 0) {
        return false;
    }
    if (info.lapic_count == 0) {
        appendLapicId(info, lapic.localApicId());
    }
    const bsp_id = lapic.localApicId();
    @atomicStore(u32, &observed_cpu_count, if (info.lapic_count == 0) 1 else info.lapic_count, .release);
    runtimeLapicIdPtr(0).* = bsp_id;
    setCpuState(0, .idle);
    markStarted(0);

    var cpu_slot: usize = 1;
    var i: usize = 0;
    while (i < info.lapic_count and cpu_slot < max_cpus) : (i += 1) {
        const apic_id = info.lapic_ids[i];
        if (apic_id == bsp_id) continue;
        @atomicStore(u32, &ap_started[cpu_slot], 0, .release);
        @atomicStore(u32, &mmio_cache_matches[cpu_slot], 0, .release);
        runtimeLapicIdPtr(cpu_slot).* = 0xFF;
        setCpuState(cpu_slot, .booting);
        if (!x86_platform.mapCpuRuntimeStacks(cpu_slot)) {
            setCpuState(cpu_slot, .absent);
            return false;
        }
        const stack_top = x86_platform.cpuKernelStackTop(cpu_slot) orelse {
            setCpuState(cpu_slot, .absent);
            return false;
        };
        // APs are started serially. isStarted() is published only after the
        // AP has left this page and installed its permanent GDT and stack.
        const trampoline_base = info.trampoline_base;
        const vector = buildTrampoline(
            trampoline_base,
            kernel_cr3,
            stack_top,
            cpu_slot,
            @intFromPtr(&apIdleEntry),
        ) orelse {
            setCpuState(cpu_slot, .absent);
            return false;
        };
        if (!lapic.sendInitSipi(apic_id, vector)) {
            setCpuState(cpu_slot, .absent);
            return false;
        }
        var spins: u32 = 0;
        while (!isStarted(cpu_slot) and spins < 10000000) : (spins += 1) {
            asm volatile ("pause");
        }
        if (!isStarted(cpu_slot) or cpuState(cpu_slot) != .idle) {
            setCpuState(cpu_slot, .absent);
            return false;
        }
        cpu_slot += 1;
    }
    const success = cpu_slot == info.lapic_count;
    if (success and info.broadcast_topology_complete) {
        const mask = onlineCpuMask();
        if (@popCount(mask) == info.lapic_count) {
            @atomicStore(u64, &broadcast_online_mask, mask, .release);
            var matching: u64 = 0;
            for (&mmio_cache_matches, 0..) |*value, slot| {
                if (@atomicLoad(u32, value, .acquire) != 0) matching |= @as(u64, 1) << @intCast(slot);
            }
            if (matching == mask) @atomicStore(u64, &mmio_cache_online_mask, mask, .release);
        }
    }
    return success;
}

fn apIdleEntry(cpu_slot: usize) callconv(.winapi) noreturn {
    asm volatile ("cli");
    if (!x86_platform.enableNxForCurrentCpu()) {
        setCpuState(cpu_slot, .absent);
        markStarted(cpu_slot);
        while (true) asm volatile ("hlt");
    }
    if (!x86_platform.enableAvxXStateForCurrentCpu()) {
        setCpuState(cpu_slot, .absent);
        markStarted(cpu_slot);
        while (true) asm volatile ("hlt");
    }
    if (!x86_platform.loadGdtAndReloadSegmentsForCpu(cpu_slot)) {
        setCpuState(cpu_slot, .absent);
        markStarted(cpu_slot);
        while (true) asm volatile ("hlt");
    }
    x86_platform.loadInterruptTableForCurrentCpu();
    if (ap_syscall_entry != 0) {
        x86_platform.enableSyscallEntry(ap_syscall_entry);
    }
    _ = x86_platform.enablePcidIfSupported();
    _ = x86_platform.enablePkuIfSupported();
    // Vector, divider and one-shot mode are CPU-local initialization. Leave
    // the timer stopped until this AP dispatches a user thread.
    const apic_ready = if (ap_user_timer_vector != 0)
        lapic.initTimer(ap_user_timer_vector, 0)
    else
        lapic.enableLocalApic();
    if (!apic_ready) {
        setCpuState(cpu_slot, .absent);
        markStarted(cpu_slot);
        while (true) asm volatile ("hlt");
    }
    const cache_snapshot = &mmio_cache_snapshots[cpu_slot];
    const cache_matches = mtrr.captureCacheSnapshot(cache_snapshot) and
        @atomicLoad(u32, &mmio_cache_matches[0], .acquire) != 0 and
        mmio_cache_snapshots[0].matches(cache_snapshot);
    @atomicStore(u32, &mmio_cache_matches[cpu_slot], @intFromBool(cache_matches), .release);
    runtimeLapicIdPtr(cpu_slot).* = lapic.localApicId();
    @import("realtime_clock.zig").registerCpu(cpu_slot, x86_platform.kernelPointerPaddr);
    setCpuState(cpu_slot, .idle);
    markStarted(cpu_slot);
    lapic.disarmTimer();
    apIdleLoop(cpu_slot);
}

pub fn ucMinusMmioAllowed(paddr: u64, bytes: u64) bool {
    const verified = @atomicLoad(u64, &mmio_cache_online_mask, .acquire);
    return verified != 0 and verified == onlineCpuMask() and
        ucMinusMmioWithSnapshot(&mmio_cache_snapshots[0], paddr, bytes);
}

fn kernelIdentityRanges() [2][2]u64 {
    return .{
        .{ 0, @import("arch/x86_64/physical_layout.zig").identity_limit },
        .{ x86_platform.phys_copy_window_va + (1 << 21), x86_platform.phys_copy_window_va +
            (@as(u64, x86_platform.high_mmio_pdp_table_count) << 30) },
    };
}

fn identityAliasesUncached(snapshot: *const mtrr.CacheSnapshot, paddr: u64, bytes: u64) bool {
    // Caller already validated the entire aligned physical range and overflow.
    for (kernelIdentityRanges()) |range| {
        const start = @max(paddr, range[0]);
        const end = @min(paddr + bytes, range[1]);
        if (start < end and !snapshot.rangeIsUncached(start, end - start)) return false;
    }
    return true;
}

fn ucMinusMmioWithSnapshot(snapshot: *const mtrr.CacheSnapshot, paddr: u64, bytes: u64) bool {
    return snapshot.rangeIsUcMinusUncached(paddr, bytes) and
        identityAliasesUncached(snapshot, paddr, bytes);
}

/// Explicit UC for the kernel's temporary MMIO window. Existing WB-encoded
/// identity aliases must remain effectively UC, including the high window.
pub fn uncachedKernelMmioPageAllowed(paddr: u64) bool {
    const verified = @atomicLoad(u64, &mmio_cache_online_mask, .acquire);
    if (verified == 0 or verified != onlineCpuMask()) return false;
    return uncachedKernelMmioPageWithSnapshot(&mmio_cache_snapshots[0], paddr);
}

fn uncachedKernelMmioPageWithSnapshot(snapshot: *const mtrr.CacheSnapshot, paddr: u64) bool {
    if (!snapshot.rangeSupportsUncachedMapping(paddr, 4096)) return false;
    return identityAliasesUncached(snapshot, paddr, 4096);
}

test "MMIO alias cache proof includes low and high identity boundaries" {
    var snapshot = mtrr.CacheSnapshot{
        .pat = 6 | (@as(u64, 7) << 16),
        .physical_bits = 46,
        .default_type = (1 << 11) | 6,
    };
    const low_end = @import("arch/x86_64/physical_layout.zig").identity_limit;
    const high_start = x86_platform.phys_copy_window_va + (1 << 21);
    const high_end = x86_platform.phys_copy_window_va +
        (@as(u64, x86_platform.high_mmio_pdp_table_count) << 30);
    for ([_]u64{ low_end - 4096, high_start, high_end - 4096 }) |paddr| {
        try std.testing.expect(!uncachedKernelMmioPageWithSnapshot(&snapshot, paddr));
        try std.testing.expect(!ucMinusMmioWithSnapshot(&snapshot, paddr, 4096));
    }
    for ([_]u64{ low_end, high_start - 4096, high_end, 0x380000000000 }) |paddr| {
        try std.testing.expect(uncachedKernelMmioPageWithSnapshot(&snapshot, paddr));
        try std.testing.expect(ucMinusMmioWithSnapshot(&snapshot, paddr, 4096));
    }
    for ([_]u64{ low_end - 4096, high_start - 4096, high_end - 4096 }) |paddr| {
        try std.testing.expect(!ucMinusMmioWithSnapshot(&snapshot, paddr, 8192));
    }
    try std.testing.expect(!ucMinusMmioWithSnapshot(&snapshot, 0x380000000000, std.math.maxInt(u64)));
    try std.testing.expect(!ucMinusMmioWithSnapshot(&snapshot, (1 << 46) - 4096, 8192));
    snapshot.default_type = 1 << 11;
    for ([_]u64{ low_end - 4096, high_start, high_end - 4096 }) |paddr| {
        try std.testing.expect(uncachedKernelMmioPageWithSnapshot(&snapshot, paddr));
        try std.testing.expect(ucMinusMmioWithSnapshot(&snapshot, paddr, 8192));
    }
    try std.testing.expect(!uncachedKernelMmioPageWithSnapshot(&snapshot, 1 << 46));
}

pub fn returnCurrentApToIdleFromInterrupt() noreturn {
    const cpu_slot = currentCpuSlot();
    lapic.disarmTimer();
    x86_platform.writeCr3(x86_platform.kernel_cr3_value);
    setCpuState(cpu_slot, .idle);
    apIdleLoop(cpu_slot);
}

pub fn wakeCpu(cpu_slot: usize) bool {
    if (cpu_slot == currentCpuSlot()) return true;
    if (cpu_slot >= max_cpus) return false;
    // A runnable publication needs its own APIC edge. Coalescing against an
    // earlier vector loses the later publication when the target consumes the
    // first vector immediately before returning to HLT.
    var attempts: usize = 0;
    while (attempts < 3) : (attempts += 1) {
        if (interruptCpu(cpu_slot)) return true;
    }
    return false;
}

/// Deliver the scheduler/wake vector to any online CPU, including the BSP.
/// The vector also carries kernel-internal cross-CPU maintenance requests.
pub fn interruptCpu(cpu_slot: usize) bool {
    if (cpu_slot >= runtime_lapic_ids.len) return false;
    const apic_id = runtime_lapic_ids[cpu_slot];
    if (apic_id == 0xFF) return false;
    if (cpuState(cpu_slot) == .absent) return false;
    if (wake_ipi_vector == 0) return false;
    return lapic.sendFixedIpi(apic_id, wake_ipi_vector);
}

/// Called after EOI and after device publication completes. A maintenance
/// IPI must not acknowledge lower-priority device interrupts still in IRR.
pub fn acknowledgeInterruptDrain() void {
    const generation = @atomicLoad(u64, &irq_drain_generation, .acquire);
    const cpu = currentCpuSlot();
    if (cpu >= max_cpus or generation == 0 or
        @atomicLoad(u64, &irq_drain_ack[cpu], .acquire) == generation) return;
    const first = @atomicLoad(u8, &irq_drain_first, .acquire);
    const count = @atomicLoad(u8, &irq_drain_count, .acquire);
    if (lapic.interruptRangePending(first, count)) return;
    @atomicStore(u64, &irq_drain_ack[cpu], generation, .release);
}

/// Drain every CPU because native MSI address programming belongs to the
/// device owner. Source masking/readback precedes this call; no thread may
/// re-enable that source until it returns. IRQ handlers only try the held
/// kernel-state lock, so the interrupt window cannot recursively acquire it.
pub fn drainInterruptRange(first: u8, count: u8) bool {
    const targets = onlineCpuMask();
    if (targets == 0 or count == 0) return false;
    const prior = @atomicLoad(u64, &irq_drain_generation, .acquire);
    if (prior == std.math.maxInt(u64)) return false;
    const generation = prior + 1;
    @atomicStore(u8, &irq_drain_first, first, .release);
    @atomicStore(u8, &irq_drain_count, count, .release);
    @atomicStore(u64, &irq_drain_generation, generation, .release);
    var cpu: usize = 0;
    var sent = true;
    while (cpu < max_cpus and cpu < 64) : (cpu += 1) {
        if ((targets & (@as(u64, 1) << @intCast(cpu))) != 0 and cpu != currentCpuSlot())
            sent = interruptCpu(cpu) and sent;
    }
    if (!sent) return false;
    var spins: usize = 0;
    while (spins < 10_000_000) : (spins += 1) {
        acknowledgeInterruptDrain();
        var complete = true;
        cpu = 0;
        while (cpu < max_cpus and cpu < 64) : (cpu += 1) {
            if ((targets & (@as(u64, 1) << @intCast(cpu))) != 0 and
                @atomicLoad(u64, &irq_drain_ack[cpu], .acquire) != generation) complete = false;
        }
        if (complete) return true;
        // Let pending device edges run after the maintenance ISR returned.
        // Do not keep injecting a higher-priority maintenance IPI here.
        asm volatile ("sti; pause; cli" ::: .{ .memory = true });
    }
    return false;
}

/// IF must remain clear. Failure means the caller must use per-CPU sends;
/// duplicate delivery is harmless for generation-acknowledged maintenance.
pub fn interruptAllOtherOnlineCpus(target_mask: u64) bool {
    const complete_mask = @atomicLoad(u64, &broadcast_online_mask, .acquire);
    const current = currentCpuSlot();
    if (wake_ipi_vector == 0 or !broadcastMaskMatches(complete_mask, target_mask, onlineCpuMask(), current)) return false;
    if (@popCount(target_mask) <= 1) return true;
    return lapic.sendFixedIpiAllExcludingSelf(wake_ipi_vector);
}

fn broadcastMaskMatches(complete: u64, target: u64, online: u64, current: usize) bool {
    if (complete == 0 or target != complete or online != complete or current >= 64) return false;
    return (target & (@as(u64, 1) << @intCast(current))) != 0;
}

test "broadcast gate rejects incomplete topology and supports 64 CPUs" {
    try std.testing.expect(!broadcastMaskMatches(0, 3, 3, 0));
    try std.testing.expect(!broadcastMaskMatches(3, 1, 3, 0));
    try std.testing.expect(!broadcastMaskMatches(3, 3, 1, 0));
    try std.testing.expect(!broadcastMaskMatches(3, 3, 3, 2));
    try std.testing.expect(!broadcastMaskMatches(3, 3, 3, 64));
    try std.testing.expect(broadcastMaskMatches(1, 1, 1, 0));
    try std.testing.expect(broadcastMaskMatches(255, 255, 255, 7));
    const all = std.math.maxInt(u64);
    try std.testing.expect(broadcastMaskMatches(all, all, all, 63));
}

test "broadcast enumeration fails closed on capacity and reserved APIC ID" {
    var info: BootInfo = .{ .broadcast_topology_complete = true };
    for (0..64) |id| appendLapicId(&info, @intCast(id));
    try std.testing.expect(info.broadcast_topology_complete);
    appendLapicId(&info, 0); // Duplicate records do not drop a physical CPU.
    try std.testing.expect(info.broadcast_topology_complete);
    appendLapicId(&info, 64);
    try std.testing.expect(!info.broadcast_topology_complete);
    try std.testing.expectEqual(@as(u8, 64), info.lapic_count);
    info = .{ .broadcast_topology_complete = true };
    appendLapicId(&info, 0xff);
    try std.testing.expect(!info.broadcast_topology_complete);
}

// Both callers enter with IF clear and the local timer already stopped.
// In particular, interrupt return stops it before changing CR3/CPU state;
// repeating that hardware write here adds no protection.
fn apIdleLoop(cpu_slot: usize) noreturn {
    while (true) {
        scheduler_observer.observeIdle(cpu_slot);
        scheduler_observer.pollIdleScheduler(cpu_slot);
        var user_entry: scheduler_observer.UserEntry = undefined;
        if (scheduler_observer.claimIdleUserEntry(cpu_slot, &user_entry)) {
            setCpuState(cpu_slot, .user);
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

fn enterUserModeFromIdle(entry: *scheduler_observer.UserEntry) noreturn {
    asm volatile ("cli");
    // A runnable thread may carry a pending signal whose IPI reached another
    // thread or an inhibited/kernel context. Every AP redispatch is a user
    // return boundary; do not depend on an uninterrupted timer slice to stage
    // it. Stage before restoring the selected thread's live XState.
    @import("traps.zig").stagePendingSignalForUserReturn(&entry.frame);
    if (ap_user_timer_vector != 0) {
        _ = lapic.armTimer(ap_user_timer_initial_count);
    }
    const x_state: *align(x86_platform.xstate_alignment) const [x86_platform.xstate_bytes]u8 =
        @ptrFromInt(entry.x_state_addr);
    x86_platform.restoreXState(x_state);
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
