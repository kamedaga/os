#![no_std]

extern crate alloc;

use alloc::string::String;
use core::fmt;
use core::fmt::Write as _;

pub use rt_core;

pub mod env;
pub mod fs;
pub mod io;
pub mod path;
pub mod process;
pub mod time;

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub enum ErrorKind {
    InvalidInput,
    InvalidData,
    Closed,
    NotFound,
    NotDirectory,
    IsDirectory,
    PermissionDenied,
    TimedOut,
    Busy,
    Unsupported,
    BufferTooSmall,
    ConnectionFailed,
    Other,
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub struct Error {
    kind: ErrorKind,
}

impl Error {
    pub const fn new(kind: ErrorKind) -> Self {
        Self { kind }
    }

    pub const fn kind(self) -> ErrorKind {
        self.kind
    }
}

impl From<rt_io::Error> for Error {
    fn from(value: rt_io::Error) -> Self {
        use rt_io::Error as RtIoError;

        let kind = match value {
            RtIoError::PathTooLong | RtIoError::Invalid | RtIoError::WrongConnection => {
                ErrorKind::InvalidInput
            }
            RtIoError::NotFound => ErrorKind::NotFound,
            RtIoError::NotDir => ErrorKind::NotDirectory,
            RtIoError::IsDir => ErrorKind::IsDirectory,
            RtIoError::NoRight => ErrorKind::PermissionDenied,
            RtIoError::Timeout => ErrorKind::TimedOut,
            RtIoError::Busy => ErrorKind::Busy,
            RtIoError::NotSupported => ErrorKind::Unsupported,
            RtIoError::BufferTooSmall | RtIoError::TooBig => ErrorKind::BufferTooSmall,
            RtIoError::MissingService
            | RtIoError::ConnectSendFailed
            | RtIoError::EndpointInstallFailed
            | RtIoError::RequestAllocFailed
            | RtIoError::RequestMapFailed
            | RtIoError::ResponseAllocFailed
            | RtIoError::ResponseMapFailed
            | RtIoError::ResponseGrantFailed => ErrorKind::ConnectionFailed,
            RtIoError::InvalidResponse | RtIoError::IoError => ErrorKind::Other,
        };
        Self::new(kind)
    }
}

impl From<rt_core::SyscallError> for Error {
    fn from(value: rt_core::SyscallError) -> Self {
        use rt_core::SyscallError;

        let kind = match value {
            SyscallError::Invalid => ErrorKind::InvalidInput,
            SyscallError::NotReady | SyscallError::Empty => ErrorKind::Busy,
            SyscallError::Grant => ErrorKind::PermissionDenied,
            SyscallError::Send | SyscallError::Endpoint => ErrorKind::ConnectionFailed,
            SyscallError::Alloc
            | SyscallError::Map
            | SyscallError::Move
            | SyscallError::DropPresent
            | SyscallError::Revoke
            | SyscallError::Unexpected(_) => ErrorKind::Other,
        };
        Self::new(kind)
    }
}

pub type Result<T> = core::result::Result<T, Error>;

#[doc(hidden)]
pub fn __print(args: fmt::Arguments<'_>) -> Result<()> {
    let mut message = String::new();
    message
        .write_fmt(args)
        .map_err(|_| Error::new(ErrorKind::Other))?;
    rt_core::log(&message);
    Ok(())
}

pub fn run<T>(main: fn() -> T) -> !
where
    T: process::Termination,
{
    process::finish(main().report())
}

#[macro_export]
macro_rules! entry_point {
    ($main:path) => {
        fn __cap_std_entry() -> ! {
            $crate::run($main)
        }

        $crate::rt_core::entry_point!(__cap_std_entry);
    };
}

#[macro_export]
macro_rules! print {
    ($($arg:tt)*) => {
        $crate::__print(format_args!($($arg)*))
    };
}

#[macro_export]
macro_rules! println {
    () => {
        $crate::__print(format_args!("\n"))
    };
    ($($arg:tt)*) => {
        $crate::__print(format_args!("{}\n", format_args!($($arg)*)))
    };
}
