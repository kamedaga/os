extern crate alloc;

use alloc::string::String;
use alloc::vec::Vec;
use core::fmt::Write as _;
use core::ptr::{read_volatile, write_bytes};

use crate::{Error, ErrorKind, Result};

const CHILD_EXIT_STATUS_SLOT_CANDIDATES: [u64; 8] = [
    0x3F21_2000,
    0x3F21_3000,
    0x3F21_4000,
    0x3F21_5000,
    0x3F21_6000,
    0x3F21_7000,
    0x3F21_8000,
    0x3F21_9000,
];
const PROCESS_EXIT_STATUS_MAGIC: u64 = 0x5052_5845_5449_5431;
const PROCESS_EXIT_STATUS_VERSION: u64 = 1;
const PROCESS_EXIT_STATUS_STATE_IDLE: u64 = 0;
const PROCESS_EXIT_STATUS_STATE_EXITED: u64 = 1;
const PROCESS_EXIT_STATUS_PAGE_BYTES: usize = 4096;
const CHILD_ARGS_ENV_SLOT_CANDIDATES: [u64; 8] = [
    0x3F21_A000,
    0x3F21_B000,
    0x3F21_C000,
    0x3F21_D000,
    0x3F21_E000,
    0x3F21_F000,
    0x3F22_0000,
    0x3F22_1000,
];
const PROCESS_ARGS_ENV_MAGIC: u64 = 0x5052_4147_4556_3131;
const PROCESS_ARGS_ENV_VERSION: u64 = 1;
const PROCESS_ARGS_ENV_MAX_ARGS: usize = 32;
const PROCESS_ARGS_ENV_MAX_ENV: usize = 32;
const PROCESS_ARGS_ENV_DATA_BYTES: usize = 3792;

#[repr(C)]
#[derive(Copy, Clone)]
struct ArgsEnvEntry {
    offset: u16,
    len: u16,
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub struct ExitCode(u8);

impl ExitCode {
    pub const SUCCESS: Self = Self(0);
    pub const FAILURE: Self = Self(1);

    pub const fn from_raw(raw: u8) -> Self {
        Self(raw)
    }

    pub const fn raw(self) -> u8 {
        self.0
    }

    pub const fn success(self) -> bool {
        self.0 == 0
    }
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub enum ExitStatus {
    Exited(ExitCode),
    Faulted(u8),
}

impl ExitStatus {
    pub const fn success(self) -> bool {
        matches!(self, Self::Exited(code) if code.success())
    }

    pub const fn code(self) -> Option<ExitCode> {
        match self {
            Self::Exited(code) => Some(code),
            Self::Faulted(_) => None,
        }
    }

    pub const fn fault_vector(self) -> Option<u8> {
        match self {
            Self::Exited(_) => None,
            Self::Faulted(vector) => Some(vector),
        }
    }
}

pub trait Termination {
    fn report(self) -> ExitCode;
}

#[derive(Copy, Clone)]
struct ChildExitStatusSlot {
    source_va: u64,
    allocated: bool,
    in_use: bool,
}

#[derive(Copy, Clone)]
struct ChildArgsEnvSlot {
    source_va: u64,
    allocated: bool,
    in_use: bool,
}

static mut CHILD_EXIT_STATUS_SLOTS: [ChildExitStatusSlot; CHILD_EXIT_STATUS_SLOT_CANDIDATES.len()] =
    [ChildExitStatusSlot {
        source_va: 0,
        allocated: false,
        in_use: false,
    }; CHILD_EXIT_STATUS_SLOT_CANDIDATES.len()];

static mut CHILD_ARGS_ENV_SLOTS: [ChildArgsEnvSlot; CHILD_ARGS_ENV_SLOT_CANDIDATES.len()] =
    [ChildArgsEnvSlot {
        source_va: 0,
        allocated: false,
        in_use: false,
    }; CHILD_ARGS_ENV_SLOT_CANDIDATES.len()];

#[derive(Debug, Eq, PartialEq)]
pub struct Child {
    process_slot: u64,
    thread_slot: u64,
    exit_status_slot_index: Option<usize>,
    exit_status_source_va: u64,
    args_env_slot_index: Option<usize>,
}

impl Child {
    pub(crate) const fn from_rt(
        value: rt_handle::SpawnedProcess,
        exit_status_slot_index: Option<usize>,
        exit_status_source_va: u64,
        args_env_slot_index: Option<usize>,
    ) -> Self {
        Self {
            process_slot: value.process_slot(),
            thread_slot: value.thread_slot(),
            exit_status_slot_index,
            exit_status_source_va,
            args_env_slot_index,
        }
    }

    pub const fn process_slot(&self) -> u64 {
        self.process_slot
    }

    pub const fn thread_slot(&self) -> u64 {
        self.thread_slot
    }

    pub fn wait(mut self) -> Result<ExitStatus> {
        loop {
            let status = rt_core::get_process_status(self.process_slot);
            match status.kind() {
                rt_core::ProcessStatusKind::Active => {
                    let raw = rt_core::syscall::call2(rt_core::syscall::WAIT_EVENT, 0, 1);
                    if raw == rt_core::syscall::OK
                        || raw == rt_core::syscall::ERR_NOT_READY
                        || raw == rt_core::syscall::ERR_EMPTY
                    {
                        continue;
                    }
                    self.release_slots();
                    return Err(Error::from(rt_core::SyscallError::from_error_raw(raw)));
                }
                rt_core::ProcessStatusKind::Faulted => {
                    let vector = status.fault_vector();
                    self.release_slots();
                    return Ok(ExitStatus::Faulted(vector));
                }
                rt_core::ProcessStatusKind::Inactive => {
                    let exit_status = read_exit_status(self.exit_status_source_va);
                    self.release_slots();
                    return Ok(exit_status);
                }
            }
        }
    }

    fn release_slots(&mut self) {
        if let Some(index) = self.exit_status_slot_index.take() {
            release_child_exit_status_slot(index);
        }
        if let Some(index) = self.args_env_slot_index.take() {
            release_child_args_env_slot(index);
        }
        self.exit_status_source_va = 0;
    }
}

impl Drop for Child {
    fn drop(&mut self) {
        let Some(index) = self.exit_status_slot_index else {
            return;
        };
        if rt_core::get_process_status(self.process_slot).kind()
            == rt_core::ProcessStatusKind::Active
        {
            return;
        }
        let _ = index;
        self.release_slots();
    }
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub enum Stdio {
    Inherit,
    KernelLog,
    Null,
}

impl Stdio {
    pub const fn inherit() -> Self {
        Self::Inherit
    }

    pub const fn kernel_log() -> Self {
        Self::KernelLog
    }

    pub const fn null() -> Self {
        Self::Null
    }

    const fn to_rt(self) -> rt_handle::StdioMode {
        match self {
            Self::Inherit => rt_handle::StdioMode::Inherit,
            Self::KernelLog => rt_handle::StdioMode::KernelLog,
            Self::Null => rt_handle::StdioMode::Null,
        }
    }
}

pub struct Command {
    exec: crate::fs::Executable,
    log: Stdio,
    stdout: Stdio,
    stderr: Stdio,
    child_bootstrap_owner: bool,
    arg0: Option<String>,
    args: Vec<String>,
    env: Vec<(String, String)>,
}

impl Command {
    pub fn new(exec: crate::fs::Executable) -> Self {
        Self {
            exec,
            log: Stdio::Inherit,
            stdout: Stdio::Inherit,
            stderr: Stdio::Inherit,
            child_bootstrap_owner: false,
            arg0: None,
            args: Vec::new(),
            env: Vec::new(),
        }
    }

    pub const fn executable(&self) -> crate::fs::Executable {
        self.exec
    }

    pub const fn log_stdio(&self) -> Stdio {
        self.log
    }

    pub const fn stdout_stdio(&self) -> Stdio {
        self.stdout
    }

    pub const fn stderr_stdio(&self) -> Stdio {
        self.stderr
    }

    pub const fn child_bootstrap_owner_enabled(&self) -> bool {
        self.child_bootstrap_owner
    }

    pub fn arg0(&mut self, value: impl AsRef<str>) -> &mut Self {
        self.arg0 = Some(String::from(value.as_ref()));
        self
    }

    pub fn with_arg0(mut self, value: impl AsRef<str>) -> Self {
        self.arg0(value);
        self
    }

    pub fn arg(&mut self, value: impl AsRef<str>) -> &mut Self {
        self.args.push(String::from(value.as_ref()));
        self
    }

    pub fn with_arg(mut self, value: impl AsRef<str>) -> Self {
        self.arg(value);
        self
    }

    pub fn args<I, S>(&mut self, values: I) -> &mut Self
    where
        I: IntoIterator<Item = S>,
        S: AsRef<str>,
    {
        for value in values {
            self.arg(value);
        }
        self
    }

    pub fn env(&mut self, key: impl AsRef<str>, value: impl AsRef<str>) -> &mut Self {
        self.env
            .push((String::from(key.as_ref()), String::from(value.as_ref())));
        self
    }

    pub fn with_env(mut self, key: impl AsRef<str>, value: impl AsRef<str>) -> Self {
        self.env(key, value);
        self
    }

    pub fn stdio(&mut self, stdio: Stdio) -> &mut Self {
        self.log = stdio;
        self.stdout = stdio;
        self.stderr = stdio;
        self
    }

    pub fn with_stdio(mut self, stdio: Stdio) -> Self {
        self.stdio(stdio);
        self
    }

    pub fn log(&mut self, stdio: Stdio) -> &mut Self {
        self.log = stdio;
        self
    }

    pub fn with_log(mut self, stdio: Stdio) -> Self {
        self.log = stdio;
        self
    }

    pub fn stdout(&mut self, stdio: Stdio) -> &mut Self {
        self.stdout = stdio;
        self
    }

    pub fn with_stdout(mut self, stdio: Stdio) -> Self {
        self.stdout = stdio;
        self
    }

    pub fn stderr(&mut self, stdio: Stdio) -> &mut Self {
        self.stderr = stdio;
        self
    }

    pub fn with_stderr(mut self, stdio: Stdio) -> Self {
        self.stderr = stdio;
        self
    }

    pub fn child_bootstrap_owner(&mut self, enabled: bool) -> &mut Self {
        self.child_bootstrap_owner = enabled;
        self
    }

    pub fn with_child_bootstrap_owner(mut self, enabled: bool) -> Self {
        self.child_bootstrap_owner = enabled;
        self
    }

    pub fn spawn(&self) -> Result<Child> {
        let (exit_status_slot_index, exit_status_source_va) = acquire_child_exit_status_slot()?;
        let mut builder = rt_handle::SpawnBuilder::new(self.exec.token());
        builder
            .push_page(
                exit_status_source_va,
                rt_handle::fixed_va::PROCESS_EXIT_STATUS_TARGET_VA,
                rt_handle::SPAWN_FLAG_BOOTSTRAP_PAGE_WRITABLE,
            )
            .map_err(|_| {
                release_child_exit_status_slot(exit_status_slot_index);
                Error::new(ErrorKind::Other)
            })?;
        let mut args_env_slot_index = None;
        if self.arg0.is_some() || !self.args.is_empty() || !self.env.is_empty() {
            let (index, source_va) = acquire_child_args_env_slot()?;
            if !write_child_args_env_page(source_va, self.arg0.as_deref(), &self.args, &self.env) {
                release_child_args_env_slot(index);
                release_child_exit_status_slot(exit_status_slot_index);
                return Err(Error::new(ErrorKind::BufferTooSmall));
            }
            builder
                .push_page(
                    source_va,
                    rt_handle::fixed_va::PROCESS_ARGS_ENV_TARGET_VA,
                    0,
                )
                .map_err(|_| {
                    release_child_args_env_slot(index);
                    release_child_exit_status_slot(exit_status_slot_index);
                    Error::new(ErrorKind::Other)
                })?;
            args_env_slot_index = Some(index);
        }
        builder.set_stdio(rt_handle::SpawnStdio::new(
            self.log.to_rt(),
            self.stdout.to_rt(),
            self.stderr.to_rt(),
        ));
        builder.set_child_bootstrap_owner(self.child_bootstrap_owner);
        let spawned = builder.spawn().map_err(|err| {
            if let Some(index) = args_env_slot_index {
                release_child_args_env_slot(index);
            }
            release_child_exit_status_slot(exit_status_slot_index);
            Error::from(err)
        })?;
        Ok(Child::from_rt(
            spawned,
            Some(exit_status_slot_index),
            exit_status_source_va,
            args_env_slot_index,
        ))
    }
}

impl Termination for ExitCode {
    fn report(self) -> ExitCode {
        self
    }
}

impl Termination for () {
    fn report(self) -> ExitCode {
        ExitCode::SUCCESS
    }
}

impl Termination for Result<()> {
    fn report(self) -> ExitCode {
        match self {
            Ok(()) => ExitCode::SUCCESS,
            Err(err) => {
                let mut message = String::from("cap_std: main returned error kind=");
                let _ = write!(&mut message, "{:?}\n", err.kind());
                rt_core::log(&message);
                ExitCode::FAILURE
            }
        }
    }
}

fn init_child_exit_status_source_va(source_va: u64) {
    // SAFETY: `source_va` points at a process-owned scratch page reserved solely
    // for child exit-status propagation.
    unsafe {
        write_bytes(source_va as *mut u8, 0, PROCESS_EXIT_STATUS_PAGE_BYTES);
        let words = source_va as *mut u64;
        words.add(0).write(PROCESS_EXIT_STATUS_MAGIC);
        words.add(1).write(PROCESS_EXIT_STATUS_VERSION);
        words.add(2).write(PROCESS_EXIT_STATUS_STATE_IDLE);
        words.add(3).write(0);
    }
}

fn acquire_child_exit_status_slot() -> Result<(usize, u64)> {
    // SAFETY: CapabilityOS user processes are single-threaded today.
    unsafe {
        let slots = &raw mut CHILD_EXIT_STATUS_SLOTS;
        let mut index = 0;
        while index < CHILD_EXIT_STATUS_SLOT_CANDIDATES.len() {
            let slot = &mut (*slots)[index];
            if slot.in_use {
                index += 1;
                continue;
            }
            if !slot.allocated {
                let source_va = CHILD_EXIT_STATUS_SLOT_CANDIDATES[index];
                let status =
                    rt_core::syscall::call4(rt_core::syscall::ALLOC_MAP_PAGES, source_va, 1, 1, 0);
                if status != rt_core::syscall::OK {
                    index += 1;
                    continue;
                }
                slot.source_va = source_va;
                slot.allocated = true;
            }
            init_child_exit_status_source_va(slot.source_va);
            slot.in_use = true;
            return Ok((index, slot.source_va));
        }
    }
    Err(Error::new(ErrorKind::Busy))
}

fn release_child_exit_status_slot(index: usize) {
    if index >= CHILD_EXIT_STATUS_SLOT_CANDIDATES.len() {
        return;
    }
    // SAFETY: CapabilityOS user processes are single-threaded today, and the
    // slot index was allocated from this process-local table.
    unsafe {
        let slot = &mut CHILD_EXIT_STATUS_SLOTS[index];
        if !slot.allocated {
            return;
        }
        init_child_exit_status_source_va(slot.source_va);
        slot.in_use = false;
    }
}

fn init_child_args_env_source_va(source_va: u64) {
    // SAFETY: `source_va` points at a process-owned scratch page reserved solely
    // for child args/env propagation.
    unsafe {
        write_bytes(source_va as *mut u8, 0, PROCESS_EXIT_STATUS_PAGE_BYTES);
        let words = source_va as *mut u64;
        words.add(0).write(PROCESS_ARGS_ENV_MAGIC);
        words.add(1).write(PROCESS_ARGS_ENV_VERSION);
        words.add(2).write(0);
        words.add(3).write(0);
        words.add(4).write(0);
        words.add(5).write(0);
    }
}

fn acquire_child_args_env_slot() -> Result<(usize, u64)> {
    // SAFETY: CapabilityOS user processes are single-threaded today.
    unsafe {
        let slots = &raw mut CHILD_ARGS_ENV_SLOTS;
        let mut index = 0;
        while index < CHILD_ARGS_ENV_SLOT_CANDIDATES.len() {
            let slot = &mut (*slots)[index];
            if slot.in_use {
                index += 1;
                continue;
            }
            if !slot.allocated {
                let source_va = CHILD_ARGS_ENV_SLOT_CANDIDATES[index];
                let status =
                    rt_core::syscall::call4(rt_core::syscall::ALLOC_MAP_PAGES, source_va, 1, 1, 0);
                if status != rt_core::syscall::OK {
                    index += 1;
                    continue;
                }
                slot.source_va = source_va;
                slot.allocated = true;
            }
            init_child_args_env_source_va(slot.source_va);
            slot.in_use = true;
            return Ok((index, slot.source_va));
        }
    }
    Err(Error::new(ErrorKind::Busy))
}

fn release_child_args_env_slot(index: usize) {
    if index >= CHILD_ARGS_ENV_SLOT_CANDIDATES.len() {
        return;
    }
    // SAFETY: CapabilityOS user processes are single-threaded today, and the
    // slot index was allocated from this process-local table.
    unsafe {
        let slot = &mut CHILD_ARGS_ENV_SLOTS[index];
        if !slot.allocated {
            return;
        }
        init_child_args_env_source_va(slot.source_va);
        slot.in_use = false;
    }
}

fn write_child_args_env_page(
    source_va: u64,
    arg0: Option<&str>,
    args: &[String],
    env: &[(String, String)],
) -> bool {
    let mut page = ChildArgsEnvPageWriter::new(source_va);
    if let Some(arg0) = arg0 {
        if !page.push_arg(arg0) {
            return false;
        }
    }
    for arg in args {
        if !page.push_arg(arg.as_str()) {
            return false;
        }
    }
    for (key, value) in env {
        if !page.push_env(key.as_str(), value.as_str()) {
            return false;
        }
    }
    true
}

fn read_exit_status(source_va: u64) -> ExitStatus {
    if source_va == 0 {
        return ExitStatus::Exited(ExitCode::SUCCESS);
    }
    // SAFETY: `source_va` points at a parent-owned shared exit-status page that
    // stays mapped while the `Child` owns the slot.
    unsafe {
        let words = source_va as *const u64;
        if read_volatile(words.add(0)) != PROCESS_EXIT_STATUS_MAGIC
            || read_volatile(words.add(1)) != PROCESS_EXIT_STATUS_VERSION
        {
            return ExitStatus::Exited(ExitCode::SUCCESS);
        }
        let state = read_volatile(words.add(2));
        let code = read_volatile(words.add(3)) as u8;
        if state == PROCESS_EXIT_STATUS_STATE_EXITED {
            ExitStatus::Exited(ExitCode::from_raw(code))
        } else {
            ExitStatus::Exited(ExitCode::SUCCESS)
        }
    }
}

struct ChildArgsEnvPageWriter {
    source_va: u64,
    arg_count: u64,
    env_count: u64,
    string_bytes: usize,
}

impl ChildArgsEnvPageWriter {
    fn new(source_va: u64) -> Self {
        init_child_args_env_source_va(source_va);
        Self {
            source_va,
            arg_count: 0,
            env_count: 0,
            string_bytes: 0,
        }
    }

    fn push_arg(&mut self, value: &str) -> bool {
        if self.arg_count as usize >= PROCESS_ARGS_ENV_MAX_ARGS {
            return false;
        }
        let entry_offset = 48 + (self.arg_count as usize * core::mem::size_of::<ArgsEnvEntry>());
        if !self.write_entry(entry_offset, value.as_bytes()) {
            return false;
        }
        self.arg_count += 1;
        self.write_header_counts();
        true
    }

    fn push_env(&mut self, key: &str, value: &str) -> bool {
        if self.env_count as usize >= PROCESS_ARGS_ENV_MAX_ENV {
            return false;
        }
        if key.as_bytes().contains(&b'=') {
            return false;
        }
        let total_len = key.len() + 1 + value.len();
        if self.string_bytes + total_len > PROCESS_ARGS_ENV_DATA_BYTES
            || total_len > u16::MAX as usize
        {
            return false;
        }
        let entry_offset = 48
            + (PROCESS_ARGS_ENV_MAX_ARGS * core::mem::size_of::<ArgsEnvEntry>())
            + (self.env_count as usize * core::mem::size_of::<ArgsEnvEntry>());
        // SAFETY: Offsets are validated against the fixed 4 KiB bootstrap page,
        // and writes target only the caller-owned scratch page.
        unsafe {
            let entry = self.entry_ptr(entry_offset);
            (*entry).offset = self.string_bytes as u16;
            (*entry).len = total_len as u16;
            let data_ptr = self.data_ptr();
            copy_bytes(data_ptr.add(self.string_bytes), key.as_bytes());
            *data_ptr.add(self.string_bytes + key.len()) = b'=';
            copy_bytes(
                data_ptr.add(self.string_bytes + key.len() + 1),
                value.as_bytes(),
            );
        }
        self.string_bytes += total_len;
        self.env_count += 1;
        self.write_header_counts();
        true
    }

    fn write_entry(&mut self, entry_offset: usize, bytes: &[u8]) -> bool {
        if self.string_bytes + bytes.len() > PROCESS_ARGS_ENV_DATA_BYTES
            || bytes.len() > u16::MAX as usize
        {
            return false;
        }
        // SAFETY: Offsets are validated against the fixed 4 KiB bootstrap page,
        // and writes target only the caller-owned scratch page.
        unsafe {
            let entry = self.entry_ptr(entry_offset);
            (*entry).offset = self.string_bytes as u16;
            (*entry).len = bytes.len() as u16;
            copy_bytes(self.data_ptr().add(self.string_bytes), bytes);
        }
        self.string_bytes += bytes.len();
        self.write_header_counts();
        true
    }

    fn write_header_counts(&self) {
        // SAFETY: Header words live in the caller-owned scratch page.
        unsafe {
            let words = self.source_va as *mut u64;
            words.add(2).write(self.arg_count);
            words.add(3).write(self.env_count);
            words.add(4).write(self.string_bytes as u64);
        }
    }

    unsafe fn entry_ptr(&self, entry_offset: usize) -> *mut ArgsEnvEntry {
        unsafe { (self.source_va as *mut u8).add(entry_offset) as *mut ArgsEnvEntry }
    }

    unsafe fn data_ptr(&self) -> *mut u8 {
        unsafe {
            (self.source_va as *mut u8).add(
                48 + ((PROCESS_ARGS_ENV_MAX_ARGS + PROCESS_ARGS_ENV_MAX_ENV)
                    * core::mem::size_of::<ArgsEnvEntry>()),
            )
        }
    }
}

unsafe fn copy_bytes(dst: *mut u8, src: &[u8]) {
    let mut i = 0;
    while i < src.len() {
        unsafe {
            *dst.add(i) = src[i];
        }
        i += 1;
    }
}

pub fn abort() -> ! {
    rt_core::abort()
}

pub fn exit(code: ExitCode) -> ! {
    rt_core::process_exit(code.raw())
}

pub fn finish(code: ExitCode) -> ! {
    exit(code)
}
