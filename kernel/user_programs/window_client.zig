const syscall_create_window: u64 = 0xD;
const syscall_map_page: u64 = 0x2;
const syscall_send_cap: u64 = 0x6;
const syscall_log: u64 = 0x9;

const syscall_ok: u64 = 0;
pub const endpoint_to_process1: u64 = 0x11;

pub const window_cap_magic: u32 = 0x57434150; // 'WCAP'
pub const window_meta_magic: u32 = 0x574D5441; // 'WMTA'

pub const WindowCap = packed struct {
    magic: u32,
    version: u16,
    rights_bits: u16,
    window_id: u32,
    owner_pid: u32,
    vfb_cap_paddr: u64,
    meta_cap_paddr: u64,
    vfb_size_bytes: u32,
    vfb_page_count: u16,
    pixels_per_scan_line: u16,
    pixel_format: u32,
    evt_cap_paddr: u64,
    width: u16,
    height: u16,
    min_width: u16,
    min_height: u16,
    flags: u32,
    z_hint: i32,
    reserved0: u32,
};

pub const WindowMeta = extern struct {
    magic: u32,
    version: u16,
    state: u16,
    seq: u64,
    pos_x: i32,
    pos_y: i32,
    width: u16,
    height: u16,
    title_len: u16,
    title: [64]u8,
};

fn userLog(message: []const u8) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_log),
          [arg0] "{rdi}" (@as(u64, @intFromPtr(message.ptr))),
          [arg1] "{rsi}" (@as(u64, @intCast(message.len))),
        : .{ .memory = true });
}

fn mapPage(va: u64, paddr: u64, writable: bool) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_map_page),
          [arg0] "{rdi}" (va),
          [arg1] "{rsi}" (paddr),
          [arg2] "{rdx}" (@as(u64, if (writable) 1 else 0)),
        : .{ .memory = true });
}

fn sendCap(paddr: u64, endpoint_id: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_send_cap),
          [arg0] "{rdi}" (paddr),
          [arg1] "{rsi}" (endpoint_id),
        : .{ .memory = true });
}

fn createWindowSys(width: u16, height: u16, flags: u32) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_create_window),
          [arg0] "{rdi}" (@as(u64, width)),
          [arg1] "{rsi}" (@as(u64, height)),
          [arg2] "{rdx}" (@as(u64, flags)),
        : .{ .memory = true });
}

pub fn createAndPublishWindow(
    width: u16,
    height: u16,
    flags: u32,
    cap_tmp_va: u64,
    vfb_map_va: u64,
    meta_map_va: u64,
) bool {
    const cap_paddr = createWindowSys(width, height, flags);
    if (cap_paddr < 0x1000) {
        _ = userLog("window_client: create_window failed\n");
        return false;
    }
    if (mapPage(cap_tmp_va, cap_paddr, false) != syscall_ok) {
        _ = userLog("window_client: map cap page failed\n");
        return false;
    }
    const cap: *const volatile WindowCap = @ptrFromInt(cap_tmp_va);
    if (cap.magic != window_cap_magic or cap.version != 1) {
        _ = userLog("window_client: bad window cap magic\n");
        return false;
    }
    if (cap.vfb_page_count != 1) {
        _ = userLog("window_client: vfb_page_count != 1 unsupported\n");
        return false;
    }
    if (mapPage(vfb_map_va, cap.vfb_cap_paddr, true) != syscall_ok) {
        _ = userLog("window_client: map vfb failed\n");
        return false;
    }
    if (mapPage(meta_map_va, cap.meta_cap_paddr, true) != syscall_ok) {
        _ = userLog("window_client: map meta failed\n");
        return false;
    }
    if (sendCap(cap_paddr, endpoint_to_process1) != syscall_ok) {
        _ = userLog("window_client: send window cap failed\n");
        return false;
    }
    return true;
}

pub fn setWindowTitle(meta_va: u64, title: []const u8) void {
    const meta: *volatile WindowMeta = @ptrFromInt(meta_va);
    if (meta.magic != window_meta_magic) return;
    const copy_len: usize = if (title.len < 63) title.len else 63;
    var i: usize = 0;
    while (i < 64) : (i += 1) {
        meta.title[i] = 0;
    }
    i = 0;
    while (i < copy_len) : (i += 1) {
        meta.title[i] = title[i];
    }
    meta.title_len = @intCast(copy_len);
    meta.seq +%= 1;
}
