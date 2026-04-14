#![no_std]

use core::arch::asm;
use core::panic::PanicInfo;

pub mod syscall {
    use core::arch::asm;

    pub const ALLOC_PAGE: u64 = 0x1;
    pub const MAP_PAGE: u64 = 0x2;
    pub const LOG: u64 = 0x9;
    pub const ALLOC_MAP_PAGES: u64 = 0xC;
    pub const SPAWN_EXEC: u64 = 0x1D;
    pub const INSTALL_VM_OBJECT: u64 = 0x1E;
    pub const GRANT_VM_OBJECT: u64 = 0x1F;
    pub const INSTALL_EXEC_IMAGE: u64 = 0x20;
    pub const GRANT_EXEC_IMAGE: u64 = 0x21;
    pub const MAP_VM_OBJECT: u64 = 0x28;
    pub const SLICE_VM_OBJECT: u64 = 0x29;
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

pub fn log_bytes(bytes: &[u8]) {
    if bytes.is_empty() {
        return;
    }
    let _ = syscall::call2(syscall::LOG, bytes.as_ptr() as u64, bytes.len() as u64);
}

pub fn log(message: &str) {
    log_bytes(message.as_bytes());
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
