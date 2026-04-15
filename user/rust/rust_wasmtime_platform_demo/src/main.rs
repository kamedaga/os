#![no_std]
#![no_main]

extern crate alloc;

use alloc::string::String;
use core::fmt::Write;
use rt_alloc as _;
use rt_core::entry_point;
use rt_wasmtime_platform::{PageAlignedBuffer, page_size, tls_get, tls_set};

fn main() -> ! {
    let mut buffer = match PageAlignedBuffer::new_zeroed(page_size() * 2) {
        Ok(buffer) => buffer,
        Err(_) => {
            rt_core::log("rust wasmtime platform demo alloc failed\n");
            rt_core::abort()
        }
    };

    let ptr = buffer.as_mut_ptr();
    tls_set(ptr);
    let tls_ptr = tls_get();

    let mut message = String::new();
    let _ = write!(
        &mut message,
        "rust wasmtime platform demo page_size={} len={} ptr=0x{:x} tls=0x{:x} aligned={} zeroed={}\n",
        page_size(),
        buffer.len(),
        ptr as usize,
        tls_ptr as usize,
        buffer.is_page_aligned(),
        buffer.is_zeroed_prefix(64)
    );
    rt_core::log(&message);
    rt_core::abort()
}

entry_point!(main);
