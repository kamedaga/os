const std = @import("std");
const builtin = @import("builtin");
const x86_platform = @import("../arch/x86_64/platform.zig");
const types = @import("types.zig");
const capsule = types.capsule;
const initial_process_count = types.initial_process_count;
const initial_process_capacity = types.initial_process_capacity;
const process_count = types.process_count;
const process_principal_value_count = types.process_principal_value_count;
const initial_thread_capacity = types.initial_thread_capacity;
const thread_id_value_count = types.thread_id_value_count;
const device_principal_raw = types.device_principal_raw;
const PrincipalRaw = types.PrincipalRaw;
const PrincipalId = types.PrincipalId;
const CapsuleKind = types.CapsuleKind;
const CapsuleRights = types.CapsuleRights;
const CapsuleMetadata = types.CapsuleMetadata;
const CapsuleSnapshot = types.CapsuleSnapshot;
const CapsuleDmaDirection = types.CapsuleDmaDirection;
const CapsuleIrqKind = types.CapsuleIrqKind;
const MapProt = types.MapProt;
const ProcessDescriptor = types.ProcessDescriptor;
const ProcessStatus = types.ProcessStatus;
const DebugProcessLifecycleReason = types.DebugProcessLifecycleReason;
const KernelError = types.KernelError;
const DmaDeviceId = types.DmaDeviceId;
const invalid_dma_device_id = types.invalid_dma_device_id;
const Fd = types.Fd;
const fd_table_entries = types.fd_table_entries;
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
const max_ipc_queue_messages = types.max_ipc_queue_messages;
const max_ipc_message_fds = types.max_ipc_message_fds;
const IpcEndpointRef = types.IpcEndpointRef;
const IpcChannelRef = types.IpcChannelRef;
const IpcChannelHandle = types.IpcChannelHandle;
const IpcReplyRef = types.IpcReplyRef;
const IpcWaitKey = types.IpcWaitKey;
const IpcWaiter = types.IpcWaiter;
const ThreadWakeTarget = types.ThreadWakeTarget;
const TaskFdWaiter = types.TaskFdWaiter;
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
const RegionFreeRange = types.RegionFreeRange;
const FreePageList = types.FreePageList;
const PageCapability = types.PageCapability;
const empty_process_descriptors_extra = types.empty_process_descriptors_extra;
const empty_vma_tables_extra = types.empty_vma_tables_extra;
const processPrincipalFromIndex = types.processPrincipalFromIndex;
const processIndexFromPrincipal = types.processIndexFromPrincipal;
const principalLabel = types.principalLabel;
const fdFlagsFromBits = types.fdFlagsFromBits;
const fdFlagsToBits = types.fdFlagsToBits;
const fdRightsFromBits = types.fdRightsFromBits;
const fdRightsToBits = types.fdRightsToBits;
const isFdRightsSubset = types.isFdRightsSubset;
const vmObjectBackingStoreStats = types.vmObjectBackingStoreStats;
const allocEmptyVmoBackingPageStore = types.allocEmptyVmoBackingPageStore;
const vmoBackingPageStorePaddr = types.vmoBackingPageStorePaddr;
const setVmoBackingPageStorePaddr = types.setVmoBackingPageStorePaddr;
const freeVmoBackingPageStore = types.freeVmoBackingPageStore;
const resetVmoBackingPageStore = types.resetVmoBackingPageStore;
const kernelStaticStorageEndAddr = types.kernelStaticStorageEndAddr;
const runtimeStorageBytes = types.runtimeStorageBytes;
const initRuntimeStorage = types.initRuntimeStorage;

// Construction only: self is fresh storage, never a live owner. Reading its
// owner before initialization would itself be undefined behavior. Do not
// expose a reset alias: wiping a live owner loses published/retired/pending
// extents without either borrower exclusion or PMM return.
fn initializeStorageInPlace(self: anytype) void {
    @memset(std.mem.asBytes(self), 0);
    inline for (.{ "fd_objects", "pipes", "native_vmos", "ipc_endpoints", "ipc_channels", "ipc_replies", "task_fd_waiters", "fd_wait_groups", "fd_wait_group_links", "fd_wait_group_thread_counts", "irq_publish_slots" }) |name| @field(self, name) = .{};
    self.fd_process_capacity = types.process_count;
    self.process_timers = .{};
    // Force the first lock release to publish the (empty) timer deadline.
    self.timer_deadline_dirty = true;
    self.process_descriptors_extra = empty_process_descriptors_extra[0..];
    self.fd_tables_extra = .{};
    self.vma_tables_extra = &.{};
    self.process_capacity = process_count;
    for (&self.vma_tables) |*table| table.* = .{};
    for (&self.fd_tables) |*table| table.* = .{};
    self.aslr_secret = 0x6a09_e667_f3bc_c909;
    self.aslr_sequence = 1;

    {
        var pool_iter_12 = self.fd_objects.iterator();
        while (pool_iter_12.next()) |pool_entry_12| {
            const slot = pool_entry_12.value;
            slot.generation = 1;
        }
    }
    {
        var pool_iter_13 = self.pipes.iterator();
        while (pool_iter_13.next()) |pool_entry_13| {
            const slot = pool_entry_13.value;
            slot.generation = 1;
        }
    }
    {
        var pool_iter_14 = self.native_vmos.iterator();
        while (pool_iter_14.next()) |pool_entry_14| {
            const slot = pool_entry_14.value;
            slot.generation = 1;
        }
    }
    self.native_cow_tables = .{};
    self.reference_nodes = .{};
    self.object_reference_slots = .{};
    self.clock_reference = .{};
    {
        var pool_iter_15 = self.ipc_endpoints.iterator();
        while (pool_iter_15.next()) |pool_entry_15| {
            const slot = pool_entry_15.value;
            slot.generation = 1;
        }
    }
    {
        var pool_iter_16 = self.ipc_channels.iterator();
        while (pool_iter_16.next()) |pool_entry_16| {
            const slot = pool_entry_16.value;
            slot.generation = 1;
        }
    }
    {
        var pool_iter_17 = self.ipc_replies.iterator();
        while (pool_iter_17.next()) |pool_entry_17| {
            const slot = pool_entry_17.value;
            slot.generation = 1;
        }
    }
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
    initializeStorageInPlace(self);
    self.initPrincipalState();
}

pub fn initFromDetectedRegionsInPlace(self: anytype, region_count: usize) KernelError!void {
    if (region_count == 0) return KernelError.EmptyRegionSet;

    initializeStorageInPlace(self);
    if (builtin.is_test) {
        self.initPrincipalState();
    } else {
        self.initDynamicPrincipalState();
    }
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
    const process_capacity = self.processCapacitySnapshot();

    var pidx: usize = 0;
    while (pidx < process_capacity) : (pidx += 1) {
        if ((self.processDescriptorSlotConst(pidx) orelse continue).active) active_total += 1;
    }

    write("Kernel.mem_diag where=");
    write(where);
    const vm_store = vmObjectBackingStoreStats();
    @TypeOf(self.*).debugWriteField(write, print_number, "free_pages", @intCast(free_list.pageCount()));
    @TypeOf(self.*).debugWriteField(write, print_number, "free_ranges", @intCast(free_list.rangeCount()));
    @TypeOf(self.*).debugWriteField(write, print_number, "process_capacity", @intCast(process_capacity));
    @TypeOf(self.*).debugWriteField(write, print_number, "active_processes", active_total);
    @TypeOf(self.*).debugWriteField(write, print_number, "tracked_active", @intCast(self.active_process_count));
    @TypeOf(self.*).debugWriteField(write, print_number, "vm_store_used", vm_store.used_entries);
    @TypeOf(self.*).debugWriteField(write, print_number, "vm_store_capacity", vm_store.capacity);
    @TypeOf(self.*).debugWriteField(write, print_number, "vm_store_free", vm_store.free_entries);
    write("\n");

    pidx = 0;
    while (pidx < process_capacity) : (pidx += 1) {
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
    const paddr = try free_list.popPagePreferred(&.{
        .{ .minimum = @TypeOf(self.*).low_memory_limit, .limit = std.math.maxInt(u64) },
        .{ .minimum = 0, .limit = @TypeOf(self.*).low_memory_limit },
    });
    // Intrusive PMM needs no return metadata allocation. DMA-held PFNs
    // remain in the common retention ledger until device drain is proved.
    errdefer free_list.appendPage(paddr) catch |err| switch (err) {
        error.PageQuarantined => {},
        else => @panic("invalid PMM return"),
    };
    try self.zeroAllocatedPage(paddr);
    return .{ .paddr = paddr };
}

pub fn allocLowPhysicalPage(
    self: anytype,
    free_list: *FreePageList,
) KernelError!PageCapability {
    const paddr = try free_list.popFrontBelow(@TypeOf(self.*).low_memory_limit);
    errdefer free_list.appendPage(paddr) catch |err| switch (err) {
        error.PageQuarantined => {},
        else => @panic("invalid PMM return"),
    };
    try self.zeroAllocatedPage(paddr);
    return .{ .paddr = paddr };
}

pub fn zeroAllocatedPage(self: anytype, paddr: u64) KernelError!void {
    if (self.zero_physical_page_hook) |hook| {
        if (!hook(paddr)) return KernelError.InvalidState;
        return;
    }
    // Host fixtures may inject the same zeroing failure without dereferencing RAM.
    if (builtin.is_test) return;
    const page_bytes = @import("../arch/x86_64/physical_layout.zig").identitySlice(u8, .fromInt(paddr), 4096) orelse return KernelError.InvalidState;
    @memset(page_bytes[0..4096], 0);
}

/// KernelState remains held through reserve/use/rollback. The transaction is
/// invocation-local, but rejected workspace returns need an address-stable
/// owner and allocator context (the global PMM) after this invocation ends.
pub fn beginInvocationStorage(self: anytype, free_list: *FreePageList) @import("../memory/owned_storage.zig").Transaction {
    self.retryInvocationStorage();
    const allocator = if (builtin.is_test)
        self.invocation_test_allocator orelse (if (comptime @import("../lock_profile.zig").enabled) @import("../memory/identity_allocator.zig").allocatorFor(free_list, .invocation) else @import("../memory/identity_allocator.zig").allocator(free_list))
    else
        (if (comptime @import("../lock_profile.zig").enabled) @import("../memory/identity_allocator.zig").allocatorFor(free_list, .invocation) else @import("../memory/identity_allocator.zig").allocator(free_list));
    return @import("../memory/owned_storage.zig").Transaction.init(&self.descriptor_storage, allocator);
}

/// One finite pass under KernelState; never retry-spin on a full PMM table.
pub fn retryInvocationStorage(self: anytype) void {
    if (self.descriptor_storage.pending != null)
        self.descriptor_storage.retryUnpublished() catch @panic("moved invocation storage owner");
}
