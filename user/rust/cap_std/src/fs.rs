extern crate alloc;

use alloc::rc::Rc;
use alloc::string::String;
use core::cell::RefCell;
use core::str;

use rt_handle::{ExecImageToken, FsConnectionId, FsObjectKind};
use rt_io::{PersistentFsClient, ReaddirResult};

use crate::io::{Read, Seek, SeekFrom, Write, apply_seek};
use crate::path::{Path, PathBuf};
use crate::time::SystemTime;
use crate::{Error, ErrorKind, Result};

struct Session {
    client: RefCell<PersistentFsClient>,
}

type SharedSession = Rc<Session>;

impl Session {
    fn connect_default() -> Result<SharedSession> {
        Self::connect(FsConnectionId::new(1))
    }

    fn connect(connection_id: FsConnectionId) -> Result<SharedSession> {
        let client = PersistentFsClient::connect_from_shadow(connection_id).map_err(Error::from)?;
        Ok(Rc::new(Self {
            client: RefCell::new(client),
        }))
    }
}

fn closed_error() -> Error {
    Error::new(ErrorKind::Closed)
}

fn open_dir_path(session: &SharedSession, dir: rt_io::Dir, path: &Path) -> Result<rt_io::Dir> {
    session
        .client
        .borrow_mut()
        .lookup_dir(dir, path.as_str())
        .map_err(Error::from)
}

fn open_file_path(
    session: &SharedSession,
    dir: rt_io::Dir,
    path: &Path,
) -> Result<rt_io::OpenFile> {
    let mut client = session.client.borrow_mut();
    let vnode = client
        .lookup_file(dir, path.as_str())
        .map_err(Error::from)?;
    client.open_file(vnode).map_err(Error::from)
}

fn metadata_path(session: &SharedSession, dir: rt_io::Dir, path: &Path) -> Result<Metadata> {
    let mut client = session.client.borrow_mut();
    match client
        .lookup(dir, path.as_str())
        .map_err(Error::from)?
        .object_kind
    {
        FsObjectKind::VnodeDir => {
            let dir = client.lookup_dir(dir, path.as_str()).map_err(Error::from)?;
            client
                .stat_dir(dir)
                .map(Metadata::from_stat)
                .map_err(Error::from)
        }
        FsObjectKind::VnodeFile => {
            let file = client
                .lookup_file(dir, path.as_str())
                .map_err(Error::from)?;
            client
                .stat_file(file)
                .map(Metadata::from_stat)
                .map_err(Error::from)
        }
        _ => Err(Error::new(ErrorKind::Other)),
    }
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub struct FileType {
    kind: FsObjectKind,
}

impl FileType {
    fn from_kind(kind: FsObjectKind) -> Self {
        Self { kind }
    }

    pub fn is_dir(self) -> bool {
        self.kind == FsObjectKind::VnodeDir
    }

    pub fn is_file(self) -> bool {
        self.kind == FsObjectKind::VnodeFile || self.kind == FsObjectKind::OpenFile
    }

    pub fn kind_name(self) -> &'static str {
        self.kind.name()
    }
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub struct Metadata {
    file_type: FileType,
    len: u64,
    modified: SystemTime,
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub struct StorageStats {
    block_size: u64,
    capacity_blocks: u64,
    used_blocks: u64,
}

impl Metadata {
    fn from_stat(stat: rt_io::StatResult) -> Self {
        Self {
            file_type: FileType::from_kind(stat.object_kind),
            len: stat.size_bytes,
            modified: SystemTime::from_unix_seconds(stat.mtime_unix_sec),
        }
    }

    pub fn file_type(self) -> FileType {
        self.file_type
    }

    pub fn len(self) -> u64 {
        self.len
    }

    pub fn modified(self) -> SystemTime {
        self.modified
    }

    pub fn is_dir(self) -> bool {
        self.file_type.is_dir()
    }

    pub fn is_file(self) -> bool {
        self.file_type.is_file()
    }
}

impl StorageStats {
    fn from_statfs(stat: rt_io::StatFsResult) -> Self {
        Self {
            block_size: stat.block_size,
            capacity_blocks: stat.capacity_blocks,
            used_blocks: stat.used_blocks,
        }
    }

    pub fn block_size(self) -> u64 {
        self.block_size
    }

    pub fn capacity_blocks(self) -> u64 {
        self.capacity_blocks
    }

    pub fn used_blocks(self) -> u64 {
        self.used_blocks
    }

    pub fn capacity_bytes(self) -> u64 {
        self.capacity_blocks.saturating_mul(self.block_size)
    }

    pub fn used_bytes(self) -> u64 {
        self.used_blocks.saturating_mul(self.block_size)
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct DirEntry {
    name: PathBuf,
    file_type: FileType,
}

impl DirEntry {
    pub fn file_name(&self) -> &Path {
        self.name.as_path()
    }

    pub fn file_type(&self) -> FileType {
        self.file_type
    }
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub struct Executable {
    token: ExecImageToken,
    file_bytes: u64,
}

impl Executable {
    pub fn token(self) -> ExecImageToken {
        self.token
    }

    pub fn len(self) -> u64 {
        self.file_bytes
    }

    pub fn command(self) -> crate::process::Command {
        crate::process::Command::new(self)
    }

    pub fn spawn(self) -> Result<crate::process::Child> {
        self.command().spawn()
    }
}

pub struct RootDir {
    session: SharedSession,
    raw: Option<rt_io::Dir>,
}

impl RootDir {
    pub fn connect_default() -> Result<Self> {
        let session = Session::connect_default()?;
        let raw = session
            .client
            .borrow_mut()
            .root_dir()
            .map_err(Error::from)?;
        Ok(Self {
            session,
            raw: Some(raw),
        })
    }

    pub fn connect(connection_id: FsConnectionId) -> Result<Self> {
        let session = Session::connect(connection_id)?;
        let raw = session
            .client
            .borrow_mut()
            .root_dir()
            .map_err(Error::from)?;
        Ok(Self {
            session,
            raw: Some(raw),
        })
    }

    fn raw(&self) -> Result<rt_io::Dir> {
        self.raw.ok_or_else(closed_error)
    }

    fn close_inner(&mut self) -> Result<()> {
        let raw = self.raw.take().ok_or_else(closed_error)?;
        self.session
            .client
            .borrow_mut()
            .close_dir(raw)
            .map_err(Error::from)
    }

    pub fn metadata<P>(&self, path: P) -> Result<Metadata>
    where
        P: AsRef<Path>,
    {
        metadata_path(&self.session, self.raw()?, path.as_ref())
    }

    pub fn storage_stats(&self) -> Result<StorageStats> {
        self.session
            .client
            .borrow_mut()
            .statfs()
            .map(StorageStats::from_statfs)
            .map_err(Error::from)
    }

    pub fn open_dir<P>(&self, path: P) -> Result<Dir>
    where
        P: AsRef<Path>,
    {
        let raw = open_dir_path(&self.session, self.raw()?, path.as_ref())?;
        Ok(Dir {
            session: Rc::clone(&self.session),
            raw: Some(raw),
        })
    }

    pub fn open_file<P>(&self, path: P) -> Result<File>
    where
        P: AsRef<Path>,
    {
        let raw = open_file_path(&self.session, self.raw()?, path.as_ref())?;
        Ok(File {
            session: Rc::clone(&self.session),
            raw: Some(raw),
            cursor: 0,
            file_bytes: raw.file_bytes(),
        })
    }

    pub fn create_dir<P>(&self, path: P) -> Result<Dir>
    where
        P: AsRef<Path>,
    {
        let raw = self
            .session
            .client
            .borrow_mut()
            .create_dir(self.raw()?, path.as_ref().as_str())
            .map_err(Error::from)?;
        Ok(Dir {
            session: Rc::clone(&self.session),
            raw: Some(raw),
        })
    }

    pub fn create_file<P>(&self, path: P) -> Result<File>
    where
        P: AsRef<Path>,
    {
        let mut client = self.session.client.borrow_mut();
        let vnode = client
            .create_file(self.raw()?, path.as_ref().as_str())
            .map_err(Error::from)?;
        let raw = client.open_file(vnode).map_err(Error::from)?;
        Ok(File {
            session: Rc::clone(&self.session),
            raw: Some(raw),
            cursor: 0,
            file_bytes: raw.file_bytes(),
        })
    }

    pub fn read_dir<P>(&self, path: P) -> Result<ReadDir>
    where
        P: AsRef<Path>,
    {
        let dir = self.open_dir(path)?;
        Ok(ReadDir {
            dir: Some(dir),
            cursor: 0,
            done: false,
        })
    }

    pub fn remove<P>(&self, path: P) -> Result<()>
    where
        P: AsRef<Path>,
    {
        self.session
            .client
            .borrow_mut()
            .unlink(self.raw()?, path.as_ref().as_str())
            .map_err(Error::from)
    }

    pub fn rename<P, Q>(&self, old_path: P, new_path: Q) -> Result<()>
    where
        P: AsRef<Path>,
        Q: AsRef<Path>,
    {
        self.session
            .client
            .borrow_mut()
            .rename(
                self.raw()?,
                old_path.as_ref().as_str(),
                new_path.as_ref().as_str(),
            )
            .map_err(Error::from)
    }

    pub fn open_exec<P>(&self, path: P) -> Result<Executable>
    where
        P: AsRef<Path>,
    {
        let mut client = self.session.client.borrow_mut();
        let vnode = client
            .lookup_file(self.raw()?, path.as_ref().as_str())
            .map_err(Error::from)?;
        let exec = client.open_exec(vnode).map_err(Error::from)?;
        Ok(Executable {
            token: exec.token,
            file_bytes: exec.file_bytes,
        })
    }

    pub fn close(mut self) -> Result<()> {
        self.close_inner()
    }
}

impl Drop for RootDir {
    fn drop(&mut self) {
        let _ = self.close_inner();
    }
}

pub struct Dir {
    session: SharedSession,
    raw: Option<rt_io::Dir>,
}

impl Dir {
    fn raw(&self) -> Result<rt_io::Dir> {
        self.raw.ok_or_else(closed_error)
    }

    fn close_inner(&mut self) -> Result<()> {
        let raw = self.raw.take().ok_or_else(closed_error)?;
        self.session
            .client
            .borrow_mut()
            .close_dir(raw)
            .map_err(Error::from)
    }

    pub fn metadata<P>(&self, path: P) -> Result<Metadata>
    where
        P: AsRef<Path>,
    {
        metadata_path(&self.session, self.raw()?, path.as_ref())
    }

    pub fn open_dir<P>(&self, path: P) -> Result<Dir>
    where
        P: AsRef<Path>,
    {
        let raw = open_dir_path(&self.session, self.raw()?, path.as_ref())?;
        Ok(Self {
            session: Rc::clone(&self.session),
            raw: Some(raw),
        })
    }

    pub fn open_file<P>(&self, path: P) -> Result<File>
    where
        P: AsRef<Path>,
    {
        let raw = open_file_path(&self.session, self.raw()?, path.as_ref())?;
        Ok(File {
            session: Rc::clone(&self.session),
            raw: Some(raw),
            cursor: 0,
            file_bytes: raw.file_bytes(),
        })
    }

    pub fn create_dir<P>(&self, path: P) -> Result<Dir>
    where
        P: AsRef<Path>,
    {
        let raw = self
            .session
            .client
            .borrow_mut()
            .create_dir(self.raw()?, path.as_ref().as_str())
            .map_err(Error::from)?;
        Ok(Self {
            session: Rc::clone(&self.session),
            raw: Some(raw),
        })
    }

    pub fn create_file<P>(&self, path: P) -> Result<File>
    where
        P: AsRef<Path>,
    {
        let mut client = self.session.client.borrow_mut();
        let vnode = client
            .create_file(self.raw()?, path.as_ref().as_str())
            .map_err(Error::from)?;
        let raw = client.open_file(vnode).map_err(Error::from)?;
        Ok(File {
            session: Rc::clone(&self.session),
            raw: Some(raw),
            cursor: 0,
            file_bytes: raw.file_bytes(),
        })
    }

    pub fn read_dir<P>(&self, path: P) -> Result<ReadDir>
    where
        P: AsRef<Path>,
    {
        let dir = self.open_dir(path)?;
        Ok(ReadDir {
            dir: Some(dir),
            cursor: 0,
            done: false,
        })
    }

    pub fn remove<P>(&self, path: P) -> Result<()>
    where
        P: AsRef<Path>,
    {
        self.session
            .client
            .borrow_mut()
            .unlink(self.raw()?, path.as_ref().as_str())
            .map_err(Error::from)
    }

    pub fn rename<P, Q>(&self, old_path: P, new_path: Q) -> Result<()>
    where
        P: AsRef<Path>,
        Q: AsRef<Path>,
    {
        self.session
            .client
            .borrow_mut()
            .rename(
                self.raw()?,
                old_path.as_ref().as_str(),
                new_path.as_ref().as_str(),
            )
            .map_err(Error::from)
    }

    pub fn open_exec<P>(&self, path: P) -> Result<Executable>
    where
        P: AsRef<Path>,
    {
        let mut client = self.session.client.borrow_mut();
        let vnode = client
            .lookup_file(self.raw()?, path.as_ref().as_str())
            .map_err(Error::from)?;
        let exec = client.open_exec(vnode).map_err(Error::from)?;
        Ok(Executable {
            token: exec.token,
            file_bytes: exec.file_bytes,
        })
    }

    pub fn close(mut self) -> Result<()> {
        self.close_inner()
    }
}

impl Drop for Dir {
    fn drop(&mut self) {
        let _ = self.close_inner();
    }
}

pub struct ReadDir {
    dir: Option<Dir>,
    cursor: u64,
    done: bool,
}

impl ReadDir {
    fn dir_ref(&self) -> Result<&Dir> {
        self.dir.as_ref().ok_or_else(closed_error)
    }

    pub fn next_entry(&mut self) -> Result<Option<DirEntry>> {
        if self.done {
            return Ok(None);
        }

        let dir = self.dir_ref()?;
        let mut name_buf = [0u8; 128];
        let result = dir
            .session
            .client
            .borrow_mut()
            .readdir_one(dir.raw()?, self.cursor, &mut name_buf)
            .map_err(Error::from)?;
        match result {
            ReaddirResult::End => {
                self.done = true;
                Ok(None)
            }
            ReaddirResult::Entry(entry) => {
                self.cursor = entry.next_cursor;
                let name =
                    str::from_utf8(entry.name).map_err(|_| Error::new(ErrorKind::InvalidData))?;
                Ok(Some(DirEntry {
                    name: PathBuf::from(String::from(name)),
                    file_type: FileType::from_kind(entry.object_kind),
                }))
            }
        }
    }

    pub fn close(mut self) -> Result<()> {
        self.dir.take().ok_or_else(closed_error)?.close()
    }
}

impl Iterator for ReadDir {
    type Item = Result<DirEntry>;

    fn next(&mut self) -> Option<Self::Item> {
        match self.next_entry() {
            Ok(Some(entry)) => Some(Ok(entry)),
            Ok(None) => None,
            Err(err) => {
                self.done = true;
                Some(Err(err))
            }
        }
    }
}

pub struct File {
    session: SharedSession,
    raw: Option<rt_io::OpenFile>,
    cursor: u64,
    file_bytes: u64,
}

impl File {
    fn raw(&self) -> Result<rt_io::OpenFile> {
        self.raw.ok_or_else(closed_error)
    }

    fn close_inner(&mut self) -> Result<()> {
        let raw = self.raw.take().ok_or_else(closed_error)?;
        self.session
            .client
            .borrow_mut()
            .close_open_file(raw)
            .map_err(Error::from)
    }

    pub fn metadata(&self) -> Result<Metadata> {
        self.session
            .client
            .borrow_mut()
            .stat_open_file(self.raw()?)
            .map(Metadata::from_stat)
            .map_err(Error::from)
    }

    pub fn len(&self) -> u64 {
        self.file_bytes
    }

    pub fn close(mut self) -> Result<()> {
        self.close_inner()
    }
}

impl Drop for File {
    fn drop(&mut self) {
        let _ = self.close_inner();
    }
}

impl Read for File {
    fn read(&mut self, buf: &mut [u8]) -> Result<usize> {
        let read = self
            .session
            .client
            .borrow_mut()
            .read(self.raw()?, self.cursor, buf)
            .map_err(Error::from)?;
        self.cursor = read.next_offset;
        self.file_bytes = read.file_bytes;
        Ok(read.bytes_read)
    }
}

impl Write for File {
    fn write(&mut self, buf: &[u8]) -> Result<usize> {
        let file_bytes = self
            .session
            .client
            .borrow_mut()
            .write(self.raw()?, self.cursor, buf)
            .map_err(Error::from)?;
        self.cursor = self
            .cursor
            .checked_add(buf.len() as u64)
            .ok_or_else(|| Error::new(ErrorKind::InvalidInput))?;
        self.file_bytes = file_bytes;
        Ok(buf.len())
    }
}

impl Seek for File {
    fn seek(&mut self, position: SeekFrom) -> Result<u64> {
        let next = apply_seek(self.cursor, self.file_bytes, position)?;
        self.cursor = next;
        Ok(next)
    }
}
