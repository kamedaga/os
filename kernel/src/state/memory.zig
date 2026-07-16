const std = @import("std");
const builtin = @import("builtin");
const vtd = @import("../vtd.zig");
const x86_platform = @import("../arch/x86_64/platform.zig");
const types = @import("types.zig");
const capsule = types.capsule;
const initial_process_count = types.initial_process_count;
const initial_process_capacity = types.initial_process_capacity;
const process_count = types.process_count;
const max_process_slots = types.max_process_slots;
const initial_thread_capacity = types.initial_thread_capacity;
const max_thread_slots = types.max_thread_slots;
const device_count = types.device_count;
const device_principal_raw = types.device_principal_raw;
const principal_count = types.principal_count;
const PrincipalRaw = types.PrincipalRaw;
const PrincipalId = types.PrincipalId;
const CapsuleKind = types.CapsuleKind;
const CapsuleRights = types.CapsuleRights;
const CapsuleMetadata = types.CapsuleMetadata;
const CapsuleSnapshot = types.CapsuleSnapshot;
const CapsuleDmaDirection = types.CapsuleDmaDirection;
const CapsuleIrqKind = types.CapsuleIrqKind;
const MapProt = types.MapProt;
const EndpointRoute = types.EndpointRoute;
const Region = types.Region;
const ProcessDescriptor = types.ProcessDescriptor;
const ProcessStatus = types.ProcessStatus;
const DebugProcessLifecycleReason = types.DebugProcessLifecycleReason;
const KernelError = types.KernelError;
const DmaDeviceId = types.DmaDeviceId;
const invalid_dma_device_id = types.invalid_dma_device_id;
const Fd = types.Fd;
const fd_table_entries = types.fd_table_entries;
const max_fd_objects = types.max_fd_objects;
const max_pipes = types.max_pipes;
const pipe_buffer_bytes = types.pipe_buffer_bytes;
const fd_known_flags_mask = types.fd_known_flags_mask;
const fd_known_rights_mask = types.fd_known_rights_mask;
const FdFlags = types.FdFlags;
const FdRights = types.FdRights;
const KernelObjectKind = types.KernelObjectKind;
const TaskObjectState = types.TaskObjectState;
const ProcessObject = types.ProcessObject;
const ThreadObject = types.ThreadObject;
const DeviceObject = types.DeviceObject;
const MmioRegionObject = types.MmioRegionObject;
const DmaBufferObject = types.DmaBufferObject;
const DmaMappingObject = types.DmaMappingObject;
const IrqObject = types.IrqObject;
const IrqPublishSlot = types.IrqPublishSlot;
const TimerObject = types.TimerObject;
const TimerFdState = types.TimerFdState;
const SerialObject = types.SerialObject;
const SchedulerControlObject = types.SchedulerControlObject;
const SchedulerEventObject = types.SchedulerEventObject;
const PipeRef = types.PipeRef;
const PipeEndpointObject = types.PipeEndpointObject;
const PipePair = types.PipePair;
const PipeIoError = types.PipeIoError;
const KernelObjectRef = types.KernelObjectRef;
const KernelObjectPayload = types.KernelObjectPayload;
const KernelObjectSlot = types.KernelObjectSlot;
const FdEntry = types.FdEntry;
const FdInfo = types.FdInfo;
const FdTable = types.FdTable;
const FdTransferMode = types.FdTransferMode;
const max_ipc_endpoints = types.max_ipc_endpoints;
const max_ipc_channels = types.max_ipc_channels;
const max_ipc_replies = types.max_ipc_replies;
const max_ipc_queue_messages = types.max_ipc_queue_messages;
const max_ipc_message_fds = types.max_ipc_message_fds;
const max_ipc_waiters = types.max_ipc_waiters;
const IpcEndpointRef = types.IpcEndpointRef;
const IpcChannelRef = types.IpcChannelRef;
const IpcChannelHandle = types.IpcChannelHandle;
const IpcReplyRef = types.IpcReplyRef;
const IpcWaitKey = types.IpcWaitKey;
const IpcWaiter = types.IpcWaiter;
const ThreadWakeTarget = types.ThreadWakeTarget;
const TaskFdWaiter = types.TaskFdWaiter;
const max_task_fd_waiters = types.max_task_fd_waiters;
const max_ipc_object_waiters = types.max_ipc_object_waiters;
const PipeSlot = types.PipeSlot;
const IpcWaitList = types.IpcWaitList;
const IpcTransferredFd = types.IpcTransferredFd;
const IpcMessage = types.IpcMessage;
const IpcQueue = types.IpcQueue;
const IpcEndpointSlot = types.IpcEndpointSlot;
const IpcChannelSlot = types.IpcChannelSlot;
const IpcReplySlot = types.IpcReplySlot;
const IpcSendFd = types.IpcSendFd;
const IpcSendMessage = types.IpcSendMessage;
const IpcRecvFd = types.IpcRecvFd;
const IpcRecvResult = types.IpcRecvResult;
const native_page_size = types.native_page_size;
const max_native_vmos = types.max_native_vmos;
const max_vmas_per_process = types.max_vmas_per_process;
const NativeVmoKind = types.NativeVmoKind;
const NativeVmoRef = types.NativeVmoRef;
const NativeCowTableRef = types.NativeCowTableRef;
const NativeVmoSlot = types.NativeVmoSlot;
const NativeCowTableSlot = types.NativeCowTableSlot;
const VmaProt = types.VmaProt;
const MmapFlags = types.MmapFlags;
const VmaEntry = types.VmaEntry;
const VmaTable = types.VmaTable;
const NativeVmaFaultMapping = types.NativeVmaFaultMapping;
const EndpointTable = types.EndpointTable;
const PublishedEndpointTable = types.PublishedEndpointTable;
const max_vmo_backing_pages = types.max_vmo_backing_pages;
const max_vmo_backing_store_pages = types.max_vmo_backing_store_pages;
const max_vmo_backing_store_free_ranges = types.max_vmo_backing_store_free_ranges;
const VmoBackingStoreFreeRange = types.VmoBackingStoreFreeRange;
const RegionFreeRange = types.RegionFreeRange;
const FreePageList = types.FreePageList;
const PageCapability = types.PageCapability;
const empty_vmo_backing_page_store = types.empty_vmo_backing_page_store;
const vmo_backing_page_store = types.vmo_backing_page_store;
const vmo_backing_page_store_next = types.vmo_backing_page_store_next;
const empty_vmo_backing_page_store_free_ranges = types.empty_vmo_backing_page_store_free_ranges;
const vmo_backing_page_store_free_ranges = types.vmo_backing_page_store_free_ranges;
const vmo_backing_page_store_free_range_len = types.vmo_backing_page_store_free_range_len;
const empty_process_descriptors_extra = types.empty_process_descriptors_extra;
const empty_endpoint_tables_extra = types.empty_endpoint_tables_extra;
const empty_fd_tables_extra = types.empty_fd_tables_extra;
const empty_vma_tables_extra = types.empty_vma_tables_extra;
const processPrincipalFromIndex = types.processPrincipalFromIndex;
const processIndexFromPrincipal = types.processIndexFromPrincipal;
const principalLabel = types.principalLabel;
const fdFlagsFromBits = types.fdFlagsFromBits;
const fdFlagsToBits = types.fdFlagsToBits;
const fdRightsFromBits = types.fdRightsFromBits;
const fdRightsToBits = types.fdRightsToBits;
const isFdRightsSubset = types.isFdRightsSubset;
const vmObjectBackingFreePageCount = types.vmObjectBackingFreePageCount;
const removeVmoBackingFreeRange = types.removeVmoBackingFreeRange;
const insertVmoBackingFreeRange = types.insertVmoBackingFreeRange;
const allocEmptyVmoBackingPageStore = types.allocEmptyVmoBackingPageStore;
const vmoBackingPageStorePaddr = types.vmoBackingPageStorePaddr;
const setVmoBackingPageStorePaddr = types.setVmoBackingPageStorePaddr;
const freeVmoBackingPageStore = types.freeVmoBackingPageStore;
const resetVmoBackingPageStore = types.resetVmoBackingPageStore;
const kernelStaticStorageEndAddr = types.kernelStaticStorageEndAddr;
const runtimeStorageBytes = types.runtimeStorageBytes;
const initRuntimeStorage = types.initRuntimeStorage;

pub fn resetStorageInPlace(self: anytype) void {
    @memset(std.mem.asBytes(self), 0);
    self.process_descriptors_extra = empty_process_descriptors_extra[0..];
    self.endpoint_tables_extra = empty_endpoint_tables_extra[0..];
    self.fd_tables_extra = empty_fd_tables_extra[0..];
    self.vma_tables_extra = empty_vma_tables_extra[0..];
    self.process_capacity = process_count;
    self.aslr_secret = 0x6a09_e667_f3bc_c909;
    self.aslr_sequence = 1;

    for (&self.fd_objects) |*slot| slot.generation = 1;
    for (&self.pipes) |*slot| slot.generation = 1;
    for (&self.native_vmos) |*slot| slot.generation = 1;
    self.native_cow_table_initial = .{};
    self.native_cow_table_overflow_head = null;
    self.native_cow_table_chunk_count = 1;
    for (&self.ipc_endpoints) |*slot| slot.generation = 1;
    for (&self.ipc_channels) |*slot| slot.generation = 1;
    for (&self.ipc_replies) |*slot| slot.generation = 1;
}

pub fn pageAlignUp(value: u64) u64 {
    return (value + 4095) & ~@as(u64, 4095);
}

pub fn pageAlignDown(value: u64) u64 {
    return value & ~@as(u64, 4095);
}

pub fn isPageAligned(value: u64) bool {
    return (value & (native_page_size - 1)) == 0;
}

pub fn mixAslr64(value: u64) u64 {
    var x = value;
    x ^= x >> 30;
    x *%= 0xbf58_476d_1ce4_e5b9;
    x ^= x >> 27;
    x *%= 0x94d0_49bb_1331_11eb;
    x ^= x >> 31;
    return x;
}

pub fn nextAslrWord(self: anytype, owner: PrincipalId, purpose: u64) u64 {
    self.aslr_sequence +%= 1;
    const entropy = self.aslr_secret ^
        self.aslr_sequence ^
        x86_platform.readTimestampCounter() ^
        (@as(u64, @intFromEnum(owner)) << 32) ^
        purpose ^
        @intFromPtr(self);
    self.aslr_secret = @TypeOf(self.*).mixAslr64(entropy);
    return @TypeOf(self.*).mixAslr64(self.aslr_secret ^ (self.aslr_sequence *% 0x9e37_79b9_7f4a_7c15));
}

pub fn fillRandomBytes(self: anytype, owner: PrincipalId, out: []u8) void {
    var offset: usize = 0;
    var word_index: u64 = 0;
    while (offset < out.len) : (word_index += 1) {
        var word = self.nextAslrWord(owner, 0x524e_4442_5954_4553 ^ word_index);
        var byte_index: usize = 0;
        while (byte_index < 8 and offset < out.len) : ({
            byte_index += 1;
            offset += 1;
        }) {
            out[offset] = @truncate(word);
            word >>= 8;
        }
    }
}

pub fn findRandomizedFreeUserMapVa(
    self: anytype,
    owner: PrincipalId,
    size_bytes: u64,
    purpose: u64,
) KernelError!u64 {
    return self.findFreeUserMapVa(owner, size_bytes, self.nextAslrWord(owner, purpose));
}

pub fn initPhase1InPlace(self: anytype) void {
    self.resetStorageInPlace();
    self.regions[0] = .{
        .id = 0,
    };
    self.region_len = 1;
    self.initPrincipalState();
}

pub fn initFromDetectedRegionsInPlace(self: anytype, region_count: usize) KernelError!void {
    if (region_count == 0) return KernelError.EmptyRegionSet;
    if (region_count > @TypeOf(self.*).max_regions) return KernelError.TooManyRegions;

    self.resetStorageInPlace();
    if (builtin.is_test) {
        self.initPrincipalState();
    } else {
        self.initDynamicPrincipalState();
    }

    var i: usize = 0;
    while (i < region_count) : (i += 1) {
        self.regions[i] = .{
            .id = i,
        };
    }
    self.region_len = region_count;
}

pub fn getRegion(self: anytype, region_id: u64) ?*Region {
    var i: usize = 0;
    while (i < self.region_len) : (i += 1) {
        if (self.regions[i].id == region_id) return &self.regions[i];
    }
    return null;
}

pub fn getRegionConst(self: anytype, region_id: u64) ?*const Region {
    var i: usize = 0;
    while (i < self.region_len) : (i += 1) {
        if (self.regions[i].id == region_id) return &self.regions[i];
    }
    return null;
}

pub fn debugWriteField(
    write: *const fn ([]const u8) void,
    print_number: *const fn (u64) void,
    name: []const u8,
    value: u64,
) void {
    write(" ");
    write(name);
    write("=");
    print_number(value);
}

pub fn debugLogMemoryOwnership(
    self: anytype,
    free_list: *const FreePageList,
    write: *const fn ([]const u8) void,
    print_number: *const fn (u64) void,
    where: []const u8,
) void {
    var active_total: u64 = 0;

    var pidx: usize = 0;
    while (pidx < self.process_capacity) : (pidx += 1) {
        if ((self.processDescriptorSlotConst(pidx) orelse continue).active) active_total += 1;
    }

    write("Kernel.mem_diag where=");
    write(where);
    @TypeOf(self.*).debugWriteField(write, print_number, "free_pages", @intCast(free_list.len));
    @TypeOf(self.*).debugWriteField(write, print_number, "free_ranges", @intCast(free_list.range_len));
    @TypeOf(self.*).debugWriteField(write, print_number, "process_capacity", @intCast(self.process_capacity));
    @TypeOf(self.*).debugWriteField(write, print_number, "active_processes", active_total);
    @TypeOf(self.*).debugWriteField(write, print_number, "tracked_active", @intCast(self.active_process_count));
    @TypeOf(self.*).debugWriteField(write, print_number, "vm_store_next", @intCast(vmo_backing_page_store_next));
    @TypeOf(self.*).debugWriteField(write, print_number, "vm_store_free_pages", vmObjectBackingFreePageCount());
    @TypeOf(self.*).debugWriteField(write, print_number, "vm_store_free_ranges", @intCast(vmo_backing_page_store_free_range_len));
    write("\n");

    pidx = 0;
    while (pidx < self.process_capacity) : (pidx += 1) {
        const desc = self.processDescriptorSlotConst(pidx) orelse continue;
        if (!desc.active) continue;

        write("Kernel.mem_diag.proc");
        @TypeOf(self.*).debugWriteField(write, print_number, "idx", @intCast(pidx));
        @TypeOf(self.*).debugWriteField(write, print_number, "active", if (desc.active) 1 else 0);
        write(" label=");
        write(desc.label);
        write("\n");
    }
}

pub fn allocPhysicalPage(
    self: anytype,
    free_list: *FreePageList,
) KernelError!PageCapability {
    const paddr = free_list.popFrontAtOrAbove(@TypeOf(self.*).low_memory_limit) catch |err| switch (err) {
        KernelError.OutOfFreePages => try free_list.popFrontBelow(@TypeOf(self.*).low_memory_limit),
        else => return err,
    };
    errdefer free_list.appendPage(0, paddr) catch {};
    try self.zeroAllocatedPage(paddr);
    return .{ .paddr = paddr };
}

pub fn allocLowPhysicalPage(
    self: anytype,
    free_list: *FreePageList,
) KernelError!PageCapability {
    const paddr = try free_list.popFrontBelow(@TypeOf(self.*).low_memory_limit);
    errdefer free_list.appendPage(0, paddr) catch {};
    try self.zeroAllocatedPage(paddr);
    return .{ .paddr = paddr };
}

pub fn zeroAllocatedPage(self: anytype, paddr: u64) KernelError!void {
    if (builtin.is_test) return;
    if (self.zero_physical_page_hook) |hook| {
        if (!hook(paddr)) return KernelError.InvalidState;
        return;
    }
    const page_bytes: [*]u8 = @ptrFromInt(paddr);
    @memset(page_bytes[0..4096], 0);
}
