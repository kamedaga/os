#![no_std]

extern crate alloc;

use alloc::boxed::Box;
use alloc::vec::Vec;
use core::mem::MaybeUninit;
use core::ptr::{addr_of, read_volatile};
use rt_core::{SyscallError, syscall};

pub mod fixed_va {
    pub const AUX_BASE_VA: u64 = 0x3C00_0000;
    pub const AUX_PAGE_BYTES: u64 = 0x1000;
    pub const STANDARD_CONFIG_TARGET_VA: u64 = aux_page_va(2);
    pub const SERVICE_REGISTRY_PAGE_VA: u64 = aux_page_va(5);
    pub const SERVICE_REGISTRY_SHADOW_VA: u64 = 0x3C2C_0000;

    pub const fn aux_page_va(page_index: u64) -> u64 {
        AUX_BASE_VA + page_index * AUX_PAGE_BYTES
    }
}

pub const SPAWN_FLAG_BOOTSTRAP_PAGE_WRITABLE: u64 = 1 << 0;
pub const SPAWN_FLAG_BOOTSTRAP_DESCRIPTOR_TABLE: u64 = 1 << 1;
pub const SPAWN_FLAG_BOOTSTRAP_EXTENDED_DESCRIPTOR_TABLE: u64 = 1 << 2;
pub const SPAWN_FLAG_CHILD_BOOTSTRAP_OWNER: u64 = 1 << 3;

pub const MAX_BOOTSTRAP_PAGE_DESCRIPTORS: usize = 136;
pub const MAX_BOOTSTRAP_CAP_DESCRIPTORS: usize = 8;
pub const SERVICE_REGISTRY_MAGIC: u64 = 0x5352_5643;
pub const SERVICE_REGISTRY_VERSION: u64 = 1;
pub const MAX_SERVICE_ENTRIES: usize = 6;

const VM_OBJECT_TOKEN_TAG: u64 = 1 << 62;
const EXEC_IMAGE_TOKEN_TAG: u64 = (1 << 62) | (1 << 61);
const FS_CAP_TOKEN_TAG: u64 = 1 << 63;
const SPAWN_RESULT_TAG: u64 = 1 << 63;
const SPAWN_RESULT_PROCESS_BITS: u32 = 32;
const SPAWN_RESULT_THREAD_BITS: u32 = 16;
const SPAWN_RESULT_PROCESS_MASK: u64 = (1u64 << SPAWN_RESULT_PROCESS_BITS) - 1;
const SPAWN_RESULT_THREAD_MASK: u64 = (1u64 << SPAWN_RESULT_THREAD_BITS) - 1;
const SPAWN_RESULT_THREAD_SHIFT: u32 = SPAWN_RESULT_PROCESS_BITS;

fn unit_result(raw: u64) -> Result<(), SyscallError> {
    if raw == syscall::OK {
        Ok(())
    } else {
        Err(SyscallError::from_error_raw(raw))
    }
}

fn vm_object_result(raw: u64) -> Result<VmObjectToken, SyscallError> {
    VmObjectToken::from_raw(raw).ok_or_else(|| SyscallError::from_error_raw(raw))
}

fn exec_image_result(raw: u64) -> Result<ExecImageToken, SyscallError> {
    ExecImageToken::from_raw(raw).ok_or_else(|| SyscallError::from_error_raw(raw))
}

fn spawned_process_result(raw: u64) -> Result<SpawnedProcess, SyscallError> {
    SpawnedProcess::from_raw(raw).ok_or_else(|| SyscallError::from_error_raw(raw))
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub struct VmObjectRights(u32);

impl VmObjectRights {
    pub const READ: Self = Self(1 << 0);
    pub const WRITE: Self = Self(1 << 1);
    pub const MAP: Self = Self(1 << 2);
    pub const GRANT: Self = Self(1 << 3);

    pub const fn from_bits(bits: u64) -> Self {
        Self(bits as u32)
    }

    pub const fn bits(self) -> u64 {
        self.0 as u64
    }

    pub const fn contains(self, other: Self) -> bool {
        (self.0 & other.0) == other.0
    }

    pub const fn union(self, other: Self) -> Self {
        Self(self.0 | other.0)
    }
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub struct VmObjectToken(u64);

impl VmObjectToken {
    pub fn encode(cap_id: u64) -> Option<Self> {
        if cap_id == 0
            || (cap_id & VM_OBJECT_TOKEN_TAG) != 0
            || (cap_id & EXEC_IMAGE_TOKEN_TAG) == EXEC_IMAGE_TOKEN_TAG
        {
            return None;
        }
        Some(Self(VM_OBJECT_TOKEN_TAG | cap_id))
    }

    pub fn from_raw(raw: u64) -> Option<Self> {
        if (raw & EXEC_IMAGE_TOKEN_TAG) == EXEC_IMAGE_TOKEN_TAG || (raw & VM_OBJECT_TOKEN_TAG) == 0
        {
            return None;
        }
        let cap_id = raw & !VM_OBJECT_TOKEN_TAG;
        if cap_id == 0 {
            return None;
        }
        Some(Self(raw))
    }

    pub const fn raw(self) -> u64 {
        self.0
    }

    pub const fn cap_id(self) -> u64 {
        self.0 & !VM_OBJECT_TOKEN_TAG
    }

    pub fn install_from_mapped_range(
        base_va: u64,
        size_bytes: u64,
        rights: VmObjectRights,
    ) -> Result<Self, SyscallError> {
        vm_object_result(syscall::call3(
            syscall::INSTALL_VM_OBJECT,
            base_va,
            size_bytes,
            rights.bits(),
        ))
    }

    pub fn grant_to_process(
        self,
        process_slot: u64,
        rights: VmObjectRights,
    ) -> Result<Self, SyscallError> {
        vm_object_result(syscall::call3(
            syscall::GRANT_VM_OBJECT,
            self.raw(),
            process_slot,
            rights.bits(),
        ))
    }

    pub fn map_into(self, target_va: u64) -> Result<(), SyscallError> {
        unit_result(syscall::call2(
            syscall::MAP_VM_OBJECT,
            self.raw(),
            target_va,
        ))
    }

    pub fn slice(
        self,
        offset_bytes: u64,
        size_bytes: u64,
        rights: VmObjectRights,
    ) -> Result<Self, SyscallError> {
        vm_object_result(syscall::call4(
            syscall::SLICE_VM_OBJECT,
            self.raw(),
            offset_bytes,
            size_bytes,
            rights.bits(),
        ))
    }
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub struct ExecImageRights(u32);

impl ExecImageRights {
    pub const EXEC: Self = Self(1 << 0);
    pub const GRANT: Self = Self(1 << 1);

    pub const fn from_bits(bits: u64) -> Self {
        Self(bits as u32)
    }

    pub const fn bits(self) -> u64 {
        self.0 as u64
    }

    pub const fn contains(self, other: Self) -> bool {
        (self.0 & other.0) == other.0
    }

    pub const fn union(self, other: Self) -> Self {
        Self(self.0 | other.0)
    }
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub struct ExecImageToken(u64);

impl ExecImageToken {
    pub fn encode(cap_id: u64) -> Option<Self> {
        if cap_id == 0 || (cap_id & EXEC_IMAGE_TOKEN_TAG) != 0 {
            return None;
        }
        Some(Self(EXEC_IMAGE_TOKEN_TAG | cap_id))
    }

    pub fn from_raw(raw: u64) -> Option<Self> {
        if (raw & EXEC_IMAGE_TOKEN_TAG) != EXEC_IMAGE_TOKEN_TAG {
            return None;
        }
        let cap_id = raw & !EXEC_IMAGE_TOKEN_TAG;
        if cap_id == 0 {
            return None;
        }
        Some(Self(raw))
    }

    pub const fn raw(self) -> u64 {
        self.0
    }

    pub const fn cap_id(self) -> u64 {
        self.0 & !EXEC_IMAGE_TOKEN_TAG
    }

    pub fn install_from_vm_object(
        vm_object: VmObjectToken,
        rights: ExecImageRights,
    ) -> Result<Self, SyscallError> {
        exec_image_result(syscall::call2(
            syscall::INSTALL_EXEC_IMAGE,
            vm_object.raw(),
            rights.bits(),
        ))
    }

    pub fn grant_to_process(
        self,
        process_slot: u64,
        rights: ExecImageRights,
    ) -> Result<Self, SyscallError> {
        exec_image_result(syscall::call3(
            syscall::GRANT_EXEC_IMAGE,
            self.raw(),
            process_slot,
            rights.bits(),
        ))
    }
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub struct SpawnedProcess {
    process_slot: u64,
    thread_slot: u64,
}

impl SpawnedProcess {
    pub fn from_raw(raw: u64) -> Option<Self> {
        if (raw & SPAWN_RESULT_TAG) == 0 {
            return None;
        }
        let process_slot = raw & SPAWN_RESULT_PROCESS_MASK;
        if process_slot == 0 {
            return None;
        }
        Some(Self {
            process_slot,
            thread_slot: (raw >> SPAWN_RESULT_THREAD_SHIFT) & SPAWN_RESULT_THREAD_MASK,
        })
    }

    pub const fn raw(self) -> u64 {
        SPAWN_RESULT_TAG | self.process_slot | (self.thread_slot << SPAWN_RESULT_THREAD_SHIFT)
    }

    pub const fn process_slot(self) -> u64 {
        self.process_slot
    }

    pub const fn thread_slot(self) -> u64 {
        self.thread_slot
    }
}

#[repr(u8)]
#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub enum BootstrapCapKind {
    VmObject = 2,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct BootstrapPageDescriptor {
    pub source_va: u64,
    pub target_va: u64,
    pub flags: u64,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct BootstrapCapDescriptor {
    pub source_token: u64,
    pub target_token_va: u64,
    pub rights_bits: u64,
    pub kind: BootstrapCapKind,
    pub reserved: [u8; 7],
}

#[repr(C)]
pub struct BootstrapDescriptorTable {
    pub page_count: u16,
    pub cap_count: u16,
    pub reserved0: u32,
    pub page_descriptors: [BootstrapPageDescriptor; MAX_BOOTSTRAP_PAGE_DESCRIPTORS],
    pub cap_descriptors: [BootstrapCapDescriptor; MAX_BOOTSTRAP_CAP_DESCRIPTORS],
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub enum BootstrapBuildError {
    PageTableFull,
    CapTableFull,
}

pub struct BootstrapBuilder {
    table: Box<BootstrapDescriptorTable>,
}

impl BootstrapBuilder {
    pub fn new() -> Self {
        let mut table = Box::<MaybeUninit<BootstrapDescriptorTable>>::new_uninit();
        // SAFETY: An all-zero descriptor table is a valid empty starting state.
        unsafe {
            table.as_mut_ptr().write_bytes(0, 1);
            let table = Box::into_raw(table) as *mut BootstrapDescriptorTable;
            Self {
                table: Box::from_raw(table),
            }
        }
    }

    fn table_mut(&mut self) -> &mut BootstrapDescriptorTable {
        self.table.as_mut()
    }

    fn table_ref(&self) -> &BootstrapDescriptorTable {
        self.table.as_ref()
    }

    pub fn is_empty(&self) -> bool {
        self.table_ref().page_count == 0 && self.table_ref().cap_count == 0
    }

    pub fn page_count(&self) -> usize {
        self.table_ref().page_count as usize
    }

    pub fn cap_count(&self) -> usize {
        self.table_ref().cap_count as usize
    }

    pub fn push_page(
        &mut self,
        source_va: u64,
        target_va: u64,
        flags: u64,
    ) -> Result<(), BootstrapBuildError> {
        let table = self.table_mut();
        let index = table.page_count as usize;
        if index >= MAX_BOOTSTRAP_PAGE_DESCRIPTORS {
            return Err(BootstrapBuildError::PageTableFull);
        }
        table.page_descriptors[index] = BootstrapPageDescriptor {
            source_va,
            target_va,
            flags,
        };
        table.page_count += 1;
        Ok(())
    }

    pub fn push_vm_object_cap(
        &mut self,
        token: VmObjectToken,
        target_token_va: u64,
        rights: VmObjectRights,
    ) -> Result<(), BootstrapBuildError> {
        let table = self.table_mut();
        let index = table.cap_count as usize;
        if index >= MAX_BOOTSTRAP_CAP_DESCRIPTORS {
            return Err(BootstrapBuildError::CapTableFull);
        }
        table.cap_descriptors[index] = BootstrapCapDescriptor {
            source_token: token.raw(),
            target_token_va,
            rights_bits: rights.bits(),
            kind: BootstrapCapKind::VmObject,
            reserved: [0; 7],
        };
        table.cap_count += 1;
        Ok(())
    }

    pub fn table(&self) -> &BootstrapDescriptorTable {
        self.table_ref()
    }
}

impl Default for BootstrapBuilder {
    fn default() -> Self {
        Self::new()
    }
}

pub struct SpawnBuilder {
    exec: ExecImageToken,
    bootstrap: BootstrapBuilder,
    child_bootstrap_owner: bool,
}

impl SpawnBuilder {
    pub fn new(exec: ExecImageToken) -> Self {
        Self {
            exec,
            bootstrap: BootstrapBuilder::new(),
            child_bootstrap_owner: false,
        }
    }

    pub const fn exec_image(&self) -> ExecImageToken {
        self.exec
    }

    pub const fn bootstrap(&self) -> &BootstrapBuilder {
        &self.bootstrap
    }

    pub fn bootstrap_mut(&mut self) -> &mut BootstrapBuilder {
        &mut self.bootstrap
    }

    pub fn push_page(
        &mut self,
        source_va: u64,
        target_va: u64,
        flags: u64,
    ) -> Result<(), BootstrapBuildError> {
        self.bootstrap.push_page(source_va, target_va, flags)
    }

    pub fn push_vm_object_cap(
        &mut self,
        token: VmObjectToken,
        target_token_va: u64,
        rights: VmObjectRights,
    ) -> Result<(), BootstrapBuildError> {
        self.bootstrap
            .push_vm_object_cap(token, target_token_va, rights)
    }

    pub fn set_child_bootstrap_owner(&mut self, enabled: bool) {
        self.child_bootstrap_owner = enabled;
    }

    pub fn with_child_bootstrap_owner(mut self, enabled: bool) -> Self {
        self.child_bootstrap_owner = enabled;
        self
    }

    pub fn spawn(&self) -> Result<SpawnedProcess, SyscallError> {
        let mut flags = 0;
        let mut bootstrap_source_va = 0;
        if !self.bootstrap.is_empty() {
            bootstrap_source_va = self.bootstrap.table() as *const BootstrapDescriptorTable as u64;
            flags |= SPAWN_FLAG_BOOTSTRAP_EXTENDED_DESCRIPTOR_TABLE;
        }
        if self.child_bootstrap_owner {
            flags |= SPAWN_FLAG_CHILD_BOOTSTRAP_OWNER;
        }

        spawned_process_result(syscall::call4(
            syscall::SPAWN_EXEC,
            self.exec.raw(),
            bootstrap_source_va,
            0,
            flags,
        ))
    }
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub struct FsConnectionId(u32);

impl FsConnectionId {
    pub const fn new(raw: u32) -> Self {
        Self(raw)
    }

    pub const fn raw(self) -> u32 {
        self.0
    }

    pub fn from_raw(raw: u64) -> Option<Self> {
        let value = u32::try_from(raw).ok()?;
        if value == 0 {
            return None;
        }
        Some(Self(value))
    }
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub struct FsRights(u32);

impl FsRights {
    pub const LOOKUP: Self = Self(1 << 0);
    pub const READ: Self = Self(1 << 1);
    pub const WRITE: Self = Self(1 << 2);
    pub const READDIR: Self = Self(1 << 3);
    pub const STAT: Self = Self(1 << 4);
    pub const CREATE: Self = Self(1 << 5);
    pub const UNLINK: Self = Self(1 << 6);
    pub const RENAME: Self = Self(1 << 7);
    pub const EXEC: Self = Self(1 << 8);
    pub const MOUNT: Self = Self(1 << 9);
    pub const GRANT: Self = Self(1 << 10);
    pub const ADMIN: Self = Self(1 << 11);

    pub const fn from_bits(bits: u64) -> Self {
        Self(bits as u32)
    }

    pub const fn bits(self) -> u64 {
        self.0 as u64
    }

    pub const fn contains(self, other: Self) -> bool {
        (self.0 & other.0) == other.0
    }

    pub const fn union(self, other: Self) -> Self {
        Self(self.0 | other.0)
    }
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub enum FsObjectKind {
    None,
    Mount,
    VnodeDir,
    VnodeFile,
    OpenFile,
    Exec,
    BlockDevice,
    Unknown(u8),
}

impl FsObjectKind {
    pub const fn from_raw(raw: u64) -> Self {
        match raw as u8 {
            0 => Self::None,
            1 => Self::Mount,
            2 => Self::VnodeDir,
            3 => Self::VnodeFile,
            4 => Self::OpenFile,
            5 => Self::Exec,
            6 => Self::BlockDevice,
            value => Self::Unknown(value),
        }
    }

    pub const fn raw(self) -> u64 {
        match self {
            Self::None => 0,
            Self::Mount => 1,
            Self::VnodeDir => 2,
            Self::VnodeFile => 3,
            Self::OpenFile => 4,
            Self::Exec => 5,
            Self::BlockDevice => 6,
            Self::Unknown(value) => value as u64,
        }
    }

    pub const fn name(self) -> &'static str {
        match self {
            Self::None => "none",
            Self::Mount => "mount",
            Self::VnodeDir => "vnode_dir",
            Self::VnodeFile => "vnode_file",
            Self::OpenFile => "open_file",
            Self::Exec => "exec",
            Self::BlockDevice => "block_device",
            Self::Unknown(_) => "unknown",
        }
    }
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub struct FsObjectToken(u64);

impl FsObjectToken {
    pub fn encode(cap_id: u64) -> Option<Self> {
        if cap_id == 0 || (cap_id & FS_CAP_TOKEN_TAG) != 0 {
            return None;
        }
        Some(Self(FS_CAP_TOKEN_TAG | cap_id))
    }

    pub fn from_raw(raw: u64) -> Option<Self> {
        if (raw & FS_CAP_TOKEN_TAG) == 0 {
            return None;
        }
        let cap_id = raw & !FS_CAP_TOKEN_TAG;
        if cap_id == 0 {
            return None;
        }
        Some(Self(raw))
    }

    pub const fn raw(self) -> u64 {
        self.0
    }

    pub const fn cap_id(self) -> u64 {
        self.0 & !FS_CAP_TOKEN_TAG
    }
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub struct DirToken(FsObjectToken);

impl DirToken {
    pub const fn from_fs_token(token: FsObjectToken) -> Self {
        Self(token)
    }

    pub const fn raw(self) -> u64 {
        self.0.raw()
    }

    pub const fn fs_token(self) -> FsObjectToken {
        self.0
    }
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub struct VnodeFileToken(FsObjectToken);

impl VnodeFileToken {
    pub const fn from_fs_token(token: FsObjectToken) -> Self {
        Self(token)
    }

    pub const fn raw(self) -> u64 {
        self.0.raw()
    }

    pub const fn fs_token(self) -> FsObjectToken {
        self.0
    }
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub struct OpenFileToken(FsObjectToken);

impl OpenFileToken {
    pub const fn from_fs_token(token: FsObjectToken) -> Self {
        Self(token)
    }

    pub const fn raw(self) -> u64 {
        self.0.raw()
    }

    pub const fn fs_token(self) -> FsObjectToken {
        self.0
    }
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub enum ClockKind {
    Monotonic,
    Wall,
}

impl ClockKind {
    pub const fn raw(self) -> u64 {
        match self {
            Self::Monotonic => 1,
            Self::Wall => 2,
        }
    }

    pub const fn from_raw(raw: u64) -> Option<Self> {
        match raw {
            1 => Some(Self::Monotonic),
            2 => Some(Self::Wall),
            _ => None,
        }
    }

    pub const fn name(self) -> &'static str {
        match self {
            Self::Monotonic => "monotonic",
            Self::Wall => "wall",
        }
    }
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub enum RandomKind {
    Default,
}

impl RandomKind {
    pub const fn raw(self) -> u64 {
        match self {
            Self::Default => 1,
        }
    }

    pub const fn from_raw(raw: u64) -> Option<Self> {
        match raw {
            1 => Some(Self::Default),
            _ => None,
        }
    }

    pub const fn name(self) -> &'static str {
        match self {
            Self::Default => "default",
        }
    }
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub enum ServiceKind {
    Window,
    Vfs,
    Block,
    PersistentFs,
    Unknown(u64),
}

impl ServiceKind {
    pub const fn from_raw(raw: u64) -> Self {
        match raw {
            1 => Self::Window,
            2 => Self::Vfs,
            4 => Self::Block,
            5 => Self::PersistentFs,
            _ => Self::Unknown(raw),
        }
    }

    pub const fn name(self) -> &'static str {
        match self {
            Self::Window => "window",
            Self::Vfs => "vfs",
            Self::Block => "block",
            Self::PersistentFs => "persistent_fs",
            Self::Unknown(_) => "unknown",
        }
    }
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub struct ServiceBinding {
    pub kind: ServiceKind,
    pub process_slot: u64,
    pub endpoint_id: u64,
    pub flags: u64,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct ServiceEntry {
    pub kind: u64,
    pub process_slot: u64,
    pub endpoint_id: u64,
    pub flags: u64,
}

impl ServiceEntry {
    pub const fn kind_enum(self) -> ServiceKind {
        ServiceKind::from_raw(self.kind)
    }
}

#[repr(C)]
struct RegistryPage {
    magic: u64,
    version: u64,
    entry_count: u64,
    reserved0: u64,
    entries: [ServiceEntry; MAX_SERVICE_ENTRIES],
}

const EMPTY_SERVICE_ENTRY: ServiceEntry = ServiceEntry {
    kind: 0,
    process_slot: 0,
    endpoint_id: 0,
    flags: 0,
};

#[derive(Copy, Clone)]
pub struct ServiceRegistrySnapshot {
    count: usize,
    entries: [ServiceEntry; MAX_SERVICE_ENTRIES],
}

impl ServiceRegistrySnapshot {
    pub const fn len(&self) -> usize {
        self.count
    }

    pub const fn is_empty(&self) -> bool {
        self.count == 0
    }

    pub fn entries(&self) -> &[ServiceEntry] {
        &self.entries[..self.count]
    }

    pub fn find_kind(&self, kind: ServiceKind) -> Option<ServiceBinding> {
        for entry in self.entries() {
            if entry.kind_enum() == kind {
                return Some(ServiceBinding {
                    kind,
                    process_slot: entry.process_slot,
                    endpoint_id: entry.endpoint_id,
                    flags: entry.flags,
                });
            }
        }
        None
    }
}

pub unsafe fn snapshot_service_registry(va: usize) -> Option<ServiceRegistrySnapshot> {
    let page = va as *const RegistryPage;
    let magic = unsafe { read_volatile(addr_of!((*page).magic)) };
    let version = unsafe { read_volatile(addr_of!((*page).version)) };
    if magic != SERVICE_REGISTRY_MAGIC || version != SERVICE_REGISTRY_VERSION {
        return None;
    }

    let raw_count = unsafe { read_volatile(addr_of!((*page).entry_count)) };
    let count = usize::min(raw_count as usize, MAX_SERVICE_ENTRIES);
    let mut entries = [EMPTY_SERVICE_ENTRY; MAX_SERVICE_ENTRIES];
    let mut index = 0;
    while index < count {
        entries[index] = unsafe { read_volatile(addr_of!((*page).entries[index])) };
        index += 1;
    }

    Some(ServiceRegistrySnapshot { count, entries })
}

pub unsafe fn snapshot_service_registry_shadow() -> Option<ServiceRegistrySnapshot> {
    unsafe { snapshot_service_registry(fixed_va::SERVICE_REGISTRY_SHADOW_VA as usize) }
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub struct HandleId(u32);

impl HandleId {
    pub const fn raw(self) -> u32 {
        self.0
    }
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub enum HandleKind {
    ServiceRegistryPage,
    VmObject,
    ExecImage,
    Dir,
    VnodeFile,
    OpenFile,
    Clock,
    Random,
}

impl HandleKind {
    pub const fn name(self) -> &'static str {
        match self {
            Self::ServiceRegistryPage => "service_registry_page",
            Self::VmObject => "vm_object",
            Self::ExecImage => "exec_image",
            Self::Dir => "dir",
            Self::VnodeFile => "vnode_file",
            Self::OpenFile => "open_file",
            Self::Clock => "clock",
            Self::Random => "random",
        }
    }
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub struct HandleRights(u64);

impl HandleRights {
    pub const LOOKUP: Self = Self(1 << 5);
    pub const READDIR: Self = Self(1 << 6);
    pub const STAT: Self = Self(1 << 7);
    pub const CREATE: Self = Self(1 << 8);
    pub const UNLINK: Self = Self(1 << 9);
    pub const RENAME: Self = Self(1 << 10);
    pub const MOUNT: Self = Self(1 << 11);
    pub const ADMIN: Self = Self(1 << 12);

    pub const READ: Self = Self(1 << 0);
    pub const WRITE: Self = Self(1 << 1);
    pub const MAP: Self = Self(1 << 2);
    pub const GRANT: Self = Self(1 << 3);
    pub const EXEC: Self = Self(1 << 4);

    pub const fn bits(self) -> u64 {
        self.0
    }

    pub const fn contains(self, other: Self) -> bool {
        (self.0 & other.0) == other.0
    }

    pub const fn union(self, other: Self) -> Self {
        Self(self.0 | other.0)
    }

    pub const fn from_vm_object(rights: VmObjectRights) -> Self {
        let mut bits = 0;
        if rights.contains(VmObjectRights::READ) {
            bits |= Self::READ.0;
        }
        if rights.contains(VmObjectRights::WRITE) {
            bits |= Self::WRITE.0;
        }
        if rights.contains(VmObjectRights::MAP) {
            bits |= Self::MAP.0;
        }
        if rights.contains(VmObjectRights::GRANT) {
            bits |= Self::GRANT.0;
        }
        Self(bits)
    }

    pub const fn from_exec_image(rights: ExecImageRights) -> Self {
        let mut bits = 0;
        if rights.contains(ExecImageRights::EXEC) {
            bits |= Self::EXEC.0;
        }
        if rights.contains(ExecImageRights::GRANT) {
            bits |= Self::GRANT.0;
        }
        Self(bits)
    }

    pub const fn from_fs(rights: FsRights) -> Self {
        let mut bits = 0;
        if rights.contains(FsRights::LOOKUP) {
            bits |= Self::LOOKUP.0;
        }
        if rights.contains(FsRights::READ) {
            bits |= Self::READ.0;
        }
        if rights.contains(FsRights::WRITE) {
            bits |= Self::WRITE.0;
        }
        if rights.contains(FsRights::READDIR) {
            bits |= Self::READDIR.0;
        }
        if rights.contains(FsRights::STAT) {
            bits |= Self::STAT.0;
        }
        if rights.contains(FsRights::CREATE) {
            bits |= Self::CREATE.0;
        }
        if rights.contains(FsRights::UNLINK) {
            bits |= Self::UNLINK.0;
        }
        if rights.contains(FsRights::RENAME) {
            bits |= Self::RENAME.0;
        }
        if rights.contains(FsRights::EXEC) {
            bits |= Self::EXEC.0;
        }
        if rights.contains(FsRights::MOUNT) {
            bits |= Self::MOUNT.0;
        }
        if rights.contains(FsRights::GRANT) {
            bits |= Self::GRANT.0;
        }
        if rights.contains(FsRights::ADMIN) {
            bits |= Self::ADMIN.0;
        }
        Self(bits)
    }
}

#[derive(Copy, Clone)]
pub struct HandleEntry {
    id: HandleId,
    kind: HandleKind,
    rights: HandleRights,
    primary: u64,
    secondary: u64,
    tertiary: u64,
}

impl HandleEntry {
    pub const fn id(self) -> HandleId {
        self.id
    }

    pub const fn kind(self) -> HandleKind {
        self.kind
    }

    pub const fn rights(self) -> HandleRights {
        self.rights
    }

    pub const fn service_registry_va(self) -> Option<usize> {
        match self.kind {
            HandleKind::ServiceRegistryPage => Some(self.primary as usize),
            _ => None,
        }
    }

    pub fn vm_object_token(self) -> Option<VmObjectToken> {
        match self.kind {
            HandleKind::VmObject => VmObjectToken::from_raw(self.primary),
            _ => None,
        }
    }

    pub fn vm_object_rights(self) -> Option<VmObjectRights> {
        match self.kind {
            HandleKind::VmObject => Some(VmObjectRights::from_bits(self.secondary)),
            _ => None,
        }
    }

    pub fn exec_image_token(self) -> Option<ExecImageToken> {
        match self.kind {
            HandleKind::ExecImage => ExecImageToken::from_raw(self.primary),
            _ => None,
        }
    }

    pub fn exec_image_rights(self) -> Option<ExecImageRights> {
        match self.kind {
            HandleKind::ExecImage => Some(ExecImageRights::from_bits(self.secondary)),
            _ => None,
        }
    }

    pub fn fs_connection_id(self) -> Option<FsConnectionId> {
        match self.kind {
            HandleKind::Dir | HandleKind::VnodeFile | HandleKind::OpenFile => {
                FsConnectionId::from_raw(self.tertiary)
            }
            _ => None,
        }
    }

    pub fn fs_object_token(self) -> Option<FsObjectToken> {
        match self.kind {
            HandleKind::Dir | HandleKind::VnodeFile | HandleKind::OpenFile => {
                FsObjectToken::from_raw(self.primary)
            }
            _ => None,
        }
    }

    pub fn fs_rights(self) -> Option<FsRights> {
        match self.kind {
            HandleKind::Dir | HandleKind::VnodeFile | HandleKind::OpenFile => {
                Some(FsRights::from_bits(self.secondary))
            }
            _ => None,
        }
    }

    pub fn fs_object_kind(self) -> Option<FsObjectKind> {
        match self.kind {
            HandleKind::Dir => Some(FsObjectKind::VnodeDir),
            HandleKind::VnodeFile => Some(FsObjectKind::VnodeFile),
            HandleKind::OpenFile => Some(FsObjectKind::OpenFile),
            _ => None,
        }
    }

    pub fn dir_token(self) -> Option<DirToken> {
        match self.kind {
            HandleKind::Dir => self.fs_object_token().map(DirToken::from_fs_token),
            _ => None,
        }
    }

    pub fn vnode_file_token(self) -> Option<VnodeFileToken> {
        match self.kind {
            HandleKind::VnodeFile => self.fs_object_token().map(VnodeFileToken::from_fs_token),
            _ => None,
        }
    }

    pub fn open_file_token(self) -> Option<OpenFileToken> {
        match self.kind {
            HandleKind::OpenFile => self.fs_object_token().map(OpenFileToken::from_fs_token),
            _ => None,
        }
    }

    pub fn clock_kind(self) -> Option<ClockKind> {
        match self.kind {
            HandleKind::Clock => ClockKind::from_raw(self.primary),
            _ => None,
        }
    }

    pub fn random_kind(self) -> Option<RandomKind> {
        match self.kind {
            HandleKind::Random => RandomKind::from_raw(self.primary),
            _ => None,
        }
    }

    pub const fn secondary(self) -> u64 {
        self.secondary
    }

    pub const fn tertiary(self) -> u64 {
        self.tertiary
    }
}

pub struct HandleTable {
    next_id: u32,
    entries: Vec<HandleEntry>,
}

impl HandleTable {
    pub fn new() -> Self {
        Self {
            next_id: 1,
            entries: Vec::new(),
        }
    }

    pub fn len(&self) -> usize {
        self.entries.len()
    }

    pub fn is_empty(&self) -> bool {
        self.entries.is_empty()
    }

    pub fn insert_service_registry_shadow(&mut self) -> HandleId {
        self.insert_entry(
            HandleKind::ServiceRegistryPage,
            HandleRights::READ,
            fixed_va::SERVICE_REGISTRY_SHADOW_VA,
            0,
            0,
        )
    }

    pub fn insert_service_registry_va(&mut self, va: u64) -> HandleId {
        self.insert_entry(
            HandleKind::ServiceRegistryPage,
            HandleRights::READ,
            va,
            0,
            0,
        )
    }

    pub fn insert_vm_object(&mut self, token: VmObjectToken, rights: VmObjectRights) -> HandleId {
        self.insert_entry(
            HandleKind::VmObject,
            HandleRights::from_vm_object(rights),
            token.raw(),
            rights.bits(),
            0,
        )
    }

    pub fn insert_exec_image(
        &mut self,
        token: ExecImageToken,
        rights: ExecImageRights,
    ) -> HandleId {
        self.insert_entry(
            HandleKind::ExecImage,
            HandleRights::from_exec_image(rights),
            token.raw(),
            rights.bits(),
            0,
        )
    }

    pub fn insert_dir(
        &mut self,
        connection_id: FsConnectionId,
        token: DirToken,
        rights: FsRights,
    ) -> HandleId {
        self.insert_entry(
            HandleKind::Dir,
            HandleRights::from_fs(rights),
            token.raw(),
            rights.bits(),
            connection_id.raw() as u64,
        )
    }

    pub fn insert_vnode_file(
        &mut self,
        connection_id: FsConnectionId,
        token: VnodeFileToken,
        rights: FsRights,
    ) -> HandleId {
        self.insert_entry(
            HandleKind::VnodeFile,
            HandleRights::from_fs(rights),
            token.raw(),
            rights.bits(),
            connection_id.raw() as u64,
        )
    }

    pub fn insert_open_file(
        &mut self,
        connection_id: FsConnectionId,
        token: OpenFileToken,
        rights: FsRights,
    ) -> HandleId {
        self.insert_entry(
            HandleKind::OpenFile,
            HandleRights::from_fs(rights),
            token.raw(),
            rights.bits(),
            connection_id.raw() as u64,
        )
    }

    pub fn insert_clock(&mut self, kind: ClockKind) -> HandleId {
        self.insert_entry(HandleKind::Clock, HandleRights::READ, kind.raw(), 0, 0)
    }

    pub fn insert_random(&mut self, kind: RandomKind) -> HandleId {
        self.insert_entry(HandleKind::Random, HandleRights::READ, kind.raw(), 0, 0)
    }

    pub fn get(&self, id: HandleId) -> Option<&HandleEntry> {
        self.entries.iter().find(|entry| entry.id == id)
    }

    pub fn iter(&self) -> core::slice::Iter<'_, HandleEntry> {
        self.entries.iter()
    }

    fn insert_entry(
        &mut self,
        kind: HandleKind,
        rights: HandleRights,
        primary: u64,
        secondary: u64,
        tertiary: u64,
    ) -> HandleId {
        let id = HandleId(self.next_id);
        self.next_id += 1;
        self.entries.push(HandleEntry {
            id,
            kind,
            rights,
            primary,
            secondary,
            tertiary,
        });
        id
    }
}

impl Default for HandleTable {
    fn default() -> Self {
        Self::new()
    }
}
