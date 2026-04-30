const support = @import("abi_root");
const bootfs_format = support.bootfs_format;
const dynamic_linker_bootstrap_abi = support.dynamic_linker_bootstrap_abi;
const exec_loader_bootstrap_abi = support.exec_loader_bootstrap_abi;
const fs_abi = support.fs_abi;
const fs_client = support.fs_client;
const image_abi = support.image_abi;
const process_abi = support.process_abi;
const process_builder_abi = support.process_builder_abi;
const trap_abi = support.trap_abi;
const user_vm = support.user_vm;

const syscall_log: u64 = 0x9;
const syscall_ok: u64 = 0;
const syscall_alloc_map_pages: u64 = 0xC;
const syscall_get_process_slot: u64 = 0x2E;

const main_source_map_va: u64 = 0x2800_0000;
const interpreter_source_map_va: u64 = 0x2900_0000;
const bootfs_source_map_va: u64 = 0x2A00_0000;
const lib_source_map_va: u64 = 0x2B00_0000;
const page_bytes: u64 = 4096;
const stdio_sink_target_va: u64 = 0x3C02_2000;
const stdio_sink_magic: u64 = 0x5354_4449_4F53_4831;
const stdio_sink_version: u64 = 1;
const stdio_mode_kernel_log: u64 = 1;
const stdio_log_mode_shift: u6 = 0;
const stdio_stdout_mode_shift: u6 = 2;
const stdio_stderr_mode_shift: u6 = 4;
const process_exit_status_target_va: u64 = 0x3C02_3000;
const process_exit_status_magic: u64 = 0x5052_5845_5449_5431;
const process_exit_status_version: u64 = 1;
const process_exit_status_state_idle: u64 = 0;
const elf_header_bytes: usize = 64;
const elf_phdr_bytes: u16 = 56;
const elf_magic0: u8 = 0x7f;
const elf_magic1: u8 = 'E';
const elf_magic2: u8 = 'L';
const elf_magic3: u8 = 'F';
const elf_class_64: u8 = 2;
const elf_data_lsb: u8 = 1;
const elf_version_current: u8 = 1;
const elf_type_exec: u16 = 2;
const elf_type_dyn: u16 = 3;
const elf_machine_x86_64: u16 = 0x3e;
const user_load_min_va: u64 = 0x2000_0000;
const dynamic_reserved_high_base_va: u64 = 0x3B00_0000;
const dynamic_reserved_high_end_va: u64 = 0x3D00_0000;
const et_dyn_alloc_start_va: u64 = user_load_min_va;
const et_dyn_alloc_end_va: u64 = dynamic_reserved_high_base_va;
const max_vm_layout_ranges: usize = 16;
const max_child_mapped_pages: usize = 2048;
const pt_load: u32 = 1;
const pt_dynamic: u32 = 2;
const pt_interp: u32 = 3;
const pt_tls: u32 = 7;
const pt_gnu_relro: u32 = 0x6474_e552;
const interp_path_ld = "/lib/ld.so";
const pf_x: u32 = 1 << 0;
const pf_w: u32 = 1 << 1;
const pf_r: u32 = 1 << 2;
const dt_null: i64 = 0;
const dt_needed: i64 = 1;
const dt_strtab: i64 = 5;
const dt_strsz: i64 = 10;
const dt_rela: i64 = 7;
const dt_relasz: i64 = 8;
const dt_relaent: i64 = 9;
const dt_soname: i64 = 14;
const dt_rpath: i64 = 15;
const dt_runpath: i64 = 29;
const max_needed_count: usize = dynamic_linker_bootstrap_abi.max_loaded_libs;
const max_lib_name_bytes: usize = 192;
const max_lib_path_bytes: usize = 256;
const max_lib_image_bytes: usize = 2 * 1024 * 1024;
const elf_dyn_bytes: usize = 16;
const max_lib_queue_depth: usize = max_needed_count;
const elf_rela_bytes: u64 = 24;
const r_x86_64_relative: u64 = 8;

var lib_image_scratch: [max_lib_image_bytes]u8 align(4096) = undefined;
var needed_queue_scratch: [max_lib_queue_depth]NeededPath = undefined;
var loaded_path_scratch: [max_lib_queue_depth]NeededPath = undefined;
var loaded_soname_scratch: [max_lib_queue_depth]NeededPath = undefined;

const NeededPath = struct {
    bytes: [max_lib_path_bytes]u8,
    len: usize,
};

const LoadedImage = struct {
    ehdr: ElfHeader,
    load_bias: u64,
    interp_phdr: ?ProgramHeader,
    dynamic_phdr: ?ProgramHeader,
    tls_phdr: ?ProgramHeader,
    relro_phdr: ?ProgramHeader,
};

const ElfHeader = struct {
    elf_type: u16,
    entry: u64,
    phoff: u64,
    phentsize: u16,
    phnum: u16,
};

const ProgramHeader = struct {
    p_type: u32,
    flags: u32,
    offset: u64,
    vaddr: u64,
    filesz: u64,
    memsz: u64,
    align_bytes: u64,
};

const ImageSpan = struct {
    min_vaddr: u64,
    max_vaddr: u64,
    align_bytes: u64,
};

const ReservedRange = struct {
    start: u64,
    end: u64,
};

const dynamic_reserved_ranges = [_]ReservedRange{
    .{ .start = 0, .end = user_load_min_va },
    .{ .start = dynamic_reserved_high_base_va, .end = dynamic_reserved_high_end_va },
};

const VmLayout = struct {
    ranges: [max_vm_layout_ranges]ReservedRange = [_]ReservedRange{.{ .start = 0, .end = 0 }} ** max_vm_layout_ranges,
    range_count: usize = 0,

    fn initForExecChild() ?VmLayout {
        var layout = VmLayout{};
        for (dynamic_reserved_ranges) |reserved| {
            if (!layout.reserve(reserved.start, reserved.end)) return null;
        }
        return layout;
    }

    fn reserveBootFsImage(self: *VmLayout, byte_count: u64) bool {
        if (byte_count == 0) return true;
        const bytes = pageUp(byte_count) orelse return false;
        const end, const overflow = @addWithOverflow(dynamic_linker_bootstrap_abi.bootfs_image_target_va, bytes);
        if (overflow != 0 or end > dynamic_reserved_high_base_va) return false;
        return self.reserve(dynamic_linker_bootstrap_abi.bootfs_image_target_va, end);
    }

    fn reserve(self: *VmLayout, start: u64, end: u64) bool {
        if (end <= start) return false;
        if (self.range_count >= self.ranges.len) return false;
        for (self.ranges[0..self.range_count]) |range| {
            if (rangesOverlap(start, end, range.start, range.end)) return false;
        }
        self.ranges[self.range_count] = .{ .start = start, .end = end };
        self.range_count += 1;
        return true;
    }

    fn firstOverlap(self: *const VmLayout, start: u64, end: u64) ?ReservedRange {
        for (self.ranges[0..self.range_count]) |range| {
            if (rangesOverlap(start, end, range.start, range.end)) return range;
        }
        return null;
    }

    fn allocateEtDynImage(self: *VmLayout, span: ImageSpan) ?u64 {
        var load_bias = alignUp(et_dyn_alloc_start_va, span.align_bytes) orelse return null;
        while (true) {
            const start, const start_overflow = @addWithOverflow(load_bias, span.min_vaddr);
            if (start_overflow != 0) return null;
            const end, const end_overflow = @addWithOverflow(load_bias, span.max_vaddr);
            if (end_overflow != 0) return null;
            if (end <= start or end > et_dyn_alloc_end_va) return null;

            if (self.firstOverlap(start, end)) |overlap| {
                const next_bias_min = if (overlap.end > span.min_vaddr) overlap.end - span.min_vaddr else overlap.end;
                if (next_bias_min <= load_bias) return null;
                load_bias = alignUp(next_bias_min, span.align_bytes) orelse return null;
                continue;
            }

            if (!self.reserve(start, end)) return null;
            return load_bias;
        }
    }
};

const ChildPageTracker = struct {
    pages: [max_child_mapped_pages]u64 = [_]u64{0} ** max_child_mapped_pages,
    page_count: usize = 0,

    fn contains(self: *const ChildPageTracker, page_va: u64) bool {
        var index: usize = 0;
        while (index < self.page_count) : (index += 1) {
            if (self.pages[index] == page_va) return true;
        }
        return false;
    }

    fn add(self: *ChildPageTracker, page_va: u64) bool {
        if (self.contains(page_va)) return true;
        if (self.page_count >= self.pages.len) return false;
        self.pages[self.page_count] = page_va;
        self.page_count += 1;
        return true;
    }

    fn addRange(self: *ChildPageTracker, start_va: u64, page_count: u64) bool {
        var index: u64 = 0;
        while (index < page_count) : (index += 1) {
            const page_va, const overflow = @addWithOverflow(start_va, index * page_bytes);
            if (overflow != 0) return false;
            if (!self.add(page_va)) return false;
        }
        return true;
    }
};

var child_mapped_pages = ChildPageTracker{};

fn syscall1(nr: u64, arg0: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (nr),
          [arg0] "{rdi}" (arg0),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn syscall3(nr: u64, arg0: u64, arg1: u64, arg2: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (nr),
          [arg0] "{rdi}" (arg0),
          [arg1] "{rsi}" (arg1),
          [arg2] "{rdx}" (arg2),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn syscall4(nr: u64, arg0: u64, arg1: u64, arg2: u64, arg3: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (nr),
          [arg0] "{rdi}" (arg0),
          [arg1] "{rsi}" (arg1),
          [arg2] "{rdx}" (arg2),
          [arg3] "{rcx}" (arg3),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn syscall5(nr: u64, arg0: u64, arg1: u64, arg2: u64, arg3: u64, arg4: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (nr),
          [arg0] "{rdi}" (arg0),
          [arg1] "{rsi}" (arg1),
          [arg2] "{rdx}" (arg2),
          [arg3] "{rcx}" (arg3),
          [arg4] "{r8}" (arg4),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn userLog(message: []const u8) void {
    _ = syscall3(syscall_log, @intFromPtr(message.ptr), message.len, 0);
}

fn getProcessSlot() u64 {
    return syscall1(syscall_get_process_slot, 0);
}

fn allocMapPagesSelf(base_va: u64, page_count: u64, writable: bool) u64 {
    return syscall4(syscall_alloc_map_pages, base_va, page_count, if (writable) 1 else 0, 0);
}

fn mapStaticScratchBuffer(base_va: u64, byte_count: usize) void {
    const page_count = (byte_count + user_vm.page_bytes - 1) / user_vm.page_bytes;
    var page_index: usize = 0;
    while (page_index < page_count) : (page_index += 1) {
        const page_va = base_va + @as(u64, @intCast(page_index * user_vm.page_bytes));
        _ = allocMapPagesSelf(page_va, 1, true);
    }
}

fn mapExecLoaderScratch() void {
    mapStaticScratchBuffer(@intFromPtr(&lib_image_scratch), @sizeOf(@TypeOf(lib_image_scratch)));
    mapStaticScratchBuffer(@intFromPtr(&needed_queue_scratch), @sizeOf(@TypeOf(needed_queue_scratch)));
    mapStaticScratchBuffer(@intFromPtr(&loaded_path_scratch), @sizeOf(@TypeOf(loaded_path_scratch)));
    mapStaticScratchBuffer(@intFromPtr(&loaded_soname_scratch), @sizeOf(@TypeOf(loaded_soname_scratch)));
    mapStaticScratchBuffer(@intFromPtr(&child_mapped_pages), @sizeOf(@TypeOf(child_mapped_pages)));
}

fn connectRootFs(endpoint_id: u64, compat_process_slot: u64) ?struct { client: fs_client.Client, root_token: u64 } {
    if (endpoint_id == 0) {
        userLog("ExecLoader: fs endpoint absent\n");
        return null;
    }
    const process_slot = getProcessSlot();
    if (process_slot == 0) {
        userLog("ExecLoader: process slot unavailable\n");
        return null;
    }
    const ipc_va = user_vm.reservePages(2) orelse {
        userLog("ExecLoader: IPC VA reserve failed\n");
        return null;
    };
    var client = fs_client.Client.connect(.{
        .request_va = @intCast(ipc_va),
        .response_va = @intCast(ipc_va + user_vm.page_bytes),
        .client_process_slot = process_slot,
        .endpoint_id = endpoint_id,
        .compat_process_slot = compat_process_slot,
        .allow_process_slot_compat = compat_process_slot != 0,
    }) catch {
        userLog("ExecLoader: persistent fs connect failed\n");
        return null;
    };
    const root = client.lookup(client.mount_token, ".") catch {
        userLog("ExecLoader: root lookup failed\n");
        return null;
    };
    if (root.object_kind != fs_abi.ObjectKind.vnode_dir) {
        userLog("ExecLoader: root lookup returned non-dir\n");
        return null;
    }
    return .{ .client = client, .root_token = root.token };
}

fn rootFsFileExists(client: *fs_client.Client, root_token: u64, path: []const u8) bool {
    const lookup = client.lookup(root_token, path) catch return false;
    return lookup.object_kind == fs_abi.ObjectKind.vnode_file;
}

fn readRootFsFile(client: *fs_client.Client, root_token: u64, path: []const u8, out: []u8) ?usize {
    const lookup = client.lookup(root_token, path) catch return null;
    if (lookup.object_kind != fs_abi.ObjectKind.vnode_file) return null;
    const open_file = client.open(lookup.token) catch return null;
    defer client.close(open_file.token) catch {};
    if (open_file.file_bytes == 0 or open_file.file_bytes > out.len) return null;

    const file_bytes: usize = @intCast(open_file.file_bytes);
    var offset: usize = 0;
    while (offset < file_bytes) {
        const read_result = client.read(open_file.token, offset, out[offset..file_bytes]) catch return null;
        if (read_result.bytes_read == 0) return null;
        offset += read_result.bytes_read;
    }
    return file_bytes;
}

fn sameBytes(a: []const u8, b: []const u8) bool {
    if (a.len != b.len) return false;
    var index: usize = 0;
    while (index < a.len) : (index += 1) {
        if (a[index] != b[index]) return false;
    }
    return true;
}

fn mapVmObject(token: u64, target_va: u64) bool {
    return syscall3(image_abi.syscall_map_vm_object, token, target_va, 0) == syscall_ok;
}

fn sliceVmObject(token: u64, offset_bytes: u64, size_bytes: u64, rights: image_abi.VmObjectRights) ?u64 {
    const out = syscall4(
        image_abi.syscall_slice_vm_object,
        token,
        offset_bytes,
        size_bytes,
        image_abi.vmObjectRightsToBits(rights),
    );
    return if (image_abi.decodeVmObjectToken(out) != null) out else null;
}

fn installVmObject(base_va: u64, size_bytes: u64, rights: image_abi.VmObjectRights) ?u64 {
    const out = syscall3(
        image_abi.syscall_install_vm_object,
        base_va,
        size_bytes,
        image_abi.vmObjectRightsToBits(rights),
    );
    return if (image_abi.decodeVmObjectToken(out) != null) out else null;
}

fn createSuspendedProcess() ?u64 {
    const token = syscall1(process_builder_abi.syscall_create_suspended_process, 0);
    return if (process_builder_abi.decodeProcessBuilderToken(token) != null) token else null;
}

fn mapVmObjectToProcess(process_token: u64, vm_token: u64, target_va: u64, prot: process_builder_abi.MapProt) bool {
    return syscall4(
        process_builder_abi.syscall_map_vm_object_to_process,
        process_token,
        vm_token,
        target_va,
        process_builder_abi.mapProtToBits(prot),
    ) == syscall_ok;
}

fn allocMapPagesToProcess(process_token: u64, target_va: u64, page_count: u64, prot: process_builder_abi.MapProt) bool {
    return syscall5(
        process_builder_abi.syscall_alloc_map_pages_to_process,
        process_token,
        target_va,
        page_count,
        process_builder_abi.mapProtToBits(prot),
        0,
    ) == syscall_ok;
}

fn copyToProcess(process_token: u64, dest_va: u64, src_va: u64, byte_len: u64) bool {
    return syscall4(
        process_builder_abi.syscall_copy_to_process,
        process_token,
        dest_va,
        src_va,
        byte_len,
    ) == syscall_ok;
}

fn setProcessInitialContext(process_token: u64, rip: u64, rsp: u64) bool {
    return syscall3(process_builder_abi.syscall_set_process_initial_context, process_token, rip, rsp) == syscall_ok;
}

fn startProcess(process_token: u64) ?u64 {
    const spawned = syscall1(process_builder_abi.syscall_start_process, process_token);
    return if (process_abi.decodeSpawnedProcessSlot(spawned) != null) spawned else null;
}

fn setProcessAbiTrapDelegate(process_token: u64, endpoint_id: u64, target_process_slot: u64, flavor: u64) bool {
    return syscall4(process_builder_abi.syscall_set_process_abi_trap_delegate, process_token, endpoint_id, target_process_slot, flavor) == syscall_ok;
}

fn abortProcess(process_token: u64) void {
    _ = syscall1(process_builder_abi.syscall_abort_process, process_token);
}

fn bytesAt(source_base_va: u64, off: u64, len: usize, file_bytes: u64) ?[]const u8 {
    if (off > file_bytes) return null;
    if (@as(u64, @intCast(len)) > file_bytes - off) return null;
    const ptr: [*]const u8 = @ptrFromInt(source_base_va + off);
    return ptr[0..len];
}

fn readU16Le(bytes: []const u8, off: usize) ?u16 {
    if (off + 2 > bytes.len) return null;
    return @as(u16, bytes[off]) | (@as(u16, bytes[off + 1]) << 8);
}

fn readU32Le(bytes: []const u8, off: usize) ?u32 {
    if (off + 4 > bytes.len) return null;
    return @as(u32, bytes[off]) |
        (@as(u32, bytes[off + 1]) << 8) |
        (@as(u32, bytes[off + 2]) << 16) |
        (@as(u32, bytes[off + 3]) << 24);
}

fn readU64Le(bytes: []const u8, off: usize) ?u64 {
    if (off + 8 > bytes.len) return null;
    var value: u64 = 0;
    var index: usize = 0;
    while (index < 8) : (index += 1) {
        value |= @as(u64, bytes[off + index]) << @intCast(index * 8);
    }
    return value;
}

fn readI64Le(bytes: []const u8, off: usize) ?i64 {
    return @bitCast(readU64Le(bytes, off) orelse return null);
}

fn parseElfHeader(source_base_va: u64, file_bytes: u64) ?ElfHeader {
    const ehdr = bytesAt(source_base_va, 0, elf_header_bytes, file_bytes) orelse return null;
    if (ehdr[0] != elf_magic0 or ehdr[1] != elf_magic1 or ehdr[2] != elf_magic2 or ehdr[3] != elf_magic3) return null;
    if (ehdr[4] != elf_class_64 or ehdr[5] != elf_data_lsb or ehdr[6] != elf_version_current) return null;
    const elf_type = readU16Le(ehdr, 16) orelse return null;
    if (elf_type != elf_type_exec and elf_type != elf_type_dyn) return null;
    if ((readU16Le(ehdr, 18) orelse return null) != elf_machine_x86_64) return null;
    if ((readU32Le(ehdr, 20) orelse return null) != elf_version_current) return null;
    return .{
        .elf_type = elf_type,
        .entry = readU64Le(ehdr, 24) orelse return null,
        .phoff = readU64Le(ehdr, 32) orelse return null,
        .phentsize = readU16Le(ehdr, 54) orelse return null,
        .phnum = readU16Le(ehdr, 56) orelse return null,
    };
}

fn parseProgramHeader(source_base_va: u64, ehdr: ElfHeader, index: u16, file_bytes: u64) ?ProgramHeader {
    if (ehdr.phentsize < elf_phdr_bytes) return null;
    const off = ehdr.phoff + @as(u64, index) * @as(u64, ehdr.phentsize);
    const phdr = bytesAt(source_base_va, off, elf_phdr_bytes, file_bytes) orelse return null;
    return .{
        .p_type = readU32Le(phdr, 0) orelse return null,
        .flags = readU32Le(phdr, 4) orelse return null,
        .offset = readU64Le(phdr, 8) orelse return null,
        .vaddr = readU64Le(phdr, 16) orelse return null,
        .filesz = readU64Le(phdr, 32) orelse return null,
        .memsz = readU64Le(phdr, 40) orelse return null,
        .align_bytes = readU64Le(phdr, 48) orelse return null,
    };
}

fn pageDown(value: u64) u64 {
    return value & ~(page_bytes - 1);
}

fn pageUp(value: u64) ?u64 {
    const plus, const overflow = @addWithOverflow(value, page_bytes - 1);
    if (overflow != 0) return null;
    return pageDown(plus);
}

fn alignUp(value: u64, alignment: u64) ?u64 {
    if (alignment == 0 or (alignment & (alignment - 1)) != 0) return null;
    const mask = alignment - 1;
    const plus, const overflow = @addWithOverflow(value, mask);
    if (overflow != 0) return null;
    return plus & ~mask;
}

fn addSigned(base: u64, addend: i64) ?u64 {
    if (addend >= 0) {
        const value, const overflow = @addWithOverflow(base, @as(u64, @intCast(addend)));
        return if (overflow == 0) value else null;
    }
    if (addend == -9223372036854775808) return null;
    const magnitude: u64 = @intCast(-addend);
    if (base < magnitude) return null;
    return base - magnitude;
}

fn writeProcessU64(process_token: u64, dest_va: u64, value: u64) bool {
    var bytes: [8]u8 = undefined;
    var index: usize = 0;
    while (index < bytes.len) : (index += 1) {
        bytes[index] = @intCast((value >> @intCast(index * 8)) & 0xff);
    }
    return copyToProcess(process_token, dest_va, @intFromPtr(&bytes), bytes.len);
}


fn bootfsLibPath(name: []const u8, out: *[max_lib_path_bytes]u8) ?[]const u8 {
    if (name.len == 0) return null;
    if (name[0] == '/') {
        if (name.len > out.len) return null;
        @memcpy(out[0..name.len], name);
        return out[0..name.len];
    }
    const prefix = "/lib/";
    if (prefix.len + name.len > out.len) return null;
    @memcpy(out[0..prefix.len], prefix);
    @memcpy(out[prefix.len .. prefix.len + name.len], name);
    return out[0 .. prefix.len + name.len];
}

fn copyPath(path: []const u8, out: *[max_lib_path_bytes]u8) ?[]const u8 {
    if (path.len == 0 or path.len > out.len) return null;
    @memcpy(out[0..path.len], path);
    return out[0..path.len];
}

fn originDir(path: []const u8) []const u8 {
    if (path.len == 0 or path[0] != '/') return "/";
    var index = path.len;
    while (index > 0) {
        index -= 1;
        if (path[index] == '/') {
            if (index == 0) return "/";
            return path[0..index];
        }
    }
    return "/";
}

fn appendPathComponent(out: *[max_lib_path_bytes]u8, len: *usize, bytes: []const u8) bool {
    if (len.* + bytes.len > out.len) return false;
    @memcpy(out[len.* .. len.* + bytes.len], bytes);
    len.* += bytes.len;
    return true;
}

fn pathFromSearchDir(dir: []const u8, origin: []const u8, name: []const u8, out: *[max_lib_path_bytes]u8) ?[]const u8 {
    if (dir.len == 0 or name.len == 0) return null;
    var len: usize = 0;
    const origin_token = "$ORIGIN";
    if (dir.len >= origin_token.len and sameBytes(dir[0..origin_token.len], origin_token)) {
        if (!appendPathComponent(out, &len, origin)) return null;
        if (dir.len > origin_token.len) {
            if (!appendPathComponent(out, &len, dir[origin_token.len..])) return null;
        }
    } else {
        if (!appendPathComponent(out, &len, dir)) return null;
    }
    if (len == 0) return null;
    if (out[len - 1] != '/') {
        if (!appendPathComponent(out, &len, "/")) return null;
    }
    if (!appendPathComponent(out, &len, name)) return null;
    return out[0..len];
}

fn resolveFromSearchList(
    client: *fs_client.Client,
    root_token: u64,
    list: []const u8,
    origin: []const u8,
    name: []const u8,
    out: *[max_lib_path_bytes]u8,
) ?[]const u8 {
    var start: usize = 0;
    while (start <= list.len) {
        var end = start;
        while (end < list.len and list[end] != ':') : (end += 1) {}
        if (end > start) {
            const dir = list[start..end];
            if (pathFromSearchDir(dir, origin, name, out)) |candidate| {
                if (rootFsFileExists(client, root_token, candidate)) return candidate;
            }
        }
        if (end >= list.len) break;
        start = end + 1;
    }
    return null;
}

fn resolveNeededPath(
    client: *fs_client.Client,
    root_token: u64,
    requester_path: []const u8,
    runpath: []const u8,
    rpath: []const u8,
    name: []const u8,
    out: *[max_lib_path_bytes]u8,
) ?[]const u8 {
    if (name.len == 0) return null;
    if (name[0] == '/') {
        const path = copyPath(name, out) orelse return null;
        return if (rootFsFileExists(client, root_token, path)) path else null;
    }
    const origin = originDir(requester_path);
    if (runpath.len != 0) {
        if (resolveFromSearchList(client, root_token, runpath, origin, name, out)) |path| {
            userLog("ExecLoader: resolve via RUNPATH: ");
            userLog(path);
            userLog("\n");
            return path;
        }
    }
    if (rpath.len != 0) {
        if (resolveFromSearchList(client, root_token, rpath, origin, name, out)) |path| {
            userLog("ExecLoader: resolve via RPATH: ");
            userLog(path);
            userLog("\n");
            return path;
        }
    }
    const path = bootfsLibPath(name, out) orelse return null;
    return if (rootFsFileExists(client, root_token, path)) path else null;
}

fn dynString(strtab: []const u8, strsz: u64, offset: u64) ?[]const u8 {
    if (offset >= strsz) return null;
    const max_len_u64 = strsz - offset;
    const max_len: usize = @intCast(@min(max_len_u64, max_lib_name_bytes));
    const start: usize = @intCast(offset);
    var len: usize = 0;
    while (len < max_len) : (len += 1) {
        if (strtab[start + len] == 0) return strtab[start .. start + len];
    }
    return null;
}

fn storePath(slot: *NeededPath, path: []const u8) bool {
    if (path.len == 0 or path.len > slot.bytes.len) return false;
    @memcpy(slot.bytes[0..path.len], path);
    slot.len = path.len;
    return true;
}

fn imageSoname(source_base_va: u64, ehdr: ElfHeader, file_bytes: u64) ?[]const u8 {
    var dyn_offset: u64 = 0;
    var dyn_filesz: u64 = 0;
    var phdr_index: u16 = 0;
    while (phdr_index < ehdr.phnum) : (phdr_index += 1) {
        const phdr = parseProgramHeader(source_base_va, ehdr, phdr_index, file_bytes) orelse return null;
        if (phdr.p_type == pt_dynamic and phdr.filesz >= elf_dyn_bytes) {
            dyn_offset = phdr.offset;
            dyn_filesz = phdr.filesz;
            break;
        }
    }
    if (dyn_filesz == 0) return null;

    const dyn_bytes = bytesAt(source_base_va, dyn_offset, @intCast(dyn_filesz), file_bytes) orelse return null;
    var strtab_vaddr: u64 = 0;
    var strsz: u64 = 0;
    var soname_off: u64 = 0;
    var dyn_off: usize = 0;
    while (dyn_off + elf_dyn_bytes <= dyn_bytes.len) : (dyn_off += elf_dyn_bytes) {
        const tag = readI64Le(dyn_bytes, dyn_off) orelse return null;
        const value = readU64Le(dyn_bytes, dyn_off + 8) orelse return null;
        if (tag == dt_null) break;
        if (tag == dt_strtab) strtab_vaddr = value;
        if (tag == dt_strsz) strsz = value;
        if (tag == dt_soname) soname_off = value;
    }
    if (strtab_vaddr == 0 or strsz == 0 or soname_off == 0) return null;
    const strtab_file_off = fileOffsetForVaddr(source_base_va, ehdr, strtab_vaddr, strsz, file_bytes) orelse return null;
    const strtab = bytesAt(source_base_va, strtab_file_off, @intCast(strsz), file_bytes) orelse return null;
    return dynString(strtab, strsz, soname_off);
}

fn pathEqual(a: []const u8, b: []const u8) bool {
    if (a.len != b.len) return false;
    var index: usize = 0;
    while (index < a.len) : (index += 1) {
        if (a[index] != b[index]) return false;
    }
    return true;
}

fn pathInList(list: []const NeededPath, count: usize, path: []const u8) bool {
    var index: usize = 0;
    while (index < count) : (index += 1) {
        if (pathEqual(list[index].bytes[0..list[index].len], path)) return true;
    }
    return false;
}

fn collectNeededPaths(
    client: *fs_client.Client,
    root_token: u64,
    requester_path: []const u8,
    source_base_va: u64,
    ehdr: ElfHeader,
    file_bytes: u64,
    work_queue: []NeededPath,
    work_count: *usize,
    loaded: []const NeededPath,
    loaded_sonames: []const NeededPath,
    loaded_count: usize,
) void {
    var dyn_offset: u64 = 0;
    var dyn_filesz: u64 = 0;
    var phdr_index: u16 = 0;
    while (phdr_index < ehdr.phnum) : (phdr_index += 1) {
        const phdr = parseProgramHeader(source_base_va, ehdr, phdr_index, file_bytes) orelse return;
        if (phdr.p_type == pt_dynamic and phdr.filesz >= elf_dyn_bytes) {
            dyn_offset = phdr.offset;
            dyn_filesz = phdr.filesz;
            break;
        }
    }
    if (dyn_filesz == 0) return;

    const dyn_bytes = bytesAt(source_base_va, dyn_offset, @intCast(dyn_filesz), file_bytes) orelse return;
    var strtab_vaddr: u64 = 0;
    var strsz: u64 = 0;
    var runpath_off: u64 = 0;
    var rpath_off: u64 = 0;
    var dyn_off: usize = 0;
    while (dyn_off + elf_dyn_bytes <= dyn_bytes.len) : (dyn_off += elf_dyn_bytes) {
        const tag = readI64Le(dyn_bytes, dyn_off) orelse break;
        const value = readU64Le(dyn_bytes, dyn_off + 8) orelse break;
        if (tag == dt_null) break;
        if (tag == dt_strtab) strtab_vaddr = value;
        if (tag == dt_strsz) strsz = value;
        if (tag == dt_runpath) runpath_off = value;
        if (tag == dt_rpath) rpath_off = value;
    }
    if (strtab_vaddr == 0 or strsz == 0) return;

    const strtab_file_off = fileOffsetForVaddr(source_base_va, ehdr, strtab_vaddr, strsz, file_bytes) orelse return;
    const strtab = bytesAt(source_base_va, strtab_file_off, @intCast(strsz), file_bytes) orelse return;
    const runpath = if (runpath_off != 0) dynString(strtab, strsz, runpath_off) orelse "" else "";
    const rpath = if (rpath_off != 0) dynString(strtab, strsz, rpath_off) orelse "" else "";

    dyn_off = 0;
    while (dyn_off + elf_dyn_bytes <= dyn_bytes.len and work_count.* < work_queue.len) : (dyn_off += elf_dyn_bytes) {
        const tag = readI64Le(dyn_bytes, dyn_off) orelse break;
        const str_off = readU64Le(dyn_bytes, dyn_off + 8) orelse break;
        if (tag == dt_null) break;
        if (tag != dt_needed) continue;
        const name = dynString(strtab, strsz, str_off) orelse continue;
        if (name.len == 0) continue;
        if (pathInList(loaded_sonames, loaded_count, name)) continue;

        var path_buf: [max_lib_path_bytes]u8 = undefined;
        const path = resolveNeededPath(client, root_token, requester_path, runpath, rpath, name, &path_buf) orelse continue;
        if (pathInList(work_queue, work_count.*, path)) continue;
        if (pathInList(loaded, loaded_count, path)) continue;

        const slot = &work_queue[work_count.*];
        if (!storePath(slot, path)) continue;
        work_count.* += 1;
    }
}

fn findBootFsEntry(bootfs_base_va: u64, bootfs_bytes: u64, path: []const u8) ?bootfs_format.BootFsEntry {
    if (bootfs_base_va == 0 or bootfs_bytes < @sizeOf(bootfs_format.BootFsHeader)) return null;
    const header: *const bootfs_format.BootFsHeader = @ptrFromInt(bootfs_base_va);
    if (header.magic != bootfs_format.magic or header.version != bootfs_format.version) return null;
    if (header.header_bytes != @sizeOf(bootfs_format.BootFsHeader)) return null;
    if (header.image_bytes == 0 or header.image_bytes > bootfs_bytes) return null;
    if (header.entry_bytes < @sizeOf(bootfs_format.BootFsEntry)) return null;
    const entry_table_bytes = @as(u64, header.entry_count) * @as(u64, header.entry_bytes);
    if (header.entry_table_offset + entry_table_bytes > header.image_bytes) return null;
    if (header.string_table_offset + header.string_table_bytes > header.image_bytes) return null;

    const entries: [*]const bootfs_format.BootFsEntry = @ptrFromInt(bootfs_base_va + header.entry_table_offset);
    var index: usize = 0;
    while (index < header.entry_count) : (index += 1) {
        const entry = entries[index];
        if (entry.kind != bootfs_format.kind_regular) continue;
        const path_end = @as(u64, entry.path_offset) + @as(u64, entry.path_bytes);
        if (path_end > header.string_table_bytes) continue;
        const entry_path_ptr: [*]const u8 = @ptrFromInt(bootfs_base_va + header.string_table_offset + entry.path_offset);
        const entry_path = entry_path_ptr[0..entry.path_bytes];
        if (!sameBytes(entry_path, path)) continue;
        if (entry.data_offset + entry.data_bytes > header.image_bytes) return null;
        return entry;
    }
    return null;
}

fn loadNeededLibs(
    process_token: u64,
    layout: *VmLayout,
    mapped_pages: *ChildPageTracker,
    client: *fs_client.Client,
    root_token: u64,
    main_source_base_va: u64,
    main_ehdr: ElfHeader,
    main_file_bytes: u64,
    out_libs: []dynamic_linker_bootstrap_abi.LoadedLibInfo,
) usize {
    if (out_libs.len == 0) return 0;

    var queue = needed_queue_scratch[0..];
    var loaded_paths = loaded_path_scratch[0..];
    var loaded_sonames = loaded_soname_scratch[0..];
    var queue_count: usize = 0;
    var queue_index: usize = 0;
    var lib_count: usize = 0;

    collectNeededPaths(client, root_token, "/cmd/musl_smoke.elf", main_source_base_va, main_ehdr, main_file_bytes, queue[0..], &queue_count, loaded_paths[0..], loaded_sonames[0..], 0);

    while (queue_index < queue_count and lib_count < out_libs.len) : (queue_index += 1) {
        const path = queue[queue_index].bytes[0..queue[queue_index].len];
        if (path.len == 0 or pathInList(loaded_paths[0..], lib_count, path)) continue;

        const file_bytes = readRootFsFile(client, root_token, path, lib_image_scratch[0..]) orelse {
            userLog("ExecLoader: lib read failed: ");
            userLog(path);
            userLog("\n");
            continue;
        };

        const scratch = lib_image_scratch[0..file_bytes];
        const scratch_va = @intFromPtr(scratch.ptr);
        const identity_ehdr = parseElfHeader(scratch_va, file_bytes) orelse {
            userLog("ExecLoader: lib header failed: ");
            userLog(path);
            userLog("\n");
            continue;
        };
        const soname = imageSoname(scratch_va, identity_ehdr, file_bytes) orelse "";
        if (soname.len != 0 and pathInList(loaded_sonames[0..], lib_count, soname)) {
            userLog("ExecLoader: skip duplicate SONAME: ");
            userLog(soname);
            userLog("\n");
            continue;
        }

        const lib_vm = installVmObject(scratch_va, file_bytes, .{ .read = true, .map = true }) orelse {
            userLog("ExecLoader: lib vm install failed: ");
            userLog(path);
            userLog("\n");
            continue;
        };
        const lib_image = loadElfImage(process_token, layout, mapped_pages, lib_vm, scratch_va, file_bytes, "ExecLoader: lib") orelse continue;

        const dynamic_va = if (lib_image.dynamic_phdr) |d| blk: {
            const va, const ov = @addWithOverflow(lib_image.load_bias, d.vaddr);
            break :blk if (ov == 0) va else 0;
        } else 0;
        out_libs[lib_count] = .{
            .load_bias = lib_image.load_bias,
            .dynamic_va = dynamic_va,
            .dynamic_filesz = if (lib_image.dynamic_phdr) |d| d.filesz else 0,
            .tls_vaddr = if (lib_image.tls_phdr) |t| t.vaddr else 0,
            .tls_filesz = if (lib_image.tls_phdr) |t| t.filesz else 0,
            .tls_memsz = if (lib_image.tls_phdr) |t| t.memsz else 0,
            .tls_align = if (lib_image.tls_phdr) |t| t.align_bytes else 0,
            .relro_vaddr = if (lib_image.relro_phdr) |r| r.vaddr else 0,
            .relro_memsz = if (lib_image.relro_phdr) |r| r.memsz else 0,
        };

        if (!storePath(&loaded_paths[lib_count], path)) continue;
        loaded_sonames[lib_count].len = 0;
        if (soname.len != 0) {
            _ = storePath(&loaded_sonames[lib_count], soname);
            userLog("ExecLoader: lib SONAME: ");
            userLog(soname);
            userLog("\n");
        }
        lib_count += 1;
        userLog("ExecLoader: lib loaded: ");
        userLog(path);
        userLog("\n");

        collectNeededPaths(client, root_token, path, scratch_va, lib_image.ehdr, file_bytes, queue[0..], &queue_count, loaded_paths[0..], loaded_sonames[0..], lib_count);
    }
    return lib_count;
}

fn installRuntimeBootstrapPages(process_token: u64) bool {
    if (!allocMapPagesToProcess(process_token, stdio_sink_target_va, 1, .{ .read = true, .write = true })) return false;
    const stdio_modes =
        (stdio_mode_kernel_log << stdio_log_mode_shift) |
        (stdio_mode_kernel_log << stdio_stdout_mode_shift) |
        (stdio_mode_kernel_log << stdio_stderr_mode_shift);
    if (!writeProcessU64(process_token, stdio_sink_target_va + 0, stdio_sink_magic)) return false;
    if (!writeProcessU64(process_token, stdio_sink_target_va + 8, stdio_sink_version)) return false;
    if (!writeProcessU64(process_token, stdio_sink_target_va + 56, stdio_modes)) return false;

    if (!allocMapPagesToProcess(process_token, process_exit_status_target_va, 1, .{ .read = true, .write = true })) return false;
    if (!writeProcessU64(process_token, process_exit_status_target_va + 0, process_exit_status_magic)) return false;
    if (!writeProcessU64(process_token, process_exit_status_target_va + 8, process_exit_status_version)) return false;
    if (!writeProcessU64(process_token, process_exit_status_target_va + 16, process_exit_status_state_idle)) return false;
    return true;
}

fn mapProtFromPhdr(phdr: ProgramHeader) process_builder_abi.MapProt {
    return .{
        .read = (phdr.flags & pf_r) != 0,
        .write = (phdr.flags & pf_w) != 0,
        .exec = (phdr.flags & pf_x) != 0,
    };
}

fn pageProtFromImage(source_base_va: u64, ehdr: ElfHeader, file_bytes: u64, page_vaddr: u64) ?process_builder_abi.MapProt {
    var read = false;
    var write = false;
    var exec = false;
    var index: u16 = 0;
    while (index < ehdr.phnum) : (index += 1) {
        const phdr = parseProgramHeader(source_base_va, ehdr, index, file_bytes) orelse return null;
        if (phdr.p_type != pt_load or phdr.memsz == 0) continue;
        const segment_start = pageDown(phdr.vaddr);
        const page_delta = phdr.vaddr - segment_start;
        const segment_delta_end, const delta_overflow = @addWithOverflow(page_delta, phdr.memsz);
        if (delta_overflow != 0) return null;
        const segment_bytes = pageUp(segment_delta_end) orelse return null;
        const segment_end, const end_overflow = @addWithOverflow(segment_start, segment_bytes);
        if (end_overflow != 0) return null;
        const page_end, const page_overflow = @addWithOverflow(page_vaddr, page_bytes);
        if (page_overflow != 0) return null;
        if (!rangesOverlap(page_vaddr, page_end, segment_start, segment_end)) continue;
        read = read or (phdr.flags & pf_r) != 0;
        write = write or (phdr.flags & pf_w) != 0;
        exec = exec or (phdr.flags & pf_x) != 0;
    }
    if (!read and !write and !exec) return null;
    if (exec) write = false;
    return .{ .read = true, .write = write, .exec = exec };
}

fn validateLoadSegment(phdr: ProgramHeader, file_bytes: u64) bool {
    if (phdr.memsz < phdr.filesz) return false;
    const file_end, const file_overflow = @addWithOverflow(phdr.offset, phdr.filesz);
    if (file_overflow != 0 or file_end > file_bytes) return false;
    return true;
}

fn loadMappedSegment(process_token: u64, source_vm_token: u64, phdr: ProgramHeader, load_bias: u64) bool {
    if (((phdr.offset ^ phdr.vaddr) & (page_bytes - 1)) != 0) return false;

    const file_page_off = pageDown(phdr.offset);
    const segment_page_va = pageDown(phdr.vaddr);
    const target_va, const target_overflow = @addWithOverflow(load_bias, segment_page_va);
    if (target_overflow != 0) return false;
    const page_delta = phdr.vaddr - segment_page_va;
    const segment_end, const segment_overflow = @addWithOverflow(page_delta, phdr.filesz);
    if (segment_overflow != 0) return false;
    const map_bytes = pageUp(segment_end) orelse return false;
    const segment_vm = sliceVmObject(source_vm_token, file_page_off, map_bytes, .{
        .read = true,
        .map = true,
    }) orelse return false;
    return mapVmObjectToProcess(process_token, segment_vm, target_va, mapProtFromPhdr(phdr));
}

fn canMapSegmentDirectly(phdr: ProgramHeader, file_bytes: u64) bool {
    const file_page_off = pageDown(phdr.offset);
    const segment_page_va = pageDown(phdr.vaddr);
    const page_delta = phdr.vaddr - segment_page_va;
    const segment_end, const segment_overflow = @addWithOverflow(page_delta, phdr.filesz);
    if (segment_overflow != 0) return false;
    const map_bytes = pageUp(segment_end) orelse return false;
    const file_map_end, const file_map_overflow = @addWithOverflow(file_page_off, map_bytes);
    return file_map_overflow == 0 and file_map_end <= file_bytes;
}

fn loadPrivateSegment(
    process_token: u64,
    mapped_pages: *ChildPageTracker,
    source_base_va: u64,
    ehdr: ElfHeader,
    file_bytes: u64,
    phdr: ProgramHeader,
    load_bias: u64,
) bool {
    if (phdr.memsz == 0) return true;

    const segment_page_va = pageDown(phdr.vaddr);
    const target_va, const target_overflow = @addWithOverflow(load_bias, segment_page_va);
    if (target_overflow != 0) return false;
    const page_delta = phdr.vaddr - segment_page_va;
    const segment_end, const segment_overflow = @addWithOverflow(page_delta, phdr.memsz);
    if (segment_overflow != 0) return false;
    const mapped_bytes = pageUp(segment_end) orelse return false;
    const page_count = mapped_bytes / page_bytes;
    var page_index: u64 = 0;
    while (page_index < page_count) : (page_index += 1) {
        const page_vaddr = segment_page_va + page_index * page_bytes;
        const page_target_va = target_va + page_index * page_bytes;
        if (!mapped_pages.contains(page_target_va)) {
            const prot = pageProtFromImage(source_base_va, ehdr, file_bytes, page_vaddr) orelse return false;
            if (!allocMapPagesToProcess(process_token, page_target_va, 1, prot)) return false;
            if (!mapped_pages.add(page_target_va)) return false;
        }
    }

    if (phdr.filesz == 0) return true;
    const src_va, const src_overflow = @addWithOverflow(source_base_va, phdr.offset);
    if (src_overflow != 0) return false;
    const dest_va, const dest_overflow = @addWithOverflow(load_bias, phdr.vaddr);
    if (dest_overflow != 0) return false;
    return copyToProcess(process_token, dest_va, src_va, phdr.filesz);
}

fn loadSegment(
    process_token: u64,
    mapped_pages: *ChildPageTracker,
    source_vm_token: u64,
    source_base_va: u64,
    ehdr: ElfHeader,
    phdr: ProgramHeader,
    file_bytes: u64,
    load_bias: u64,
) bool {
    if (!validateLoadSegment(phdr, file_bytes)) return false;
    _ = source_vm_token;
    return loadPrivateSegment(process_token, mapped_pages, source_base_va, ehdr, file_bytes, phdr, load_bias);
}

fn imageSpan(source_base_va: u64, ehdr: ElfHeader, file_bytes: u64) ?ImageSpan {
    var have_load = false;
    var min_vaddr: u64 = 0;
    var max_vaddr: u64 = 0;
    var max_align: u64 = page_bytes;
    var index: u16 = 0;
    while (index < ehdr.phnum) : (index += 1) {
        const phdr = parseProgramHeader(source_base_va, ehdr, index, file_bytes) orelse return null;
        if (phdr.p_type != pt_load) continue;
        if (phdr.memsz < phdr.filesz) return null;
        if (phdr.align_bytes > 1) {
            if ((phdr.align_bytes & (phdr.align_bytes - 1)) != 0) return null;
            if (phdr.align_bytes > max_align) max_align = phdr.align_bytes;
        }
        const start = pageDown(phdr.vaddr);
        const page_delta = phdr.vaddr - start;
        const end_delta, const delta_overflow = @addWithOverflow(page_delta, phdr.memsz);
        if (delta_overflow != 0) return null;
        const end_size = pageUp(end_delta) orelse return null;
        const end, const end_overflow = @addWithOverflow(start, end_size);
        if (end_overflow != 0) return null;
        if (!have_load or start < min_vaddr) min_vaddr = start;
        if (!have_load or end > max_vaddr) max_vaddr = end;
        have_load = true;
    }
    if (!have_load or max_vaddr <= min_vaddr) return null;
    return .{ .min_vaddr = min_vaddr, .max_vaddr = max_vaddr, .align_bytes = max_align };
}

fn rangesOverlap(a_start: u64, a_end: u64, b_start: u64, b_end: u64) bool {
    return a_start < b_end and b_start < a_end;
}

fn chooseLoadBias(layout: *VmLayout, source_base_va: u64, ehdr: ElfHeader, file_bytes: u64) ?u64 {
    if (ehdr.elf_type == elf_type_exec) return 0;
    const span = imageSpan(source_base_va, ehdr, file_bytes) orelse return null;
    return layout.allocateEtDynImage(span);
}

fn fileOffsetForVaddr(source_base_va: u64, ehdr: ElfHeader, vaddr: u64, size: u64, file_bytes: u64) ?u64 {
    var index: u16 = 0;
    while (index < ehdr.phnum) : (index += 1) {
        const phdr = parseProgramHeader(source_base_va, ehdr, index, file_bytes) orelse return null;
        if (phdr.p_type != pt_load) continue;
        if (vaddr < phdr.vaddr) continue;
        const delta = vaddr - phdr.vaddr;
        if (delta > phdr.filesz) continue;
        if (size > phdr.filesz - delta) continue;
        const file_off, const overflow = @addWithOverflow(phdr.offset, delta);
        if (overflow != 0) return null;
        return file_off;
    }
    return null;
}

fn applyRelativeRelocations(process_token: u64, source_base_va: u64, ehdr: ElfHeader, dynamic: ProgramHeader, load_bias: u64, file_bytes: u64) bool {
    const dyn_bytes = bytesAt(source_base_va, dynamic.offset, @intCast(dynamic.filesz), file_bytes) orelse return false;
    var rela_va: u64 = 0;
    var rela_size: u64 = 0;
    var rela_ent: u64 = elf_rela_bytes;

    var dyn_off: usize = 0;
    while (dyn_off + elf_dyn_bytes <= dyn_bytes.len) : (dyn_off += elf_dyn_bytes) {
        const tag = readI64Le(dyn_bytes, dyn_off) orelse return false;
        const value = readU64Le(dyn_bytes, dyn_off + 8) orelse return false;
        if (tag == dt_null) break;
        if (tag == dt_rela) rela_va = value;
        if (tag == dt_relasz) rela_size = value;
        if (tag == dt_relaent) rela_ent = value;
    }

    if (rela_va == 0 or rela_size == 0) return true;
    if (rela_ent != elf_rela_bytes) return false;
    if ((rela_size % elf_rela_bytes) != 0) return false;
    const rela_file_off = fileOffsetForVaddr(source_base_va, ehdr, rela_va, rela_size, file_bytes) orelse return false;

    var rela_off = rela_file_off;
    const rela_end = rela_file_off + rela_size;
    while (rela_off < rela_end) : (rela_off += elf_rela_bytes) {
        const rela = bytesAt(source_base_va, rela_off, @intCast(elf_rela_bytes), file_bytes) orelse return false;
        const r_offset = readU64Le(rela, 0) orelse return false;
        const r_info = readU64Le(rela, 8) orelse return false;
        const r_addend = readI64Le(rela, 16) orelse return false;
        const r_type = r_info & 0xffff_ffff;
        const r_sym = r_info >> 32;
        if (r_type != r_x86_64_relative or r_sym != 0) continue;

        const dest_va, const dest_overflow = @addWithOverflow(load_bias, r_offset);
        if (dest_overflow != 0) return false;
        const relocated = addSigned(load_bias, r_addend) orelse return false;
        if (!writeProcessU64(process_token, dest_va, relocated)) return false;
    }
    return true;
}

fn loadElfImage(
    process_token: u64,
    layout: *VmLayout,
    mapped_pages: *ChildPageTracker,
    source_vm_token: u64,
    source_base_va: u64,
    file_bytes: u64,
    label: []const u8,
) ?LoadedImage {
    const ehdr = parseElfHeader(source_base_va, file_bytes) orelse {
        userLog(label);
        userLog(" ELF header failed\n");
        return null;
    };
    const load_bias = chooseLoadBias(layout, source_base_va, ehdr, file_bytes) orelse {
        userLog(label);
        userLog(" load bias failed\n");
        return null;
    };
    var dynamic_phdr: ?ProgramHeader = null;
    var interp_phdr: ?ProgramHeader = null;
    var tls_phdr: ?ProgramHeader = null;
    var relro_phdr: ?ProgramHeader = null;
    var index: u16 = 0;
    while (index < ehdr.phnum) : (index += 1) {
        const phdr = parseProgramHeader(source_base_va, ehdr, index, file_bytes) orelse {
            userLog(label);
            userLog(" phdr failed\n");
            return null;
        };
        if (phdr.p_type == pt_dynamic) dynamic_phdr = phdr;
        if (phdr.p_type == pt_interp) interp_phdr = phdr;
        if (phdr.p_type == pt_tls) tls_phdr = phdr;
        if (phdr.p_type == pt_gnu_relro) relro_phdr = phdr;
        if (phdr.p_type != pt_load) continue;
        if (!loadSegment(process_token, mapped_pages, source_vm_token, source_base_va, ehdr, phdr, file_bytes, load_bias)) {
            userLog(label);
            userLog(" PT_LOAD failed\n");
            return null;
        }
    }

    if (dynamic_phdr) |phdr| {
        if (!applyRelativeRelocations(process_token, source_base_va, ehdr, phdr, load_bias, file_bytes)) {
            userLog(label);
            userLog(" relocation failed\n");
            return null;
        }
    }

    return .{ .ehdr = ehdr, .load_bias = load_bias, .interp_phdr = interp_phdr, .dynamic_phdr = dynamic_phdr, .tls_phdr = tls_phdr, .relro_phdr = relro_phdr };
}

fn readInterpPath(source_base_va: u64, phdr: ProgramHeader, file_bytes: u64) ?[]const u8 {
    if (phdr.filesz == 0 or phdr.filesz > 256) return null;
    const bytes = bytesAt(source_base_va, phdr.offset, @intCast(phdr.filesz), file_bytes) orelse return null;
    if (bytes[bytes.len - 1] != 0) return null;
    return bytes[0 .. bytes.len - 1];
}

fn installDynamicLinkerBootstrapPage(
    process_token: u64,
    main_image: LoadedImage,
    bootfs_image_bytes: u64,
    loaded_libs: []const dynamic_linker_bootstrap_abi.LoadedLibInfo,
) bool {
    if (!allocMapPagesToProcess(process_token, dynamic_linker_bootstrap_abi.target_va, 1, .{ .read = true, .write = true })) return false;
    const main_entry, const entry_overflow = @addWithOverflow(main_image.load_bias, main_image.ehdr.entry);
    if (entry_overflow != 0) return false;
    if (!writeProcessU64(process_token, dynamic_linker_bootstrap_abi.target_va + @offsetOf(dynamic_linker_bootstrap_abi.Config, "magic"), dynamic_linker_bootstrap_abi.magic)) return false;
    if (!writeProcessU64(process_token, dynamic_linker_bootstrap_abi.target_va + @offsetOf(dynamic_linker_bootstrap_abi.Config, "version"), dynamic_linker_bootstrap_abi.version)) return false;
    if (!writeProcessU64(process_token, dynamic_linker_bootstrap_abi.target_va + @offsetOf(dynamic_linker_bootstrap_abi.Config, "main_entry"), main_entry)) return false;
    if (!writeProcessU64(process_token, dynamic_linker_bootstrap_abi.target_va + @offsetOf(dynamic_linker_bootstrap_abi.Config, "main_load_bias"), main_image.load_bias)) return false;
    const main_phdr, const phdr_overflow = @addWithOverflow(main_image.load_bias, main_image.ehdr.phoff);
    if (phdr_overflow != 0) return false;
    if (!writeProcessU64(process_token, dynamic_linker_bootstrap_abi.target_va + @offsetOf(dynamic_linker_bootstrap_abi.Config, "main_phdr"), main_phdr)) return false;
    if (!writeProcessU64(process_token, dynamic_linker_bootstrap_abi.target_va + @offsetOf(dynamic_linker_bootstrap_abi.Config, "main_phnum"), main_image.ehdr.phnum)) return false;
    if (!writeProcessU64(process_token, dynamic_linker_bootstrap_abi.target_va + @offsetOf(dynamic_linker_bootstrap_abi.Config, "main_phent"), main_image.ehdr.phentsize)) return false;
    if (!writeProcessU64(process_token, dynamic_linker_bootstrap_abi.target_va + @offsetOf(dynamic_linker_bootstrap_abi.Config, "bootfs_image_va"), if (bootfs_image_bytes != 0) dynamic_linker_bootstrap_abi.bootfs_image_target_va else 0)) return false;
    if (!writeProcessU64(process_token, dynamic_linker_bootstrap_abi.target_va + @offsetOf(dynamic_linker_bootstrap_abi.Config, "bootfs_image_bytes"), bootfs_image_bytes)) return false;
    if (!writeProcessU64(process_token, dynamic_linker_bootstrap_abi.target_va + @offsetOf(dynamic_linker_bootstrap_abi.Config, "loaded_lib_count"), loaded_libs.len)) return false;
    const lib_info_size = @sizeOf(dynamic_linker_bootstrap_abi.LoadedLibInfo);
    const libs_base = dynamic_linker_bootstrap_abi.target_va + @offsetOf(dynamic_linker_bootstrap_abi.Config, "loaded_libs");
    var lib_index: usize = 0;
    while (lib_index < loaded_libs.len) : (lib_index += 1) {
        const lib_va = libs_base + lib_index * lib_info_size;
        const lib = loaded_libs[lib_index];
        if (!writeProcessU64(process_token, lib_va + @offsetOf(dynamic_linker_bootstrap_abi.LoadedLibInfo, "load_bias"), lib.load_bias)) return false;
        if (!writeProcessU64(process_token, lib_va + @offsetOf(dynamic_linker_bootstrap_abi.LoadedLibInfo, "dynamic_va"), lib.dynamic_va)) return false;
        if (!writeProcessU64(process_token, lib_va + @offsetOf(dynamic_linker_bootstrap_abi.LoadedLibInfo, "dynamic_filesz"), lib.dynamic_filesz)) return false;
        if (!writeProcessU64(process_token, lib_va + @offsetOf(dynamic_linker_bootstrap_abi.LoadedLibInfo, "tls_vaddr"), lib.tls_vaddr)) return false;
        if (!writeProcessU64(process_token, lib_va + @offsetOf(dynamic_linker_bootstrap_abi.LoadedLibInfo, "tls_filesz"), lib.tls_filesz)) return false;
        if (!writeProcessU64(process_token, lib_va + @offsetOf(dynamic_linker_bootstrap_abi.LoadedLibInfo, "tls_memsz"), lib.tls_memsz)) return false;
        if (!writeProcessU64(process_token, lib_va + @offsetOf(dynamic_linker_bootstrap_abi.LoadedLibInfo, "tls_align"), lib.tls_align)) return false;
        if (!writeProcessU64(process_token, lib_va + @offsetOf(dynamic_linker_bootstrap_abi.LoadedLibInfo, "relro_vaddr"), lib.relro_vaddr)) return false;
        if (!writeProcessU64(process_token, lib_va + @offsetOf(dynamic_linker_bootstrap_abi.LoadedLibInfo, "relro_memsz"), lib.relro_memsz)) return false;
    }
    return true;
}

fn launchExec(executable_vm_token: u64, executable_file_bytes: u64, interpreter_vm_token: u64, interpreter_file_bytes: u64) bool {
    if (!mapVmObject(executable_vm_token, main_source_map_va)) {
        userLog("ExecLoader: source map failed\n");
        return false;
    }
    const process_token = createSuspendedProcess() orelse {
        userLog("ExecLoader: create suspended failed\n");
        return false;
    };
    var vm_layout = VmLayout.initForExecChild() orelse {
        abortProcess(process_token);
        userLog("ExecLoader: vm layout init failed\n");
        return false;
    };
    const cfg: *const volatile exec_loader_bootstrap_abi.Config = @ptrFromInt(exec_loader_bootstrap_abi.target_va);
    const bootfs_image_bytes = if (image_abi.decodeVmObjectToken(cfg.bootfs_vm_token) != null) cfg.bootfs_file_bytes else 0;
    if (!vm_layout.reserveBootFsImage(bootfs_image_bytes)) {
        abortProcess(process_token);
        userLog("ExecLoader: bootfs reserve failed\n");
        return false;
    }
    child_mapped_pages = .{};
    const main_image = loadElfImage(process_token, &vm_layout, &child_mapped_pages, executable_vm_token, main_source_map_va, executable_file_bytes, "ExecLoader: main") orelse {
        abortProcess(process_token);
        userLog("ExecLoader: main ELF load failed\n");
        return false;
    };

    const main_entry, const main_entry_overflow = @addWithOverflow(main_image.load_bias, main_image.ehdr.entry);
    if (main_entry_overflow != 0) {
        abortProcess(process_token);
        userLog("ExecLoader: entry overflow\n");
        return false;
    }
    var initial_entry = main_entry;

    if (main_image.interp_phdr) |interp_phdr| {
        const interp_path = readInterpPath(main_source_map_va, interp_phdr, executable_file_bytes) orelse {
            abortProcess(process_token);
            userLog("ExecLoader: bad PT_INTERP\n");
            return false;
        };
        if (!sameBytes(interp_path, interp_path_ld)) {
            abortProcess(process_token);
            userLog("ExecLoader: unsupported PT_INTERP\n");
            return false;
        }
        if (image_abi.decodeVmObjectToken(interpreter_vm_token) == null or interpreter_file_bytes == 0) {
            abortProcess(process_token);
            userLog("ExecLoader: missing interpreter ELF\n");
            return false;
        }
        if (!mapVmObject(interpreter_vm_token, interpreter_source_map_va)) {
            abortProcess(process_token);
            userLog("ExecLoader: interpreter source map failed\n");
            return false;
        }
        const interp_image = loadElfImage(process_token, &vm_layout, &child_mapped_pages, interpreter_vm_token, interpreter_source_map_va, interpreter_file_bytes, "ExecLoader: interpreter") orelse {
            abortProcess(process_token);
            userLog("ExecLoader: interpreter ELF load failed\n");
            return false;
        };
        const interp_entry, const interp_entry_overflow = @addWithOverflow(interp_image.load_bias, interp_image.ehdr.entry);
        if (interp_entry_overflow != 0) {
            abortProcess(process_token);
            userLog("ExecLoader: interpreter entry overflow\n");
            return false;
        }
        initial_entry = interp_entry;
        if (bootfs_image_bytes != 0 and !mapVmObjectToProcess(process_token, cfg.bootfs_vm_token, dynamic_linker_bootstrap_abi.bootfs_image_target_va, .{ .read = true })) {
            abortProcess(process_token);
            userLog("ExecLoader: bootfs map failed\n");
            return false;
        }
        var loaded_lib_buf: [dynamic_linker_bootstrap_abi.max_loaded_libs]dynamic_linker_bootstrap_abi.LoadedLibInfo = [_]dynamic_linker_bootstrap_abi.LoadedLibInfo{.{}} ** dynamic_linker_bootstrap_abi.max_loaded_libs;
        var rootfs = connectRootFs(cfg.fs_endpoint_id, cfg.fs_compat_process_slot) orelse {
            abortProcess(process_token);
            userLog("ExecLoader: rootfs unavailable for dependencies\n");
            return false;
        };
        const loaded_lib_count = loadNeededLibs(
            process_token,
            &vm_layout,
            &child_mapped_pages,
            &rootfs.client,
            rootfs.root_token,
            main_source_map_va,
            main_image.ehdr,
            executable_file_bytes,
            loaded_lib_buf[0..],
        );
        if (!installDynamicLinkerBootstrapPage(process_token, main_image, bootfs_image_bytes, loaded_lib_buf[0..loaded_lib_count])) {
            abortProcess(process_token);
            userLog("ExecLoader: dynamic linker bootstrap failed\n");
            return false;
        }
    }

    if (!allocMapPagesToProcess(process_token, process_abi.user_stack_page_va, 1, .{ .read = true, .write = true })) {
        abortProcess(process_token);
        userLog("ExecLoader: stack alloc failed\n");
        return false;
    }
    if (!installRuntimeBootstrapPages(process_token)) {
        abortProcess(process_token);
        userLog("ExecLoader: runtime bootstrap failed\n");
        return false;
    }
    if (!setProcessInitialContext(process_token, initial_entry, process_abi.user_entry_rsp)) {
        abortProcess(process_token);
        userLog("ExecLoader: set context failed\n");
        return false;
    }
    if (cfg.abi_trap_endpoint_id != 0 and cfg.abi_trap_endpoint_process_slot != 0) {
        const flavor = if (cfg.abi_trap_flavor != 0) cfg.abi_trap_flavor else @as(u64, @intFromEnum(trap_abi.AbiFlavor.linux_x86_64));
        if (!setProcessAbiTrapDelegate(process_token, cfg.abi_trap_endpoint_id, cfg.abi_trap_endpoint_process_slot, flavor)) {
            abortProcess(process_token);
            userLog("ExecLoader: abi trap delegate failed\n");
            return false;
        }
        userLog("ExecLoader: abi trap delegate ready\n");
    }
    _ = startProcess(process_token) orelse {
        abortProcess(process_token);
        userLog("ExecLoader: start failed\n");
        return false;
    };
    return true;
}

pub export fn _start() noreturn {
    userLog("ExecLoader: started\n");
    const cfg: *const volatile exec_loader_bootstrap_abi.Config = @ptrFromInt(exec_loader_bootstrap_abi.target_va);
    if (cfg.magic != exec_loader_bootstrap_abi.magic or cfg.version != exec_loader_bootstrap_abi.version) {
        userLog("ExecLoader: missing bootstrap config\n");
    } else if (image_abi.decodeVmObjectToken(cfg.executable_vm_token) == null or cfg.executable_file_bytes == 0) {
        userLog("ExecLoader: invalid executable token\n");
    } else if (launchExec(cfg.executable_vm_token, cfg.executable_file_bytes, cfg.interpreter_vm_token, cfg.interpreter_file_bytes)) {
        userLog("ExecLoader: child started\n");
    }

    while (true) asm volatile ("pause");
}

