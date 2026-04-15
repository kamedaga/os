#![no_std]
#![no_main]

extern crate alloc;

use cap_std::fs::RootDir;
use cap_std::path::Path;
use cap_std::process::{Command, ExitStatus, Stdio};
use rt_alloc as _;

const EXEC_PATH: &str = "/cmd/cap_std_child_exit_demo.elf";

fn main() -> cap_std::Result<()> {
    let root = RootDir::connect_default()?;
    let exec = root.open_exec(Path::new(EXEC_PATH))?;
    let child = Command::new(exec)
        .with_arg0(EXEC_PATH)
        .with_arg("from_wait_demo")
        .with_env("PWD", "/tmp")
        .with_log(Stdio::Inherit)
        .with_stdout(Stdio::Inherit)
        .with_stderr(Stdio::Inherit)
        .spawn()?;
    let status = child.wait()?;
    match status {
        ExitStatus::Exited(code) => {
            cap_std::println!(
                "cap std wait demo path={} child_exit={} success={}",
                EXEC_PATH,
                code.raw(),
                code.success()
            )?;
        }
        ExitStatus::Faulted(vector) => {
            cap_std::println!(
                "cap std wait demo path={} child_fault_vector={}",
                EXEC_PATH,
                vector
            )?;
        }
    }
    Ok(())
}

cap_std::entry_point!(main);
