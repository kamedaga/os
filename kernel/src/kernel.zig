const std = @import("std");

pub const types = @import("state/types.zig");
const memory_state = @import("state/memory.zig");
const process_state = @import("state/process.zig");
const fd_state = @import("state/fd.zig");
const vmo_state = @import("state/vmo.zig");
const vma_state = @import("state/vma.zig");
const pipe_state = @import("state/pipe.zig");
const ipc_state = @import("state/ipc.zig");

pub const capsule = types.capsule;
pub const initial_process_count = types.initial_process_count;
pub const initial_process_capacity = types.initial_process_capacity;
pub const process_count = types.process_count;
pub const max_process_slots = types.max_process_slots;
pub const initial_thread_capacity = types.initial_thread_capacity;
pub const max_thread_slots = types.max_thread_slots;
pub const device_count = types.device_count;
pub const device_principal_raw = types.device_principal_raw;
pub const principal_count = types.principal_count;
pub const PrincipalRaw = types.PrincipalRaw;
pub const PrincipalId = types.PrincipalId;
pub const CapsuleKind = types.CapsuleKind;
pub const CapsuleRights = types.CapsuleRights;
pub const CapsuleMetadata = types.CapsuleMetadata;
pub const CapsuleSnapshot = types.CapsuleSnapshot;
pub const CapsuleDmaDirection = types.CapsuleDmaDirection;
pub const CapsuleIrqKind = types.CapsuleIrqKind;
pub const MapProt = types.MapProt;
pub const EndpointRoute = types.EndpointRoute;
pub const Region = types.Region;
pub const ProcessDescriptor = types.ProcessDescriptor;
pub const ProcessStatus = types.ProcessStatus;
pub const DebugProcessLifecycleReason = types.DebugProcessLifecycleReason;
pub const KernelError = types.KernelError;
pub const DmaDeviceId = types.DmaDeviceId;
pub const invalid_dma_device_id = types.invalid_dma_device_id;
pub const Fd = types.Fd;
pub const fd_table_entries = types.fd_table_entries;
pub const max_fd_objects = types.max_fd_objects;
pub const max_pipes = types.max_pipes;
pub const pipe_buffer_bytes = types.pipe_buffer_bytes;
pub const fd_known_flags_mask = types.fd_known_flags_mask;
pub const fd_known_rights_mask = types.fd_known_rights_mask;
pub const FdFlags = types.FdFlags;
pub const FdRights = types.FdRights;
pub const KernelObjectKind = types.KernelObjectKind;
pub const TaskObjectState = types.TaskObjectState;
pub const ProcessObject = types.ProcessObject;
pub const ThreadObject = types.ThreadObject;
pub const DeviceObject = types.DeviceObject;
pub const MmioRegionObject = types.MmioRegionObject;
pub const DmaBufferObject = types.DmaBufferObject;
pub const DmaMappingObject = types.DmaMappingObject;
pub const IrqObject = types.IrqObject;
pub const IrqPublishSlot = types.IrqPublishSlot;
pub const TimerObject = types.TimerObject;
pub const TimerFdState = types.TimerFdState;
pub const SerialObject = types.SerialObject;
pub const SchedulerControlObject = types.SchedulerControlObject;
pub const SchedulerEventObject = types.SchedulerEventObject;
pub const PipeRef = types.PipeRef;
pub const PipeEndpointObject = types.PipeEndpointObject;
pub const PipePair = types.PipePair;
pub const PipeIoError = types.PipeIoError;
pub const KernelObjectRef = types.KernelObjectRef;
pub const KernelObjectPayload = types.KernelObjectPayload;
pub const KernelObjectSlot = types.KernelObjectSlot;
pub const FdEntry = types.FdEntry;
pub const FdInfo = types.FdInfo;
pub const FdTable = types.FdTable;
pub const FdTransferMode = types.FdTransferMode;
pub const max_ipc_endpoints = types.max_ipc_endpoints;
pub const max_ipc_channels = types.max_ipc_channels;
pub const max_ipc_replies = types.max_ipc_replies;
pub const max_ipc_queue_messages = types.max_ipc_queue_messages;
pub const max_ipc_message_fds = types.max_ipc_message_fds;
pub const max_ipc_waiters = types.max_ipc_waiters;
pub const IpcEndpointRef = types.IpcEndpointRef;
pub const IpcChannelRef = types.IpcChannelRef;
pub const IpcChannelHandle = types.IpcChannelHandle;
pub const IpcReplyRef = types.IpcReplyRef;
pub const IpcWaitKey = types.IpcWaitKey;
pub const IpcWaiter = types.IpcWaiter;
pub const ThreadWakeTarget = types.ThreadWakeTarget;
pub const TaskFdWaiter = types.TaskFdWaiter;
pub const max_task_fd_waiters = types.max_task_fd_waiters;
pub const max_ipc_object_waiters = types.max_ipc_object_waiters;
pub const PipeSlot = types.PipeSlot;
pub const IpcWaitList = types.IpcWaitList;
pub const IpcTransferredFd = types.IpcTransferredFd;
pub const IpcMessage = types.IpcMessage;
pub const IpcQueue = types.IpcQueue;
pub const IpcEndpointSlot = types.IpcEndpointSlot;
pub const IpcChannelSlot = types.IpcChannelSlot;
pub const IpcReplySlot = types.IpcReplySlot;
pub const IpcSendFd = types.IpcSendFd;
pub const IpcSendMessage = types.IpcSendMessage;
pub const IpcRecvFd = types.IpcRecvFd;
pub const IpcRecvResult = types.IpcRecvResult;
pub const native_page_size = types.native_page_size;
pub const max_native_vmos = types.max_native_vmos;
pub const max_vmas_per_process = types.max_vmas_per_process;
pub const max_native_cow_tables = types.max_native_cow_tables;
pub const NativeVmoKind = types.NativeVmoKind;
pub const NativeVmoRef = types.NativeVmoRef;
pub const NativeCowTableRef = types.NativeCowTableRef;
pub const NativeVmoSlot = types.NativeVmoSlot;
pub const NativeCowTableSlot = types.NativeCowTableSlot;
pub const VmaProt = types.VmaProt;
pub const MmapFlags = types.MmapFlags;
pub const VmaEntry = types.VmaEntry;
pub const VmaTable = types.VmaTable;
pub const NativeVmaFaultMapping = types.NativeVmaFaultMapping;
pub const EndpointTable = types.EndpointTable;
pub const PublishedEndpointTable = types.PublishedEndpointTable;
pub const max_vmo_backing_pages = types.max_vmo_backing_pages;
pub const max_vmo_backing_store_pages = types.max_vmo_backing_store_pages;
pub const max_vmo_backing_store_free_ranges = types.max_vmo_backing_store_free_ranges;
pub const VmoBackingStoreFreeRange = types.VmoBackingStoreFreeRange;
pub const RegionFreeRange = types.RegionFreeRange;
pub const FreePageList = types.FreePageList;
pub const PageCapability = types.PageCapability;
pub const empty_vmo_backing_page_store = types.empty_vmo_backing_page_store;
pub const vmo_backing_page_store = types.vmo_backing_page_store;
pub const vmo_backing_page_store_next = types.vmo_backing_page_store_next;
pub const empty_vmo_backing_page_store_free_ranges = types.empty_vmo_backing_page_store_free_ranges;
pub const vmo_backing_page_store_free_ranges = types.vmo_backing_page_store_free_ranges;
pub const vmo_backing_page_store_free_range_len = types.vmo_backing_page_store_free_range_len;
pub const empty_process_descriptors_extra = types.empty_process_descriptors_extra;
pub const empty_endpoint_tables_extra = types.empty_endpoint_tables_extra;
pub const empty_fd_tables_extra = types.empty_fd_tables_extra;
pub const empty_vma_tables_extra = types.empty_vma_tables_extra;
pub const processPrincipalFromIndex = types.processPrincipalFromIndex;
pub const processIndexFromPrincipal = types.processIndexFromPrincipal;
pub const principalLabel = types.principalLabel;
pub const fdFlagsFromBits = types.fdFlagsFromBits;
pub const fdFlagsToBits = types.fdFlagsToBits;
pub const fdRightsFromBits = types.fdRightsFromBits;
pub const fdRightsToBits = types.fdRightsToBits;
pub const isFdRightsSubset = types.isFdRightsSubset;
pub const vmObjectBackingFreePageCount = types.vmObjectBackingFreePageCount;
pub const removeVmoBackingFreeRange = types.removeVmoBackingFreeRange;
pub const insertVmoBackingFreeRange = types.insertVmoBackingFreeRange;
pub const allocEmptyVmoBackingPageStore = types.allocEmptyVmoBackingPageStore;
pub const vmoBackingPageStorePaddr = types.vmoBackingPageStorePaddr;
pub const setVmoBackingPageStorePaddr = types.setVmoBackingPageStorePaddr;
pub const freeVmoBackingPageStore = types.freeVmoBackingPageStore;
pub const resetVmoBackingPageStore = types.resetVmoBackingPageStore;
pub const kernelStaticStorageEndAddr = types.kernelStaticStorageEndAddr;
pub const runtimeStorageBytes = types.runtimeStorageBytes;
pub const initRuntimeStorage = types.initRuntimeStorage;

pub const KernelState = struct {
    pub const max_regions = 256;
    pub const low_memory_limit: u64 = 4 * 1024 * 1024 * 1024;

    regions: [max_regions]Region = undefined,
    region_len: usize = 0,
    process_descriptors: [process_count]ProcessDescriptor = [_]ProcessDescriptor{.{}} ** process_count,
    process_descriptors_extra: []ProcessDescriptor = empty_process_descriptors_extra[0..],
    process_capacity: usize = process_count,
    active_process_count: usize = 0,
    endpoint_tables: [principal_count]EndpointTable = [_]EndpointTable{.{}} ** principal_count,
    endpoint_tables_extra: []EndpointTable = empty_endpoint_tables_extra[0..],
    published_service_endpoints: PublishedEndpointTable = .{},
    endpoint_generation: u64 = 0,
    fd_tables: [process_count]FdTable = [_]FdTable{.{}} ** process_count,
    vma_tables: [process_count]VmaTable = [_]VmaTable{.{}} ** process_count,
    fd_tables_extra: []FdTable = empty_fd_tables_extra[0..],
    vma_tables_extra: []VmaTable = empty_vma_tables_extra[0..],
    fd_objects: [max_fd_objects]KernelObjectSlot = [_]KernelObjectSlot{.{}} ** max_fd_objects,
    pipes: [max_pipes]PipeSlot = [_]PipeSlot{.{}} ** max_pipes,
    task_fd_waiters: [max_task_fd_waiters]TaskFdWaiter = [_]TaskFdWaiter{.{}} ** max_task_fd_waiters,
    irq_publish_slots: [max_fd_objects]IrqPublishSlot = [_]IrqPublishSlot{.{}} ** max_fd_objects,
    next_fd_object_scan: usize = 0,
    next_pipe_scan: usize = 0,
    native_vmos: [max_native_vmos]NativeVmoSlot = [_]NativeVmoSlot{.{}} ** max_native_vmos,
    next_native_vmo_scan: usize = 0,
    native_cow_tables: [max_native_cow_tables]NativeCowTableSlot = [_]NativeCowTableSlot{.{}} ** max_native_cow_tables,
    next_native_cow_table_scan: usize = 0,
    aslr_secret: u64 = 0x6a09_e667_f3bc_c909,
    aslr_sequence: u64 = 1,
    ipc_endpoints: [max_ipc_endpoints]IpcEndpointSlot = [_]IpcEndpointSlot{.{}} ** max_ipc_endpoints,
    next_ipc_endpoint_scan: usize = 0,
    ipc_channels: [max_ipc_channels]IpcChannelSlot = [_]IpcChannelSlot{.{}} ** max_ipc_channels,
    next_ipc_channel_scan: usize = 0,
    ipc_replies: [max_ipc_replies]IpcReplySlot = [_]IpcReplySlot{.{}} ** max_ipc_replies,
    next_ipc_reply_scan: usize = 0,
    zero_physical_page_hook: ?*const fn (paddr: u64) bool = null,
    debug_process_lifecycle_hook: ?*const fn (state: *const KernelState, principal: PrincipalId, reason: DebugProcessLifecycleReason) void = null,

    pub const IpcChannelPair = struct {
        a: Fd,
        b: Fd,
    };

    pub const resetStorageInPlace = memory_state.resetStorageInPlace;
    pub const pageAlignUp = memory_state.pageAlignUp;
    pub const pageAlignDown = memory_state.pageAlignDown;
    pub const isPageAligned = memory_state.isPageAligned;
    pub const mixAslr64 = memory_state.mixAslr64;
    pub const nextAslrWord = memory_state.nextAslrWord;
    pub const fillRandomBytes = memory_state.fillRandomBytes;
    pub const findRandomizedFreeUserMapVa = memory_state.findRandomizedFreeUserMapVa;
    pub const checkedEnd = vma_state.checkedEnd;
    pub const nativeVmoSlot = vmo_state.nativeVmoSlot;
    pub const nativeVmoSlotConst = vmo_state.nativeVmoSlotConst;
    pub const clearNativeVmoSlot = vmo_state.clearNativeVmoSlot;
    pub const releaseNativeVmoOwnedPages = vmo_state.releaseNativeVmoOwnedPages;
    pub const createNativeVmoWithPageStore = vmo_state.createNativeVmoWithPageStore;
    pub const createNativeVmo = vmo_state.createNativeVmo;
    pub const ensureNativeVmoPageStore = vmo_state.ensureNativeVmoPageStore;
    pub const retainNativeVmo = vmo_state.retainNativeVmo;
    pub const releaseNativeVmo = vmo_state.releaseNativeVmo;
    pub const releaseNativeVmoWithFreeList = vmo_state.releaseNativeVmoWithFreeList;
    pub const releaseIpcMessage = ipc_state.releaseIpcMessage;
    pub const releaseIpcMessageWithFreeList = ipc_state.releaseIpcMessageWithFreeList;
    pub const clearIpcQueue = ipc_state.clearIpcQueue;
    pub const clearIpcQueueWithFreeList = ipc_state.clearIpcQueueWithFreeList;
    pub const ipcEndpointSlot = ipc_state.ipcEndpointSlot;
    pub const ipcEndpointSlotConst = ipc_state.ipcEndpointSlotConst;
    pub const clearIpcEndpointSlot = ipc_state.clearIpcEndpointSlot;
    pub const clearIpcEndpointSlotWithFreeList = ipc_state.clearIpcEndpointSlotWithFreeList;
    pub const createIpcEndpoint = ipc_state.createIpcEndpoint;
    pub const ipcChannelSlot = ipc_state.ipcChannelSlot;
    pub const ipcChannelSlotConst = ipc_state.ipcChannelSlotConst;
    pub const createIpcChannel = ipc_state.createIpcChannel;
    pub const releaseIpcChannelHandle = ipc_state.releaseIpcChannelHandle;
    pub const releaseIpcChannelHandleWithFreeList = ipc_state.releaseIpcChannelHandleWithFreeList;
    pub const ipcReplySlot = ipc_state.ipcReplySlot;
    pub const ipcReplySlotConst = ipc_state.ipcReplySlotConst;
    pub const clearIpcReplySlot = ipc_state.clearIpcReplySlot;
    pub const clearIpcReplySlotWithFreeList = ipc_state.clearIpcReplySlotWithFreeList;
    pub const createIpcReply = ipc_state.createIpcReply;
    pub const pipeSlot = pipe_state.pipeSlot;
    pub const pipeSlotConst = pipe_state.pipeSlotConst;
    pub const createPipe = pipe_state.createPipe;
    pub const clearPipeSlot = pipe_state.clearPipeSlot;
    pub const releasePipeEndpoint = pipe_state.releasePipeEndpoint;
    pub const pipeEndpointFromPayload = pipe_state.pipeEndpointFromPayload;
    pub const pipeEndpointForFd = pipe_state.pipeEndpointForFd;
    pub const pipeUsed = pipe_state.pipeUsed;
    pub const pipeFree = pipe_state.pipeFree;
    pub const pipeReadyEventsForEndpoint = pipe_state.pipeReadyEventsForEndpoint;
    pub const pipeReadyEventsForFd = pipe_state.pipeReadyEventsForFd;
    pub const pipeReadyEventsForSide = pipe_state.pipeReadyEventsForSide;
    pub const pipeReadBytes = pipe_state.pipeReadBytes;
    pub const pipeWriteBytes = pipe_state.pipeWriteBytes;
    pub const resetNativeIpcObjects = ipc_state.resetNativeIpcObjects;
    pub const nativeVmoRefCount = vmo_state.nativeVmoRefCount;
    pub const nativeVmoSize = vmo_state.nativeVmoSize;
    pub const nativeVmoHasPageStore = vmo_state.nativeVmoHasPageStore;
    pub const nativeVmoPagePaddr = vmo_state.nativeVmoPagePaddr;
    pub const nativeVmoPagePaddrOrHole = vmo_state.nativeVmoPagePaddrOrHole;
    pub const nativeVmoOwnPagePaddr = vmo_state.nativeVmoOwnPagePaddr;
    pub const nativeVmoResolvedPagePaddr = vmo_state.nativeVmoResolvedPagePaddr;
    pub const nativeVmoHasParent = vmo_state.nativeVmoHasParent;
    pub const nativeVmoIsShadow = vmo_state.nativeVmoIsShadow;
    pub const nativeVmoRefsEqual = vmo_state.nativeVmoRefsEqual;
    pub const nativeCowTableSlot = vmo_state.nativeCowTableSlot;
    pub const nativeCowTableSlotConst = vmo_state.nativeCowTableSlotConst;
    pub const createNativeCowTable = vmo_state.createNativeCowTable;
    pub const retainNativeCowTable = vmo_state.retainNativeCowTable;
    pub const clearNativeCowPageSlots = vmo_state.clearNativeCowPageSlots;
    pub const releaseNativeCowTable = vmo_state.releaseNativeCowTable;
    pub const nativeCowPagePaddr = vmo_state.nativeCowPagePaddr;
    pub const nativeCowTableIsUnique = vmo_state.nativeCowTableIsUnique;
    pub const dirtyCowMappingProt = vmo_state.dirtyCowMappingProt;
    pub const setNativeCowPagePaddr = vmo_state.setNativeCowPagePaddr;
    pub const entryCowPageIndex = vmo_state.entryCowPageIndex;
    pub const entryDirtyPagePaddr = vmo_state.entryDirtyPagePaddr;
    pub const ensureEntryCowTable = vmo_state.ensureEntryCowTable;
    pub const detachSharedEntryCowTable = vmo_state.detachSharedEntryCowTable;
    pub const releaseUnmappedAnonymousVmoPageRange = vmo_state.releaseUnmappedAnonymousVmoPageRange;
    pub const readFdVmoBytes = vmo_state.readFdVmoBytes;
    pub const installNativeVmoPages = vmo_state.installNativeVmoPages;
    pub const replaceNativeVmoContiguousPages = vmo_state.replaceNativeVmoContiguousPages;
    pub const fdIndex = fd_state.fdIndex;
    pub const findFreeFd = fd_state.findFreeFd;
    pub const nextObjectGeneration = fd_state.nextObjectGeneration;
    pub const objectOwner = fd_state.objectOwner;
    pub const irqPublishSlotForRef = fd_state.irqPublishSlotForRef;
    pub const irqPublishSlotForRefConst = fd_state.irqPublishSlotForRefConst;
    pub const publishIrqObject = fd_state.publishIrqObject;
    pub const unpublishIrqObject = fd_state.unpublishIrqObject;
    pub const irqPublishedEventCount = fd_state.irqPublishedEventCount;
    pub const releaseMmioRegionObject = fd_state.releaseMmioRegionObject;
    pub const releaseDmaBufferObject = fd_state.releaseDmaBufferObject;
    pub const releaseDmaMappingObject = fd_state.releaseDmaMappingObject;
    pub const releaseIrqObject = fd_state.releaseIrqObject;
    pub const objectPayloadMatches = fd_state.objectPayloadMatches;
    pub const releaseKernelObjectPayload = fd_state.releaseKernelObjectPayload;
    pub const releaseKernelObjectPayloadWithFreeList = fd_state.releaseKernelObjectPayloadWithFreeList;
    pub const clearKernelObjectSlot = fd_state.clearKernelObjectSlot;
    pub const clearKernelObjectSlotWithFreeList = fd_state.clearKernelObjectSlotWithFreeList;
    pub const resetKernelObjectTable = fd_state.resetKernelObjectTable;
    pub const resetNativeVmoTable = fd_state.resetNativeVmoTable;
    pub const createKernelObject = fd_state.createKernelObject;
    pub const kernelObjectSlot = fd_state.kernelObjectSlot;
    pub const kernelObjectSlotConst = fd_state.kernelObjectSlotConst;
    pub const retainKernelObject = fd_state.retainKernelObject;
    pub const releaseKernelObject = fd_state.releaseKernelObject;
    pub const releaseKernelObjectWithFreeList = fd_state.releaseKernelObjectWithFreeList;
    pub const kernelObjectRefCount = fd_state.kernelObjectRefCount;
    pub const allocKernelSlice = process_state.allocKernelSlice;
    pub const processPrincipal = process_state.processPrincipal;
    pub const isActiveProcess = process_state.isActiveProcess;
    pub const processDescriptor = process_state.processDescriptor;
    pub const processStatus = process_state.processStatus;
    pub const isBootstrapOwner = process_state.isBootstrapOwner;
    pub const hasActivePrincipal = process_state.hasActivePrincipal;
    pub const requireActiveProcess = process_state.requireActiveProcess;
    pub const requireActivePrincipal = process_state.requireActivePrincipal;
    pub const principalStorageIndex = process_state.principalStorageIndex;
    pub const extraIndex = process_state.extraIndex;
    pub const processDescriptorSlot = process_state.processDescriptorSlot;
    pub const processDescriptorSlotConst = process_state.processDescriptorSlotConst;
    pub const endpointTableForProcessIndex = process_state.endpointTableForProcessIndex;
    pub const endpointTableForProcessIndexConst = process_state.endpointTableForProcessIndexConst;
    pub const fdTableForProcessIndex = process_state.fdTableForProcessIndex;
    pub const fdTableForProcessIndexConst = process_state.fdTableForProcessIndexConst;
    pub const vmaTableForProcessIndex = process_state.vmaTableForProcessIndex;
    pub const vmaTableForProcessIndexConst = process_state.vmaTableForProcessIndexConst;
    pub const getFdTable = process_state.getFdTable;
    pub const getFdTableConst = process_state.getFdTableConst;
    pub const inheritFdsForProcessCreate = process_state.inheritFdsForProcessCreate;
    pub const cloneFdTableForFork = process_state.cloneFdTableForFork;
    pub const cloneVmaTableForFork = vma_state.cloneVmaTableForFork;
    pub const copyForkAnonymousPresentPageToChild = vma_state.copyForkAnonymousPresentPageToChild;
    pub const getVmaTable = vma_state.getVmaTable;
    pub const getVmaTableConst = vma_state.getVmaTableConst;
    pub const fdTableForActiveProcess = fd_state.fdTableForActiveProcess;
    pub const fdTableForActiveProcessConst = fd_state.fdTableForActiveProcessConst;
    pub const fdEntryConst = fd_state.fdEntryConst;
    pub const vmaEntryConst = vma_state.vmaEntryConst;
    pub const vmaEntryForVaConst = vma_state.vmaEntryForVaConst;
    pub const vmaProtAllowsFault = vma_state.vmaProtAllowsFault;
    pub const nativeFaultMappingProt = vma_state.nativeFaultMappingProt;
    pub const nativeVmaFaultMapping = vma_state.nativeVmaFaultMapping;
    pub const nativeVmaInitialMapping = vma_state.nativeVmaInitialMapping;
    pub const ensureNativeVmaFaultMapping = vma_state.ensureNativeVmaFaultMapping;
    pub const copyPhysicalPage = vma_state.copyPhysicalPage;
    pub const replaceVmaPageWithAnonymousPrivatePage = vma_state.replaceVmaPageWithAnonymousPrivatePage;
    pub const ensureNativeVmaCowMapping = vma_state.ensureNativeVmaCowMapping;
    pub const packNativeVmaContiguousMapping = vma_state.packNativeVmaContiguousMapping;
    pub const setVmaProtRange = vma_state.setVmaProtRange;
    pub const nativeVmoRefForFd = vmo_state.nativeVmoRefForFd;
    pub const nativeVmoRefForKernelObject = vmo_state.nativeVmoRefForKernelObject;
    pub const kernelObjectMatchesNativeVmo = vmo_state.kernelObjectMatchesNativeVmo;
    pub const nativeVmoRefForRevokeFd = vmo_state.nativeVmoRefForRevokeFd;
    pub const revokeNativeVmoFromFdTablesWithFreeList = vmo_state.revokeNativeVmoFromFdTablesWithFreeList;
    pub const revokeNativeVmoFromIpcQueueWithFreeList = vmo_state.revokeNativeVmoFromIpcQueueWithFreeList;
    pub const revokeNativeVmoFromIpcMessagesWithFreeList = vmo_state.revokeNativeVmoFromIpcMessagesWithFreeList;
    pub const revokeNativeVmoFromVmaTablesWithFreeList = vmo_state.revokeNativeVmoFromVmaTablesWithFreeList;
    pub const revokeVmoFdWithFreeList = vmo_state.revokeVmoFdWithFreeList;
    pub const releaseFdTableForProcessIndex = vma_state.releaseFdTableForProcessIndex;
    pub const releaseFdTableForProcessIndexWithFreeList = vma_state.releaseFdTableForProcessIndexWithFreeList;
    pub const releaseVmaCowPageRange = vma_state.releaseVmaCowPageRange;
    pub const releaseVmaCowResources = vma_state.releaseVmaCowResources;
    pub const releaseVmaTableForProcessIndex = vma_state.releaseVmaTableForProcessIndex;
    pub const releaseVmaTableForProcessIndexWithFreeList = vma_state.releaseVmaTableForProcessIndexWithFreeList;
    pub const releasePrincipalNativeMemory = vma_state.releasePrincipalNativeMemory;
    pub const replaceVmaTableForExec = vma_state.replaceVmaTableForExec;
    pub const resetProcessRuntimeTables = vma_state.resetProcessRuntimeTables;
    pub const findFreeVma = vma_state.findFreeVma;
    pub const installVmaEntry = vma_state.installVmaEntry;
    pub const clearVmaEntry = vma_state.clearVmaEntry;
    pub const removeActiveVmaIndex = vma_state.removeActiveVmaIndex;
    pub const vmaRangeOverlaps = vma_state.vmaRangeOverlaps;
    pub const findFreeUserMapVa = vma_state.findFreeUserMapVa;
    pub const userMapRangeIsFree = vma_state.userMapRangeIsFree;
    pub const vmaProtAllowedByRights = vma_state.vmaProtAllowedByRights;
    pub const createAnonymousVmoFd = vma_state.createAnonymousVmoFd;
    pub const createAnonymousVmoFdWithPages = vma_state.createAnonymousVmoFdWithPages;
    pub const createAnonymousVmaWithPages = vma_state.createAnonymousVmaWithPages;
    pub const createVmaWithRetainedVmo = vma_state.createVmaWithRetainedVmo;
    pub const fdInfo = fd_state.fdInfo;
    pub const eventReadCounter = fd_state.eventReadCounter;
    pub const eventWriteCounter = fd_state.eventWriteCounter;
    pub const eventWakeOwnersForFd = fd_state.eventWakeOwnersForFd;
    pub const timerDueCount = fd_state.timerDueCount;
    pub const timerNextWakeTick = fd_state.timerNextWakeTick;
    pub const timerReadExpirations = fd_state.timerReadExpirations;
    pub const timerFdState = fd_state.timerFdState;
    pub const setTimerFd = fd_state.setTimerFd;
    pub const fdIpcReadable = fd_state.fdIpcReadable;
    pub const fdIpcWritable = fd_state.fdIpcWritable;
    pub const fdPollEvents = fd_state.fdPollEvents;
    pub const fdNextWakeTick = fd_state.fdNextWakeTick;
    pub const fdPayloadWithRightsConst = fd_state.fdPayloadWithRightsConst;
    pub const fdPayloadWithRights = fd_state.fdPayloadWithRights;
    pub const processFromPayload = process_state.processFromPayload;
    pub const threadFromPayload = process_state.threadFromPayload;
    pub const processObjectForFd = process_state.processObjectForFd;
    pub const threadObjectForFd = process_state.threadObjectForFd;
    pub const createProcessFd = process_state.createProcessFd;
    pub const createThreadFd = process_state.createThreadFd;
    pub const setProcessObjectStateForFd = process_state.setProcessObjectStateForFd;
    pub const setThreadObjectStateForFd = process_state.setThreadObjectStateForFd;
    pub const markProcessObjectsExited = process_state.markProcessObjectsExited;
    pub const markThreadObjectsExitedForPrincipal = process_state.markThreadObjectsExitedForPrincipal;
    pub const markThreadObjectsExitedBySlot = process_state.markThreadObjectsExitedBySlot;
    pub const deviceObjectForFd = fd_state.deviceObjectForFd;
    pub const dmaBufferObjectForFd = fd_state.dmaBufferObjectForFd;
    pub const irqObjectForFd = fd_state.irqObjectForFd;
    pub const irqKindMatchesInterrupt = fd_state.irqKindMatchesInterrupt;
    pub const appendUniquePrincipal = fd_state.appendUniquePrincipal;
    pub const recordDeviceInterruptEvent = fd_state.recordDeviceInterruptEvent;
    pub const irqEventCountForFd = fd_state.irqEventCountForFd;
    pub const createDeviceFd = fd_state.createDeviceFd;
    pub const createMmioRegionFd = fd_state.createMmioRegionFd;
    pub const createDmaBufferFd = fd_state.createDmaBufferFd;
    pub const createDmaMappingFd = fd_state.createDmaMappingFd;
    pub const createIrqFd = fd_state.createIrqFd;
    pub const createTimerFd = fd_state.createTimerFd;
    pub const createEventFd = fd_state.createEventFd;
    pub const pipeRights = pipe_state.pipeRights;
    pub const createPipePairFds = pipe_state.createPipePairFds;
    pub const createSerialFdAt = fd_state.createSerialFdAt;
    pub const createSchedulerControlFdAt = fd_state.createSchedulerControlFdAt;
    pub const createSchedulerEventFdAt = fd_state.createSchedulerEventFdAt;
    pub const mmapFd = vma_state.mmapFd;
    pub const mmapFdIntoProcess = vma_state.mmapFdIntoProcess;
    pub const munmapRangeWithFreeList = vma_state.munmapRangeWithFreeList;
    pub const mremapRangeWithFreeList = vma_state.mremapRangeWithFreeList;
    pub const installFd = fd_state.installFd;
    pub const closeFd = fd_state.closeFd;
    pub const closeFdWithFreeList = fd_state.closeFdWithFreeList;
    pub const closeCloexecFdsWithFreeList = fd_state.closeCloexecFdsWithFreeList;
    pub const dupFd = fd_state.dupFd;
    pub const replaceFd = fd_state.replaceFd;
    pub const setFdFlags = fd_state.setFdFlags;
    pub const transferFd = fd_state.transferFd;
    pub const endpointRights = ipc_state.endpointRights;
    pub const replyReceiveRights = ipc_state.replyReceiveRights;
    pub const replySendRights = ipc_state.replySendRights;
    pub const createIpcEndpointFd = ipc_state.createIpcEndpointFd;
    pub const createIpcChannelPairFds = ipc_state.createIpcChannelPairFds;
    pub const ipcMessageQueueForSend = ipc_state.ipcMessageQueueForSend;
    pub const markReplySentIfNeeded = ipc_state.markReplySentIfNeeded;
    pub const endpointRefEqual = ipc_state.endpointRefEqual;
    pub const channelRefEqual = ipc_state.channelRefEqual;
    pub const replyRefEqual = ipc_state.replyRefEqual;
    pub const ipcPayloadReceivesFromSendPayload = ipc_state.ipcPayloadReceivesFromSendPayload;
    pub const ipcRecvWaitKeyFromPayload = ipc_state.ipcRecvWaitKeyFromPayload;
    pub const ipcRecvWaitKeyFromSendPayload = ipc_state.ipcRecvWaitKeyFromSendPayload;
    pub const ipcWaitListForRecvPayload = ipc_state.ipcWaitListForRecvPayload;
    pub const ipcWaitListForSendPayload = ipc_state.ipcWaitListForSendPayload;
    pub const registerIpcReadableWaiterForFd = ipc_state.registerIpcReadableWaiterForFd;
    pub const registerIpcRecvCompletionWaiterForFd = ipc_state.registerIpcRecvCompletionWaiterForFd;
    pub const unregisterIpcReadableWaiterForFd = ipc_state.unregisterIpcReadableWaiterForFd;
    pub const pipeWaitListForEndpoint = pipe_state.pipeWaitListForEndpoint;
    pub const registerPipeWaiterForFd = pipe_state.registerPipeWaiterForFd;
    pub const unregisterPipeWaiterForFd = pipe_state.unregisterPipeWaiterForFd;
    pub const takePipeWaiters = pipe_state.takePipeWaiters;
    pub const registerTaskReadableWaiterForFd = fd_state.registerTaskReadableWaiterForFd;
    pub const unregisterTaskReadableWaiterForThread = fd_state.unregisterTaskReadableWaiterForThread;
    pub const takeTaskReadableWaitersForPrincipal = fd_state.takeTaskReadableWaitersForPrincipal;
    pub const wakeIpcWaitersForSendFd = ipc_state.wakeIpcWaitersForSendFd;
    pub const ipcRecvWakeOwnersForSendFd = ipc_state.ipcRecvWakeOwnersForSendFd;
    pub const ipcMessageQueueForRecv = ipc_state.ipcMessageQueueForRecv;
    pub const fdFreeCountFrom = ipc_state.fdFreeCountFrom;
    pub const validateIpcSendFds = ipc_state.validateIpcSendFds;
    pub const appendIpcSendFd = ipc_state.appendIpcSendFd;
    pub const closeMovedIpcSendFdsWithFreeList = ipc_state.closeMovedIpcSendFdsWithFreeList;
    pub const buildIpcMessage = ipc_state.buildIpcMessage;
    pub const enqueueIpcMessage = ipc_state.enqueueIpcMessage;
    pub const ipcSend = ipc_state.ipcSend;
    pub const ipcReply = ipc_state.ipcReply;
    pub const ipcCall = ipc_state.ipcCall;
    pub const ipcRecv = ipc_state.ipcRecv;
    pub const nextProcessCapacity = process_state.nextProcessCapacity;
    pub const ensureProcessCapacity = process_state.ensureProcessCapacity;
    pub const clearPrincipalTablesForReuse = process_state.clearPrincipalTablesForReuse;
    pub const createProcessDescriptor = process_state.createProcessDescriptor;
    pub const createProcessDescriptorWithCapacity = process_state.createProcessDescriptorWithCapacity;
    pub const ensureProcessDescriptor = process_state.ensureProcessDescriptor;
    pub const setBootstrapOwner = process_state.setBootstrapOwner;
    pub const markProcessFaulted = process_state.markProcessFaulted;
    pub const markProcessExited = process_state.markProcessExited;
    pub const removeProcessDescriptor = process_state.removeProcessDescriptor;
    pub const clearPrincipalState = process_state.clearPrincipalState;
    pub const initPrincipalState = process_state.initPrincipalState;
    pub const initDynamicPrincipalState = process_state.initDynamicPrincipalState;
    pub const initPhase1InPlace = memory_state.initPhase1InPlace;
    pub fn initPhase1() KernelState {
        var state: KernelState = undefined;
        state.initPhase1InPlace();
        return state;
    }
    pub const initFromDetectedRegionsInPlace = memory_state.initFromDetectedRegionsInPlace;
    pub fn initFromDetectedRegions(region_count: usize) KernelError!KernelState {
        var state: KernelState = undefined;
        try state.initFromDetectedRegionsInPlace(region_count);
        return state;
    }
    pub const getRegion = memory_state.getRegion;
    pub const getRegionConst = memory_state.getRegionConst;
    pub const getEndpointTable = process_state.getEndpointTable;
    pub const getEndpointTableConst = process_state.getEndpointTableConst;
    pub const bumpEndpointGeneration = process_state.bumpEndpointGeneration;
    pub const installEndpoint = process_state.installEndpoint;
    pub const installServiceEndpointForActiveProcesses = process_state.installServiceEndpointForActiveProcesses;
    pub const publishServiceEndpoint = process_state.publishServiceEndpoint;
    pub const unpublishServiceEndpointsForTarget = process_state.unpublishServiceEndpointsForTarget;
    pub const endpointTargetFor = process_state.endpointTargetFor;
    pub const endpointTargetForKnownActiveOwner = process_state.endpointTargetForKnownActiveOwner;
    pub const debugWriteField = memory_state.debugWriteField;
    pub const debugLogMemoryOwnership = memory_state.debugLogMemoryOwnership;
    pub const allocPhysicalPage = memory_state.allocPhysicalPage;
    pub const allocLowPhysicalPage = memory_state.allocLowPhysicalPage;
    pub const zeroAllocatedPage = memory_state.zeroAllocatedPage;
};

fn containsCapId(ids: []const u64, target: u64) bool {
    for (ids) |id| {
        if (id == target) return true;
    }
    return false;
}

test "ipc send wake owner scan follows channel receive side" {
    var state = KernelState.initPhase1();
    const sender = processPrincipalFromIndex(0).?;
    const receiver = processPrincipalFromIndex(1).?;
    const rights = FdRights{
        .inspect = true,
        .transfer = true,
        .wait = true,
        .poll = true,
        .close = true,
        .send = true,
        .recv = true,
    };
    const pair = try state.createIpcChannelPairFds(sender, rights, .{}, 16);
    const receiver_fd = try state.transferFd(sender, receiver, pair.b, 16, rights, .{}, .move);
    try std.testing.expect(receiver_fd >= 16);

    var owners: [8]PrincipalId = undefined;
    const count = try state.ipcRecvWakeOwnersForSendFd(sender, pair.a, owners[0..]);
    try std.testing.expectEqual(@as(usize, 1), count);
    try std.testing.expectEqual(receiver, owners[0]);
}

test "ipc send wake owner scan includes endpoint receiver" {
    var state = KernelState.initPhase1();
    const owner = processPrincipalFromIndex(0).?;
    const rights = FdRights{
        .inspect = true,
        .wait = true,
        .poll = true,
        .close = true,
        .send = true,
        .recv = true,
    };
    const endpoint = try state.createIpcEndpointFd(owner, rights, .{}, 16);

    var owners: [8]PrincipalId = undefined;
    const count = try state.ipcRecvWakeOwnersForSendFd(owner, endpoint, owners[0..]);
    try std.testing.expectEqual(@as(usize, 1), count);
    try std.testing.expectEqual(owner, owners[0]);
}

test "mmapFdIntoProcess installs vmo fd into target vma table" {
    var state = KernelState.initPhase1();
    const source = processPrincipalFromIndex(0).?;
    const target = processPrincipalFromIndex(1).?;
    const vmo_fd = try state.createAnonymousVmoFd(source, native_page_size, .{
        .inspect = true,
        .close = true,
        .map_read = true,
        .map_exec = true,
    }, .{}, 16);

    const mapped = try state.mmapFdIntoProcess(
        source,
        vmo_fd,
        target,
        0x400000,
        native_page_size,
        .{ .read = true, .exec = true },
        .{ .fixed = true, .shared = true },
        0,
    );
    try std.testing.expectEqual(@as(u64, 0x400000), mapped);

    const vma = state.vmaEntryConst(target, 0x400000) orelse return error.TestExpectedEqual;
    try std.testing.expectEqual(native_page_size, vma.size_bytes);
    try std.testing.expect(vma.prot.read);
    try std.testing.expect(vma.prot.exec);
    try std.testing.expect(!vma.prot.write);
    try std.testing.expect(vma.flags.fixed);
    try std.testing.expect(vma.flags.shared);
}
