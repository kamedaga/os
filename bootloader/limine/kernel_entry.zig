/// Limine entry point for CapabilityOS.
/// The full OS handoff is wired in stages: this file owns the Limine protocol
/// requests and converts them into boot/entry.zig resources.
const std = @import("std");
pub const smp_profile_enabled = @import("smp_profile_options").enabled;
pub const ipc_profile_enabled = @import("ipc_profile_options").ipc_enabled;
const kernel_api = @import("kernel_boot_api");
const x86_platform = kernel_api.x86_platform;
const entry = kernel_api.entry;
const kernel_runtime = kernel_api.kernel_runtime;
const limine = @import("protocol.zig");
const limine_boot = @import("resources.zig");
const early_gop_marker = @import("early_gop_marker.zig");
const boot_checkpoint_name = @import("boot_checkpoint_options").name;
const entry_checkpoint_reset = @import("entry_checkpoint_reset.zig");
const boot_diag = kernel_api.boot_diag;
pub const boot_diagnostic_enabled = std.mem.eql(u8, boot_checkpoint_name, "DIAG") or
    std.mem.eql(u8, boot_checkpoint_name, "DIAGTEST");

// Diagnostic images differ only in this initial byte. Volatile reads keep the
// complete normal boot path linked in every checkpoint image; otherwise the
// compiler would strip later phases and change Limine's loading layout.
var selected_boot_checkpoint: u8 = if (std.mem.eql(u8, boot_checkpoint_name, "off"))
    0 else boot_checkpoint_name[0];

// ---------------------------------------------------------------------------
// Exported symbols accessed by assembly stubs in traps.zig / syscalls.zig
// ---------------------------------------------------------------------------

export var user_return_saved_r10: u64 = 0;
export var user_return_saved_gprs: [15]u64 align(16) = [_]u64{0} ** 15;
export var user_return_iret_frame: [5]u64 align(16) = [_]u64{0} ** 5;

// ---------------------------------------------------------------------------
// Limine requests
// ---------------------------------------------------------------------------

export var limine_requests_start align(8) linksection(".limine_requests_start") = limine.requests_start_marker;

export var limine_base_revision align(8) linksection(".limine_requests") = [3]u64{
    limine.base_revision_magic0,
    limine.base_revision_magic1,
    3,
};

const init_module_path: [*:0]const u8 = "INITAPP.ELF";
const init_module_tag: [*:0]const u8 = "init";
const bootfs_module_path: [*:0]const u8 = "BOOTFS.IMG";
const bootfs_module_tag: [*:0]const u8 = "bootfs";

export var init_internal_module align(8) linksection(".limine_requests") = limine.InternalModule{
    .path = init_module_path,
    .string = init_module_tag,
    .flags = limine.internal_module_required,
};

export var bootfs_internal_module align(8) linksection(".limine_requests") = limine.InternalModule{
    .path = bootfs_module_path,
    .string = bootfs_module_tag,
    .flags = limine.internal_module_required,
};

export var internal_modules align(8) linksection(".limine_requests") = [2]?*limine.InternalModule{
    &init_internal_module,
    &bootfs_internal_module,
};

export var hhdm_request align(8) linksection(".limine_requests") = limine.HhdmRequest{};
export var framebuffer_request align(8) linksection(".limine_requests") = limine.FramebufferRequest{};
export var memmap_request align(8) linksection(".limine_requests") = limine.MemmapRequest{};
export var module_request align(8) linksection(".limine_requests") = limine.ModuleRequest{
    .internal_module_count = internal_modules.len,
    .internal_modules = &internal_modules,
};
export var rsdp_request align(8) linksection(".limine_requests") = limine.RsdpRequest{};
export var executable_address_request align(8) linksection(".limine_requests") = limine.ExecutableAddressRequest{};

export var limine_requests_end align(8) linksection(".limine_requests_end") = limine.requests_end_marker;

// ---------------------------------------------------------------------------
// Panic and early halt
// ---------------------------------------------------------------------------

pub fn panic(msg: []const u8, trace: ?*std.builtin.StackTrace, ret_addr: ?usize) noreturn {
    _ = trace;
    _ = ret_addr;
    serialWrite("PANIC: ");
    serialWrite(msg);
    serialWrite("\n");
    if (comptime boot_diagnostic_enabled) boot_diag.showPanic(msg);
    haltForever();
}

fn serialWrite(bytes: []const u8) void {
    for (bytes) |byte| {
        asm volatile ("outb %[value], %[port]"
            :
            : [value] "{al}" (byte),
              [port] "{dx}" (@as(u16, 0x3f8)),
        );
    }
}

fn haltForever() noreturn {
    while (true) asm volatile ("hlt");
}

// ---------------------------------------------------------------------------
// Limine entry
// ---------------------------------------------------------------------------

pub export fn _start() callconv(.c) noreturn {
    const boot_stack_top = @as(usize, @intCast(x86_platform.ring0StackTop())) & ~@as(usize, 0xF);
    asm volatile (
        \\mov %[stack_top], %%rsp
        \\mov %[target], %%rax
        \\call *%%rax
        :
        : [stack_top] "r" (boot_stack_top),
          [target] "r" (&limineKernelMain),
        : .{ .memory = true });
    unreachable;
}

fn limineKernelMain() noreturn {
    maybeResetAtCheckpoint('A');
    const requests = limine_boot.Requests{
        .hhdm = &hhdm_request,
        .framebuffer = &framebuffer_request,
        .memmap = &memmap_request,
        .module = &module_request,
        .rsdp = &rsdp_request,
        .executable_address = &executable_address_request,
    };
    markEarlyGopStage(0);
    entry.prepareBootPrelude();
    markEarlyGopStage(1);
    limine_boot.probeBootResourcesOrHalt(requests);
    maybeResetAtCheckpoint('B');
    markEarlyGopStage(2);
    const smp_resources = limine_boot.smpBootResourcesOrHalt(requests);
    markEarlyGopStage(3);
    serialWrite("pacha: limine entry\n");
    entry.prepareLimineKernelStorageOrHalt();
    markEarlyGopStage(4);
    const resources = entry.persistBootResourcesOrHalt(limine_boot.buildBootResourcesOrHalt(
        requests,
        kernel_runtime.global_free_list,
        entry.currentUserSpaces(),
    ));
    if (comptime boot_diagnostic_enabled) {
        markEarlyGopStage(if (boot_diag.arm(resources.framebuffer_info)) 6 else 7);
    }
    maybeResetAtCheckpoint('C');
    markEarlyGopStage(5);
    serialWrite("pacha: limine resources ready\n");
    entry.initializeLimineRuntimeOrHalt(smp_resources);
    maybeResetAtCheckpoint('D');
    serialWrite("pacha: limine runtime ready\n");
    serialWrite("pacha: entering boot resources\n");
    entry.bootWithResources(resources, maybeResetAtCheckpoint);
}

fn maybeResetAtCheckpoint(checkpoint: u8) void {
    if (comptime boot_diagnostic_enabled) boot_diag.mark(checkpoint);
    if (comptime std.mem.eql(u8, boot_checkpoint_name, "DIAGTEST")) {
        if (checkpoint == 'E') kernel_api.halt.haltWithMessage("QEMU diagnostic display halt at E");
    }
    if (comptime boot_checkpoint_name.len == 1) {
        const selected: *volatile u8 = &selected_boot_checkpoint;
        if (selected.* == checkpoint) entry_checkpoint_reset.delayedReset();
    }
}

fn markEarlyGopStage(stage: usize) void {
    // The marker is a pre-userland fault locator. Ordinary live boots use
    // the userland console, and must never expose diagnostic squares.
    if (comptime std.mem.eql(u8, boot_checkpoint_name, "off")) return;
    const response = framebuffer_request.response orelse return;
    if (response.framebuffer_count == 0) return;
    const framebuffer = response.framebuffers[0] orelse return;
    _ = early_gop_marker.mark(framebuffer.address, framebuffer.width,
        framebuffer.height, framebuffer.pitch, framebuffer.bpp,
        framebuffer.memory_model == limine.framebuffer_rgb, stage);
}
