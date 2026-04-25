#![no_std]

use core::ptr::{addr_of, read_volatile, write_volatile};
use core::sync::atomic::{Ordering, compiler_fence};

use rt_core::{SyscallError, syscall};
use rt_handle::{
    ClockKind, DirToken, ExecImageToken, FsConnectionId, FsObjectKind, FsObjectToken, FsRights,
    OpenFileToken, RandomKind, ServiceKind, VnodeFileToken, snapshot_service_registry_shadow,
};

const PAGE_BYTES: usize = 4096;
const MAX_PATH_BYTES: usize = 128;
const REQUEST_MAGIC: u32 = 0x5153_4653;
const RESPONSE_MAGIC: u32 = 0x5253_4653;
const VERSION: u16 = 1;
const DEFAULT_RESPONSE_POLL_LIMIT: u64 = 256;
const CREATE_FLAG_DIRECTORY: u32 = 1 << 0;
const PAGE_RIGHT_CPU_READ: u64 = 0x1;
const PAGE_RIGHT_CPU_WRITE: u64 = 0x2;

const REQUEST_VA: u64 = 0x3F20_0000;
const RESPONSE_VA: u64 = 0x3F20_1000;

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub enum Error {
    RequestAllocFailed,
    RequestMapFailed,
    ResponseAllocFailed,
    ResponseMapFailed,
    ResponseGrantFailed,
    ConnectSendFailed,
    EndpointInstallFailed,
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
    MissingService,
    WrongConnection,
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
#[repr(u16)]
enum Opcode {
    Connect = 1,
    Lookup = 16,
    Open = 17,
    Read = 18,
    Readdir = 19,
    Stat = 20,
    Close = 21,
    Create = 22,
    Write = 23,
    Unlink = 24,
    Rename = 25,
    StatFs = 26,
    OpenExec = 32,
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
#[repr(i32)]
enum Status {
    Ok = 0,
    Invalid = 1,
    NotFound = 2,
    NotDir = 3,
    IsDir = 4,
    NoRight = 5,
    TooBig = 6,
    NotSupported = 7,
    IoError = 8,
    Busy = 9,
    EndOfDir = 10,
}

#[repr(C)]
struct FsRequestHeader {
    magic: u32,
    version: u16,
    op: u16,
    request_seq: u64,
    object_token: u64,
    offset: u64,
    length: u32,
    flags: u32,
    path_bytes: u16,
    inline_bytes: u16,
    reserved0: u32,
    arg0: u64,
    arg1: u64,
}

#[repr(C)]
struct FsResponseHeader {
    magic: u32,
    version: u16,
    op: u16,
    response_seq: u64,
    status: i32,
    result_flags: u32,
    result_token: u64,
    file_bytes: u64,
    cursor_next: u64,
    inline_bytes: u16,
    object_kind: u8,
    reserved0: u8,
    reserved1: u32,
    arg0: u64,
    arg1: u64,
}

#[repr(C)]
struct FsStatRecord {
    object_kind: u8,
    reserved0: [u8; 7],
    size_bytes: u64,
    mode_bits: u32,
    reserved1: u32,
    mtime_unix_sec: u64,
    reserved2: [u64; 2],
}

#[repr(C)]
struct FsDirentRecord {
    next_cursor: u64,
    object_kind: u8,
    reserved0: [u8; 7],
    name_bytes: u16,
    reserved1: u16,
    reserved2: u32,
}

const REQUEST_HEADER_BYTES: usize = core::mem::size_of::<FsRequestHeader>();
const RESPONSE_HEADER_BYTES: usize = core::mem::size_of::<FsResponseHeader>();
const REQUEST_PAYLOAD_BYTES: usize = PAGE_BYTES - REQUEST_HEADER_BYTES;
const RESPONSE_PAYLOAD_BYTES: usize = PAGE_BYTES - RESPONSE_HEADER_BYTES;
const STAT_RECORD_BYTES: usize = core::mem::size_of::<FsStatRecord>();
const DIRENT_RECORD_BYTES: usize = core::mem::size_of::<FsDirentRecord>();

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub struct LookupResult {
    pub token: FsObjectToken,
    pub object_kind: FsObjectKind,
    pub file_bytes: u64,
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub struct StatResult {
    pub object_kind: FsObjectKind,
    pub size_bytes: u64,
    pub mode_bits: u32,
    pub mtime_unix_sec: u64,
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub struct StatFsResult {
    pub block_size: u64,
    pub capacity_blocks: u64,
    pub used_blocks: u64,
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub struct ReadResult {
    pub bytes_read: usize,
    pub file_bytes: u64,
    pub next_offset: u64,
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub struct OpenExecResult {
    pub token: ExecImageToken,
    pub file_bytes: u64,
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub struct ReaddirEntry<'a> {
    pub next_cursor: u64,
    pub object_kind: FsObjectKind,
    pub name: &'a [u8],
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub enum ReaddirResult<'a> {
    Entry(ReaddirEntry<'a>),
    End,
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub struct Dir {
    connection_id: FsConnectionId,
    token: DirToken,
}

impl Dir {
    pub const fn connection_id(self) -> FsConnectionId {
        self.connection_id
    }

    pub const fn token(self) -> DirToken {
        self.token
    }

    pub const fn assumed_rights(self) -> FsRights {
        FsRights::LOOKUP
            .union(FsRights::READ)
            .union(FsRights::WRITE)
            .union(FsRights::READDIR)
            .union(FsRights::STAT)
            .union(FsRights::CREATE)
            .union(FsRights::UNLINK)
            .union(FsRights::RENAME)
            .union(FsRights::EXEC)
    }
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub struct VnodeFile {
    connection_id: FsConnectionId,
    token: VnodeFileToken,
    file_bytes: u64,
}

impl VnodeFile {
    pub const fn connection_id(self) -> FsConnectionId {
        self.connection_id
    }

    pub const fn token(self) -> VnodeFileToken {
        self.token
    }

    pub const fn file_bytes(self) -> u64 {
        self.file_bytes
    }

    pub const fn assumed_rights(self) -> FsRights {
        FsRights::READ
            .union(FsRights::WRITE)
            .union(FsRights::STAT)
            .union(FsRights::EXEC)
    }
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub struct OpenFile {
    connection_id: FsConnectionId,
    token: OpenFileToken,
    file_bytes: u64,
}

impl OpenFile {
    pub const fn connection_id(self) -> FsConnectionId {
        self.connection_id
    }

    pub const fn token(self) -> OpenFileToken {
        self.token
    }

    pub const fn file_bytes(self) -> u64 {
        self.file_bytes
    }

    pub const fn assumed_rights(self) -> FsRights {
        FsRights::READ.union(FsRights::WRITE).union(FsRights::STAT)
    }
}

#[derive(Copy, Clone, Debug, Default, Eq, PartialEq)]
pub struct MonotonicClock;

impl MonotonicClock {
    pub const fn new() -> Self {
        Self
    }

    pub const fn kind(self) -> ClockKind {
        ClockKind::Monotonic
    }

    pub fn now_ticks(self) -> u64 {
        monotonic_now_ticks()
    }
}

#[derive(Copy, Clone, Debug, Default, Eq, PartialEq)]
pub struct DefaultRandomSource;

impl DefaultRandomSource {
    pub const fn new() -> Self {
        Self
    }

    pub const fn kind(self) -> RandomKind {
        RandomKind::Default
    }

    pub fn fill_bytes(self, _out: &mut [u8]) -> Result<(), Error> {
        Err(Error::NotSupported)
    }
}

pub struct PersistentFsClient {
    connection_id: FsConnectionId,
    request_va: u64,
    response_va: u64,
    request_paddr: u64,
    response_paddr: u64,
    server_endpoint_id: u64,
    mount_token: FsObjectToken,
    next_seq: u64,
    response_poll_limit: u64,
}

impl PersistentFsClient {
    pub fn connect_from_shadow(connection_id: FsConnectionId) -> Result<Self, Error> {
        let process_slot = current_process_slot()?;
        Self::connect_from_registry_shadow(connection_id, REQUEST_VA, RESPONSE_VA, process_slot)
    }

    pub fn connect_from_registry_shadow(
        connection_id: FsConnectionId,
        request_va: u64,
        response_va: u64,
        client_process_slot: u64,
    ) -> Result<Self, Error> {
        // SAFETY: The launcher bootstraps the service registry shadow page for user processes.
        let binding = unsafe {
            snapshot_service_registry_shadow()
                .and_then(|snapshot| snapshot.find_kind(ServiceKind::PersistentFs))
        }
        .ok_or(Error::MissingService)?;

        if unit_syscall(syscall::call3(
            syscall::INSTALL_ENDPOINT,
            0,
            binding.endpoint_id,
            binding.process_slot,
        ))
        .is_err()
        {
            return Err(Error::EndpointInstallFailed);
        }

        Self::connect(
            connection_id,
            request_va,
            response_va,
            client_process_slot,
            binding.endpoint_id,
            binding.process_slot,
        )
    }

    pub fn connect(
        connection_id: FsConnectionId,
        request_va: u64,
        response_va: u64,
        client_process_slot: u64,
        endpoint_id: u64,
        server_process_slot: u64,
    ) -> Result<Self, Error> {
        let request_paddr = alloc_page().ok_or(Error::RequestAllocFailed)?;
        if unit_syscall(syscall::call3(
            syscall::MAP_PAGE,
            request_va,
            request_paddr,
            1,
        ))
        .is_err()
        {
            return Err(Error::RequestMapFailed);
        }

        let response_paddr = alloc_page().ok_or(Error::ResponseAllocFailed)?;
        if unit_syscall(syscall::call3(
            syscall::MAP_PAGE,
            response_va,
            response_paddr,
            1,
        ))
        .is_err()
        {
            return Err(Error::ResponseMapFailed);
        }
        if unit_syscall(syscall::call3(
            syscall::GRANT_CAP,
            response_paddr,
            server_process_slot,
            PAGE_RIGHT_CPU_READ | PAGE_RIGHT_CPU_WRITE,
        ))
        .is_err()
        {
            return Err(Error::ResponseGrantFailed);
        }

        let mut client = Self {
            connection_id,
            request_va,
            response_va,
            request_paddr,
            response_paddr,
            server_endpoint_id: endpoint_id,
            mount_token: FsObjectToken::encode(1).expect("placeholder fs token must encode"),
            next_seq: 2,
            response_poll_limit: DEFAULT_RESPONSE_POLL_LIMIT,
        };

        client.clear_mapped_pages();
        let request = client.request_header();
        request.magic = REQUEST_MAGIC;
        request.version = VERSION;
        request.op = opcode_raw(Opcode::Connect);
        request.object_token = 0;
        request.offset = 0;
        request.length = 0;
        request.flags = 0;
        request.path_bytes = 0;
        request.inline_bytes = 0;
        request.reserved0 = 0;
        request.arg0 = response_paddr;
        request.arg1 = client_process_slot;
        compiler_fence(Ordering::SeqCst);
        request.request_seq = 1;

        if unit_syscall(syscall::call2(
            syscall::SHARE_CAP,
            request_paddr,
            endpoint_id,
        ))
        .is_err()
        {
            return Err(Error::ConnectSendFailed);
        }
        let response = client.finish_request_ok(1, Opcode::Connect)?;
        let result_token = response.result_token;
        let object_kind = FsObjectKind::from_raw(response.object_kind as u64);
        client.mount_token = FsObjectToken::from_raw(result_token).ok_or(Error::InvalidResponse)?;
        if object_kind != FsObjectKind::Mount {
            return Err(Error::InvalidResponse);
        }
        Ok(client)
    }

    pub const fn connection_id(&self) -> FsConnectionId {
        self.connection_id
    }

    pub const fn request_va(&self) -> u64 {
        self.request_va
    }

    pub const fn response_va(&self) -> u64 {
        self.response_va
    }

    pub const fn request_paddr(&self) -> u64 {
        self.request_paddr
    }

    pub const fn response_paddr(&self) -> u64 {
        self.response_paddr
    }

    pub fn root_dir(&mut self) -> Result<Dir, Error> {
        let root = self.lookup_raw(self.mount_token, ".")?;
        if root.object_kind != FsObjectKind::VnodeDir {
            return Err(Error::InvalidResponse);
        }
        Ok(Dir {
            connection_id: self.connection_id,
            token: DirToken::from_fs_token(root.token),
        })
    }

    pub fn lookup(&mut self, dir: Dir, path: &str) -> Result<LookupResult, Error> {
        self.ensure_connection(dir.connection_id())?;
        self.lookup_raw(dir.token().fs_token(), path)
    }

    pub fn lookup_dir(&mut self, dir: Dir, path: &str) -> Result<Dir, Error> {
        let result = self.lookup(dir, path)?;
        if result.object_kind != FsObjectKind::VnodeDir {
            return Err(Error::NotDir);
        }
        Ok(Dir {
            connection_id: self.connection_id,
            token: DirToken::from_fs_token(result.token),
        })
    }

    pub fn lookup_file(&mut self, dir: Dir, path: &str) -> Result<VnodeFile, Error> {
        let result = self.lookup(dir, path)?;
        if result.object_kind != FsObjectKind::VnodeFile {
            return Err(Error::IsDir);
        }
        Ok(VnodeFile {
            connection_id: self.connection_id,
            token: VnodeFileToken::from_fs_token(result.token),
            file_bytes: result.file_bytes,
        })
    }

    pub fn stat_dir(&mut self, dir: Dir) -> Result<StatResult, Error> {
        self.ensure_connection(dir.connection_id())?;
        self.stat_raw(dir.token().fs_token())
    }

    pub fn stat_file(&mut self, file: VnodeFile) -> Result<StatResult, Error> {
        self.ensure_connection(file.connection_id())?;
        self.stat_raw(file.token().fs_token())
    }

    pub fn stat_open_file(&mut self, file: OpenFile) -> Result<StatResult, Error> {
        self.ensure_connection(file.connection_id())?;
        self.stat_raw(file.token().fs_token())
    }

    pub fn statfs(&mut self) -> Result<StatFsResult, Error> {
        let seq = self.begin_request(Opcode::StatFs, self.mount_token.raw(), 0, 0, 0, "", &[])?;
        let response = self.finish_request_ok(seq, Opcode::StatFs)?;
        Ok(StatFsResult {
            block_size: response.arg0,
            capacity_blocks: response.arg1,
            used_blocks: response.file_bytes,
        })
    }

    pub fn readdir_one<'a>(
        &mut self,
        dir: Dir,
        cursor: u64,
        name_buf: &'a mut [u8],
    ) -> Result<ReaddirResult<'a>, Error> {
        self.ensure_connection(dir.connection_id())?;
        let seq = self.begin_request(Opcode::Readdir, dir.token().raw(), cursor, 1, 0, "", &[])?;
        let (status, inline_bytes) = {
            let response = self.finish_request(seq, Opcode::Readdir)?;
            (
                parse_status(response.status).ok_or(Error::InvalidResponse)?,
                response.inline_bytes as usize,
            )
        };
        match status {
            Status::Ok => {}
            Status::EndOfDir => return Ok(ReaddirResult::End),
            status => return Err(status_to_error(status)),
        }
        if inline_bytes < DIRENT_RECORD_BYTES {
            return Err(Error::InvalidResponse);
        }
        let record = self.response_dirent_record();
        let name_len = record.name_bytes as usize;
        if name_len > name_buf.len() {
            return Err(Error::BufferTooSmall);
        }
        let needed = DIRENT_RECORD_BYTES + name_len;
        if needed > inline_bytes {
            return Err(Error::InvalidResponse);
        }
        unsafe {
            copy_volatile_bytes(
                self.response_payload().add(DIRENT_RECORD_BYTES),
                &mut name_buf[..name_len],
            );
        }
        Ok(ReaddirResult::Entry(ReaddirEntry {
            next_cursor: record.next_cursor,
            object_kind: FsObjectKind::from_raw(record.object_kind as u64),
            name: &name_buf[..name_len],
        }))
    }

    pub fn create(&mut self, dir: Dir, path: &str) -> Result<LookupResult, Error> {
        self.create_with_flags(dir, path, 0)
    }

    pub fn create_dir(&mut self, dir: Dir, path: &str) -> Result<Dir, Error> {
        let result = self.create_with_flags(dir, path, CREATE_FLAG_DIRECTORY)?;
        if result.object_kind != FsObjectKind::VnodeDir {
            return Err(Error::NotDir);
        }
        Ok(Dir {
            connection_id: self.connection_id,
            token: DirToken::from_fs_token(result.token),
        })
    }

    pub fn create_file(&mut self, dir: Dir, path: &str) -> Result<VnodeFile, Error> {
        let result = self.create_with_flags(dir, path, 0)?;
        if result.object_kind != FsObjectKind::VnodeFile {
            return Err(Error::IsDir);
        }
        Ok(VnodeFile {
            connection_id: self.connection_id,
            token: VnodeFileToken::from_fs_token(result.token),
            file_bytes: result.file_bytes,
        })
    }

    pub fn open_file(&mut self, file: VnodeFile) -> Result<OpenFile, Error> {
        self.ensure_connection(file.connection_id())?;
        let seq = self.begin_request(Opcode::Open, file.token().raw(), 0, 0, 0, "", &[])?;
        self.open_file_from_response(seq, Opcode::Open)
    }

    pub fn open_exec(&mut self, file: VnodeFile) -> Result<OpenExecResult, Error> {
        self.ensure_connection(file.connection_id())?;
        let seq = self.begin_request(Opcode::OpenExec, file.token().raw(), 0, 0, 0, "", &[])?;
        self.open_exec_from_response(seq, Opcode::OpenExec)
    }

    pub fn read(
        &mut self,
        file: OpenFile,
        offset: u64,
        out: &mut [u8],
    ) -> Result<ReadResult, Error> {
        self.ensure_connection(file.connection_id())?;
        let request_len = usize::min(out.len(), RESPONSE_PAYLOAD_BYTES);
        let seq = self.begin_request(
            Opcode::Read,
            file.token().raw(),
            offset,
            request_len as u32,
            0,
            "",
            &[],
        )?;
        let response = self.finish_request_ok(seq, Opcode::Read)?;
        let inline_bytes = response.inline_bytes as usize;
        let file_bytes = response.file_bytes;
        let next_offset = response.cursor_next;
        if inline_bytes > out.len() || inline_bytes > RESPONSE_PAYLOAD_BYTES {
            return Err(Error::BufferTooSmall);
        }
        copy_volatile_bytes(self.response_payload(), &mut out[..inline_bytes]);
        Ok(ReadResult {
            bytes_read: inline_bytes,
            file_bytes,
            next_offset,
        })
    }

    pub fn write(&mut self, file: OpenFile, offset: u64, bytes: &[u8]) -> Result<u64, Error> {
        self.ensure_connection(file.connection_id())?;
        if bytes.len() > REQUEST_PAYLOAD_BYTES {
            return Err(Error::BufferTooSmall);
        }
        let seq = self.begin_request(
            Opcode::Write,
            file.token().raw(),
            offset,
            bytes.len() as u32,
            0,
            "",
            bytes,
        )?;
        let response = self.finish_request_ok(seq, Opcode::Write)?;
        Ok(response.file_bytes)
    }

    pub fn close_dir(&mut self, dir: Dir) -> Result<(), Error> {
        self.ensure_connection(dir.connection_id())?;
        self.close_raw(dir.token().fs_token())
    }

    pub fn close_file(&mut self, file: VnodeFile) -> Result<(), Error> {
        self.ensure_connection(file.connection_id())?;
        self.close_raw(file.token().fs_token())
    }

    pub fn close_open_file(&mut self, file: OpenFile) -> Result<(), Error> {
        self.ensure_connection(file.connection_id())?;
        self.close_raw(file.token().fs_token())
    }

    pub fn unlink(&mut self, dir: Dir, path: &str) -> Result<(), Error> {
        self.ensure_connection(dir.connection_id())?;
        let seq = self.begin_request(Opcode::Unlink, dir.token().raw(), 0, 0, 0, path, &[])?;
        let _ = self.finish_request_ok(seq, Opcode::Unlink)?;
        Ok(())
    }

    pub fn rename(&mut self, dir: Dir, old_path: &str, new_path: &str) -> Result<(), Error> {
        self.ensure_connection(dir.connection_id())?;
        let seq = self.begin_request(
            Opcode::Rename,
            dir.token().raw(),
            0,
            0,
            0,
            old_path,
            new_path.as_bytes(),
        )?;
        let _ = self.finish_request_ok(seq, Opcode::Rename)?;
        Ok(())
    }

    fn ensure_connection(&self, connection_id: FsConnectionId) -> Result<(), Error> {
        if self.connection_id != connection_id {
            return Err(Error::WrongConnection);
        }
        Ok(())
    }

    fn lookup_raw(
        &mut self,
        object_token: FsObjectToken,
        path: &str,
    ) -> Result<LookupResult, Error> {
        let seq = self.begin_request(Opcode::Lookup, object_token.raw(), 0, 0, 0, path, &[])?;
        self.lookup_result_from_response(seq, Opcode::Lookup)
    }

    fn create_with_flags(
        &mut self,
        dir: Dir,
        path: &str,
        flags: u32,
    ) -> Result<LookupResult, Error> {
        self.ensure_connection(dir.connection_id())?;
        let seq = self.begin_request(Opcode::Create, dir.token().raw(), 0, 0, flags, path, &[])?;
        self.lookup_result_from_response(seq, Opcode::Create)
    }

    fn lookup_result_from_response(
        &mut self,
        expected_seq: u64,
        expected_op: Opcode,
    ) -> Result<LookupResult, Error> {
        let response = self.finish_request_ok(expected_seq, expected_op)?;
        Ok(LookupResult {
            token: FsObjectToken::from_raw(response.result_token).ok_or(Error::InvalidResponse)?,
            object_kind: FsObjectKind::from_raw(response.object_kind as u64),
            file_bytes: response.file_bytes,
        })
    }

    fn stat_raw(&mut self, object_token: FsObjectToken) -> Result<StatResult, Error> {
        let seq = self.begin_request(Opcode::Stat, object_token.raw(), 0, 0, 0, "", &[])?;
        let response = self.finish_request_ok(seq, Opcode::Stat)?;
        if (response.inline_bytes as usize) < STAT_RECORD_BYTES {
            return Err(Error::InvalidResponse);
        }
        let stat_record = self.response_stat_record();
        Ok(StatResult {
            object_kind: FsObjectKind::from_raw(stat_record.object_kind as u64),
            size_bytes: stat_record.size_bytes,
            mode_bits: stat_record.mode_bits,
            mtime_unix_sec: stat_record.mtime_unix_sec,
        })
    }

    fn open_file_from_response(
        &mut self,
        expected_seq: u64,
        expected_op: Opcode,
    ) -> Result<OpenFile, Error> {
        let (object_kind, result_token, file_bytes) = {
            let response = self.finish_request_ok(expected_seq, expected_op)?;
            (
                FsObjectKind::from_raw(response.object_kind as u64),
                response.result_token,
                response.file_bytes,
            )
        };
        if object_kind != FsObjectKind::OpenFile {
            return Err(Error::InvalidResponse);
        }
        let token = FsObjectToken::from_raw(result_token).ok_or(Error::InvalidResponse)?;
        Ok(OpenFile {
            connection_id: self.connection_id,
            token: OpenFileToken::from_fs_token(token),
            file_bytes,
        })
    }

    fn open_exec_from_response(
        &mut self,
        expected_seq: u64,
        expected_op: Opcode,
    ) -> Result<OpenExecResult, Error> {
        let (object_kind, result_token, file_bytes) = {
            let response = self.finish_request_ok(expected_seq, expected_op)?;
            (
                FsObjectKind::from_raw(response.object_kind as u64),
                response.result_token,
                response.file_bytes,
            )
        };
        if object_kind != FsObjectKind::Exec {
            return Err(Error::InvalidResponse);
        }
        let token = ExecImageToken::from_raw(result_token).ok_or(Error::InvalidResponse)?;
        Ok(OpenExecResult { token, file_bytes })
    }

    fn close_raw(&mut self, object_token: FsObjectToken) -> Result<(), Error> {
        let seq = self.begin_request(Opcode::Close, object_token.raw(), 0, 0, 0, "", &[])?;
        let _ = self.finish_request_ok(seq, Opcode::Close)?;
        Ok(())
    }

    fn request_header(&self) -> &mut FsRequestHeader {
        unsafe { &mut *(self.request_va as *mut FsRequestHeader) }
    }

    fn response_header(&self) -> &FsResponseHeader {
        unsafe { &*(self.response_va as *const FsResponseHeader) }
    }

    fn response_stat_record(&self) -> &FsStatRecord {
        unsafe { &*((self.response_va + RESPONSE_HEADER_BYTES as u64) as *const FsStatRecord) }
    }

    fn response_dirent_record(&self) -> &FsDirentRecord {
        unsafe { &*((self.response_va + RESPONSE_HEADER_BYTES as u64) as *const FsDirentRecord) }
    }

    fn request_payload(&self) -> *mut u8 {
        (self.request_va + REQUEST_HEADER_BYTES as u64) as *mut u8
    }

    fn response_payload(&self) -> *const u8 {
        (self.response_va + RESPONSE_HEADER_BYTES as u64) as *const u8
    }

    fn clear_mapped_pages(&self) {
        clear_page(self.request_va);
        clear_page(self.response_va);
    }

    fn begin_request(
        &mut self,
        op: Opcode,
        object_token: u64,
        offset: u64,
        length: u32,
        flags: u32,
        path: &str,
        inline_payload: &[u8],
    ) -> Result<u64, Error> {
        if path.len() > MAX_PATH_BYTES || path.len() > REQUEST_PAYLOAD_BYTES {
            return Err(Error::PathTooLong);
        }
        if inline_payload.len() > REQUEST_PAYLOAD_BYTES {
            return Err(Error::BufferTooSmall);
        }
        if path.len() + inline_payload.len() > REQUEST_PAYLOAD_BYTES {
            return Err(Error::BufferTooSmall);
        }

        let seq = self.next_seq;
        self.next_seq += 1;
        self.clear_mapped_pages();
        let request = self.request_header();
        request.magic = REQUEST_MAGIC;
        request.version = VERSION;
        request.op = opcode_raw(op);
        request.object_token = object_token;
        request.offset = offset;
        request.length = length;
        request.flags = flags;
        request.path_bytes = path.len() as u16;
        request.inline_bytes = inline_payload.len() as u16;
        request.reserved0 = 0;
        request.arg0 = 0;
        request.arg1 = 0;
        if !path.is_empty() {
            copy_bytes_to_volatile(self.request_payload(), path.as_bytes());
        }
        if !inline_payload.is_empty() {
            unsafe {
                copy_bytes_to_volatile(self.request_payload().add(path.len()), inline_payload);
            }
        }
        compiler_fence(Ordering::SeqCst);
        request.request_seq = seq;
        let _ = syscall::call1(syscall::SIGNAL_ENDPOINT, self.server_endpoint_id);
        Ok(seq)
    }

    fn finish_request(
        &mut self,
        expected_seq: u64,
        expected_op: Opcode,
    ) -> Result<&FsResponseHeader, Error> {
        if !self.wait_for_response(expected_seq) {
            return Err(Error::Timeout);
        }
        let response = self.response_header();
        if response.magic != RESPONSE_MAGIC || response.version != VERSION {
            return Err(Error::InvalidResponse);
        }
        if response.op != opcode_raw(expected_op) || response.response_seq != expected_seq {
            return Err(Error::InvalidResponse);
        }
        let _ = parse_status(response.status).ok_or(Error::InvalidResponse)?;
        Ok(response)
    }

    fn finish_request_ok(
        &mut self,
        expected_seq: u64,
        expected_op: Opcode,
    ) -> Result<&FsResponseHeader, Error> {
        let response = self.finish_request(expected_seq, expected_op)?;
        let status = parse_status(response.status).ok_or(Error::InvalidResponse)?;
        if status != Status::Ok {
            return Err(status_to_error(status));
        }
        Ok(response)
    }

    fn wait_for_response(&self, expected_seq: u64) -> bool {
        let response = self.response_header();
        let mut poll_count = 0;
        while poll_count < self.response_poll_limit {
            if unsafe { read_volatile(addr_of!(response.response_seq)) } == expected_seq {
                return true;
            }
            let _ = syscall::call2(syscall::WAIT_EVENT, 0, 1);
            poll_count += 1;
        }
        false
    }
}

pub fn monotonic_now_ticks() -> u64 {
    syscall::call0(syscall::GET_TICK_COUNT)
}

fn current_process_slot() -> Result<u64, Error> {
    let raw = syscall::call0(syscall::GET_PROCESS_SLOT);
    if raw == 0 || raw == syscall::ERR_INVALID {
        return Err(Error::Invalid);
    }
    Ok(raw)
}

fn alloc_page() -> Option<u64> {
    let raw = syscall::call0(syscall::ALLOC_PAGE);
    if raw < 0x1000 { None } else { Some(raw) }
}

fn clear_page(va: u64) {
    let bytes = va as *mut u8;
    let mut i = 0;
    while i < PAGE_BYTES {
        unsafe { write_volatile(bytes.add(i), 0) };
        i += 1;
    }
}

fn copy_bytes_to_volatile(dst: *mut u8, src: &[u8]) {
    let mut i = 0;
    while i < src.len() {
        unsafe { write_volatile(dst.add(i), src[i]) };
        i += 1;
    }
}

fn copy_volatile_bytes(src: *const u8, dst: &mut [u8]) {
    let mut i = 0;
    while i < dst.len() {
        dst[i] = unsafe { read_volatile(src.add(i)) };
        i += 1;
    }
}

fn opcode_raw(op: Opcode) -> u16 {
    op as u16
}

fn parse_status(raw: i32) -> Option<Status> {
    match raw {
        0 => Some(Status::Ok),
        1 => Some(Status::Invalid),
        2 => Some(Status::NotFound),
        3 => Some(Status::NotDir),
        4 => Some(Status::IsDir),
        5 => Some(Status::NoRight),
        6 => Some(Status::TooBig),
        7 => Some(Status::NotSupported),
        8 => Some(Status::IoError),
        9 => Some(Status::Busy),
        10 => Some(Status::EndOfDir),
        _ => None,
    }
}

fn status_to_error(status: Status) -> Error {
    match status {
        Status::Ok => Error::InvalidResponse,
        Status::Invalid => Error::Invalid,
        Status::NotFound => Error::NotFound,
        Status::NotDir => Error::NotDir,
        Status::IsDir => Error::IsDir,
        Status::NoRight => Error::NoRight,
        Status::TooBig => Error::TooBig,
        Status::NotSupported => Error::NotSupported,
        Status::IoError => Error::IoError,
        Status::Busy => Error::Busy,
        Status::EndOfDir => Error::InvalidResponse,
    }
}

fn unit_syscall(raw: u64) -> Result<(), SyscallError> {
    if raw == syscall::OK {
        Ok(())
    } else {
        Err(SyscallError::from_error_raw(raw))
    }
}
