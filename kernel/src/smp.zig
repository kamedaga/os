const lock_diagnostics = @import("sync/lock_diagnostics.zig");
const std = @import("std");
const x86_platform = @import("arch/x86_64/platform.zig");
const lapic = @import("lapic.zig");
const interrupts = @import("interrupts.zig");
const scheduler_observer = @import("scheduler_observer.zig");
const mtrr = @import("arch/x86_64/mtrr.zig");

const runtime = @import("arch/x86_64/cpu_runtime.zig");
pub const CpuSet = @import("cpu_set.zig").CpuSet;
const topology = @import("cpu_topology.zig");
pub fn cpuCapacity() usize {
    return runtime.capacity();
}
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
    lapic_id: ?u32,
    state: CpuState,
    started: bool,
    is_bootstrap: bool,
};

pub const BootInfo = struct {
    trampoline_base: u64 = 0,
    lapic_ids: []u32 = &.{},
    lapic_count: usize = 0,
    // Fail closed for shorthand broadcast unless MADT described every CPU.
    broadcast_topology_complete: bool = false,
};

var observed_cpu_count: u32 = 1;
var broadcast_online_mask: CpuSet = .{ .words = &.{}, .capacity = 0 };
var online_set: CpuSet = .{ .words = &.{}, .capacity = 0 };
var matching_set: CpuSet = .{ .words = &.{}, .capacity = 0 };
var ap_started: []u32 = &.{};
var cpu_states: []u32 = &.{};
pub export var runtime_lapic_ids: [*]u32 = undefined;
var runtime_lapic_id_storage: []u32 = &.{};
var ap_user_timer_vector: u8 = 0;
var ap_user_timer_initial_count: u32 = 0;
// Each CPU's LSTAR names its own entry stub (see traps.syscallEntryForCpu).
var ap_syscall_entry: ?*const fn (usize) usize = null;
var wake_ipi_vector: u8 = 0;

// Serialized by the kernel-state lock, like native IRQ lease changes.
var irq_drain_first: u8 = 0;
var irq_drain_count: u8 = 0;
var irq_drain_generation: u64 = 0;
var irq_drain_ack: []u64 = &.{};
const NotifyWake = @import("sync/notify_wake.zig").Token;
var notify_wake: []NotifyWake = &.{};

// Captured once during serial CPU startup, immutable once userspace runs.
// No partially booted or late/unverified topology may admit UC_MINUS aliases.
// Keep each CPU's evidence available for diagnosis without rereading MSRs or
// changing the boot-time admission decision. Slot zero belongs to the BSP.
var mmio_cache_snapshots: []mtrr.CacheSnapshot = &.{};
var mmio_cache_matches: []u32 = &.{};
var mmio_cache_online_mask: CpuSet = .{ .words = &.{}, .capacity = 0 };

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
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(online_set), &online_set));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(matching_set), &matching_set));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(runtime_lapic_id_storage), &runtime_lapic_id_storage));
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
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(notify_wake), &notify_wake));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(mmio_cache_snapshots), &mmio_cache_snapshots));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(mmio_cache_matches), &mmio_cache_matches));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(mmio_cache_online_mask), &mmio_cache_online_mask));
    return end;
}

pub fn configureApUserTimer(timer_vector: u8, initial_count: u32) void {
    ap_user_timer_vector = timer_vector;
    ap_user_timer_initial_count = initial_count;
}

pub fn configureApSyscallEntry(entry_for_cpu: *const fn (usize) usize) void {
    ap_syscall_entry = entry_for_cpu;
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
    if (cpu_slot >= cpu_states.len) return .absent;
    return stateFromRaw(@atomicLoad(u32, &cpu_states[cpu_slot], .acquire));
}

// Published only by the BSP after each serial AP startup; runtime topology
// is immutable before user dispatch. Consumers borrow it without scratch races.
pub fn onlineCpuSet() CpuSet {
    return online_set;
}

pub fn initCpuStorage(count: usize) !void {
    ap_started = try runtime.alloc(u32, count);
    cpu_states = try runtime.alloc(u32, count);
    runtime_lapic_id_storage = try runtime.alloc(u32, count);
    @memset(runtime_lapic_id_storage, 0xffffffff);
    runtime_lapic_ids = runtime_lapic_id_storage.ptr;
    irq_drain_ack = try runtime.alloc(u64, count);
    notify_wake = try runtime.alloc(NotifyWake, count);
    mmio_cache_snapshots = try runtime.alloc(mtrr.CacheSnapshot, count);
    mmio_cache_matches = try runtime.alloc(u32, count);
    online_set = CpuSet.init(try runtime.alloc(usize, CpuSet.wordCount(count)), count);
    matching_set = CpuSet.init(try runtime.alloc(usize, CpuSet.wordCount(count)), count);
    broadcast_online_mask = CpuSet.init(try runtime.alloc(usize, CpuSet.wordCount(count)), count);
    mmio_cache_online_mask = CpuSet.init(try runtime.alloc(usize, CpuSet.wordCount(count)), count);
}

pub fn cpuInfo(cpu_slot: usize) ?CpuInfo {
    if (cpu_slot >= runtime_lapic_id_storage.len) return null;
    const apic_id = runtimeLapicIdPtr(cpu_slot).*;
    return .{
        .slot = cpu_slot,
        .lapic_id = if (apic_id == 0xFFFFFFFF) null else apic_id,
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
    // Live user XState crosses this helper on both sides of save/restore.
    // Do not route through the aggregate optional facade: even a return-value
    // copy may use XMM0. Preserve the historical unknown -> BSP fallback here;
    // diagnostics use the explicit unknown result instead.
    return x86_platform.cpuSlotForStackPointer(readStackPointer()).indexOr(0);
}

pub fn currentCpuSlotIfKnown() ?usize {
    return x86_platform.cpuSlotForStackPointer(readStackPointer()).index();
}

/// Strict scalar identity for live-XState code. Unlike currentCpuSlot(), an
/// unrecognized stack must not borrow the bootstrap CPU's context pin.
pub fn currentCpuSlotOrInvalid() usize {
    return x86_platform.cpuSlotForStackPointer(readStackPointer()).indexOr(cpuCapacity());
}

pub fn isBootstrapCpu() bool {
    return currentCpuSlot() == 0;
}

pub fn isCpuIdle(cpu_slot: usize) bool {
    return cpuState(cpu_slot) == .idle;
}

fn appendLapicId(info: *BootInfo, id: u32) bool {
    topology.append(info.lapic_ids, &info.lapic_count, .{ .id = id, .enabled = true, .x2 = lapic.usesX2Apic() }, lapic.usesX2Apic()) catch return false;
    return true;
}

pub fn prepareBootInfo(rsdp: u64, trampoline_base: u64) ?BootInfo {
    if (trampoline_base == 0 or trampoline_base & 0xfff != 0 or trampoline_base +| trampoline_page_bytes > 0x100000) return null;
    var entries = @import("acpi_tables.zig").madtEntries(rsdp) orelse return null;
    var count: usize = 1; // BSP fallback also has owned storage.
    while (entries.next() catch return null) |record| {
        if ((topology.decode(record) catch return null)) |entry| {
            if (entry.enabled) count = std.math.add(usize, count, 1) catch return null;
        }
    }
    var info = BootInfo{ .trampoline_base = trampoline_base, .lapic_ids = runtime.alloc(u32, count) catch return null, .broadcast_topology_complete = true };
    entries = @import("acpi_tables.zig").madtEntries(rsdp) orelse return null;
    while (entries.next() catch return null) |record| {
        if ((topology.decode(record) catch return null)) |entry| {
            if (!entry.enabled or entry.x2) info.broadcast_topology_complete = false;
            topology.append(info.lapic_ids, &info.lapic_count, entry, lapic.usesX2Apic()) catch return null;
        }
    }
    const before = info.lapic_count;
    if (!appendLapicId(&info, lapic.localApicId())) return null;
    if (before != info.lapic_count) info.broadcast_topology_complete = false;
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

fn buildTrampoline(base: u64, cr3: u64, formal_cr3: u64, stack_top: u64, cpu_slot: usize, entry: u64) ?u8 {
    if ((base & 0xFFF) != 0 or base >= 0x100000) return null;
    if ((cr3 >> 32) != 0) return null;
    const page = trampolineWriter(.fromInt(base)) orelse unreachable;
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

    // The code/GDT page is RX when paging starts with WP enabled. Preset
    // accessed bits so segment loads cannot attempt a write to that page.
    write64(page, gdt_off + 0, 0);
    write64(page, gdt_off + 8, 0x00AF9B000000FFFF);
    write64(page, gdt_off + 16, 0x00CF93000000FFFF);
    write64(page, gdt_off + 24, 0x00CF9B000000FFFF);
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
    emit32(page, &off, 0x80010000); // or eax, PG | WP
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
    emit8(page, &off, 0xBA);
    emit64(page, &off, formal_cr3); // mov rdx, imm64: consumed only in high code
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

fn runtimeLapicIdPtr(cpu_slot: usize) *volatile u32 {
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
    broadcast_online_mask.clear();
    mmio_cache_online_mask.clear();
    _ = mtrr.ensureWcPatSlot4();
    @atomicStore(u32, &mmio_cache_matches[0], @intFromBool(mtrr.captureCacheSnapshot(&mmio_cache_snapshots[0])), .release);
    if (info.trampoline_base == 0) {
        return false;
    }
    if (info.lapic_count == 0) {
        if (!appendLapicId(info, lapic.localApicId())) return false;
    }
    const bsp_id = lapic.localApicId();
    @atomicStore(u32, &observed_cpu_count, @intCast(if (info.lapic_count == 0) 1 else info.lapic_count), .release);
    runtimeLapicIdPtr(0).* = bsp_id;
    setCpuState(0, .idle);
    markStarted(0);
    online_set.insertAtomic(0);

    const boot_cr3 = x86_platform.prepareApBootRoot(info.trampoline_base, @import("kernel_runtime.zig").global_free_list) orelse return false;
    var cpu_slot: usize = 1;
    var i: usize = 0;
    while (i < info.lapic_count and cpu_slot < cpuCapacity()) : (i += 1) {
        const apic_id = info.lapic_ids[i];
        if (apic_id == bsp_id) continue;
        @atomicStore(u32, &ap_started[cpu_slot], 0, .release);
        @atomicStore(u32, &mmio_cache_matches[cpu_slot], 0, .release);
        runtimeLapicIdPtr(cpu_slot).* = 0xFFFFFFFF;
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
        if (!x86_platform.setApTrampolinePermissions(trampoline_base, true, false)) return false;
        const vector = buildTrampoline(
            trampoline_base,
            boot_cr3,
            kernel_cr3,
            stack_top,
            cpu_slot,
            @intFromPtr(&apHighEntry),
        ) orelse {
            setCpuState(cpu_slot, .absent);
            return false;
        };
        if (!x86_platform.setApTrampolinePermissions(trampoline_base, false, true)) return false;
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
        online_set.insertAtomic(cpu_slot);
        cpu_slot += 1;
    }
    if (!x86_platform.setApTrampolinePermissions(info.trampoline_base, false, false)) return false;
    const success = cpu_slot == info.lapic_count;
    if (success) {
        if (online_set.count() == info.lapic_count) {
            if (info.broadcast_topology_complete) broadcast_online_mask.copyFrom(online_set);
            for (mmio_cache_matches, 0..) |*value, slot| {
                if (@atomicLoad(u32, value, .acquire) != 0) matching_set.insert(slot);
            }
            if (cacheEvidenceForOnlineCpus(success, info.lapic_count, online_set, matching_set))
                mmio_cache_online_mask.copyFrom(online_set);
        }
    }
    if (success and !x86_platform.retireApBootRoot(@import("kernel_runtime.zig").global_free_list)) return false;
    return success;
}

// The boot root shares only image/stack slot 511. No direct-map access or
// Zig prologue may precede this full-width CR3 load; RCX preserves cpu_slot.
fn apHighEntry() callconv(.naked) noreturn {
    asm volatile (
        \\mov %%rdx, %%cr3
        \\movabs %[target], %%rax
        \\jmp *%%rax
        :
        : [target] "i" (&apIdleEntry),
    );
}

fn apIdleEntry(cpu_slot: usize) callconv(.winapi) noreturn {
    asm volatile ("cli");
    if (!x86_platform.enableNxForCurrentCpu() or !x86_platform.enableWriteProtectForCurrentCpu()) {
        setCpuState(cpu_slot, .absent);
        markStarted(cpu_slot);
        while (true) asm volatile ("hlt");
    }
    @import("kernel_log.zig").writeFmt("permissions: cpu={} WP=1 NXE=1\n", .{cpu_slot});
    if (!x86_platform.enableAvxXStateForCurrentCpu()) {
        setCpuState(cpu_slot, .absent);
        markStarted(cpu_slot);
        while (true) asm volatile ("hlt");
    }
    x86_platform.configureXStateOptimizationForCpu(cpu_slot);
    if (!x86_platform.loadGdtAndReloadSegmentsForCpu(cpu_slot)) {
        setCpuState(cpu_slot, .absent);
        markStarted(cpu_slot);
        while (true) asm volatile ("hlt");
    }
    // Entries load kernel_cr3_entry_value, which carries the PCID no-flush
    // bit once the BSP enabled PCIDs; that bit raises #GP without CR4.PCIDE.
    // Enable it before any vector can fire, and keep a CPU that cannot out.
    if (!x86_platform.enablePcidIfSupported() and x86_platform.pcid_enabled != 0) {
        setCpuState(cpu_slot, .absent);
        markStarted(cpu_slot);
        while (true) asm volatile ("hlt");
    }
    // Base save/install executes RDFSBASE/WRFSBASE once the BSP enabled
    // them; without CR4.FSGSBASE here they raise #UD on the first switch.
    if (x86_platform.fsgsbase_enabled != 0 and !x86_platform.enableFsgsbaseOnCurrentCpu()) {
        setCpuState(cpu_slot, .absent);
        markStarted(cpu_slot);
        while (true) asm volatile ("hlt");
    }
    x86_platform.loadInterruptTableForCurrentCpu();
    if (ap_syscall_entry) |entry_for_cpu| {
        x86_platform.enableSyscallEntry(entry_for_cpu(cpu_slot));
    }
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
    _ = mtrr.ensureWcPatSlot4();
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

pub const UcMinusMmioStatus = enum { ready, cache_evidence_missing, unsafe_range };

fn ucMinusMmioStatusWithSnapshot(
    snapshot: *const mtrr.CacheSnapshot,
    verified: CpuSet,
    online: CpuSet,
    paddr: u64,
    bytes: u64,
) UcMinusMmioStatus {
    if (verified.count() == 0 or !verified.eql(online)) return .cache_evidence_missing;
    return if (ucMinusMmioWithSnapshot(snapshot, paddr, bytes)) .ready else .unsafe_range;
}

pub fn ucMinusMmioStatus(paddr: u64, bytes: u64) UcMinusMmioStatus {
    const verified = mmio_cache_online_mask;
    return ucMinusMmioStatusWithSnapshot(&mmio_cache_snapshots[0], verified, onlineCpuSet(), paddr, bytes);
}

pub fn ucMinusMmioAllowed(paddr: u64, bytes: u64) bool {
    return ucMinusMmioStatus(paddr, bytes) == .ready;
}

pub const WcMmioProof = struct {
    pat_slot: u3,
    mtrr_type: mtrr.MemoryType,
};

pub const RamCacheProofError = error{ CacheEvidenceMissing, InvalidRange, RamNotWriteBack, WcPatUnavailable };

/// RAM must be restorable to actual WB, not merely a PTE spelling of WB.
/// This is separate from BAR proof, which can legitimately admit MTRR UC/WC.
/// Caller excludes CPU topology/PAT/MTRR changes until commit completes.
pub fn ramCachePatProof(pages: []const u64, mode: @import("memory/ram_cache_transition.zig").Mode) RamCacheProofError!u3 {
    return ramCachePatProofWithSnapshots(mmio_cache_snapshots, mmio_cache_online_mask, onlineCpuSet(), pages, mode);
}

fn ramCachePatProofWithSnapshots(
    snapshots: []const mtrr.CacheSnapshot,
    verified: CpuSet,
    online: CpuSet,
    pages: []const u64,
    mode: @import("memory/ram_cache_transition.zig").Mode,
) RamCacheProofError!u3 {
    if (verified.count() == 0 or !verified.eql(online) or !online.contains(0) or snapshots.len == 0)
        return error.CacheEvidenceMissing;
    @import("memory/ram_cache_paging.zig").validatePages(pages, @as(u64, 1) << 52) catch return error.InvalidRange;
    const boot = &snapshots[0];
    if (!boot.valid()) return error.CacheEvidenceMissing;
    var seen: usize = 0;
    var common_wc: u8 = 0xff;
    for (snapshots, 0..) |*snapshot, cpu| {
        if (!online.contains(cpu)) continue;
        if (!boot.matches(snapshot)) return error.CacheEvidenceMissing;
        seen += 1;
        var wc: u8 = 0;
        for (0..8) |slot| {
            if (snapshot.patSlotIsWc(@intCast(slot))) wc |= @as(u8, 1) << @as(u3, @intCast(slot));
        }
        common_wc &= wc;
    }
    if (seen != online.count()) return error.CacheEvidenceMissing;
    // Matching snapshots prove all CPUs share the MTRR interpretation.
    // The snapshot deliberately cannot prove fixed-MTRR RAM below 1 MiB.
    for (pages) |pa| {
        const kind = boot.uniformMtrrType(pa, 4096) orelse return error.InvalidRange;
        if (kind != .wb) {
            if (!@import("builtin").is_test) {
                // A VMO offset does not identify the CPU cache type. Keep the
                // first rejected physical page and boot MSR evidence in the
                // existing debug ring; never weaken the WB proof on failure.
                @import("kernel_log.zig").writeFmt(
                    "ram-cache: proof paddr=0x{x} mtrr={s} syscfg=0x{x} top_mem2=0x{x} def_type=0x{x}\n",
                    .{ pa, @tagName(kind), boot.amd_syscfg, boot.amd_top_mem2, boot.default_type },
                );
            }
            return error.RamNotWriteBack;
        }
    }
    return switch (mode) {
        .wb => 0, // valid()/matches() prove slots 0/2/3 on every CPU.
        .uc_minus => 2,
        .uc => 3,
        .wc => if (common_wc != 0) @intCast(@ctz(common_wc)) else error.WcPatUnavailable,
    };
}

test "MMIO alias RAM proof requires restorable WB and common PAT on all CPUs" {
    const base = mtrr.CacheSnapshot{
        .pat = 6 | (@as(u64, 7) << 16) | (@as(u64, 1) << 32),
        .capability = 1 | (1 << 10),
        .default_type = (1 << 11) | 6,
        .physical_bits = 40,
    };
    var snapshots = [_]mtrr.CacheSnapshot{ base, base };
    const pages = [_]u64{ 0x200000, 0x203000 };
    for ([_]@import("memory/ram_cache_transition.zig").Mode{ .wb, .uc_minus, .uc, .wc }, [_]u3{ 0, 2, 3, 4 }) |mode, pat|
        try std.testing.expectEqual(pat, try ramCachePatProofWithSnapshots(&snapshots, testSet(3), testSet(3), &pages, mode));
    try std.testing.expectError(error.CacheEvidenceMissing, ramCachePatProofWithSnapshots(&snapshots, testSet(1), testSet(3), &pages, .wc));
    snapshots[1].pat &= ~(@as(u64, 0xff) << 32);
    try std.testing.expectError(error.WcPatUnavailable, ramCachePatProofWithSnapshots(&snapshots, testSet(3), testSet(3), &pages, .wc));
    snapshots[1] = base;
    try std.testing.expectError(error.InvalidRange, ramCachePatProofWithSnapshots(&snapshots, testSet(3), testSet(3), &.{0x80000}, .wb));
    try std.testing.expectError(error.InvalidRange, ramCachePatProofWithSnapshots(&snapshots, testSet(3), testSet(3), &.{@as(u64, 1) << 40}, .wb));
    try std.testing.expectError(error.InvalidRange, ramCachePatProofWithSnapshots(&snapshots, testSet(3), testSet(3), &.{ 0x200000, 0x200000 }, .wb));
    const mask: u64 = (((@as(u64, 1) << 40) - 1) & ~@as(u64, 4095)) | (1 << 11);
    for (&snapshots) |*snapshot| snapshot.ranges[0] = .{ .base = pages[1], .mask = mask };
    try std.testing.expectError(error.RamNotWriteBack, ramCachePatProofWithSnapshots(&snapshots, testSet(3), testSet(3), &pages, .wc));
}

test "MMIO alias RAM proof accepts AMD TOM2 default only with matching CPU evidence" {
    const base = mtrr.CacheSnapshot{
        .pat = 6 | (@as(u64, 7) << 16),
        .capability = 1,
        .default_type = 1 << 11, // UC outside TOM2.
        .physical_bits = 46,
        .amd_pat_mtrr_semantics = true,
        .amd_syscfg = (1 << 21) | (1 << 22),
        .amd_top_mem2 = 0x8_0000_0000,
    };
    var snapshots = [_]mtrr.CacheSnapshot{ base, base };
    try std.testing.expectEqual(@as(u3, 2), try ramCachePatProofWithSnapshots(&snapshots, testSet(3), testSet(3), &.{0x1_4000_0000}, .uc_minus));
    try std.testing.expectError(error.RamNotWriteBack, ramCachePatProofWithSnapshots(&snapshots, testSet(3), testSet(3), &.{0x8_0000_0000}, .uc_minus));
    snapshots[1].amd_top_mem2 -= 0x80_0000;
    try std.testing.expectError(error.CacheEvidenceMissing, ramCachePatProofWithSnapshots(&snapshots, testSet(3), testSet(3), &.{0x1_4000_0000}, .uc_minus));
}

pub const WcMmioProofError = error{ CacheEvidenceMissing, InvalidRange, UnsupportedCpu, WcPatUnavailable };

fn wcMmioProofWithSnapshots(
    snapshots: []const mtrr.CacheSnapshot,
    verified: CpuSet,
    online: CpuSet,
    paddr: u64,
    bytes: u64,
) WcMmioProofError!WcMmioProof {
    if (verified.count() == 0 or !verified.eql(online) or !online.contains(0) or snapshots.len == 0)
        return error.CacheEvidenceMissing;
    const boot = &snapshots[0];
    if (!boot.valid()) return error.CacheEvidenceMissing;
    var seen: usize = 0;
    for (snapshots, 0..) |*snapshot, cpu_slot| {
        if (!online.contains(cpu_slot)) continue;
        if (!boot.matches(snapshot)) return error.CacheEvidenceMissing;
        seen += 1;
    }
    if (seen != online.count()) return error.CacheEvidenceMissing;

    const mtrr_type = boot.uniformMtrrType(paddr, bytes) orelse return error.InvalidRange;
    // AMD APM Vol. 2, effective-type table: PAT0 WB + MTRR WC is WC.
    // No firmware WC PAT entry is required for this proven combination.
    if (mtrr_type == .wc) return .{ .pat_slot = 0, .mtrr_type = .wc };

    // PAT WC overrides other supported MTRR types on AMD, but not on every
    // x86 implementation. Never infer this from the PAT MSR alone.
    if (!boot.amd_pat_mtrr_semantics) return error.UnsupportedCpu;
    // This proves only the proposed mapping's effective type; the caller
    // must settle every preexisting physical alias before publishing its PTE.
    for ([_]u3{ 1, 4, 5, 6, 7 }) |pat_slot| {
        var common = true;
        for (snapshots, 0..) |*snapshot, cpu_slot| {
            if (online.contains(cpu_slot) and !snapshot.patSlotIsWc(pat_slot)) {
                common = false;
                break;
            }
        }
        if (common) return .{ .pat_slot = pat_slot, .mtrr_type = mtrr_type };
    }
    return error.WcPatUnavailable;
}

/// Read-only cache proof for the actual CPU PAT/MTRR state. This does not
/// authorize a BAR lease or modify any identity mapping.
pub fn wcMmioProof(paddr: u64, bytes: u64) WcMmioProofError!WcMmioProof {
    const verified = mmio_cache_online_mask;
    return wcMmioProofWithSnapshots(mmio_cache_snapshots, verified, onlineCpuSet(), paddr, bytes);
}

test "MMIO alias WC proof requires all online CPUs to share a usable PAT slot" {
    const address: u64 = 0xd0000000;
    const base = mtrr.CacheSnapshot{
        .pat = 6 | (@as(u64, 1) << 8) | (@as(u64, 7) << 16),
        .capability = 1 << 10,
        .default_type = 1 << 11, // MTRR default UC.
        .physical_bits = 46,
        .amd_pat_mtrr_semantics = true,
    };
    var snapshots = [_]mtrr.CacheSnapshot{ base, base };
    const ready = try wcMmioProofWithSnapshots(&snapshots, testSet(0b11), testSet(0b11), address, 8192);
    try std.testing.expectEqual(@as(u3, 1), ready.pat_slot);
    try std.testing.expectEqual(mtrr.MemoryType.uc, ready.mtrr_type);

    snapshots[1].pat &= ~(@as(u64, 0xff) << 8);
    try std.testing.expectError(error.WcPatUnavailable, wcMmioProofWithSnapshots(&snapshots, testSet(0b11), testSet(0b11), address, 8192));
    snapshots[0].pat = 6 | (@as(u64, 7) << 16) | (@as(u64, 1) << 32);
    snapshots[1].pat |= @as(u64, 1) << 32;
    const high_slot = try wcMmioProofWithSnapshots(&snapshots, testSet(0b11), testSet(0b11), address, 8192);
    try std.testing.expectEqual(@as(u3, 4), high_slot.pat_slot);

    try std.testing.expectError(error.CacheEvidenceMissing, wcMmioProofWithSnapshots(&snapshots, testSet(0b01), testSet(0b11), address, 8192));
    try std.testing.expectError(error.InvalidRange, wcMmioProofWithSnapshots(&snapshots, testSet(0b11), testSet(0b11), address + 1, 8192));
    snapshots[0].amd_pat_mtrr_semantics = false;
    snapshots[1].amd_pat_mtrr_semantics = false;
    try std.testing.expectError(error.UnsupportedCpu, wcMmioProofWithSnapshots(&snapshots, testSet(0b11), testSet(0b11), address, 8192));
}

test "MMIO alias WC proof admits PAT0 when every MTRR is WC" {
    const address: u64 = 0xd0000000;
    const base = mtrr.CacheSnapshot{
        .pat = 6 | (@as(u64, 7) << 16), // No PAT entry contains WC.
        .capability = 1 << 10,
        .default_type = (1 << 11) | 1, // MTRR default WC.
        .physical_bits = 46,
    };
    var snapshots = [_]mtrr.CacheSnapshot{ base, base };
    const proof = try wcMmioProofWithSnapshots(&snapshots, testSet(0b11), testSet(0b11), address, 8192);
    try std.testing.expectEqual(@as(u3, 0), proof.pat_slot);
    try std.testing.expectEqual(mtrr.MemoryType.wc, proof.mtrr_type);

    snapshots[1].default_type = (1 << 11) | 6;
    try std.testing.expectError(error.CacheEvidenceMissing, wcMmioProofWithSnapshots(&snapshots, testSet(0b11), testSet(0b11), address, 8192));
}

pub const ExplicitUcMmioStatus = enum {
    ready,
    cache_evidence_missing,
    invalid_range,
    mapping_not_uc,
};

/// The AMD IOMMU dereferences its register BAR through the low identity VA.
/// Unlike UC_MINUS device leases, explicit PAT3 UC is safe with a WB MTRR
/// when every actual identity leaf is UC; do not relax UC_MINUS admission.
pub fn explicitUcMmioStatus(paddr: u64, bytes: u64) ExplicitUcMmioStatus {
    const verified = mmio_cache_online_mask;
    if (verified.count() == 0 or !verified.eql(onlineCpuSet())) return .cache_evidence_missing;
    if (!x86_platform.kernelMmioRangeFits(paddr, bytes) or
        !mmio_cache_snapshots[0].rangeSupportsUncachedMapping(paddr, bytes)) return .invalid_range;
    var page = paddr;
    const end = paddr + bytes;
    while (page < end) : (page += 4096) {
        if (!x86_platform.bootMmioPageIsExplicitUc(page)) return .mapping_not_uc;
    }
    return .ready;
}

pub fn mmioCacheEvidenceCounts() struct { verified: usize, matching: usize, online: usize } {
    return .{ .verified = mmio_cache_online_mask.count(), .matching = matching_set.count(), .online = online_set.count() };
}

fn cacheEvidenceForOnlineCpus(success: bool, count: usize, online: CpuSet, matching: CpuSet) bool {
    return success and online.count() != 0 and online.count() == count and matching.eql(online);
}

fn ucMinusMmioWithSnapshot(snapshot: *const mtrr.CacheSnapshot, paddr: u64, bytes: u64) bool {
    return snapshot.rangeIsUcMinusUncached(paddr, bytes);
}

/// Explicit UC for the kernel's temporary MMIO window. Existing WB-encoded
/// identity aliases must remain effectively UC, including the high window.
pub fn uncachedKernelMmioPageAllowed(paddr: u64) bool {
    const verified = mmio_cache_online_mask;
    if (verified.count() == 0 or !verified.eql(onlineCpuSet())) return false;
    return uncachedKernelMmioPageWithSnapshot(&mmio_cache_snapshots[0], paddr);
}

fn uncachedKernelMmioPageWithSnapshot(snapshot: *const mtrr.CacheSnapshot, paddr: u64) bool {
    return snapshot.rangeSupportsUncachedMapping(paddr, 4096);
}

test "MMIO alias cache proof uses physical width independently of high window VA" {
    const snapshot = mtrr.CacheSnapshot{
        .pat = 6 | (@as(u64, 7) << 16),
        .physical_bits = 46,
        .default_type = (1 << 11) | 6,
    };
    for ([_]u64{ 0xfee00000, 16 << 30, 1 << 39, (1 << 46) - 4096 }) |pa| {
        try std.testing.expect(uncachedKernelMmioPageWithSnapshot(&snapshot, pa));
        try std.testing.expect(ucMinusMmioWithSnapshot(&snapshot, pa, 4096));
    }
    try std.testing.expect(!ucMinusMmioWithSnapshot(&snapshot, (1 << 46) - 4096, 8192));
    try std.testing.expect(!uncachedKernelMmioPageWithSnapshot(&snapshot, x86_platform.phys_copy_window_va));
}

test "MMIO alias cache status separates missing evidence from unsafe range" {
    const snapshot = mtrr.CacheSnapshot{
        .pat = 6 | (@as(u64, 7) << 16),
        .physical_bits = 46,
        .default_type = 1 << 11,
    };
    const paddr: u64 = 0xfce00000;
    try std.testing.expectEqual(UcMinusMmioStatus.cache_evidence_missing, ucMinusMmioStatusWithSnapshot(&snapshot, testSet(0), testSet(0b11), paddr, 0x80000));
    try std.testing.expectEqual(UcMinusMmioStatus.cache_evidence_missing, ucMinusMmioStatusWithSnapshot(&snapshot, testSet(0b01), testSet(0b11), paddr, 0x80000));
    try std.testing.expectEqual(UcMinusMmioStatus.ready, ucMinusMmioStatusWithSnapshot(&snapshot, testSet(0b11), testSet(0b11), paddr, 0x80000));
    try std.testing.expectEqual(UcMinusMmioStatus.unsafe_range, ucMinusMmioStatusWithSnapshot(&snapshot, testSet(0b11), testSet(0b11), paddr, 0));
}

pub fn returnCurrentApToIdleFromInterrupt() noreturn {
    redispatchCurrentAp(false);
}

pub fn returnCurrentApAfterTimerInterrupt() noreturn {
    redispatchCurrentAp(true);
}

const ApRedispatch = enum { idle, fresh_slice, inherited_slice };

const ApRedispatchBackend = struct {
    fn enterKernel() void {
        x86_platform.writeCr3(x86_platform.kernel_cr3_value);
    }
    fn markIdle(cpu: usize) void {
        setCpuState(cpu, .idle);
    }
    fn observeIdle(cpu: usize) void {
        scheduler_observer.observeIdle(cpu);
    }
    fn poll(cpu: usize) void {
        scheduler_observer.pollIdleScheduler(cpu);
    }
    fn claim(cpu: usize, entry: *scheduler_observer.UserEntry) bool {
        return scheduler_observer.claimIdleUserEntry(cpu, entry);
    }
    fn stopTimer() void {
        lapic.disarmTimer();
    }
    fn markUser(cpu: usize) void {
        setCpuState(cpu, .user);
    }
};

fn prepareApRedispatch(comptime Backend: type, cpu: usize, timer_expired: bool, entry: *scheduler_observer.UserEntry) ApRedispatch {
    // IF stays clear: this is a continuous busy-CPU handoff, not a timer
    // lease across HLT. Drop the old context's ownership before claiming the
    // next one, using the same observer/claim/rollback path as the idle loop.
    Backend.enterKernel();
    Backend.markIdle(cpu);
    Backend.observeIdle(cpu);
    Backend.poll(cpu);
    if (!Backend.claim(cpu, entry)) {
        Backend.stopTimer();
        return .idle;
    }
    Backend.markUser(cpu);
    // A pending IRQ is deliberately not drained or hidden. A normal handoff
    // inherits the busy CPU's remaining slice, as IPC/futex handoffs do; an
    // actual timer IRQ has consumed that lease and must start another one.
    return if (timer_expired) .fresh_slice else .inherited_slice;
}

fn redispatchCurrentAp(timer_expired: bool) noreturn {
    const cpu_slot = currentCpuSlot();
    var entry: scheduler_observer.UserEntry = undefined;
    const dispatch = prepareApRedispatch(ApRedispatchBackend, cpu_slot, timer_expired, &entry);
    if (dispatch != .idle) enterUserModeFromIdle(&entry, dispatch);
    apIdleLoop(cpu_slot);
}

pub fn wakeCpu(cpu_slot: usize) bool {
    if (cpu_slot == currentCpuSlot()) return true;
    if (cpu_slot >= cpuCapacity()) return false;
    @import("wake_trace.zig").record(.ipi_send, cpu_slot, 0, 0);
    // A runnable publication needs its own APIC edge. Coalescing against an
    // earlier vector loses the later publication when the target consumes the
    // first vector immediately before returning to HLT.
    var attempts: usize = 0;
    while (attempts < 3) : (attempts += 1) {
        if (interruptCpu(cpu_slot)) return true;
    }
    return false;
}

/// Caller holds K from runnable publication through this return. Only this
/// path uses the token; general lifecycle/maintenance IPIs remain independent.
pub fn wakeCpuForNotify(cpu_slot: usize) bool {
    if (cpu_slot == currentCpuSlot()) return true;
    if (cpu_slot >= notify_wake.len) return false;
    if (!notify_wake[cpu_slot].request()) return true;
    if (wakeCpu(cpu_slot)) return true;
    notify_wake[cpu_slot].failed();
    return false;
}

/// Before the handler's ring-0 early return. Idle resumes its queue-check loop;
/// busy kernel work retains the ordinary user-return/timer scheduling rules.
pub fn receiveNotifyWake() void {
    const cpu = currentCpuSlot();
    if (cpu < notify_wake.len) notify_wake[cpu].received();
}

/// Deliver the scheduler/wake vector to any online CPU, including the BSP.
/// The vector also carries kernel-internal cross-CPU maintenance requests.
pub fn interruptCpu(cpu_slot: usize) bool {
    if (cpu_slot >= runtime_lapic_id_storage.len) return false;
    const apic_id = runtime_lapic_ids[cpu_slot];
    if (apic_id == 0xFFFFFFFF) return false;
    if (cpuState(cpu_slot) == .absent) return false;
    if (wake_ipi_vector == 0) return false;
    return lapic.sendFixedIpi(apic_id, wake_ipi_vector);
}

/// Called after EOI and after device publication completes. A maintenance
/// IPI must not acknowledge lower-priority device interrupts still in IRR.
pub fn acknowledgeInterruptDrain() void {
    const generation = @atomicLoad(u64, &irq_drain_generation, .acquire);
    const cpu = currentCpuSlot();
    if (cpu >= cpuCapacity() or generation == 0 or
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
    // Publishing a request while a remote responder spins with IF=0 can
    // deadlock before the first ACK; inspect all interrupted owners as well.
    lock_diagnostics.assertCanWaitForIpi();
    const targets = onlineCpuSet();
    if (targets.count() == 0 or count == 0) return false;
    const prior = @atomicLoad(u64, &irq_drain_generation, .acquire);
    if (prior == std.math.maxInt(u64)) return false;
    const generation = prior + 1;
    @atomicStore(u8, &irq_drain_first, first, .release);
    @atomicStore(u8, &irq_drain_count, count, .release);
    @atomicStore(u64, &irq_drain_generation, generation, .release);
    var cpu: usize = 0;
    var sent = true;
    while (cpu < cpuCapacity()) : (cpu += 1) {
        if (targets.contains(cpu) and cpu != currentCpuSlot())
            sent = interruptCpu(cpu) and sent;
    }
    if (!sent) return false;
    var spins: usize = 0;
    while (spins < 10_000_000) : (spins += 1) {
        acknowledgeInterruptDrain();
        var complete = true;
        cpu = 0;
        while (cpu < cpuCapacity()) : (cpu += 1) {
            if (targets.contains(cpu) and
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
pub fn interruptAllOtherOnlineCpus(target: CpuSet) bool {
    if (wake_ipi_vector == 0 or !broadcastMaskMatches(broadcast_online_mask, target, onlineCpuSet(), currentCpuSlot())) return false;
    if (target.count() <= 1) return true;
    return lapic.sendFixedIpiAllExcludingSelf(wake_ipi_vector);
}

fn broadcastMaskMatches(complete: CpuSet, target: CpuSet, online: CpuSet, current: usize) bool {
    return complete.count() != 0 and target.eql(complete) and online.eql(complete) and target.contains(current);
}

// Both callers enter with IF clear and the local timer already stopped.
// Immediate redispatch never reaches this loop; a failed claim stops the
// timer so real idle time cannot become the next task's inherited slice.
fn apIdleLoop(cpu_slot: usize) noreturn {
    while (true) {
        scheduler_observer.observeIdle(cpu_slot);
        scheduler_observer.pollIdleScheduler(cpu_slot);
        var user_entry: scheduler_observer.UserEntry = undefined;
        if (scheduler_observer.claimIdleUserEntry(cpu_slot, &user_entry)) {
            setCpuState(cpu_slot, .user);
            enterUserModeFromIdle(&user_entry, .fresh_slice);
        }
        asm volatile ("sti; hlt; cli" ::: .{ .memory = true });
    }
}

fn jumpToUserReturn() callconv(.winapi) void {
    asm volatile ("jmp userReturnToSavedFrame" ::: .{ .memory = true });
}

fn startApUserSlice(comptime Timer: type, dispatch: ApRedispatch, vector: u8, initial_count: u32) void {
    std.debug.assert(dispatch != .idle);
    if (dispatch == .fresh_slice and vector != 0) _ = Timer.armTimer(initial_count);
}

fn enterUserModeFromIdle(entry: *scheduler_observer.UserEntry, dispatch: ApRedispatch) noreturn {
    asm volatile ("cli");
    // A runnable thread may carry a pending signal whose IPI reached another
    // thread or an inhibited/kernel context. Every AP redispatch is a user
    // return boundary; do not depend on an uninterrupted timer slice to stage
    // it. Stage before restoring the selected thread's live XState.
    @import("traps.zig").stagePendingSignalForUserReturn(&entry.frame);
    startApUserSlice(lapic, dispatch, ap_user_timer_vector, ap_user_timer_initial_count);
    const x_state: *align(x86_platform.xstate_alignment) const [x86_platform.xstate_bytes]u8 =
        @ptrFromInt(entry.x_state_addr);
    x86_platform.restoreXState(x_state);
    // The common user return installs this published thread's FS/GS/PKRU
    // after XRSTOR. Staging the frame needs none of them; doing it here too
    // repeats privileged register writes on every AP block/wake redispatch.
    stageUserReturnFromFramePointerForCurrentCpu(
        @intFromPtr(&entry.frame),
        @offsetOf(interrupts.TrapFrame, "rip"),
    );
    jumpToUserReturn();
    while (true) asm volatile ("hlt");
}

test "AP immediate redispatch inherits only an unconsumed busy slice" {
    const Probe = struct {
        var events: [16]u8 = undefined;
        var count: usize = 0;
        var ready: bool = true;
        fn event(value: u8) void {
            events[count] = value;
            count += 1;
        }
        fn enterKernel() void {
            event(1);
        }
        fn markIdle(cpu: usize) void {
            std.debug.assert(cpu == 2);
            event(2);
        }
        fn observeIdle(_: usize) void {
            event(3);
        }
        fn poll(_: usize) void {
            event(4);
        }
        fn claim(_: usize, entry: *scheduler_observer.UserEntry) bool {
            event(5);
            if (ready) entry.thread_index = 7;
            return ready;
        }
        fn stopTimer() void {
            event(6);
        }
        fn markUser(_: usize) void {
            event(7);
        }
        fn armTimer(countdown: u32) bool {
            std.debug.assert(countdown == 42);
            event(8);
            return true;
        }
    };
    var entry: scheduler_observer.UserEntry = undefined;
    Probe.count = 0;
    Probe.ready = true;
    var dispatch = prepareApRedispatch(Probe, 2, false, &entry);
    try std.testing.expectEqual(ApRedispatch.inherited_slice, dispatch);
    startApUserSlice(Probe, dispatch, 0x40, 42);
    try std.testing.expectEqualSlices(u8, &.{ 1, 2, 3, 4, 5, 7 }, Probe.events[0..Probe.count]);
    try std.testing.expectEqual(@as(usize, 7), entry.thread_index);

    Probe.count = 0;
    dispatch = prepareApRedispatch(Probe, 2, true, &entry);
    try std.testing.expectEqual(ApRedispatch.fresh_slice, dispatch);
    startApUserSlice(Probe, dispatch, 0x40, 42);
    try std.testing.expectEqualSlices(u8, &.{ 1, 2, 3, 4, 5, 7, 8 }, Probe.events[0..Probe.count]);

    for ([_]bool{ false, true }) |expired| {
        Probe.count = 0;
        Probe.ready = false;
        dispatch = prepareApRedispatch(Probe, 2, expired, &entry);
        try std.testing.expectEqual(ApRedispatch.idle, dispatch);
        try std.testing.expectEqualSlices(u8, &.{ 1, 2, 3, 4, 5, 6 }, Probe.events[0..Probe.count]);
        // An actual idle-loop claim always starts a new lease, even when
        // the previous failed fast claim had not consumed its timer IRQ.
        startApUserSlice(Probe, .fresh_slice, 0x40, 42);
        try std.testing.expectEqual(@as(u8, 8), Probe.events[Probe.count - 1]);
    }
    Probe.count = 0;
    startApUserSlice(Probe, .fresh_slice, 0, 42);
    try std.testing.expectEqual(@as(usize, 0), Probe.count);
}

// AP reset fetches low PA, but the BSP writes through the owned RAM direct
// mapping. The dedicated AP root is the only low virtual alias.
fn trampolineWriter(physical: @import("memory/addresses.zig").PhysicalAddress) ?[*]u8 {
    if (physical.toInt() == 0 or physical.toInt() & 4095 != 0 or
        !physical.rangeFits(trampoline_page_bytes, 0x100000)) return null;
    return (@import("arch/x86_64/physical_layout.zig").identitySlice(u8, physical, trampoline_page_bytes) orelse return null).ptr;
}

test "SMP address domains trampoline aperture rejects null misalignment and high memory" {
    try std.testing.expect(trampolineWriter(.fromInt(0)) == null);
    try std.testing.expect(trampolineWriter(.fromInt(4097)) == null);
    try std.testing.expect(trampolineWriter(.fromInt(0x100000)) == null);
    try std.testing.expect(trampolineWriter(.fromInt(std.math.maxInt(u64))) == null);
    try std.testing.expectEqual(@as(usize, 0xff000), @intFromPtr(trampolineWriter(.fromInt(0xff000)).?));
}

fn testSet(comptime value: usize) CpuSet {
    const Fixture = struct {
        var word = [_]usize{value};
    };
    return .{ .words = &Fixture.word, .capacity = 2 };
}

test "cache evidence and shorthand require every CPU beyond one word" {
    var a_words: [CpuSet.wordCount(72)]usize = undefined;
    var b_words: [CpuSet.wordCount(72)]usize = undefined;
    const a = CpuSet.init(&a_words, 72);
    const b = CpuSet.init(&b_words, 72);
    for (0..72) |cpu| a.insert(cpu);
    b.copyFrom(a);
    try std.testing.expect(cacheEvidenceForOnlineCpus(true, 72, a, b));
    try std.testing.expect(broadcastMaskMatches(a, b, a, 71));
    b.remove(71);
    try std.testing.expect(!cacheEvidenceForOnlineCpus(true, 72, a, b));
    try std.testing.expect(!broadcastMaskMatches(a, b, a, 0));
    try std.testing.expect(!cacheEvidenceForOnlineCpus(false, 72, a, a));
    try std.testing.expect(!cacheEvidenceForOnlineCpus(true, 71, a, a));
}
