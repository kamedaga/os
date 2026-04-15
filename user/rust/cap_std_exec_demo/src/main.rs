#![no_std]
#![no_main]

extern crate alloc;

use cap_std::fs::RootDir;
use cap_std::path::Path;
use cap_std::process::{Command, Stdio};
use rt_alloc as _;

const EXEC_PATH: &str = "/cmd/pie_user.elf";

fn main() -> cap_std::Result<()> {
    let root = RootDir::connect_default()?;
    let exec = root.open_exec(Path::new(EXEC_PATH))?;
    let child = Command::new(exec)
        .with_log(Stdio::Inherit)
        .with_stdout(Stdio::Inherit)
        .with_stderr(Stdio::Inherit)
        .spawn()?;

    cap_std::println!(
        "cap std exec demo path={} exec_bytes={} child_process={} child_thread={}",
        EXEC_PATH,
        exec.len(),
        child.process_slot(),
        child.thread_slot()
    )?;
    Ok(())
}

cap_std::entry_point!(main);
