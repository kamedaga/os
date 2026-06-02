pub const magic: u64 = 0x3149_4241_5041_5254; // TRAPABI1
pub const version: u32 = 2;

pub const TrapKind = enum(u32) {
    abi_syscall = 1,
    page_fault = 2,
    illegal_instruction = 3,
    breakpoint = 4,
    protection_fault = 5,
};

pub const AbiFlavor = enum(u32) {
    native = 0,
    linux_x86_64 = 1,
    linux_i386 = 2,
    wasm_hostcall = 3,
    debug = 4,
};

pub const TrapAction = enum(u32) {
    resume_thread = 0,
    fail = 1,
    kill = 2,
    block = 3,
    restart = 4,
};

pub const response_flag_exit: u64 = 1 << 0;
pub const response_flag_block: u64 = 1 << 1;
pub const response_flag_restart: u64 = 1 << 2;

pub const max_args: usize = 6;
pub const syscall_map_abi_trap_reply_target_pages: u64 = 0x4C;
pub const syscall_copy_from_abi_trap_reply_target: u64 = 0x4D;
pub const syscall_copy_to_abi_trap_reply_target: u64 = 0x4E;
pub const syscall_set_abi_trap_reply_target_fs_base: u64 = 0x4F;
pub const syscall_protect_abi_trap_reply_target_pages: u64 = 0x50;
pub const syscall_unmap_abi_trap_reply_target_pages: u64 = 0x51;
pub const syscall_map_abi_trap_reply_target_vm_object: u64 = 0x52;
pub const syscall_reply_abi_trap_target: u64 = 0x54;
pub const syscall_copy_to_abi_trap_target: u64 = 0x55;
pub const syscall_set_abi_trap_target_request_page: u64 = 0x57;
pub const syscall_detach_abi_trap_reply_token: u64 = 0x59;
pub const syscall_copy_from_abi_trap_target: u64 = 0x5B;
pub const syscall_reply_abi_trap_target_context: u64 = 0x64;
pub const syscall_set_abi_trap_reply_target_gs_base: u64 = 0x65;
pub const abi_trap_copy_max_bytes: usize = 4096;

pub const TrapRequest = extern struct {
    magic: u64 = magic,
    version: u32 = version,
    kind: u32,
    flavor: u32,
    reserved0: u32 = 0,
    caller_principal: u64,
    thread_id: u64,
    rip: u64,
    rsp: u64,
    fault_addr: u64,
    error_code: u64,
    nr: u64,
    args: [max_args]u64,
    r15: u64 = 0,
    r14: u64 = 0,
    r13: u64 = 0,
    r12: u64 = 0,
    r11: u64 = 0,
    r10: u64 = 0,
    r9: u64 = 0,
    r8: u64 = 0,
    rbp: u64 = 0,
    rdi: u64 = 0,
    rsi: u64 = 0,
    rdx: u64 = 0,
    rcx: u64 = 0,
    rbx: u64 = 0,
    rax: u64 = 0,
    rflags: u64 = 0,
    fs_base: u64 = 0,
    gs_base: u64 = 0,
};

pub const UserContext = extern struct {
    flags: u64 = 0,
    rip: u64 = 0,
    rsp: u64 = 0,
    rflags: u64 = 0,
    rax: u64 = 0,
    rbx: u64 = 0,
    rcx: u64 = 0,
    rdx: u64 = 0,
    rsi: u64 = 0,
    rdi: u64 = 0,
    rbp: u64 = 0,
    r8: u64 = 0,
    r9: u64 = 0,
    r10: u64 = 0,
    r11: u64 = 0,
    r12: u64 = 0,
    r13: u64 = 0,
    r14: u64 = 0,
    r15: u64 = 0,
    fs_base: u64 = 0,
    gs_base: u64 = 0,
    reserved0: u64 = 0,
    reserved1: u64 = 0,
};

pub const TrapResponse = extern struct {
    magic: u64 = magic,
    version: u32 = version,
    action: u32,
    flags: u64,
    result: u64,
    new_rip: u64,
    new_rsp: u64,
};
