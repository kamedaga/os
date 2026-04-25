const std = @import("std");
const gpu_protocol = @import("gpu_protocol.zig");
const service_registry_abi = @import("service_registry_abi.zig");

const syscall_alloc_page: u64 = 0x1;
const syscall_map_page: u64 = 0x2;
const syscall_log: u64 = 0x9;
const syscall_wait_event: u64 = 0x17;
const syscall_grant_cap_on_endpoint: u64 = 0x24;
const syscall_install_endpoint: u64 = 0x26;
const syscall_share_cap: u64 = 0x2B;
const syscall_signal_endpoint: u64 = 0x2C;
const syscall_ok: u64 = 0;
const syscall_err_endpoint: u64 = 9;

const page_right_cpu_read: u64 = 0x1;
const page_right_cpu_write: u64 = 0x2;
const default_response_poll_limit: u64 = 4096;

pub const Error = error{
    RequestAllocFailed,
    RequestMapFailed,
    ResponseAllocFailed,
    ResponseMapFailed,
    EndpointNotFound,
    EndpointInstallFailed,
    ResponseGrantFailed,
    RequestSendFailed,
    Timeout,
    InvalidResponse,
    BufferTooSmall,
    Invalid,
    Unavailable,
    IoError,
};

pub const ConnectOptions = struct {
    request_va: u64,
    response_va: u64,
    endpoint_id: u64,
    response_poll_limit: u64 = default_response_poll_limit,
    compat_process_slot: u64 = 0,
    allow_process_slot_compat: bool = false,
};

pub const RegistryConnectOptions = struct {
    response_poll_limit: u64 = default_response_poll_limit,
    allow_process_slot_compat: bool = true,
    allow_fixed_endpoint_fallback: bool = true,
};

pub const Caps = struct {
    features: u64,
    capset_id: u32,
    capset_max_version: u32,
};

pub const RenderTarget = struct {
    width: u32,
    height: u32,
    resource_id: u32,
    surface_id: u32,
    vertex_buffer_id: u32 = gpu_protocol.default_virgl_vertex_buffer_id,
};

pub const Client = struct {
    request_va: u64,
    response_va: u64,
    request_paddr: u64,
    response_paddr: u64,
    server_endpoint_id: u64,
    request_shared: bool = false,
    next_seq: u64 = 1,
    response_poll_limit: u64 = default_response_poll_limit,

    pub fn connect(options: ConnectOptions) Error!Client {
        const request_paddr = allocPage();
        if (request_paddr < 0x1000) return error.RequestAllocFailed;
        if (mapPage(options.request_va, request_paddr, true) != syscall_ok) return error.RequestMapFailed;

        const response_paddr = allocPage();
        if (response_paddr < 0x1000) return error.ResponseAllocFailed;
        if (mapPage(options.response_va, response_paddr, true) != syscall_ok) return error.ResponseMapFailed;

        var compat_installed = false;
        try grantResponseCap(response_paddr, options, &compat_installed);

        var client = Client{
            .request_va = options.request_va,
            .response_va = options.response_va,
            .request_paddr = request_paddr,
            .response_paddr = response_paddr,
            .server_endpoint_id = options.endpoint_id,
            .response_poll_limit = options.response_poll_limit,
        };
        client.clearMappedPages();
        return client;
    }

    pub fn connectFromRegistryPage(registry_page_va: u64, request_va: u64, response_va: u64) Error!Client {
        return connectFromRegistryPageOptions(registry_page_va, request_va, response_va, .{});
    }

    pub fn connectFromRegistryPageOptions(
        registry_page_va: u64,
        request_va: u64,
        response_va: u64,
        options: RegistryConnectOptions,
    ) Error!Client {
        if (service_registry_abi.findService(registry_page_va, .gpu)) |entry| {
            if (entry.endpoint_id == 0) return error.Invalid;
            const compat_allowed = service_registry_abi.allowsProcessSlotCompat(entry);
            const allow_process_slot_compat = options.allow_process_slot_compat and compat_allowed;
            return connect(.{
                .request_va = request_va,
                .response_va = response_va,
                .endpoint_id = entry.endpoint_id,
                .response_poll_limit = options.response_poll_limit,
                .compat_process_slot = if (compat_allowed) entry.process_slot else 0,
                .allow_process_slot_compat = allow_process_slot_compat,
            }) catch |err| {
                logRegistryConnectFailure(registry_page_va, entry, compat_allowed, allow_process_slot_compat, err);
                return err;
            };
        }
        if (!options.allow_fixed_endpoint_fallback) return error.EndpointNotFound;
        return connect(.{
            .request_va = request_va,
            .response_va = response_va,
            .endpoint_id = gpu_protocol.endpoint_id,
            .response_poll_limit = options.response_poll_limit,
        });
    }

    pub fn queryCaps(self: *Client) Error!Caps {
        const seq = try self.beginRequest(.query_caps, 0, 0, &[_]u8{});
        const response = try self.finishRequestOk(seq, .query_caps);
        return .{
            .features = response.arg0,
            .capset_id = @intCast(response.arg1),
            .capset_max_version = @intCast(response.arg2),
        };
    }

    pub fn submitNoop(self: *Client) Error!void {
        const seq = try self.beginRequest(.submit_nop, 0, 0, &[_]u8{});
        _ = try self.finishRequestOk(seq, .submit_nop);
    }

    pub fn submit3d(self: *Client, commands: []const u8) Error!void {
        if (commands.len == 0 or (commands.len & 0x3) != 0) return error.Invalid;
        const seq = try self.beginRequest(.submit_3d, commands.len, 0, commands);
        _ = try self.finishRequestOk(seq, .submit_3d);
    }

    pub fn prepare3d(self: *Client) Error!RenderTarget {
        const seq = try self.beginRequest(.prepare_3d, 0, 0, &[_]u8{});
        const response = try self.finishRequestOk(seq, .prepare_3d);
        return .{
            .width = @intCast(response.arg0),
            .height = @intCast(response.arg1),
            .resource_id = @intCast(response.arg2 & 0xffff_ffff),
            .surface_id = @intCast(response.arg2 >> 32),
        };
    }

    pub fn present3d(self: *Client) Error!void {
        const seq = try self.beginRequest(.present_3d, 0, 0, &[_]u8{});
        _ = try self.finishRequestOk(seq, .present_3d);
    }

    pub fn presentTestPattern(self: *Client) Error!void {
        const seq = try self.beginRequest(.present_test_pattern, 0, 0, &[_]u8{});
        _ = try self.finishRequestOk(seq, .present_test_pattern);
    }

    fn requestHeader(self: *const Client) *volatile gpu_protocol.RequestHeader {
        return @ptrFromInt(self.request_va);
    }

    fn responseHeader(self: *const Client) *volatile gpu_protocol.ResponseHeader {
        return @ptrFromInt(self.response_va);
    }

    fn requestPayload(self: *const Client) [*]volatile u8 {
        return @ptrFromInt(self.request_va + gpu_protocol.request_header_bytes);
    }

    fn clearMappedPages(self: *const Client) void {
        clearPage(self.request_va);
        clearPage(self.response_va);
    }

    fn beginRequest(self: *Client, op: gpu_protocol.Opcode, arg0: u64, arg1: u64, payload: []const u8) Error!u64 {
        if (payload.len > gpu_protocol.request_payload_bytes) return error.BufferTooSmall;
        self.clearMappedPages();
        const request = self.requestHeader();
        request.magic = gpu_protocol.request_magic;
        request.version = gpu_protocol.version;
        request.op = gpu_protocol.opcodeRaw(op);
        request.response_paddr = self.response_paddr;
        request.arg0 = arg0;
        request.arg1 = arg1;
        request.inline_bytes = @intCast(payload.len);
        request.reserved0 = 0;
        copyBytesToVolatile(self.requestPayload(), payload);
        const seq = self.next_seq;
        self.next_seq +%= 1;
        if (self.next_seq == 0) self.next_seq = 1;
        compilerBarrier();
        request.request_seq = seq;
        if (self.request_shared) {
            const result = signalEndpoint(self.server_endpoint_id);
            if (result == syscall_err_endpoint) return error.EndpointNotFound;
            if (result != syscall_ok) return error.RequestSendFailed;
        } else {
            const result = shareCap(self.request_paddr, self.server_endpoint_id);
            if (result == syscall_err_endpoint) return error.EndpointNotFound;
            if (result != syscall_ok) return error.RequestSendFailed;
            self.request_shared = true;
        }
        return seq;
    }

    fn finishRequest(self: *Client, expected_seq: u64, expected_op: gpu_protocol.Opcode) Error!*volatile gpu_protocol.ResponseHeader {
        if (!self.waitForResponse(expected_seq)) return error.Timeout;
        const response = self.responseHeader();
        if (response.magic != gpu_protocol.response_magic or response.version != gpu_protocol.version) return error.InvalidResponse;
        if (response.op != gpu_protocol.opcodeRaw(expected_op) or response.response_seq != expected_seq) return error.InvalidResponse;
        _ = parseStatus(response.status) orelse return error.InvalidResponse;
        return response;
    }

    fn finishRequestOk(self: *Client, expected_seq: u64, expected_op: gpu_protocol.Opcode) Error!*volatile gpu_protocol.ResponseHeader {
        const response = try self.finishRequest(expected_seq, expected_op);
        const status = parseStatus(response.status) orelse return error.InvalidResponse;
        if (status != .ok) return statusToError(status);
        return response;
    }

    fn waitForResponse(self: *const Client, expected_seq: u64) bool {
        const response = self.responseHeader();
        var poll_count: u64 = 0;
        while (poll_count < self.response_poll_limit) : (poll_count += 1) {
            if (response.response_seq == expected_seq) return true;
            _ = waitEvent(false, 1);
        }
        return false;
    }
};

fn allocPage() u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_alloc_page),
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

fn shareCap(paddr: u64, endpoint_id: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_share_cap),
          [arg0] "{rdi}" (paddr),
          [arg1] "{rsi}" (endpoint_id),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn grantCapOnEndpoint(paddr: u64, endpoint_id: u64, rights: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_grant_cap_on_endpoint),
          [arg0] "{rdi}" (paddr),
          [arg1] "{rsi}" (endpoint_id),
          [arg2] "{rdx}" (rights),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn signalEndpoint(endpoint_id: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_signal_endpoint),
          [arg0] "{rdi}" (endpoint_id),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn grantResponseCap(paddr: u64, options: ConnectOptions, compat_installed: *bool) Error!void {
    const result = grantCapOnEndpoint(paddr, options.endpoint_id, page_right_cpu_read | page_right_cpu_write);
    if (result == syscall_ok) return;
    if (result != syscall_err_endpoint) return error.ResponseGrantFailed;

    if (!compat_installed.* and try attemptCompatEndpointInstall(options)) {
        compat_installed.* = true;
        const retry = grantCapOnEndpoint(paddr, options.endpoint_id, page_right_cpu_read | page_right_cpu_write);
        if (retry == syscall_ok) return;
        if (retry == syscall_err_endpoint) return error.EndpointNotFound;
        return error.ResponseGrantFailed;
    }
    return error.EndpointNotFound;
}

fn attemptCompatEndpointInstall(options: ConnectOptions) Error!bool {
    if (!options.allow_process_slot_compat or options.compat_process_slot == 0) return false;
    _ = userLog("gpu_client: process-slot compat fallback\n");
    return switch (installEndpoint(options.endpoint_id, options.compat_process_slot)) {
        syscall_ok => true,
        else => error.EndpointInstallFailed,
    };
}

fn installEndpoint(endpoint_id: u64, target_process_slot: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_install_endpoint),
          [arg0] "{rdi}" (@as(u64, 0)),
          [arg1] "{rsi}" (endpoint_id),
          [arg2] "{rdx}" (target_process_slot),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn logRegistryConnectFailure(
    registry_page_va: u64,
    entry: service_registry_abi.ServiceEntry,
    compat_allowed: bool,
    allow_process_slot_compat: bool,
    err: Error,
) void {
    switch (err) {
        error.EndpointNotFound,
        error.EndpointInstallFailed,
        error.ResponseGrantFailed,
        error.RequestSendFailed,
        => {},
        else => return,
    }
    var buf: [192]u8 = undefined;
    const line = std.fmt.bufPrint(
        &buf,
        "gpu_client: registry=0x{x} ep=0x{x} slot={} flags=0x{x} compat={} allow={} err={s}\n",
        .{
            registry_page_va,
            entry.endpoint_id,
            entry.process_slot,
            entry.flags,
            @intFromBool(compat_allowed),
            @intFromBool(allow_process_slot_compat),
            @errorName(err),
        },
    ) catch return;
    _ = userLog(line);
}

fn userLog(message: []const u8) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_log),
          [arg0] "{rdi}" (@as(u64, @intFromPtr(message.ptr))),
          [arg1] "{rsi}" (@as(u64, @intCast(message.len))),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn waitEvent(wait_mailbox: bool, timeout_ticks: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_wait_event),
          [arg0] "{rdi}" (@as(u64, if (wait_mailbox) 1 else 0)),
          [arg1] "{rsi}" (timeout_ticks),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn clearPage(base_va: u64) void {
    const ptr: [*]volatile u64 = @ptrFromInt(base_va);
    var i: usize = 0;
    while (i < 512) : (i += 1) {
        ptr[i] = 0;
    }
}

fn copyBytesToVolatile(dest: [*]volatile u8, src: []const u8) void {
    var i: usize = 0;
    while (i < src.len) : (i += 1) {
        dest[i] = src[i];
    }
}

fn compilerBarrier() void {
    asm volatile ("" ::: .{ .memory = true });
}

fn parseStatus(raw: i32) ?gpu_protocol.Status {
    return std.meta.intToEnum(gpu_protocol.Status, raw) catch null;
}

fn statusToError(status: gpu_protocol.Status) Error {
    return switch (status) {
        .ok => unreachable,
        .invalid => error.Invalid,
        .unavailable => error.Unavailable,
        .io_error => error.IoError,
    };
}
