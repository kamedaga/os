#![no_std]
#![no_main]

extern crate alloc;

use alloc::string::String;
use cap_std::env;
use cap_std::process::ExitCode;
use rt_alloc as _;

fn main() -> ExitCode {
    let arg_count = env::args_count();
    let arg0 = env::arg(0).unwrap_or_else(|| String::from("<none>"));
    let arg1 = env::arg(1).unwrap_or_else(|| String::from("<none>"));
    let pwd = env::var("PWD").unwrap_or_else(|| String::from("<none>"));
    let _ = cap_std::println!(
        "cap std child exit demo code=7 argc={} arg0={} arg1={} pwd={}",
        arg_count,
        arg0,
        arg1,
        pwd
    );
    ExitCode::from_raw(7)
}

cap_std::entry_point!(main);
