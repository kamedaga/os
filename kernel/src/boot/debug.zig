const kernel = @import("../kernel.zig");
const capability = @import("../capability.zig");

pub const Hooks = struct {
    write: *const fn ([]const u8) void,
    print_number: *const fn (u64) void,
    print_hex: *const fn (u64) void,
    principal_label: *const fn (kernel.PrincipalId) []const u8,
};

pub fn logReadyTitle(hooks: Hooks, title: []const u8) void {
    hooks.write(title);
    hooks.write("\n");
}

pub fn dumpAllProcessCaps(
    state: *const kernel.KernelState,
    process_count: usize,
    principal_label: *const fn (kernel.PrincipalId) []const u8,
) void {
    var i: usize = 0;
    while (i < process_count) : (i += 1) {
        const principal = kernel.processPrincipalFromIndex(i) orelse unreachable;
        capability.dumpPrincipalCaps(state, principal, principal_label(principal));
    }
}

fn queueCapOpLabel(op: kernel.QueueOperation) []const u8 {
    return switch (op) {
        .submit => "submit",
        .notify => "notify",
    };
}

pub fn logQueueCapDeny(
    hooks: Hooks,
    proc: kernel.PrincipalId,
    token: u64,
    device: kernel.DmaDeviceId,
    queue_index: u16,
    op: kernel.QueueOperation,
    err: anyerror,
) void {
    hooks.write("queue_cap deny proc=");
    hooks.write(hooks.principal_label(proc));
    hooks.write(" op=");
    hooks.write(queueCapOpLabel(op));
    hooks.write(" device=");
    hooks.write(switch (device) {
        .virtio_gpu => "virtio_gpu",
        .virtio_input => "virtio_input",
        .virtio_blk => "virtio_blk",
    });
    hooks.write(" q=");
    hooks.print_number(queue_index);
    hooks.write(" token=");
    hooks.print_hex(token);
    hooks.write(" err=");
    hooks.write(@errorName(err));
    hooks.write("\n");
}
