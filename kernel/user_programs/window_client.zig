const syscall_create_window: u64 = 0xD;
const syscall_map_page: u64 = 0x2;
const syscall_send_cap: u64 = 0x6;
const syscall_grant_cap: u64 = 0x8;
const syscall_alloc_map_pages: u64 = 0xC;
const syscall_log: u64 = 0x9;
const protocol = @import("window_protocol.zig");

const syscall_ok: u64 = 0;
pub const endpoint_to_process1: u64 = 0x11;
pub const window_cap_magic = protocol.window_cap_magic;
pub const window_meta_magic = protocol.window_meta_magic;
pub const window_flag_allow_pixels_dma = protocol.window_flag_allow_pixels_dma;
pub const WindowCap = protocol.WindowCap;
pub const WindowMeta = protocol.WindowMeta;

fn userLog(message: []const u8) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_log),
          [arg0] "{rdi}" (@as(u64, @intFromPtr(message.ptr))),
          [arg1] "{rsi}" (@as(u64, @intCast(message.len))),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn mapPage(va: u64, paddr: u64, writable: bool) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_map_page),
          [arg0] "{rdi}" (va),
          [arg1] "{rsi}" (paddr),
          [arg2] "{rdx}" (@as(u64, if (writable) 1 else 0)),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn sendCap(paddr: u64, endpoint_id: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_send_cap),
          [arg0] "{rdi}" (paddr),
          [arg1] "{rsi}" (endpoint_id),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn grantCap(paddr: u64, to: u64, rights_bits: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_grant_cap),
          [arg0] "{rdi}" (paddr),
          [arg1] "{rsi}" (to),
          [arg2] "{rdx}" (rights_bits),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn allocMapPages(base_va: u64, page_count: u64, writable: bool, out_paddr_list_va: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_alloc_map_pages),
          [arg0] "{rdi}" (base_va),
          [arg1] "{rsi}" (page_count),
          [arg2] "{rdx}" (@as(u64, if (writable) 1 else 0)),
          [arg3] "{rcx}" (out_paddr_list_va),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn createWindowSys(width: u16, height: u16, flags: u32) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_create_window),
          [arg0] "{rdi}" (@as(u64, width)),
          [arg1] "{rsi}" (@as(u64, height)),
          [arg2] "{rdx}" (@as(u64, flags)),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

pub fn createAndPublishWindow(
    width: u16,
    height: u16,
    flags: u32,
    cap_tmp_va: u64,
    pixel_map_va: u64,
    meta_map_va: u64,
) bool {
    const effective_flags = flags | window_flag_allow_pixels_dma;
    const cap_paddr = createWindowSys(width, height, effective_flags);
    if (cap_paddr < 0x1000) {
        _ = userLog("window_client: create_window failed\n");
        return false;
    }
    if (mapPage(cap_tmp_va, cap_paddr, true) != syscall_ok) {
        _ = userLog("window_client: map cap page failed\n");
        return false;
    }
    const cap: *volatile WindowCap = @ptrFromInt(cap_tmp_va);
    if (cap.magic != window_cap_magic or cap.version != 1) {
        _ = userLog("window_client: bad window cap magic\n");
        return false;
    }
    if (cap.pixels_page_count != 1) {
        _ = userLog("window_client: pixels_page_count != 1 unsupported\n");
        return false;
    }
    var pixel_paddrs: [1]u64 = [_]u64{0};
    if (allocMapPages(pixel_map_va, 1, true, @intFromPtr(&pixel_paddrs)) != syscall_ok) {
        _ = userLog("window_client: alloc/map pixels failed\n");
        return false;
    }
    const pixel_paddr = pixel_paddrs[0];
    if (pixel_paddr < 0x1000) {
        _ = userLog("window_client: bad pixels paddr\n");
        return false;
    }

    var grant_rights: u64 = 0x1; // cpu_read
    if ((effective_flags & window_flag_allow_pixels_dma) != 0) {
        grant_rights |= 0x4; // dma
    }
    if (grantCap(pixel_paddr, 1, grant_rights) != syscall_ok) {
        _ = userLog("window_client: grant pixels cap failed\n");
        return false;
    }
    cap.pixels_cap_paddr = pixel_paddr;
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

pub fn createAndPublishWindowWithDma(
    width: u16,
    height: u16,
    cap_tmp_va: u64,
    pixel_map_va: u64,
    meta_map_va: u64,
) bool {
    return createAndPublishWindow(
        width,
        height,
        window_flag_allow_pixels_dma,
        cap_tmp_va,
        pixel_map_va,
        meta_map_va,
    );
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
