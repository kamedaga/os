pub const Config = struct {
    syscall_alloc_page: u64,
    syscall_map_page: u64,
    syscall_move_cap: u64,
    syscall_drop_present: u64,
    syscall_switch_thread: u64,
    syscall_send_cap: u64,
    syscall_revoke_tree: u64,
    syscall_grant_cap: u64,
    syscall_log: u64,
    user_unmapped_test_va: u64,
    user_dma_verify_va: u64,
    user_recovery_stop_va: u64,
};

fn writeU64LE(ptr: [*]volatile u8, offset: usize, value: u64) void {
    var i: usize = 0;
    while (i < 8) : (i += 1) {
        ptr[offset + i] = @intCast((value >> @intCast(i * 8)) & 0xFF);
    }
}

pub fn installIdleTaskCode(user_page_paddr: u64) void {
    const code: [*]volatile u8 = @ptrFromInt(user_page_paddr);
    // pause; jmp -4
    code[0] = 0xF3;
    code[1] = 0x90;
    code[2] = 0xEB;
    code[3] = 0xFC;
}

pub fn installFramebufferFillCode(
    user_page_paddr: u64,
    framebuffer_user_va: u64,
    pixel_count: u64,
    color: u32,
) void {
    const code: [*]volatile u8 = @ptrFromInt(user_page_paddr);
    var off: usize = 0;

    // mov rdi, framebuffer_user_va
    code[off] = 0x48;
    code[off + 1] = 0xBF;
    writeU64LE(code, off + 2, framebuffer_user_va);
    off += 10;

    // mov eax, color
    code[off] = 0xB8;
    code[off + 1] = @intCast(color & 0xFF);
    code[off + 2] = @intCast((color >> 8) & 0xFF);
    code[off + 3] = @intCast((color >> 16) & 0xFF);
    code[off + 4] = @intCast((color >> 24) & 0xFF);
    off += 5;

    // mov rcx, pixel_count
    code[off] = 0x48;
    code[off + 1] = 0xB9;
    writeU64LE(code, off + 2, pixel_count);
    off += 10;

    // rep stosd
    code[off] = 0xF3;
    code[off + 1] = 0xAB;
    off += 2;

    // pause; jmp -4
    code[off] = 0xF3;
    code[off + 1] = 0x90;
    code[off + 2] = 0xEB;
    code[off + 3] = 0xFC;
}

pub fn installMemoryWritePfTestCode(cfg: Config, user_page_paddr: u64) void {
    const code: [*]volatile u8 = @ptrFromInt(user_page_paddr);
    var off: usize = 0;

    code[off] = 0x48;
    code[off + 1] = 0xB8;
    writeU64LE(code, off + 2, cfg.syscall_alloc_page);
    off += 10;
    code[off] = 0xCD;
    code[off + 1] = 0x80;
    off += 2;
    code[off] = 0x48;
    code[off + 1] = 0x89;
    code[off + 2] = 0xC3;
    off += 3;

    code[off] = 0x48;
    code[off + 1] = 0xB8;
    writeU64LE(code, off + 2, cfg.syscall_map_page);
    off += 10;
    code[off] = 0x48;
    code[off + 1] = 0xBF;
    writeU64LE(code, off + 2, cfg.user_unmapped_test_va);
    off += 10;
    code[off] = 0x48;
    code[off + 1] = 0x89;
    code[off + 2] = 0xDE;
    off += 3;
    code[off] = 0x48;
    code[off + 1] = 0xBA;
    writeU64LE(code, off + 2, 0x1);
    off += 10;
    code[off] = 0xCD;
    code[off + 1] = 0x80;
    off += 2;

    code[off] = 0x48;
    code[off + 1] = 0xB8;
    writeU64LE(code, off + 2, cfg.user_unmapped_test_va);
    off += 10;
    code[off] = 0xC7;
    code[off + 1] = 0x00;
    code[off + 2] = 0xDD;
    code[off + 3] = 0xCC;
    code[off + 4] = 0xBB;
    code[off + 5] = 0xAA;
    off += 6;

    code[off] = 0x48;
    code[off + 1] = 0xB8;
    writeU64LE(code, off + 2, cfg.syscall_move_cap);
    off += 10;
    code[off] = 0x48;
    code[off + 1] = 0x89;
    code[off + 2] = 0xDF;
    off += 3;
    code[off] = 0x48;
    code[off + 1] = 0xBE;
    writeU64LE(code, off + 2, 0x1);
    off += 10;
    code[off] = 0x48;
    code[off + 1] = 0xBA;
    writeU64LE(code, off + 2, 0x4);
    off += 10;
    code[off] = 0xCD;
    code[off + 1] = 0x80;
    off += 2;

    code[off] = 0x48;
    code[off + 1] = 0xB8;
    writeU64LE(code, off + 2, cfg.user_unmapped_test_va);
    off += 10;
    code[off] = 0xC7;
    code[off + 1] = 0x00;
    code[off + 2] = 0x78;
    code[off + 3] = 0x56;
    code[off + 4] = 0x34;
    code[off + 5] = 0x12;
    off += 6;

    code[off] = 0xEB;
    code[off + 1] = 0xFE;
}

pub fn installGeneralProtectionTestCode(user_page_paddr: u64) void {
    const code: [*]volatile u8 = @ptrFromInt(user_page_paddr);
    code[0] = 0xFA;
    code[1] = 0xEB;
    code[2] = 0xFE;
}

pub fn installPfRecoveryDemoCode(cfg: Config, user_page_paddr: u64) void {
    const code: [*]volatile u8 = @ptrFromInt(user_page_paddr);
    var off: usize = 0;

    code[off] = 0x48;
    code[off + 1] = 0xB8;
    writeU64LE(code, off + 2, cfg.syscall_alloc_page);
    off += 10;
    code[off] = 0xCD;
    code[off + 1] = 0x80;
    off += 2;
    code[off] = 0x48;
    code[off + 1] = 0x89;
    code[off + 2] = 0xC3;
    off += 3;

    code[off] = 0x48;
    code[off + 1] = 0xB8;
    writeU64LE(code, off + 2, cfg.syscall_map_page);
    off += 10;
    code[off] = 0x48;
    code[off + 1] = 0xBF;
    writeU64LE(code, off + 2, cfg.user_unmapped_test_va);
    off += 10;
    code[off] = 0x48;
    code[off + 1] = 0x89;
    code[off + 2] = 0xDE;
    off += 3;
    code[off] = 0x48;
    code[off + 1] = 0xBA;
    writeU64LE(code, off + 2, 0x1);
    off += 10;
    code[off] = 0xCD;
    code[off + 1] = 0x80;
    off += 2;

    code[off] = 0x48;
    code[off + 1] = 0xB8;
    writeU64LE(code, off + 2, cfg.syscall_drop_present);
    off += 10;
    code[off] = 0x48;
    code[off + 1] = 0x89;
    code[off + 2] = 0xDF;
    off += 3;
    code[off] = 0xCD;
    code[off + 1] = 0x80;
    off += 2;

    code[off] = 0x48;
    code[off + 1] = 0xB8;
    writeU64LE(code, off + 2, cfg.user_unmapped_test_va);
    off += 10;
    code[off] = 0xC7;
    code[off + 1] = 0x00;
    code[off + 2] = 0x44;
    code[off + 3] = 0x33;
    code[off + 4] = 0x22;
    code[off + 5] = 0x11;
    off += 6;

    code[off] = 0x48;
    code[off + 1] = 0xB8;
    writeU64LE(code, off + 2, cfg.user_recovery_stop_va);
    off += 10;
    code[off] = 0xC7;
    code[off + 1] = 0x00;
    code[off + 2] = 0x88;
    code[off + 3] = 0x77;
    code[off + 4] = 0x66;
    code[off + 5] = 0x55;
    off += 6;

    code[off] = 0xEB;
    code[off + 1] = 0xFE;
}

pub fn installPfRecoveryThenSwitchCode(cfg: Config, user_page_paddr: u64, target_thread: u64) void {
    const code: [*]volatile u8 = @ptrFromInt(user_page_paddr);
    var off: usize = 0;

    code[off] = 0x48;
    code[off + 1] = 0xB8;
    writeU64LE(code, off + 2, cfg.syscall_alloc_page);
    off += 10;
    code[off] = 0xCD;
    code[off + 1] = 0x80;
    off += 2;
    code[off] = 0x48;
    code[off + 1] = 0x89;
    code[off + 2] = 0xC3;
    off += 3;

    code[off] = 0x48;
    code[off + 1] = 0xB8;
    writeU64LE(code, off + 2, cfg.syscall_map_page);
    off += 10;
    code[off] = 0x48;
    code[off + 1] = 0xBF;
    writeU64LE(code, off + 2, cfg.user_unmapped_test_va);
    off += 10;
    code[off] = 0x48;
    code[off + 1] = 0x89;
    code[off + 2] = 0xDE;
    off += 3;
    code[off] = 0x48;
    code[off + 1] = 0xBA;
    writeU64LE(code, off + 2, 0x1);
    off += 10;
    code[off] = 0xCD;
    code[off + 1] = 0x80;
    off += 2;

    code[off] = 0x48;
    code[off + 1] = 0xB8;
    writeU64LE(code, off + 2, cfg.syscall_drop_present);
    off += 10;
    code[off] = 0x48;
    code[off + 1] = 0x89;
    code[off + 2] = 0xDF;
    off += 3;
    code[off] = 0xCD;
    code[off + 1] = 0x80;
    off += 2;

    code[off] = 0x48;
    code[off + 1] = 0xB8;
    writeU64LE(code, off + 2, cfg.user_unmapped_test_va);
    off += 10;
    code[off] = 0xC7;
    code[off + 1] = 0x00;
    code[off + 2] = 0x44;
    code[off + 3] = 0x33;
    code[off + 4] = 0x22;
    code[off + 5] = 0x11;
    off += 6;

    code[off] = 0x48;
    code[off + 1] = 0xB8;
    writeU64LE(code, off + 2, cfg.syscall_switch_thread);
    off += 10;
    code[off] = 0x48;
    code[off + 1] = 0xBF;
    writeU64LE(code, off + 2, target_thread);
    off += 10;
    code[off] = 0xCD;
    code[off + 1] = 0x80;
    off += 2;

    code[off] = 0xEB;
    code[off + 1] = 0xFE;
}

pub fn installDmaUnmapVerifyCode(cfg: Config, user_page_paddr: u64) void {
    const code: [*]volatile u8 = @ptrFromInt(user_page_paddr);
    var off: usize = 0;

    code[off] = 0x48;
    code[off + 1] = 0xB8;
    writeU64LE(code, off + 2, cfg.user_dma_verify_va);
    off += 10;

    code[off] = 0xC7;
    code[off + 1] = 0x00;
    code[off + 2] = 0xEF;
    code[off + 3] = 0xBE;
    code[off + 4] = 0xAD;
    code[off + 5] = 0xDE;
    off += 6;

    code[off] = 0xEB;
    code[off + 1] = 0xFE;
}

pub fn installSchedulerProbeCode(user_page_paddr: u64, syscall_no: u64) void {
    const code: [*]volatile u8 = @ptrFromInt(user_page_paddr);
    var off: usize = 0;

    // loop: mov rax, syscall_no ; int 0x80 ; jmp loop
    code[off] = 0x48;
    code[off + 1] = 0xB8;
    writeU64LE(code, off + 2, syscall_no);
    off += 10;

    code[off] = 0xCD;
    code[off + 1] = 0x80;
    off += 2;

    code[off] = 0xEB;
    code[off + 1] = 0xF2; // jump back -14 bytes to loop head
}

pub fn installSchedulerProbeWithSendCapCode(
    cfg: Config,
    user_page_paddr: u64,
    probe_syscall_no: u64,
    endpoint_id: u64,
) void {
    const code: [*]volatile u8 = @ptrFromInt(user_page_paddr);
    var off: usize = 0;

    // one-shot: alloc page -> send_cap(page, target_process)
    code[off] = 0x48;
    code[off + 1] = 0xB8;
    writeU64LE(code, off + 2, cfg.syscall_alloc_page);
    off += 10;
    code[off] = 0xCD;
    code[off + 1] = 0x80;
    off += 2;
    code[off] = 0x48;
    code[off + 1] = 0x89;
    code[off + 2] = 0xC3; // mov rbx, rax (allocated paddr)
    off += 3;

    code[off] = 0x48;
    code[off + 1] = 0xB8;
    writeU64LE(code, off + 2, cfg.syscall_send_cap);
    off += 10;
    code[off] = 0x48;
    code[off + 1] = 0x89;
    code[off + 2] = 0xDF; // mov rdi, rbx (paddr)
    off += 3;
    code[off] = 0x48;
    code[off + 1] = 0xBE;
    writeU64LE(code, off + 2, endpoint_id); // rsi = endpoint id
    off += 10;
    code[off] = 0xCD;
    code[off + 1] = 0x80;
    off += 2;

    // loop: mov rax, probe_syscall_no ; int 0x80 ; jmp loop
    code[off] = 0x48;
    code[off + 1] = 0xB8;
    writeU64LE(code, off + 2, probe_syscall_no);
    off += 10;
    code[off] = 0xCD;
    code[off + 1] = 0x80;
    off += 2;
    code[off] = 0xEB;
    code[off + 1] = 0xF2; // jump back -14 bytes to loop head
}

pub fn installCapSendTransferDemoCode(
    cfg: Config,
    user_page_paddr: u64,
    paddr_to_send: u64,
    send_endpoint_id: u64,
    switch_to_thread: u64,
) void {
    const code: [*]volatile u8 = @ptrFromInt(user_page_paddr);
    var off: usize = 0;

    // send_cap(paddr_to_send, send_to_process)
    code[off] = 0x48;
    code[off + 1] = 0xB8;
    writeU64LE(code, off + 2, cfg.syscall_send_cap);
    off += 10;
    code[off] = 0x48;
    code[off + 1] = 0xBF;
    writeU64LE(code, off + 2, paddr_to_send);
    off += 10;
    code[off] = 0x48;
    code[off + 1] = 0xBE;
    writeU64LE(code, off + 2, send_endpoint_id);
    off += 10;
    code[off] = 0xCD;
    code[off + 1] = 0x80;
    off += 2;

    // switch_thread(switch_to_thread)
    code[off] = 0x48;
    code[off + 1] = 0xB8;
    writeU64LE(code, off + 2, cfg.syscall_switch_thread);
    off += 10;
    code[off] = 0x48;
    code[off + 1] = 0xBF;
    writeU64LE(code, off + 2, switch_to_thread);
    off += 10;
    code[off] = 0xCD;
    code[off + 1] = 0x80;
    off += 2;

    // verify return code: if RAX != 0 then trap with UD2 for fast debug.
    code[off] = 0x48;
    code[off + 1] = 0x83;
    code[off + 2] = 0xF8;
    code[off + 3] = 0x00; // cmp rax, 0
    off += 4;
    code[off] = 0x75;
    code[off + 1] = 0x02; // jne +2 -> UD2
    off += 2;
    code[off] = 0xEB;
    code[off + 1] = 0xFE; // success: busy loop
    off += 2;
    code[off] = 0x0F;
    code[off + 1] = 0x0B; // failure: UD2
}
