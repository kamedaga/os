const interrupts = @import("../interrupts.zig");
const sc = @import("numbers.zig");

const TrapFrame = interrupts.TrapFrame;

pub fn dispatch(frame: *TrapFrame) ?u64 {
    return switch (frame.rax) {
        sc.syscall_create_vm_object_from_current_pages,
        sc.syscall_grant_vm_object,
        sc.syscall_release_vm_object,
        sc.syscall_drop_vm_object,
        sc.syscall_map_vm_object,
        => sc.syscall_err_invalid,
        else => null,
    };
}
