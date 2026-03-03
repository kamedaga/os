pub const Config = struct {
    syscall_alloc_page: u64,
    syscall_map_page: u64,
    syscall_move_cap: u64,
    syscall_drop_present: u64,
    syscall_switch_process: u64,
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

pub fn installPfRecoveryThenSwitchCode(cfg: Config, user_page_paddr: u64, target_process: u64) void {
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
    writeU64LE(code, off + 2, cfg.syscall_switch_process);
    off += 10;
    code[off] = 0x48;
    code[off + 1] = 0xBF;
    writeU64LE(code, off + 2, target_process);
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
