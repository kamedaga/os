const std = @import("std");
const fs_abi = @import("fs_abi.zig");
const fs_protocol = @import("fs_protocol.zig");
const image_abi = @import("image_abi.zig");
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
const default_response_poll_limit: u64 = 256;

pub const Error = error{
    RequestAllocFailed,
    RequestMapFailed,
    ResponseAllocFailed,
    ResponseMapFailed,
    EndpointNotFound,
    EndpointInstallFailed,
    ResponseGrantFailed,
    ConnectSendFailed,
    Timeout,
    PathTooLong,
    InvalidResponse,
    BufferTooSmall,
    Invalid,
    NotFound,
    NotDir,
    IsDir,
    NoRight,
    TooBig,
    NotSupported,
    IoError,
    Busy,
};

pub const ConnectOptions = struct {
    request_va: u64,
    response_va: u64,
    client_process_slot: u64,
    endpoint_id: u64,
    response_poll_limit: u64 = default_response_poll_limit,
    compat_process_slot: u64 = 0,
    allow_process_slot_compat: bool = false,
};

pub const RegistryConnectOptions = struct {
    response_poll_limit: u64 = default_response_poll_limit,
    allow_process_slot_compat: bool = true,
};

pub const LookupResult = struct {
    token: u64,
    object_kind: fs_abi.ObjectKind,
    file_bytes: u64,
};

pub const OpenResult = struct {
    token: u64,
    file_bytes: u64,
};

pub const StatResult = struct {
    object_kind: fs_abi.ObjectKind,
    size_bytes: u64,
    mode_bits: u32,
    mtime_unix_sec: u64,
};

pub const DirEntry = struct {
    next_cursor: u64,
    object_kind: fs_abi.ObjectKind,
    name: []u8,
};

pub const ReaddirResult = union(enum) {
    entry: DirEntry,
    end,
};

pub const ReadResult = struct {
    bytes_read: usize,
    file_bytes: u64,
    next_offset: u64,
};

pub const Client = struct {
    request_va: u64,
    response_va: u64,
    request_paddr: u64,
    response_paddr: u64,
    server_endpoint_id: u64,
    mount_token: u64,
    next_seq: u64 = 2,
    response_poll_limit: u64 = default_response_poll_limit,

    pub fn connect(options: ConnectOptions) Error!Client {
        const request_paddr = allocPage();
        if (request_paddr < 0x1000) return error.RequestAllocFailed;
        if (mapPage(options.request_va, request_paddr, true) != 0) return error.RequestMapFailed;

        const response_paddr = allocPage();
        if (response_paddr < 0x1000) return error.ResponseAllocFailed;
        if (mapPage(options.response_va, response_paddr, true) != 0) return error.ResponseMapFailed;
        var compat_installed = false;
        try grantResponseCapForConnect(response_paddr, options, &compat_installed);

        var client = Client{
            .request_va = options.request_va,
            .response_va = options.response_va,
            .request_paddr = request_paddr,
            .response_paddr = response_paddr,
            .server_endpoint_id = options.endpoint_id,
            .mount_token = 0,
            .response_poll_limit = options.response_poll_limit,
        };

        client.clearMappedPages();
        const request = client.requestHeader();
        request.magic = fs_protocol.request_magic;
        request.version = fs_protocol.version;
        request.op = fs_protocol.opcodeRaw(.connect);
        request.object_token = 0;
        request.offset = 0;
        request.length = 0;
        request.flags = 0;
        request.path_bytes = 0;
        request.inline_bytes = 0;
        request.reserved0 = 0;
        request.arg0 = response_paddr;
        request.arg1 = options.client_process_slot;
        compilerBarrier();
        request.request_seq = 1;

        try shareConnectRequest(request_paddr, options, &compat_installed);
        const response = try client.finishRequestOk(1, .connect);
        client.mount_token = response.result_token;
        if (!fs_abi.isCapToken(client.mount_token)) return error.InvalidResponse;
        if (parseObjectKind(response.object_kind) != .mount) return error.InvalidResponse;
        return client;
    }

    pub fn connectFromRegistryPageKindOptions(
        registry_page_va: u64,
        service_kind: service_registry_abi.ServiceKind,
        request_va: u64,
        response_va: u64,
        client_process_slot: u64,
        options: RegistryConnectOptions,
    ) Error!Client {
        const entry = service_registry_abi.findService(registry_page_va, service_kind) orelse return error.NotFound;
        if (entry.endpoint_id == 0) return error.Invalid;
        const compat_allowed = service_registry_abi.allowsProcessSlotCompat(entry);
        return connect(.{
            .request_va = request_va,
            .response_va = response_va,
            .client_process_slot = client_process_slot,
            .endpoint_id = entry.endpoint_id,
            .response_poll_limit = options.response_poll_limit,
            .compat_process_slot = if (compat_allowed) entry.process_slot else 0,
            .allow_process_slot_compat = options.allow_process_slot_compat and compat_allowed,
        });
    }

    pub fn connectFromRegistryPageKind(
        registry_page_va: u64,
        service_kind: service_registry_abi.ServiceKind,
        request_va: u64,
        response_va: u64,
        client_process_slot: u64,
    ) Error!Client {
        return connectFromRegistryPageKindOptions(registry_page_va, service_kind, request_va, response_va, client_process_slot, .{});
    }

    pub fn connectFromRegistryPage(registry_page_va: u64, request_va: u64, response_va: u64, client_process_slot: u64) Error!Client {
        return connectFromRegistryPageKind(registry_page_va, .persistent_fs, request_va, response_va, client_process_slot);
    }

    pub fn connectFromServiceRegistryKindOptions(
        service_kind: service_registry_abi.ServiceKind,
        request_va: u64,
        response_va: u64,
        client_process_slot: u64,
        options: RegistryConnectOptions,
    ) Error!Client {
        return connectFromRegistryPageKindOptions(service_registry_abi.page_va, service_kind, request_va, response_va, client_process_slot, options);
    }

    pub fn connectFromServiceRegistryKind(
        service_kind: service_registry_abi.ServiceKind,
        request_va: u64,
        response_va: u64,
        client_process_slot: u64,
    ) Error!Client {
        return connectFromServiceRegistryKindOptions(service_kind, request_va, response_va, client_process_slot, .{});
    }

    pub fn connectFromServiceRegistry(request_va: u64, response_va: u64, client_process_slot: u64) Error!Client {
        return connectFromServiceRegistryKind(.persistent_fs, request_va, response_va, client_process_slot);
    }

    pub fn lookup(self: *Client, object_token: u64, path: []const u8) Error!LookupResult {
        const seq = try self.beginRequest(.lookup, object_token, 0, 0, 0, path, &[_]u8{});
        const response = try self.finishRequestOk(seq, .lookup);
        return self.lookupResultFromResponse(response);
    }

    pub fn stat(self: *Client, object_token: u64) Error!StatResult {
        const seq = try self.beginRequest(.stat, object_token, 0, 0, 0, "", &[_]u8{});
        const response = try self.finishRequestOk(seq, .stat);
        if (response.inline_bytes < fs_protocol.stat_record_bytes) return error.InvalidResponse;
        const stat_record: *volatile fs_protocol.FsStatRecord = @ptrFromInt(self.response_va + fs_protocol.response_header_bytes);
        return .{
            .object_kind = parseObjectKind(stat_record.object_kind) orelse return error.InvalidResponse,
            .size_bytes = stat_record.size_bytes,
            .mode_bits = stat_record.mode_bits,
            .mtime_unix_sec = stat_record.mtime_unix_sec,
        };
    }

    pub fn readdirOne(self: *Client, dir_token: u64, cursor: u64, name_buf: []u8) Error!ReaddirResult {
        const seq = try self.beginRequest(.readdir, dir_token, cursor, 1, 0, "", &[_]u8{});
        const response = try self.finishRequest(seq, .readdir);
        const status = parseStatus(response.status) orelse return error.InvalidResponse;
        switch (status) {
            .ok => {},
            .end_of_dir => return .end,
            else => return statusToError(status),
        }
        if (response.inline_bytes < fs_protocol.dirent_record_bytes) return error.InvalidResponse;
        const record: *volatile fs_protocol.FsDirentRecord = @ptrFromInt(self.response_va + fs_protocol.response_header_bytes);
        const name_len: usize = record.name_bytes;
        if (name_len > name_buf.len) return error.BufferTooSmall;
        const needed = fs_protocol.dirent_record_bytes + name_len;
        if (needed > response.inline_bytes) return error.InvalidResponse;
        copyVolatileBytes(self.responsePayload() + fs_protocol.dirent_record_bytes, name_buf[0..name_len]);
        return .{
            .entry = .{
                .next_cursor = record.next_cursor,
                .object_kind = parseObjectKind(record.object_kind) orelse return error.InvalidResponse,
                .name = name_buf[0..name_len],
            },
        };
    }

    pub fn create(self: *Client, dir_token: u64, path: []const u8) Error!LookupResult {
        return self.createWithFlags(dir_token, path, 0);
    }

    pub fn createDir(self: *Client, dir_token: u64, path: []const u8) Error!LookupResult {
        return self.createWithFlags(dir_token, path, fs_protocol.create_flag_directory);
    }

    pub fn createWithFlags(self: *Client, dir_token: u64, path: []const u8, flags: u32) Error!LookupResult {
        const seq = try self.beginRequest(.create, dir_token, 0, 0, flags, path, &[_]u8{});
        const response = try self.finishRequestOk(seq, .create);
        return self.lookupResultFromResponse(response);
    }

    pub fn unlink(self: *Client, dir_token: u64, path: []const u8) Error!void {
        const seq = try self.beginRequest(.unlink, dir_token, 0, 0, 0, path, &[_]u8{});
        _ = try self.finishRequestOk(seq, .unlink);
    }

    pub fn rename(self: *Client, dir_token: u64, old_path: []const u8, new_path: []const u8) Error!void {
        const seq = try self.beginRequest(.rename, dir_token, 0, 0, 0, old_path, new_path);
        _ = try self.finishRequestOk(seq, .rename);
    }

    pub fn open(self: *Client, vnode_file_token: u64) Error!OpenResult {
        const seq = try self.beginRequest(.open, vnode_file_token, 0, 0, 0, "", &[_]u8{});
        const response = try self.finishRequestOk(seq, .open);
        return self.openResultFromResponse(response, .open_file);
    }

    pub fn openExec(self: *Client, vnode_file_token: u64) Error!OpenResult {
        const seq = try self.beginRequest(.open_exec, vnode_file_token, 0, 0, 0, "", &[_]u8{});
        const response = try self.finishRequestOk(seq, .open_exec);
        return self.openResultFromResponse(response, .exec);
    }

    pub fn read(self: *Client, open_file_token: u64, offset: u64, out: []u8) Error!ReadResult {
        const request_len: usize = @min(out.len, fs_protocol.response_payload_bytes);
        const seq = try self.beginRequest(.read, open_file_token, offset, @intCast(request_len), 0, "", &[_]u8{});
        const response = try self.finishRequestOk(seq, .read);
        const inline_bytes: usize = response.inline_bytes;
        if (inline_bytes > out.len or inline_bytes > fs_protocol.response_payload_bytes) return error.BufferTooSmall;
        copyVolatileBytes(self.responsePayload(), out[0..inline_bytes]);
        return .{
            .bytes_read = inline_bytes,
            .file_bytes = response.file_bytes,
            .next_offset = response.cursor_next,
        };
    }

    pub fn write(self: *Client, open_file_token: u64, offset: u64, bytes: []const u8) Error!u64 {
        if (bytes.len > fs_protocol.request_payload_bytes) return error.BufferTooSmall;
        const seq = try self.beginRequest(.write, open_file_token, offset, @intCast(bytes.len), 0, "", bytes);
        const response = try self.finishRequestOk(seq, .write);
        return response.file_bytes;
    }

    pub fn close(self: *Client, object_token: u64) Error!void {
        const seq = try self.beginRequest(.close, object_token, 0, 0, 0, "", &[_]u8{});
        _ = try self.finishRequestOk(seq, .close);
    }

    fn requestHeader(self: *const Client) *volatile fs_protocol.FsRequestHeader {
        return @ptrFromInt(self.request_va);
    }

    fn responseHeader(self: *const Client) *volatile fs_protocol.FsResponseHeader {
        return @ptrFromInt(self.response_va);
    }

    fn requestPayload(self: *const Client) [*]volatile u8 {
        return @ptrFromInt(self.request_va + fs_protocol.request_header_bytes);
    }

    fn responsePayload(self: *const Client) [*]volatile u8 {
        return @ptrFromInt(self.response_va + fs_protocol.response_header_bytes);
    }

    fn clearMappedPages(self: *const Client) void {
        clearPage(self.request_va);
        clearPage(self.response_va);
    }

    fn beginRequest(
        self: *Client,
        op: fs_protocol.Opcode,
        object_token: u64,
        offset: u64,
        length: u32,
        flags: u32,
        path: []const u8,
        inline_payload: []const u8,
    ) Error!u64 {
        if (path.len > fs_protocol.request_payload_bytes) return error.PathTooLong;
        if (inline_payload.len > fs_protocol.request_payload_bytes) return error.BufferTooSmall;
        if (path.len + inline_payload.len > fs_protocol.request_payload_bytes) return error.BufferTooSmall;
        self.clearMappedPages();
        const request = self.requestHeader();
        request.magic = fs_protocol.request_magic;
        request.version = fs_protocol.version;
        request.op = fs_protocol.opcodeRaw(op);
        request.object_token = object_token;
        request.offset = offset;
        request.length = length;
        request.flags = flags;
        request.path_bytes = @intCast(path.len);
        request.inline_bytes = @intCast(inline_payload.len);
        request.reserved0 = 0;
        request.arg0 = 0;
        request.arg1 = 0;
        if (path.len != 0) copyBytesToVolatile(self.requestPayload(), path);
        if (inline_payload.len != 0) copyBytesToVolatile(self.requestPayload() + path.len, inline_payload);
        const seq = self.next_seq;
        self.next_seq += 1;
        compilerBarrier();
        request.request_seq = seq;
        _ = signalEndpoint(self.server_endpoint_id);
        return seq;
    }

    fn finishRequest(self: *Client, expected_seq: u64, expected_op: fs_protocol.Opcode) Error!*volatile fs_protocol.FsResponseHeader {
        if (!self.waitForResponse(expected_seq)) return error.Timeout;
        const response = self.responseHeader();
        if (response.magic != fs_protocol.response_magic or response.version != fs_protocol.version) {
            return error.InvalidResponse;
        }
        if (response.op != fs_protocol.opcodeRaw(expected_op) or response.response_seq != expected_seq) {
            return error.InvalidResponse;
        }
        _ = parseStatus(response.status) orelse return error.InvalidResponse;
        return response;
    }

    fn finishRequestOk(self: *Client, expected_seq: u64, expected_op: fs_protocol.Opcode) Error!*volatile fs_protocol.FsResponseHeader {
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

    fn lookupResultFromResponse(self: *Client, response: *volatile fs_protocol.FsResponseHeader) Error!LookupResult {
        _ = self;
        if (!fs_abi.isCapToken(response.result_token)) return error.InvalidResponse;
        return .{
            .token = response.result_token,
            .object_kind = parseObjectKind(response.object_kind) orelse return error.InvalidResponse,
            .file_bytes = response.file_bytes,
        };
    }

    fn openResultFromResponse(self: *Client, response: *volatile fs_protocol.FsResponseHeader, expected_kind: fs_abi.ObjectKind) Error!OpenResult {
        _ = self;
        const object_kind = parseObjectKind(response.object_kind) orelse return error.InvalidResponse;
        if (object_kind != expected_kind) return error.InvalidResponse;
        switch (expected_kind) {
            .open_file => if (!fs_abi.isCapToken(response.result_token)) return error.InvalidResponse,
            .exec => if (image_abi.decodeExecImageToken(response.result_token) == null) return error.InvalidResponse,
            else => return error.InvalidResponse,
        }
        return .{
            .token = response.result_token,
            .file_bytes = response.file_bytes,
        };
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

fn waitEvent(wait_mailbox: bool, timeout_ticks: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_wait_event),
          [arg0] "{rdi}" (@as(u64, if (wait_mailbox) 1 else 0)),
          [arg1] "{rsi}" (timeout_ticks),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn grantCapOnEndpoint(paddr: u64, endpoint_id: u64, rights_bits: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_grant_cap_on_endpoint),
          [arg0] "{rdi}" (paddr),
          [arg1] "{rsi}" (endpoint_id),
          [arg2] "{rdx}" (rights_bits),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
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
    _ = userLog("fs_client: process-slot compat fallback\n");
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

fn userLog(message: []const u8) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_log),
          [arg0] "{rdi}" (@as(u64, @intFromPtr(message.ptr))),
          [arg1] "{rsi}" (@as(u64, @intCast(message.len))),
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

fn signalEndpoint(endpoint_id: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_signal_endpoint),
          [arg0] "{rdi}" (endpoint_id),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn compilerBarrier() void {
    asm volatile ("" ::: .{ .memory = true });
}

fn clearPage(va: u64) void {
    const bytes: [*]volatile u8 = @ptrFromInt(va);
    var i: usize = 0;
    while (i < fs_protocol.page_bytes) : (i += 1) {
        bytes[i] = 0;
    }
}

fn copyBytesToVolatile(dst: [*]volatile u8, src: []const u8) void {
    for (src, 0..) |byte, i| {
        dst[i] = byte;
    }
}

fn copyVolatileBytes(src: [*]volatile u8, dst: []u8) void {
    for (dst, 0..) |*byte, i| {
        byte.* = src[i];
    }
}

fn parseStatus(raw: i32) ?fs_protocol.Status {
    return std.meta.intToEnum(fs_protocol.Status, raw) catch null;
}

fn parseObjectKind(raw: u8) ?fs_abi.ObjectKind {
    return std.meta.intToEnum(fs_abi.ObjectKind, raw) catch null;
}

fn statusToError(status: fs_protocol.Status) Error {
    return switch (status) {
        .ok => error.InvalidResponse,
        .invalid => error.Invalid,
        .not_found => error.NotFound,
        .not_dir => error.NotDir,
        .is_dir => error.IsDir,
        .no_right => error.NoRight,
        .too_big => error.TooBig,
        .not_supported => error.NotSupported,
        .io_error => error.IoError,
        .busy => error.Busy,
        .end_of_dir => error.InvalidResponse,
    };
}
