#![no_std]

use core::arch::asm;
use core::panic::PanicInfo;
use core::ptr::{copy_nonoverlapping, read_volatile, write_volatile};
use core::sync::atomic::{Ordering, compiler_fence};

const USER_LOG_MAX_BYTES: usize = 256;
const PAGE_BYTES: usize = 4096;
const PROCESS_AUX_BASE_VA: usize = 0x3C00_0000;
const DEFAULT_STACK_EXTENSION_PAGES: usize = 32;
const STDIO_SINK_TARGET_VA: usize = 0x3C02_2000;
const STDIO_SINK_MAGIC: u64 = 0x5354_4449_4F53_4831;
const STDIO_SINK_VERSION: u64 = 1;
const STDIO_SINK_STATE_IDLE: u64 = 0;
const STDIO_SINK_STATE_READY: u64 = 1;
const STDIO_SINK_PAYLOAD_BYTES: usize = 512;
const STDIO_SINK_STREAM_CONTROL: u64 = 3;
const STDIO_CONTROL_MAGIC: u64 = 0x5354_4449_4F43_5431;
const STDIO_CONTROL_VERSION: u64 = 1;
const STDIO_CONTROL_OP_ALLOCATE_INHERITED: u64 = 1;
const STDIO_CONTROL_OP_BIND_INHERITED: u64 = 2;
const STDIO_CONTROL_RESPONSE_ENDPOINT_ID: u64 = 0x91;
const CAP_TRANSFER_ID_MIN: u64 = 0x1000;
const STDIO_MODE_KERNEL_LOG: u64 = 1;
const STDIO_MODE_NULL: u64 = 2;
const STDIO_MODE_MASK: u64 = 0x3;
const STDIO_LOG_MODE_SHIFT: u64 = 0;
const STDIO_STDOUT_MODE_SHIFT: u64 = 2;
const STDIO_STDERR_MODE_SHIFT: u64 = 4;
const PROCESS_EXIT_STATUS_TARGET_VA: usize = 0x3C02_3000;
const PROCESS_EXIT_STATUS_MAGIC: u64 = 0x5052_5845_5449_5431;
const PROCESS_EXIT_STATUS_VERSION: u64 = 1;
const PROCESS_EXIT_STATUS_STATE_EXITED: u64 = 1;

#[repr(u64)]
#[derive(Copy, Clone)]
#[allow(dead_code)]
enum StdioStreamKind {
    Log = 0,
    Stdout = 1,
    Stderr = 2,
}

#[repr(C)]
struct StdioSinkPage {
    magic: u64,
    version: u64,
    state: u64,
    endpoint_id: u64,
    shell_process_slot: u64,
    stream_kind: u64,
    byte_len: u64,
    reserved0: u64,
    payload: [u8; STDIO_SINK_PAYLOAD_BYTES],
    reserved: [u8; PAGE_BYTES - 64 - STDIO_SINK_PAYLOAD_BYTES],
}

#[derive(Copy, Clone)]
struct StdioSinkState {
    endpoint_installed: bool,
}

#[repr(C)]
struct StdioControlPacket {
    magic: u64,
    version: u64,
    op: u64,
    arg0: u64,
    arg1: u64,
}

#[repr(C)]
struct ProcessExitStatusPage {
    magic: u64,
    version: u64,
    state: u64,
    exit_code: u64,
    reserved: [u8; PAGE_BYTES - 32],
}

static mut STDIO_SINK_STATE: Option<StdioSinkState> = None;
static mut STDIO_SINK_DIAG_MASK: u32 = 0;

pub mod syscall {
    use core::arch::asm;

    pub const ALLOC_PAGE: u64 = 0x1;
    pub const MAP_PAGE: u64 = 0x2;
    pub const GRANT_CAP: u64 = 0x8;
    pub const LOG: u64 = 0x9;
    pub const ALLOC_MAP_PAGES: u64 = 0xC;
    pub const WAIT_EVENT: u64 = 0x17;
    pub const SPAWN_EXEC: u64 = 0x1D;
    pub const INSTALL_VM_OBJECT: u64 = 0x1E;
    pub const GRANT_VM_OBJECT: u64 = 0x1F;
    pub const INSTALL_EXEC_IMAGE: u64 = 0x20;
    pub const GRANT_EXEC_IMAGE: u64 = 0x21;
    pub const GRANT_CAP_ON_ENDPOINT: u64 = 0x24;
    pub const INSTALL_ENDPOINT: u64 = 0x26;
    pub const MAP_VM_OBJECT: u64 = 0x28;
    pub const SLICE_VM_OBJECT: u64 = 0x29;
    pub const ACCEPT_CAP_TRANSFER: u64 = 0x2A;
    pub const SHARE_CAP: u64 = 0x2B;
    pub const SIGNAL_ENDPOINT: u64 = 0x2C;
    pub const GET_TICK_COUNT: u64 = 0x2D;
    pub const GET_PROCESS_SLOT: u64 = 0x2E;
    pub const GET_PROCESS_STATUS: u64 = 0x30;
    pub const PROCESS_EXIT: u64 = 0x34;
    pub const OK: u64 = 0;
    pub const ERR_INVALID: u64 = 1;
    pub const ERR_NOT_READY: u64 = 2;
    pub const ERR_ALLOC: u64 = 4;
    pub const ERR_MAP: u64 = 5;
    pub const ERR_MOVE: u64 = 6;
    pub const ERR_DROP_PRESENT: u64 = 7;
    pub const ERR_SEND: u64 = 8;
    pub const ERR_ENDPOINT: u64 = 9;
    pub const ERR_REVOKE: u64 = 10;
    pub const ERR_GRANT: u64 = 11;
    pub const ERR_EMPTY: u64 = 13;

    #[inline]
    pub fn call0(nr: u64) -> u64 {
        let ret: u64;
        // SAFETY: Uses the current CapabilityOS int 0x80 syscall ABI.
        unsafe {
            asm!(
                "int 0x80",
                inlateout("rax") nr => ret,
                lateout("rdx") _,
                lateout("rcx") _,
                lateout("r8") _,
                lateout("r9") _,
                lateout("r10") _,
                lateout("r11") _,
                options(nostack),
            );
        }
        ret
    }

    #[inline]
    pub fn call1(nr: u64, arg0: u64) -> u64 {
        let ret: u64;
        // SAFETY: Uses the current CapabilityOS int 0x80 syscall ABI.
        unsafe {
            asm!(
                "int 0x80",
                inlateout("rax") nr => ret,
                in("rdi") arg0,
                lateout("rdx") _,
                lateout("rcx") _,
                lateout("r8") _,
                lateout("r9") _,
                lateout("r10") _,
                lateout("r11") _,
                options(nostack),
            );
        }
        ret
    }

    #[inline]
    pub fn call2(nr: u64, arg0: u64, arg1: u64) -> u64 {
        let ret: u64;
        // SAFETY: Uses the current CapabilityOS int 0x80 syscall ABI.
        unsafe {
            asm!(
                "int 0x80",
                inlateout("rax") nr => ret,
                in("rdi") arg0,
                in("rsi") arg1,
                lateout("rdx") _,
                lateout("rcx") _,
                lateout("r8") _,
                lateout("r9") _,
                lateout("r10") _,
                lateout("r11") _,
                options(nostack),
            );
        }
        ret
    }

    #[inline]
    pub fn call3(nr: u64, arg0: u64, arg1: u64, arg2: u64) -> u64 {
        let ret: u64;
        // SAFETY: Uses the current CapabilityOS int 0x80 syscall ABI.
        unsafe {
            asm!(
                "int 0x80",
                inlateout("rax") nr => ret,
                in("rdi") arg0,
                in("rsi") arg1,
                inlateout("rdx") arg2 => _,
                lateout("rcx") _,
                lateout("r8") _,
                lateout("r9") _,
                lateout("r10") _,
                lateout("r11") _,
                options(nostack),
            );
        }
        ret
    }

    #[inline]
    pub fn call4(nr: u64, arg0: u64, arg1: u64, arg2: u64, arg3: u64) -> u64 {
        let ret: u64;
        // SAFETY: Uses the current CapabilityOS int 0x80 syscall ABI.
        unsafe {
            asm!(
                "int 0x80",
                inlateout("rax") nr => ret,
                in("rdi") arg0,
                in("rsi") arg1,
                inlateout("rdx") arg2 => _,
                inlateout("rcx") arg3 => _,
                lateout("r8") _,
                lateout("r9") _,
                lateout("r10") _,
                lateout("r11") _,
                options(nostack),
            );
        }
        ret
    }
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub enum SyscallError {
    Invalid,
    NotReady,
    Alloc,
    Map,
    Move,
    DropPresent,
    Send,
    Endpoint,
    Revoke,
    Grant,
    Empty,
    Unexpected(u64),
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub enum ProcessStatusKind {
    Inactive,
    Active,
    Faulted,
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub struct ProcessStatus {
    kind: ProcessStatusKind,
    fault_vector: u8,
}

impl ProcessStatus {
    pub const fn new(kind: ProcessStatusKind, fault_vector: u8) -> Self {
        Self { kind, fault_vector }
    }

    pub const fn kind(self) -> ProcessStatusKind {
        self.kind
    }

    pub const fn fault_vector(self) -> u8 {
        self.fault_vector
    }
}

impl SyscallError {
    pub const fn from_error_raw(raw: u64) -> Self {
        match raw {
            syscall::ERR_INVALID => Self::Invalid,
            syscall::ERR_NOT_READY => Self::NotReady,
            syscall::ERR_ALLOC => Self::Alloc,
            syscall::ERR_MAP => Self::Map,
            syscall::ERR_MOVE => Self::Move,
            syscall::ERR_DROP_PRESENT => Self::DropPresent,
            syscall::ERR_SEND => Self::Send,
            syscall::ERR_ENDPOINT => Self::Endpoint,
            syscall::ERR_REVOKE => Self::Revoke,
            syscall::ERR_GRANT => Self::Grant,
            syscall::ERR_EMPTY => Self::Empty,
            _ => Self::Unexpected(raw),
        }
    }

    pub const fn raw(self) -> u64 {
        match self {
            Self::Invalid => syscall::ERR_INVALID,
            Self::NotReady => syscall::ERR_NOT_READY,
            Self::Alloc => syscall::ERR_ALLOC,
            Self::Map => syscall::ERR_MAP,
            Self::Move => syscall::ERR_MOVE,
            Self::DropPresent => syscall::ERR_DROP_PRESENT,
            Self::Send => syscall::ERR_SEND,
            Self::Endpoint => syscall::ERR_ENDPOINT,
            Self::Revoke => syscall::ERR_REVOKE,
            Self::Grant => syscall::ERR_GRANT,
            Self::Empty => syscall::ERR_EMPTY,
            Self::Unexpected(raw) => raw,
        }
    }
}

fn decode_process_status(raw: u64) -> ProcessStatus {
    let kind = match (raw & 0xFF) as u8 {
        1 => ProcessStatusKind::Active,
        2 => ProcessStatusKind::Faulted,
        _ => ProcessStatusKind::Inactive,
    };
    let fault_vector = ((raw >> 8) & 0xFF) as u8;
    ProcessStatus::new(kind, fault_vector)
}

fn log_kernel_bytes(bytes: &[u8]) {
    let mut offset = 0;
    while offset < bytes.len() {
        let end = usize::min(offset + USER_LOG_MAX_BYTES, bytes.len());
        let chunk = &bytes[offset..end];
        let _ = syscall::call2(syscall::LOG, chunk.as_ptr() as u64, chunk.len() as u64);
        offset = end;
    }
}

fn log_stdio_diag_once(bit: u32, message: &str) {
    // SAFETY: CapabilityOS user processes are single-threaded today.
    unsafe {
        if (STDIO_SINK_DIAG_MASK & bit) != 0 {
            return;
        }
        STDIO_SINK_DIAG_MASK |= bit;
    }
    log_kernel_bytes(message.as_bytes());
}

fn stdio_mode_for_stream(page: &StdioSinkPage, kind: StdioStreamKind) -> u64 {
    let shift = match kind {
        StdioStreamKind::Log => STDIO_LOG_MODE_SHIFT,
        StdioStreamKind::Stdout => STDIO_STDOUT_MODE_SHIFT,
        StdioStreamKind::Stderr => STDIO_STDERR_MODE_SHIFT,
    };
    // SAFETY: `page` points at the fixed stdio bootstrap page, which is safe to
    // probe via volatile reads in the caller's validated context.
    unsafe { (read_volatile(&page.reserved0) >> shift) & STDIO_MODE_MASK }
}

fn current_stdio_mode(kind: StdioStreamKind) -> Option<u64> {
    // SAFETY: All spawned user processes now receive a fixed stdio bootstrap page
    // at this VA, either zeroed or shell-backed, so probing it is safe.
    unsafe {
        let page = &*(STDIO_SINK_TARGET_VA as *const StdioSinkPage);
        if read_volatile(&page.magic) != STDIO_SINK_MAGIC
            || read_volatile(&page.version) != STDIO_SINK_VERSION
        {
            return None;
        }
        Some(stdio_mode_for_stream(page, kind))
    }
}

fn try_stdio_sink_chunk(kind: StdioStreamKind, chunk: &[u8]) -> bool {
    if chunk.is_empty() {
        return true;
    }

    // SAFETY: All spawned user processes now receive a fixed stdio bootstrap page
    // at this VA, either zeroed or shell-backed, so probing it is safe.
    unsafe {
        let page = &mut *(STDIO_SINK_TARGET_VA as *mut StdioSinkPage);
        if read_volatile(&page.magic) != STDIO_SINK_MAGIC
            || read_volatile(&page.version) != STDIO_SINK_VERSION
        {
            log_stdio_diag_once(1 << 0, "rt_core: stdio page invalid\n");
            return false;
        }

        match stdio_mode_for_stream(page, kind) {
            STDIO_MODE_NULL => return true,
            STDIO_MODE_KERNEL_LOG => return false,
            _ => {}
        }

        let endpoint_id = read_volatile(&page.endpoint_id);
        let shell_process_slot = read_volatile(&page.shell_process_slot);
        if endpoint_id == 0 || shell_process_slot == 0 {
            log_stdio_diag_once(1 << 1, "rt_core: stdio sink unavailable\n");
            return false;
        }

        let mut state = STDIO_SINK_STATE.unwrap_or(StdioSinkState {
            endpoint_installed: false,
        });
        if !state.endpoint_installed {
            let install_status = syscall::call3(
                syscall::INSTALL_ENDPOINT,
                0,
                endpoint_id,
                shell_process_slot,
            );
            if install_status != syscall::OK {
                log_stdio_diag_once(1 << 2, "rt_core: stdio endpoint install failed\n");
                return false;
            }
            state.endpoint_installed = true;
        }

        if read_volatile(&page.state) != STDIO_SINK_STATE_IDLE {
            STDIO_SINK_STATE = Some(state);
            log_stdio_diag_once(1 << 4, "rt_core: stdio sink busy\n");
            return false;
        }

        write_volatile(&mut page.stream_kind, kind as u64);
        write_volatile(&mut page.byte_len, chunk.len() as u64);
        copy_nonoverlapping(chunk.as_ptr(), page.payload.as_mut_ptr(), chunk.len());
        compiler_fence(Ordering::Release);
        write_volatile(&mut page.state, STDIO_SINK_STATE_READY);
        compiler_fence(Ordering::Release);
        let _ = syscall::call1(syscall::SIGNAL_ENDPOINT, endpoint_id);
        STDIO_SINK_STATE = Some(state);
        true
    }
}

fn write_stream_bytes(kind: StdioStreamKind, bytes: &[u8]) {
    let mut offset = 0;
    while offset < bytes.len() {
        let end = usize::min(offset + STDIO_SINK_PAYLOAD_BYTES, bytes.len());
        let chunk = &bytes[offset..end];
        if !try_stdio_sink_chunk(kind, chunk) {
            log_kernel_bytes(chunk);
        }
        offset = end;
    }
}

fn try_send_stdio_control(packet: &StdioControlPacket) -> Result<bool, SyscallError> {
    // SAFETY: All spawned user processes now receive a fixed stdio bootstrap page
    // at this VA, either zeroed or shell-backed, so probing it is safe.
    unsafe {
        let page = &mut *(STDIO_SINK_TARGET_VA as *mut StdioSinkPage);
        if read_volatile(&page.magic) != STDIO_SINK_MAGIC
            || read_volatile(&page.version) != STDIO_SINK_VERSION
        {
            log_stdio_diag_once(1 << 0, "rt_core: stdio page invalid\n");
            return Ok(false);
        }

        let endpoint_id = read_volatile(&page.endpoint_id);
        let shell_process_slot = read_volatile(&page.shell_process_slot);
        if endpoint_id == 0 || shell_process_slot == 0 {
            log_stdio_diag_once(1 << 1, "rt_core: stdio sink unavailable\n");
            return Ok(false);
        }

        let mut state = STDIO_SINK_STATE.unwrap_or(StdioSinkState {
            endpoint_installed: false,
        });
        if !state.endpoint_installed {
            let install_status = syscall::call3(
                syscall::INSTALL_ENDPOINT,
                0,
                endpoint_id,
                shell_process_slot,
            );
            if install_status != syscall::OK {
                log_stdio_diag_once(1 << 2, "rt_core: stdio endpoint install failed\n");
                return Ok(false);
            }
            state.endpoint_installed = true;
        }

        let mut spin_count = 0;
        while read_volatile(&page.state) != STDIO_SINK_STATE_IDLE && spin_count < 16 {
            let _ = syscall::call2(syscall::WAIT_EVENT, 0, 1);
            spin_count += 1;
        }
        if read_volatile(&page.state) != STDIO_SINK_STATE_IDLE {
            STDIO_SINK_STATE = Some(state);
            log_stdio_diag_once(1 << 4, "rt_core: stdio sink busy\n");
            return Ok(false);
        }

        let packet_bytes = core::mem::size_of::<StdioControlPacket>();
        copy_nonoverlapping(
            packet as *const StdioControlPacket as *const u8,
            page.payload.as_mut_ptr(),
            packet_bytes,
        );
        write_volatile(&mut page.stream_kind, STDIO_SINK_STREAM_CONTROL);
        write_volatile(&mut page.byte_len, packet_bytes as u64);
        compiler_fence(Ordering::Release);
        write_volatile(&mut page.state, STDIO_SINK_STATE_READY);
        compiler_fence(Ordering::Release);
        let _ = syscall::call1(syscall::SIGNAL_ENDPOINT, endpoint_id);
        STDIO_SINK_STATE = Some(state);
        Ok(true)
    }
}

pub fn log_bytes(bytes: &[u8]) {
    if bytes.is_empty() {
        return;
    }
    let mode = current_stdio_mode(StdioStreamKind::Log);
    write_stream_bytes(StdioStreamKind::Log, bytes);
    if matches!(mode, Some(mode) if mode != STDIO_MODE_NULL && mode != STDIO_MODE_KERNEL_LOG) {
        log_kernel_bytes(bytes);
    }
}

pub fn log(message: &str) {
    log_bytes(message.as_bytes());
}

pub fn request_inherited_stdio_page() -> Result<Option<u64>, SyscallError> {
    let packet = StdioControlPacket {
        magic: STDIO_CONTROL_MAGIC,
        version: STDIO_CONTROL_VERSION,
        op: STDIO_CONTROL_OP_ALLOCATE_INHERITED,
        arg0: STDIO_CONTROL_RESPONSE_ENDPOINT_ID,
        arg1: 0,
    };
    if !try_send_stdio_control(&packet)? {
        return Ok(None);
    }

    let mut attempts = 0;
    while attempts < 128 {
        let received = syscall::call2(syscall::WAIT_EVENT, 1, 1);
        if received >= CAP_TRANSFER_ID_MIN {
            let accepted = syscall::call1(syscall::ACCEPT_CAP_TRANSFER, received);
            if accepted >= CAP_TRANSFER_ID_MIN {
                return Ok(Some(accepted));
            }
            return Err(SyscallError::from_error_raw(accepted));
        }
        if received == syscall::OK
            || received == syscall::ERR_NOT_READY
            || received == syscall::ERR_EMPTY
        {
            attempts += 1;
            continue;
        }
        return Err(SyscallError::from_error_raw(received));
    }

    Ok(None)
}

pub fn bind_inherited_stdio_page(
    paddr: u64,
    child_process_slot: u64,
) -> Result<bool, SyscallError> {
    let packet = StdioControlPacket {
        magic: STDIO_CONTROL_MAGIC,
        version: STDIO_CONTROL_VERSION,
        op: STDIO_CONTROL_OP_BIND_INHERITED,
        arg0: paddr,
        arg1: child_process_slot,
    };
    try_send_stdio_control(&packet)
}

pub fn get_process_status(process_slot: u64) -> ProcessStatus {
    decode_process_status(syscall::call1(syscall::GET_PROCESS_STATUS, process_slot))
}

fn record_process_exit(code: u8) {
    // SAFETY: The page is mapped at a fixed user VA when the bootstrap includes
    // the standard process-exit status page. The header is validated before
    // writes, and writes use volatile semantics because the parent polls it.
    unsafe {
        let page = &mut *(PROCESS_EXIT_STATUS_TARGET_VA as *mut ProcessExitStatusPage);
        if read_volatile(&page.magic) != PROCESS_EXIT_STATUS_MAGIC
            || read_volatile(&page.version) != PROCESS_EXIT_STATUS_VERSION
        {
            return;
        }
        write_volatile(&mut page.exit_code, code as u64);
        compiler_fence(Ordering::Release);
        write_volatile(&mut page.state, PROCESS_EXIT_STATUS_STATE_EXITED);
        compiler_fence(Ordering::Release);
    }
}

fn extend_default_stack() -> Result<(), SyscallError> {
    let base_va = PROCESS_AUX_BASE_VA - ((DEFAULT_STACK_EXTENSION_PAGES + 1) * PAGE_BYTES);
    let status = syscall::call4(
        syscall::ALLOC_MAP_PAGES,
        base_va as u64,
        DEFAULT_STACK_EXTENSION_PAGES as u64,
        1,
        0,
    );
    if status == syscall::OK {
        Ok(())
    } else {
        Err(SyscallError::from_error_raw(status))
    }
}

pub fn process_exit(code: u8) -> ! {
    record_process_exit(code);
    let ret = syscall::call1(syscall::PROCESS_EXIT, code as u64);
    if ret != syscall::OK {
        log("rt_core: process_exit failed\n");
    } else {
        log("rt_core: process_exit returned\n");
    }
    abort()
}

pub fn abort() -> ! {
    loop {
        // SAFETY: `pause` is safe in a tight abort loop and does not access memory.
        unsafe {
            asm!("pause", options(nomem, nostack, preserves_flags));
        }
    }
}

pub fn panic_abort(_info: &PanicInfo<'_>) -> ! {
    log("rt_core: panic\n");
    abort()
}

pub fn start(main: fn() -> !) -> ! {
    if let Err(err) = extend_default_stack() {
        log("rt_core: stack extend failed\n");
        let _ = err;
    }
    main()
}

#[macro_export]
macro_rules! entry_point {
    ($main:path) => {
        #[unsafe(no_mangle)]
        pub extern "C" fn _start() -> ! {
            $crate::start($main)
        }

        #[panic_handler]
        fn panic(info: &core::panic::PanicInfo<'_>) -> ! {
            $crate::panic_abort(info)
        }
    };
}
