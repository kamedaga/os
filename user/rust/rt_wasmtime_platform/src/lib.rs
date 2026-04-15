#![no_std]

//! Minimal Wasmtime custom-platform shim for CapabilityOS.
//!
//! This crate intentionally starts with the smallest boundary:
//! TLS get/set, page-size/aligned-allocation helpers, and a fatal path.
//! Any final binary using the allocation helpers is expected to link `rt_alloc`
//! (or another global allocator) itself.

extern crate alloc;

use alloc::alloc::{alloc_zeroed, dealloc};
use alloc::string::String;
use core::alloc::Layout;
use core::fmt::Write;
use core::ptr::{NonNull, null_mut};
use core::sync::atomic::{AtomicPtr, Ordering};

#[cfg(feature = "custom-native-signals")]
compile_error!("rt_wasmtime_platform: custom-native-signals is not implemented yet");

#[cfg(feature = "custom-sync-primitives")]
compile_error!("rt_wasmtime_platform: custom-sync-primitives is not implemented yet");

#[cfg(feature = "custom-virtual-memory")]
compile_error!("rt_wasmtime_platform: custom-virtual-memory is not implemented yet");

pub const PAGE_SIZE: usize = 4096;

static TLS_SLOT: AtomicPtr<u8> = AtomicPtr::new(null_mut());

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub enum AllocError {
    InvalidLayout,
    OutOfMemory,
}

pub struct PageAlignedBuffer {
    ptr: NonNull<u8>,
    len: usize,
    align: usize,
}

impl PageAlignedBuffer {
    pub fn new_zeroed(len: usize) -> Result<Self, AllocError> {
        Self::new_zeroed_aligned(len, PAGE_SIZE)
    }

    pub fn new_zeroed_aligned(len: usize, align: usize) -> Result<Self, AllocError> {
        if len == 0 {
            return Err(AllocError::InvalidLayout);
        }
        let layout = Layout::from_size_align(len, align).map_err(|_| AllocError::InvalidLayout)?;
        // SAFETY: `layout` was validated above.
        let ptr = unsafe { alloc_zeroed(layout) };
        let ptr = NonNull::new(ptr).ok_or(AllocError::OutOfMemory)?;
        Ok(Self { ptr, len, align })
    }

    pub const fn len(&self) -> usize {
        self.len
    }

    pub const fn align(&self) -> usize {
        self.align
    }

    pub fn as_ptr(&self) -> *const u8 {
        self.ptr.as_ptr()
    }

    pub fn as_mut_ptr(&mut self) -> *mut u8 {
        self.ptr.as_ptr()
    }

    pub fn is_page_aligned(&self) -> bool {
        (self.as_ptr() as usize) & (PAGE_SIZE - 1) == 0
    }

    pub fn is_zeroed_prefix(&self, len: usize) -> bool {
        let check_len = len.min(self.len);
        // SAFETY: buffer owns the region and `check_len <= self.len`.
        let slice = unsafe { core::slice::from_raw_parts(self.as_ptr(), check_len) };
        slice.iter().all(|byte| *byte == 0)
    }
}

impl Drop for PageAlignedBuffer {
    fn drop(&mut self) {
        let Ok(layout) = Layout::from_size_align(self.len, self.align) else {
            return;
        };
        // SAFETY: `self.ptr` was allocated with the same layout in constructors above.
        unsafe { dealloc(self.ptr.as_ptr(), layout) };
    }
}

pub const fn page_size() -> usize {
    PAGE_SIZE
}

pub fn tls_get() -> *mut u8 {
    TLS_SLOT.load(Ordering::Relaxed)
}

pub fn tls_set(ptr: *mut u8) {
    TLS_SLOT.store(ptr, Ordering::Relaxed);
}

pub fn log_diagnostic(message: &str) {
    let mut line = String::from("rt_wasmtime_platform: ");
    let _ = write!(&mut line, "{message}\n");
    rt_core::log(&line);
}

pub fn fatal(message: &str) -> ! {
    log_diagnostic(message);
    rt_core::abort()
}

#[unsafe(no_mangle)]
pub extern "C" fn wasmtime_tls_get() -> *mut u8 {
    tls_get()
}

#[unsafe(no_mangle)]
pub extern "C" fn wasmtime_tls_set(ptr: *mut u8) {
    tls_set(ptr);
}
