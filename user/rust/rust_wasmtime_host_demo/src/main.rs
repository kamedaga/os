#![no_std]
#![no_main]

extern crate alloc;

use alloc::string::String;
use core::fmt::Write;
use rt_alloc as _;
use rt_core::entry_point;

fn main() -> ! {
    let config = wasmtime_host::smoke_config();
    let mut message = String::new();
    let _ = write!(
        &mut message,
        "rust wasmtime host demo features={} page_size={} config_size={}\n",
        wasmtime_host::WASMTIME_FEATURE_POLICY,
        wasmtime_host::platform_page_size(),
        core::mem::size_of_val(&config)
    );
    rt_core::log(&message);
    rt_core::abort()
}

entry_point!(main);
