const std = @import("std");
const net_protocol = @import("net_protocol.zig");
const service_registry_abi = @import("service_registry_abi.zig");
const user_vm = @import("user_vm.zig");

const syscall_grant_cap_on_endpoint: u64 = 0x24;
const syscall_install_endpoint: u64 = 0x26;
const syscall_share_cap: u64 = 0x2B;
const syscall_signal_endpoint: u64 = 0x2C;
const syscall_wait_event: u64 = 0x17;
const syscall_log: u64 = 0x9;

const syscall_ok: u64 = 0;
const syscall_err_endpoint: u64 = 9;
const page_right_cpu_read: u64 = 0x1;
const page_right_cpu_write: u64 = 0x2;
const default_response_poll_limit: u64 = 1024;

pub const Error = error{
    RequestAllocFailed,
    EndpointNotFound,
    EndpointInstallFailed,
    ResponseGrantFailed,
    ConnectSendFailed,
    Timeout,
    InvalidResponse,
    Invalid,
    NotFound,
};

pub const ConnectOptions = struct {
    request_va: u64 = 0,
    response_va: u64 = 0,
    client_process_slot: u64,
    endpoint_id: u64,
    server_process_slot: u64 = 0,
    response_poll_limit: u64 = default_response_poll_limit,
    compat_process_slot: u64 = 0,
    allow_process_slot_compat: bool = false,
};

pub const RegistryConnectOptions = struct {
    response_poll_limit: u64 = default_response_poll_limit,
    allow_process_slot_compat: bool = true,
};

pub const Client = struct {
    request_va: u64,
    response_va: u64,
    request_paddr: u64,
    response_paddr: u64,
    server_endpoint_id: u64,
    server_process_slot: u64,
    session_nonce: u64,
    next_seq: u64 = 2,
    response_poll_limit: u64 = default_response_poll_limit,

    pub fn connect(options: ConnectOptions) Error!Client {
        const pages = try allocConnectPages(options.request_va, options.response_va);
        var compat_installed = false;
        try grantResponseCapForConnect(pages.response_paddr, options, &compat_installed);
        const session_nonce = makeSessionNonce(pages.request_paddr, pages.response_paddr, options.endpoint_id, options.client_process_slot);

        var client = Client{
            .request_va = pages.request_va,
            .response_va = pages.response_va,
            .request_paddr = pages.request_paddr,
            .response_paddr = pages.response_paddr,
            .server_endpoint_id = options.endpoint_id,
            .server_process_slot = options.server_process_slot,
            .session_nonce = session_nonce,
            .response_poll_limit = options.response_poll_limit,
        };

        client.clearMappedPages();
        const request = client.requestHeader();
        request.magic = net_protocol.request_magic;
        request.version = net_protocol.version;
        request.op = net_protocol.opcodeRaw(.connect);
        request.session_nonce = session_nonce;
        request.arg0 = pages.response_paddr;
        request.arg1 = options.client_process_slot;
        request.arg2 = 0;
        request.reserved0 = 0;
        compilerBarrier();
        request.request_seq = 1;

        try shareConnectRequest(pages.request_paddr, options, &compat_installed);
        _ = try client.finishRequestOk(1, .connect);
        return client;
    }

    pub fn connectFromRegistryPageOptions(
        registry_page_va: u64,
        request_va: u64,
        response_va: u64,
        client_process_slot: u64,
        options: RegistryConnectOptions,
    ) Error!Client {
        const entry = service_registry_abi.findService(registry_page_va, .net) orelse return error.NotFound;
        if (entry.endpoint_id == 0) return error.Invalid;
        const compat_allowed = service_registry_abi.allowsProcessSlotCompat(entry);
        const allow_process_slot_compat = options.allow_process_slot_compat and compat_allowed;
        return connect(.{
            .request_va = request_va,
            .response_va = response_va,
            .client_process_slot = client_process_slot,
            .endpoint_id = entry.endpoint_id,
            .server_process_slot = entry.process_slot,
            .response_poll_limit = options.response_poll_limit,
            .compat_process_slot = if (compat_allowed) entry.process_slot else 0,
            .allow_process_slot_compat = allow_process_slot_compat,
        }) catch |err| {
            logRegistryConnectFailure(registry_page_va, entry, compat_allowed, allow_process_slot_compat, err);
            return err;
        };
    }

    pub fn status(self: *Client) Error!net_protocol.StatusPayload {
        const seq = self.beginRequest(.get_status);
        const response = try self.finishRequestOk(seq, .get_status);
        if (response.inline_bytes != @sizeOf(net_protocol.StatusPayload)) return error.InvalidResponse;
        return self.statusPayload().*;
    }

    fn requestHeader(self: *const Client) *volatile net_protocol.RequestHeader {
        return @ptrFromInt(self.request_va);
    }

    fn responseHeader(self: *const Client) *volatile net_protocol.ResponseHeader {
        return @ptrFromInt(self.response_va);
    }

    fn statusPayload(self: *const Client) *volatile net_protocol.StatusPayload {
        return @ptrFromInt(self.response_va + @sizeOf(net_protocol.ResponseHeader));
    }

    fn clearMappedPages(self: *Client) void {
        clearPage(self.request_va);
        clearPage(self.response_va);
    }

    fn beginRequest(self: *Client, op: net_protocol.Opcode) u64 {
        self.clearMappedPages();
        const request = self.requestHeader();
        request.magic = net_protocol.request_magic;
        request.version = net_protocol.version;
        request.op = net_protocol.opcodeRaw(op);
        request.session_nonce = self.session_nonce;
        request.arg0 = 0;
        request.arg1 = 0;
        request.arg2 = 0;
        request.reserved0 = 0;
        const seq = self.next_seq;
        self.next_seq += 1;
        compilerBarrier();
        request.request_seq = seq;
        _ = signalEndpoint(self.server_endpoint_id);
        return seq;
    }

    fn finishRequestOk(self: *Client, expected_seq: u64, expected_op: net_protocol.Opcode) Error!*volatile net_protocol.ResponseHeader {
        if (!self.waitForResponse(expected_seq)) return error.Timeout;
        const response = self.responseHeader();
        if (response.magic != net_protocol.response_magic or response.version != net_protocol.version) return error.InvalidResponse;
        if (response.op != net_protocol.opcodeRaw(expected_op) or response.response_seq != expected_seq) return error.InvalidResponse;
        const status_value = std.meta.intToEnum(net_protocol.Status, response.status) catch return error.InvalidResponse;
        return switch (status_value) {
            .ok => response,
            .invalid => error.Invalid,
            .not_connected => error.EndpointNotFound,
        };
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

const ConnectPages = struct {
    request_va: u64,
    response_va: u64,
    request_paddr: u64,
    response_paddr: u64,
};

fn allocConnectPages(request_va: u64, response_va: u64) Error!ConnectPages {
    if (request_va == 0 and response_va == 0) {
        var paddrs: [2]u64 = .{ 0, 0 };
        const base_va = user_vm.allocMapPagesInto(2, true, paddrs[0..]) orelse return error.RequestAllocFailed;
        if (paddrs[0] < 0x1000 or paddrs[1] < 0x1000) return error.RequestAllocFailed;
        return .{
            .request_va = @intCast(base_va),
            .response_va = @intCast(base_va + user_vm.page_bytes),
            .request_paddr = paddrs[0],
            .response_paddr = paddrs[1],
        };
    }
    return error.Invalid;
}

fn makeSessionNonce(request_paddr: u64, response_paddr: u64, endpoint_id: u64, tag: u64) u64 {
    const nonce = request_paddr ^ ((response_paddr << 17) | (response_paddr >> 47)) ^ ((endpoint_id << 7) | (endpoint_id >> 57)) ^ tag ^ 0x6e65_742d_7374_6174;
    return if (nonce == 0) 1 else nonce;
}

fn grantResponseCapForConnect(paddr: u64, options: ConnectOptions, compat_installed: *bool) Error!void {
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

fn shareConnectRequest(paddr: u64, options: ConnectOptions, compat_installed: *bool) Error!void {
    const result = shareCap(paddr, options.endpoint_id);
    if (result == syscall_ok) return;
    if (result != syscall_err_endpoint) return error.ConnectSendFailed;

    if (!compat_installed.* and try attemptCompatEndpointInstall(options)) {
        compat_installed.* = true;
        const retry = shareCap(paddr, options.endpoint_id);
        if (retry == syscall_ok) return;
        if (retry == syscall_err_endpoint) return error.EndpointNotFound;
        return error.ConnectSendFailed;
    }
    return error.EndpointNotFound;
}

fn attemptCompatEndpointInstall(options: ConnectOptions) Error!bool {
    if (!options.allow_process_slot_compat or options.compat_process_slot == 0) return false;
    _ = userLog("net_client: process-slot compat fallback\n");
    return switch (installEndpoint(options.endpoint_id, options.compat_process_slot)) {
        syscall_ok => true,
        else => error.EndpointInstallFailed,
    };
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
        error.ConnectSendFailed,
        => {},
        else => return,
    }
    var buf: [192]u8 = undefined;
    const line = std.fmt.bufPrint(
        &buf,
        "net_client: registry=0x{x} ep=0x{x} slot={} flags=0x{x} compat={} allow={} err={s}\n",
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

fn clearPage(base_va: u64) void {
    const ptr: [*]volatile u64 = @ptrFromInt(base_va);
    var i: usize = 0;
    while (i < 512) : (i += 1) ptr[i] = 0;
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

fn signalEndpoint(endpoint_id: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_signal_endpoint),
          [arg0] "{rdi}" (endpoint_id),
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

fn userLog(message: []const u8) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_log),
          [arg0] "{rdi}" (@as(u64, @intFromPtr(message.ptr))),
          [arg1] "{rsi}" (@as(u64, @intCast(message.len))),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn compilerBarrier() void {
    asm volatile ("" ::: .{ .memory = true });
}
