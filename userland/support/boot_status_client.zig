const boot_status_abi = @import("boot_status_abi.zig");

pub fn set(status_bits: u32) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (boot_status_abi.syscall_set_boot_status),
          [arg0] "{rdi}" (@as(u64, status_bits & boot_status_abi.valid_status_mask)),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}
