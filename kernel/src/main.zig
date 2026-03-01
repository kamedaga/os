const std = @import("std");
const kernel = @import("kernel.zig");

const uefi = std.os.uefi;

const serial_port: u16 = 0x3F8;

pub fn main() void {}

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

fn serialWritePrincipal(p: kernel.PrincipalId) void {
    switch (p) {
        .Process0 => serialWrite("Process0"),
        .Device0 => serialWrite("Device0"),
    }
}

fn dumpState(state: *const kernel.KernelState, label: []const u8) void {
    var num_buf: [32]u8 = undefined;

    serialWrite("=== ");
    serialWrite(label);
    serialWrite(" ===\n");

    const r0 = state.getRegionConst(0) orelse {
        serialWrite("region0: missing\n");
        return;
    };
    serialWrite("region0.owner = ");
    serialWritePrincipal(r0.owner);
    serialWrite("\n");

    const p0 = state.getTableConst(.Process0).find(0);
    if (p0) |cap| {
        serialWrite("Process0.cap(region0): read=");
        serialWrite(if (cap.rights.read) "true" else "false");
        serialWrite(" dma=");
        serialWrite(if (cap.rights.dma) "true" else "false");
        serialWrite("\n");
    } else {
        serialWrite("Process0.cap(region0): none\n");
    }

    const dev = state.getTableConst(.Device0).find(0);
    if (dev) |cap| {
        serialWrite("Device0.cap(region0): read=");
        serialWrite(if (cap.rights.read) "true" else "false");
        serialWrite(" dma=");
        serialWrite(if (cap.rights.dma) "true" else "false");
        serialWrite("\n");
    } else {
        serialWrite("Device0.cap(region0): none\n");
    }

    const count = std.fmt.bufPrint(&num_buf, "{d}", .{state.region_len}) catch "err";
    serialWrite("regions=");
    serialWrite(count);
    serialWrite("\n");
}

pub export fn efi_main(_: uefi.Handle, _: *uefi.tables.SystemTable) callconv(.winapi) uefi.Status {
    serialInit();
    serialWrite("SakuraMicroKernel Phase1 boot\n");

    var state = kernel.KernelState.initPhase1();
    dumpState(&state, "init");

    state.startDma(0) catch |err| {
        serialWrite("DMA start failed: ");
        serialWrite(@errorName(err));
        serialWrite("\n");
        return .aborted;
    };
    serialWrite("DMA start: region 0\n");
    serialWrite("owner -> Device0\n");
    dumpState(&state, "after start_dma");

    state.completeDma(0) catch |err| {
        serialWrite("DMA complete failed: ");
        serialWrite(@errorName(err));
        serialWrite("\n");
        return .aborted;
    };
    serialWrite("DMA complete: region 0\n");
    serialWrite("owner -> Process0\n");
    dumpState(&state, "after complete_dma");

    while (true) {
        asm volatile ("hlt");
    }

    return .success;
}
